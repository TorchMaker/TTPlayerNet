#pragma once

// 在线 http(s) 流式解码源
//
// 复刻 MusicFree「播放前现拉/激活链接、流式播放」的能力：用工程已有的 WinHTTP 网络栈
// （带 MusicFree 解析出的自定义请求头：Referer / User-Agent / 签名……）把音频边下载边
// 喂进一个自定义 IStream，再走 Media Foundation 的 MFCreateMFByteStreamOnStream +
// MFCreateSourceReaderFromByteStream（与 MediaFoundationSource 的归档成员分支同构）。
//
// 这样即使 CDN 直链需要自定义头也能流式播放，且真正做到「在线播放」而不落盘。
// 对那些「只能在线播、下不下来」的链接（HLS .m3u8、会话级签名、按 Range 限流）也成立：
//   - 普通直链：走自定义字节流（带自定义头）。
//   - HLS：回退到 MFCreateSourceReaderFromURL（依赖系统 HLS 支持）。
//   - 既无描述符又非 HLS：走 MF 默认 URL reader（简单直链可直接流式）。

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <mmreg.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "ttplayer/audio/audio_engine.h"

namespace ttplayer::audio {

class HttpStreamSource final : public DecodedAudioSource {
public:
    explicit HttpStreamSource(HMODULE ttpcomm = nullptr) : ttpcomm_(ttpcomm) {}

    bool Open(const std::filesystem::path& path,
              const PlaybackOptions& options) override;
    bool Read(size_t requested_bytes, std::vector<std::byte>& output,
              bool& end_of_stream) override;
    bool Seek(std::chrono::milliseconds position) override;
    [[nodiscard]] const WAVEFORMATEX& OutputFormat() const override {
        return wave_format_;
    }
    [[nodiscard]] AudioFormat DisplayFormat() const override {
        return display_format_;
    }
    [[nodiscard]] std::chrono::milliseconds Duration() const override {
        return duration_;
    }
    [[nodiscard]] std::wstring Error() const override { return error_; }
    [[nodiscard]] HRESULT ErrorResult() const override { return error_result_; }
    [[nodiscard]] AudioMetadata Metadata() const override { return metadata_; }

private:
    bool Fail(std::wstring_view operation, HRESULT result);

    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IStream> http_stream_;
    Microsoft::WRL::ComPtr<IMFByteStream> http_byte_stream_;
    HMODULE ttpcomm_{};
    WAVEFORMATEX wave_format_{};
    AudioFormat display_format_{};
    AudioMetadata metadata_;
    std::chrono::milliseconds duration_{};
    std::vector<std::byte> pending_;
    size_t pending_offset_{};
    bool source_end_{};
    std::wstring error_;
    HRESULT error_result_{E_FAIL};
};

} // namespace ttplayer::audio
