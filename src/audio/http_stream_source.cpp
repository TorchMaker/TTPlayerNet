// 在线 http(s) 流式解码源实现（见 http_stream_source.h）。
//
// 两份关键复用：
//   1) WinHttpStream —— 实现 IStream，内部跑一个 WinHTTP 下载线程，把音频边下边喂进
//      内存缓冲；Media Foundation 的 MFCreateMFByteStreamOnStream 把它包成 IMFByteStream，
//      再交给 MFCreateSourceReaderFromByteStream 解码成 PCM。（与 MediaFoundationSource 的
//      归档成员分支同构，只是数据来源从「内存里的压缩包成员」换成「网络流」。）
//   2) HttpStreamSource::Open 的后半段 —— 完整克隆 MediaFoundationSource 的
//      SetStreamSelection / SetCurrentMediaType(PCM) / OutputFormat / Duration 流程。

#include "ttplayer/audio/http_stream_source.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <propvarutil.h>
#include <propkey.h>
#include <propsys.h>
#include <wrl/client.h>

#include "ttplayer/audio/online_stream_descriptor.h"
#include "ttplayer/platform/optional_windows_api.h"

namespace ttplayer::audio {
namespace {

std::wstring ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}
std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(n, L'\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// 把 URL 中的非 ASCII（及空格）按 UTF-8 做 %XX 编码，避免 WinHttpCrackUrl 降级成 '?'。
std::wstring EncodeNonAsciiForUrl(const std::wstring& url) {
    bool needs = false;
    for (wchar_t c : url)
        if (c < 0x21 || c > 0x7E) { needs = true; break; }
    if (!needs) return url;
    const int n = WideCharToMultiByte(CP_UTF8, 0, url.c_str(),
                                      static_cast<int>(url.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return url;
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, url.c_str(), static_cast<int>(url.size()),
                        bytes.data(), n, nullptr, nullptr);
    static const char* kHex = "0123456789ABCDEF";
    std::wstring out;
    out.reserve(bytes.size());
    for (unsigned char b : bytes) {
        if (b >= 0x21 && b < 0x7F) out.push_back(static_cast<wchar_t>(b));
        else {
            out.push_back(L'%');
            out.push_back(static_cast<wchar_t>(kHex[b >> 4]));
            out.push_back(static_cast<wchar_t>(kHex[b & 0x0F]));
        }
    }
    return out;
}

std::wstring HResultMessage(std::wstring_view operation, HRESULT result) {
    wchar_t* system_message{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0,
        reinterpret_cast<wchar_t*>(&system_message), 0, nullptr);
    std::wstring message(operation);
    message += L" failed (0x";
    std::wostringstream code;
    code << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
         << static_cast<uint32_t>(result);
    message += code.str();
    message += L")";
    if (length != 0 && system_message) {
        size_t detail_length = std::wcslen(system_message);
        while (detail_length != 0 &&
               (system_message[detail_length - 1] == L'\r' ||
                system_message[detail_length - 1] == L'\n'))
            system_message[--detail_length] = L'\0';
        message += L": ";
        message += system_message;
    }
    if (system_message) LocalFree(system_message);
    return message;
}

uint16_t ReadBe16(const std::byte* value) {
    return static_cast<uint16_t>((std::to_integer<uint8_t>(value[0]) << 8) |
                                 std::to_integer<uint8_t>(value[1]));
}
uint32_t ReadBe32(const std::byte* value) {
    return (static_cast<uint32_t>(std::to_integer<uint8_t>(value[0])) << 24) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(value[1])) << 16) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(value[2])) << 8) |
           static_cast<uint32_t>(std::to_integer<uint8_t>(value[3]));
}

std::wstring LowerExtension(const std::filesystem::path& path) {
    auto value = path.extension().wstring();
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::wstring CodecNameFromTag(uint32_t tag, const std::filesystem::path& path) {
    switch (tag) {
    case WAVE_FORMAT_PCM: return L"PCM";
    case WAVE_FORMAT_IEEE_FLOAT: return L"IEEE Float";
    case WAVE_FORMAT_MPEG: return L"MPEG Audio";
    case WAVE_FORMAT_MPEGLAYER3: return L"MPEG Layer-3";
    case 0x0160: return L"Windows Media Audio 1";
    case 0x0161: return L"Windows Media Audio 2";
    case 0x0162: return L"Windows Media Audio 9 Professional";
    case 0x0163: return L"Windows Media Audio 9 Lossless";
    case 0x1610: return L"AAC";
    case 0x2000: return L"AC-3";
    case 0x2001: return L"DTS";
    case 0xf1ac: return L"FLAC";
    default: break;
    }
    const auto extension = LowerExtension(path);
    if (extension == L".aac" || extension == L".m4a" || extension == L".m4b" ||
        extension == L".mp4") return L"AAC";
    if (extension == L".flac") return L"FLAC";
    if (extension == L".wma" || extension == L".wmv" || extension == L".asf")
        return L"Windows Media Audio";
    if (extension == L".mp3" || extension == L".mp2" || extension == L".mp1" ||
        extension == L".mpa" || extension == L".mp3pro") return L"MPEG Audio";
    return L"Audio";
}

// ---------------------------------------------------------------------------
// WinHttpStream —— 把 http(s) 音频流式喂给 Media Foundation 的自定义 IStream
// ---------------------------------------------------------------------------
class WinHttpStream final : public IStream {
public:
    WinHttpStream(std::string url, std::map<std::string, std::string> headers,
                  std::string user_agent)
        : url_(std::move(url)), headers_(std::move(headers)),
          user_agent_(std::move(user_agent)) {}

    ~WinHttpStream() {
        stop_ = true;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (request_) WinHttpCloseHandle(request_);
        if (connect_) WinHttpCloseHandle(connect_);
        if (session_) WinHttpCloseHandle(session_);
    }

    // 启动下载线程（总是在 Open 之后调用一次）。
    bool Start() {
        thread_ = std::thread(&WinHttpStream::DownloadLoop, this);
        return true;
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = --ref_;
        if (next == 0) delete this;
        return next;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IStream ||
            riid == IID_ISequentialStream) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // ISequentialStream / IStream
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (!pv || !pcbRead) return STG_E_INVALIDPOINTER;
        std::unique_lock lock(mutex_);
        while (read_pos_ >= write_pos_ && !eof_ && SUCCEEDED(error_)) {
            if (cv_.wait_for(lock, std::chrono::milliseconds(200)) ==
                std::cv_status::timeout)
                continue;
        }
        if (FAILED(error_)) { *pcbRead = 0; return error_; }
        if (read_pos_ >= write_pos_ && eof_) { *pcbRead = 0; return S_FALSE; }
        const size_t avail = write_pos_ - read_pos_;
        const size_t n = std::min<size_t>(cb, avail);
        if (n > 0) std::memcpy(pv, buffer_.data() + read_pos_, n);
        read_pos_ += n;
        *pcbRead = static_cast<ULONG>(n);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin,
                                   ULARGE_INTEGER* plibNewPosition) override {
        std::scoped_lock lock(mutex_);
        LONGLONG target = 0;
        switch (dwOrigin) {
        case STREAM_SEEK_SET: target = dlibMove.QuadPart; break;
        case STREAM_SEEK_CUR:
            target = static_cast<LONGLONG>(read_pos_) + dlibMove.QuadPart;
            break;
        case STREAM_SEEK_END:
            target = static_cast<LONGLONG>(write_pos_) + dlibMove.QuadPart;
            break;
        default: return STG_E_INVALIDFUNCTION;
        }
        if (target < 0) target = 0;
        // v1 只缓冲已下载部分：超出已下发的范围则夹到末尾（不重发 Range 请求）。
        if (target > static_cast<LONGLONG>(write_pos_))
            target = static_cast<LONGLONG>(write_pos_);
        read_pos_ = static_cast<size_t>(target);
        if (plibNewPosition) plibNewPosition->QuadPart = read_pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                         DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                          DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD) override {
        if (!pstatstg) return STG_E_INVALIDPOINTER;
        std::memset(pstatstg, 0, sizeof(STATSTG));
        pstatstg->type = STGTY_STREAM;
        pstatstg->grfMode = STGM_READ;
        // 流式：长度未知（0），让 MF 按无界顺序流处理，避免它试图 seek 到末尾探测时长。
        pstatstg->cbSize.QuadPart = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    void DownloadLoop() {
        // 同一套 WinHTTP 阻塞读取，只是把响应体逐段 append 到缓冲而非一次性返回。
        const std::wstring wurl = EncodeNonAsciiForUrl(ToWide(url_));
        URL_COMPONENTS uc{sizeof(uc)};
        uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUserNameLength =
            uc.dwPasswordLength = uc.dwUrlPathLength = uc.dwExtraInfoLength =
                static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
            SetError(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return;
        }
        auto get = [&](DWORD len, LPCWSTR ptr) -> std::wstring {
            return len ? std::wstring(ptr, len) : std::wstring{};
        };
        const std::wstring scheme = get(uc.dwSchemeLength, uc.lpszScheme);
        const std::wstring host = get(uc.dwHostNameLength, uc.lpszHostName);
        std::wstring port = uc.nPort ? std::to_wstring(uc.nPort) : std::wstring{};
        const std::wstring path = get(uc.dwUrlPathLength, uc.lpszUrlPath) +
                                  get(uc.dwExtraInfoLength, uc.lpszExtraInfo);
        if (path.empty()) { SetError(HRESULT_FROM_WIN32(ERROR_INVALID_DATA)); return; }

        session_ = WinHttpOpen(
            user_agent_.empty() ? L"TTPlayerMusicFree/0.1"
                                 : ToWide(user_agent_).c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) { SetError(HRESULT_FROM_WIN32(GetLastError())); return; }

        connect_ = WinHttpConnect(
            session_, host.c_str(),
            port.empty() ? (scheme == L"https" ? INTERNET_DEFAULT_HTTPS_PORT
                                               : INTERNET_DEFAULT_HTTP_PORT)
                         : static_cast<DWORD>(_wtoi(port.c_str())),
            0);
        if (!connect_) { SetError(HRESULT_FROM_WIN32(GetLastError())); return; }

        const DWORD flags = (scheme == L"https") ? WINHTTP_FLAG_SECURE : 0;
        request_ = WinHttpOpenRequest(connect_, L"GET", path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request_) { SetError(HRESULT_FROM_WIN32(GetLastError())); return; }

        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request_, WINHTTP_OPTION_REDIRECT_POLICY, &redirect,
                         sizeof(redirect));
        WinHttpSetTimeouts(request_, 20000, 20000, 20000, 20000);

        std::wstring header_blob;
        for (const auto& kv : headers_) {
            header_blob += ToWide(kv.first) + L": " + ToWide(kv.second) +
                           L"\r\n";
        }
        if (!user_agent_.empty())
            header_blob += L"User-Agent: " + ToWide(user_agent_) + L"\r\n";
        if (!header_blob.empty())
            WinHttpAddRequestHeaders(request_, header_blob.c_str(),
                                     static_cast<DWORD>(header_blob.size()),
                                     WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                nullptr, 0, 0, 0)) {
            SetError(HRESULT_FROM_WIN32(GetLastError()));
            return;
        }
        if (!WinHttpReceiveResponse(request_, nullptr)) {
            SetError(HRESULT_FROM_WIN32(GetLastError()));
            return;
        }

        // Content-Length（用于在诊断里判断是否完整，不影响流式消费）。
        DWORD len = 0, len_size = sizeof(len);
        if (WinHttpQueryHeaders(request_,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &len, &len_size,
                                WINHTTP_NO_HEADER_INDEX) &&
            len > 0) {
            std::scoped_lock lock(mutex_);
            total_length_ = len;
            known_length_ = true;
        }

        DWORD avail = 0;
        do {
            if (stop_) break;
            avail = 0;
            if (!WinHttpQueryDataAvailable(request_, &avail)) break;
            if (!avail) break;
            std::vector<std::byte> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(request_, buf.data(), avail, &read)) break;
            if (!read) break;
            {
                std::scoped_lock lock(mutex_);
                buffer_.insert(buffer_.end(), buf.begin(),
                                buf.begin() + static_cast<size_t>(read));
                write_pos_ = buffer_.size();
            }
            cv_.notify_all();
        } while (avail > 0 && !stop_);

        {
            std::scoped_lock lock(mutex_);
            eof_ = true;
        }
        cv_.notify_all();
    }

    void SetError(HRESULT hr) {
        std::scoped_lock lock(mutex_);
        error_ = hr;
        eof_ = true;
        cv_.notify_all();
    }

    std::string url_;
    std::map<std::string, std::string> headers_;
    std::string user_agent_;

    std::vector<std::byte> buffer_;   // 已下发数据（追加式，全程保留以支持回退 seek）
    size_t write_pos_ = 0;            // == buffer_.size()
    size_t read_pos_ = 0;             // 当前读取游标
    uint64_t total_length_ = 0;
    bool known_length_ = false;
    bool eof_ = false;
    HRESULT error_ = S_OK;

    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET request_ = nullptr;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<ULONG> ref_{1};
    std::atomic<bool> stop_{false};
};

} // namespace

bool HttpStreamSource::Fail(std::wstring_view operation, HRESULT result) {
    error_ = HResultMessage(operation, result);
    error_result_ = result;
    return false;
}

bool HttpStreamSource::Open(const std::filesystem::path& path,
                            const PlaybackOptions&) {
    auto descriptor = TakePendingOnlineStream(ToUtf8(path.native()));

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    HRESULT result = platform::MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result)) {
        attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        if (descriptor && !descriptor->is_hls) {
            // 自定义头流式：WinHttpStream -> IStream -> MFByteStream -> SourceReader
            Microsoft::WRL::ComPtr<IStream> istream(
                static_cast<IStream*>(new WinHttpStream(
                    descriptor->url, descriptor->headers, descriptor->user_agent)));
            if (!istream || !static_cast<WinHttpStream*>(istream.Get())->Start()) {
                result = E_FAIL;
            } else {
                http_stream_ = istream;
                result = platform::MFCreateMFByteStreamOnStream(
                    istream.Get(), &http_byte_stream_);
                if (SUCCEEDED(result))
                    result = platform::MFCreateSourceReaderFromByteStream(
                        http_byte_stream_.Get(), attributes.Get(), &reader_);
            }
        } else {
            // 无自定义头 / HLS：交给 MF 默认 URL reader（直链可流式；HLS 依赖系统支持）
            result = platform::MFCreateSourceReaderFromURL(path.c_str(),
                                                          attributes.Get(), &reader_);
        }
    }
    if (FAILED(result)) return Fail(L"MFCreateSourceReader", result);

    reader_->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    result = reader_->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    if (FAILED(result)) return Fail(L"SetStreamSelection", result);

    Microsoft::WRL::ComPtr<IMFMediaType> native_type;
    result = reader_->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native_type);
    if (FAILED(result)) return Fail(L"GetNativeMediaType", result);
    GUID native_subtype{};
    native_type->GetGUID(MF_MT_SUBTYPE, &native_subtype);
    UINT32 encoded_bytes_per_second{};
    native_type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           &encoded_bytes_per_second);

    Microsoft::WRL::ComPtr<IMFMediaType> requested;
    result = platform::MFCreateMediaType(&requested);
    if (FAILED(result)) return Fail(L"MFCreateMediaType", result);
    requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    UINT32 channels{};
    if (SUCCEEDED(native_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)))
        requested->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);

    result = reader_->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
        requested.Get());
    if (FAILED(result)) {
        requested.Reset();
        platform::MFCreateMediaType(&requested);
        requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        result = reader_->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
            requested.Get());
    }
    if (FAILED(result)) return Fail(L"SetCurrentMediaType(PCM)", result);

    Microsoft::WRL::ComPtr<IMFMediaType> output_type;
    result = reader_->GetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &output_type);
    if (FAILED(result)) return Fail(L"GetCurrentMediaType", result);
    UINT32 sample_rate{}, output_channels{}, bits{}, block_align{},
        output_bps{};
    output_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
    output_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &output_channels);
    output_type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    output_type->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &block_align);
    output_type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &output_bps);
    if (sample_rate == 0 || output_channels == 0 || bits == 0) {
        error_ = L"Media Foundation returned an incomplete PCM format";
        return false;
    }
    if (block_align == 0) block_align = output_channels * ((bits + 7) / 8);
    if (output_bps == 0) output_bps = sample_rate * block_align;
    wave_format_.wFormatTag = WAVE_FORMAT_PCM;
    wave_format_.nChannels = static_cast<WORD>(output_channels);
    wave_format_.nSamplesPerSec = sample_rate;
    wave_format_.nAvgBytesPerSec = output_bps;
    wave_format_.nBlockAlign = static_cast<WORD>(block_align);
    wave_format_.wBitsPerSample = static_cast<WORD>(bits);
    wave_format_.cbSize = 0;

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader_->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
            MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) {
        duration_ = std::chrono::milliseconds(duration.uhVal.QuadPart / 10000);
    }
    PropVariantClear(&duration);

    display_format_.format_tag = static_cast<uint16_t>(native_subtype.Data1);
    display_format_.channels = wave_format_.nChannels;
    display_format_.sample_rate = wave_format_.nSamplesPerSec;
    display_format_.bytes_per_second = encoded_bytes_per_second != 0
        ? encoded_bytes_per_second : wave_format_.nAvgBytesPerSec;
    display_format_.block_align = wave_format_.nBlockAlign;
    display_format_.bits_per_sample = wave_format_.wBitsPerSample;
    display_format_.codec_name = CodecNameFromTag(native_subtype.Data1, path);
    return true;
}

bool HttpStreamSource::Read(size_t requested_bytes,
                            std::vector<std::byte>& output, bool& end_of_stream) {
    output.clear();
    end_of_stream = false;
    if (!reader_ || wave_format_.nBlockAlign == 0) {
        error_ = L"Http stream source is not open";
        return false;
    }
    requested_bytes = std::max<size_t>(requested_bytes, wave_format_.nBlockAlign);
    requested_bytes -= requested_bytes % wave_format_.nBlockAlign;

    unsigned empty_samples{};
    while (output.size() < requested_bytes) {
        if (pending_offset_ < pending_.size()) {
            const size_t count = std::min(
                requested_bytes - output.size(),
                pending_.size() - pending_offset_);
            output.insert(output.end(), pending_.begin() + pending_offset_,
                          pending_.begin() + pending_offset_ + count);
            pending_offset_ += count;
            if (pending_offset_ == pending_.size()) {
                pending_.clear();
                pending_offset_ = 0;
            }
            continue;
        }
        if (source_end_) break;

        DWORD flags{};
        Microsoft::WRL::ComPtr<IMFSample> sample;
        const HRESULT result = reader_->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
            &flags, nullptr, &sample);
        if (FAILED(result)) return Fail(L"IMFSourceReader::ReadSample", result);
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            error_ = L"Media Foundation decoder reported a stream error";
            return false;
        }
        source_end_ = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (!sample) {
            if (source_end_) break;
            if (++empty_samples >= 64) {
                error_ = L"Media Foundation returned no audio samples";
                return false;
            }
            continue;
        }
        empty_samples = 0;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            error_ = L"Unable to obtain decoded audio buffer";
            return false;
        }
        BYTE* bytes{};
        DWORD current_length{};
        if (FAILED(buffer->Lock(&bytes, nullptr, &current_length))) {
            error_ = L"Unable to lock decoded audio buffer";
            return false;
        }
        if (current_length % wave_format_.nBlockAlign != 0) {
            buffer->Unlock();
            error_ = L"Media Foundation returned a partial PCM frame";
            return false;
        }
        pending_.resize(current_length);
        if (current_length != 0)
            std::memcpy(pending_.data(), bytes, current_length);
        buffer->Unlock();
        pending_offset_ = 0;
    }
    end_of_stream = source_end_ && pending_.empty();
    return true;
}

bool HttpStreamSource::Seek(std::chrono::milliseconds position) {
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = position.count() * 10000;
    const HRESULT result =
        reader_->SetCurrentPosition(GUID_NULL, value);
    PropVariantClear(&value);
    if (FAILED(result)) return Fail(L"IMFSourceReader::SetCurrentPosition", result);
    pending_.clear();
    pending_offset_ = 0;
    source_end_ = false;
    return true;
}

} // namespace ttplayer::audio
