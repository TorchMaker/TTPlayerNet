// DASH / MPEG-DASH 协议插件（编译为 <exe>\AddIn\ttp_v_dash.dll）
//
// 影视仓 / TVBox 的播放地址除了 m3u8，还有 MPEG-DASH 的 .mpd（ExoPlayer
// 的 CONTENT_TYPE_DASH）。Media Foundation 没有 DASH 源解析器，本 DLL 把
// MPD → init segment + fMP4 分段，摊平成一条本地 MP4 字节流：
// http://127.0.0.1:<port>/stream.mp4 ——
//   · 点播（MPD type="static"）：并发探测每片长度 → 精确 Content-Length +
//     Range 服务（进度条可拖，MF 的 MP4 源需要 seek）；
//   · 直播 / 探测失败：chunked 顺序流；
//   · 4 线程并发预取，init + 首片就绪即开播；
//   · 支持 SegmentTemplate（$Number$/$Time$/$RepresentationID$）、
//     SegmentTimeline、SegmentList、BaseURL；DRM（ContentProtection）
//     明确拒绝，不绕过授权。
//
// 实现骨架与 ttp_v_hls.dll 同源（本地代理 + 分片缓存 + Range 服务），
// 差异只在「清单解析」与「内容类型」。线程模型同 HLS 插件。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "ttplayer/video/video_protocol_api.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
// x86 下 WINAPI(stdcall) 会被修饰成 _ttpGetVideoProtocol@4，
// 用链接器别名额外导出契约要求的纯名，否则主程序 GetProcAddress 找不到。
#pragma comment(linker, "/EXPORT:ttpGetVideoProtocol=_ttpGetVideoProtocol@4")

namespace {

// ───────────────────────── 字符串工具 ─────────────────────────

std::wstring AsciiToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

std::string WideToAscii(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

std::string JoinUrl(const std::string& base, const std::string& ref) {
    if (ref.empty()) return base;
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
    if (ref.rfind("//", 0) == 0) {
        const size_t p = base.find("://");
        return p == std::string::npos ? ref : base.substr(0, p) + ":" + ref;
    }
    std::string b = base;
    const size_t q = b.find('?');
    if (q != std::string::npos) b = b.substr(0, q);
    if (ref[0] == '/') {
        const size_t p = b.find("://");
        const size_t slash =
            p == std::string::npos ? std::string::npos : b.find('/', p + 3);
        return slash == std::string::npos ? b + ref : b.substr(0, slash) + ref;
    }
    const size_t slash = b.rfind('/');
    return (slash == std::string::npos ? b : b.substr(0, slash + 1)) + ref;
}

// ───────────────────────── WinHTTP ─────────────────────────

struct HttpResult {
    bool ok = false;
    DWORD status = 0;
    std::vector<std::uint8_t> body;
};

bool IsRedirect(DWORD s) {
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

// 打开一个 GET 请求（含 https 证书宽容设置）。返回 null = 失败。
HINTERNET OpenGetRequest(HINTERNET con, const std::wstring& path, bool https,
                         const char* range_header) {
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return nullptr;
    if (https) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags,
                         sizeof(flags));
    }
    if (range_header) {
        std::wstring h = AsciiToWide(range_header);
        WinHttpAddRequestHeaders(req, h.c_str(),
                                 static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }
    return req;
}

// 通用 GET：跟随重定向（≤4 跳，跨协议重建连接）。
bool HttpGet(const std::wstring& url, const std::wstring& user_agent,
             const std::wstring& referer, int timeout_ms, HttpResult* out) {
    out->ok = false;
    out->status = 0;
    out->body.clear();

    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = 1;
    uc.dwUrlPathLength = 1;
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    // 片段（#...）不能进请求行，否则服务端按字面路径找 → 404。
    if (const size_t hf = path.find(L'#'); hf != std::wstring::npos)
        path.resize(hf);

    HINTERNET ses = WinHttpOpen(
        // 真实 CDN 对非浏览器 UA 直接 403，默认必须是浏览器 UA。
        user_agent.empty()
            ? L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
            : user_agent.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    const int to = timeout_ms > 0 ? timeout_ms : 8000;
    WinHttpSetTimeouts(ses, to, to, to, to);

    HINTERNET con = WinHttpConnect(
        ses, host.c_str(), uc.nPort ? uc.nPort : (https ? 443 : 80), 0);
    bool done = false;
    for (int hop = 0; hop < 5 && con && !done && !out->status; ++hop) {
        HINTERNET req = OpenGetRequest(con, path, https, nullptr);
        if (!req) break;
        const wchar_t* ref =
            referer.empty() ? WINHTTP_NO_REFERER : referer.c_str();
        const bool sent =
            WinHttpSendRequest(req, ref, ref ? -1L : 0L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr);
        if (!sent) { WinHttpCloseHandle(req); break; }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(
            req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
            WINHTTP_NO_HEADER_INDEX);
        if (IsRedirect(status)) {
            wchar_t loc[4096]{};
            DWORD ls = sizeof(loc);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &ls,
                                    WINHTTP_NO_HEADER_INDEX)) {
                std::wstring next = loc;
                if (next.find(L"http") != 0) {
                    std::wstring base =
                        url.substr(0, url.find(L'/', url.find(L"://") + 3));
                    next = base + next;
                }
                WinHttpCloseHandle(req);
                URL_COMPONENTSW uc2{};
                uc2.dwStructSize = sizeof(uc2);
                uc2.dwHostNameLength = 1;
                uc2.dwUrlPathLength = 1;
                uc2.dwExtraInfoLength = 1;
                if (WinHttpCrackUrl(next.c_str(), 0, 0, &uc2)) {
                    https = uc2.nScheme == INTERNET_SCHEME_HTTPS;
                    host.assign(uc2.lpszHostName, uc2.dwHostNameLength);
                    path.assign(uc2.lpszUrlPath, uc2.dwUrlPathLength);
                    if (uc2.dwExtraInfoLength && uc2.lpszExtraInfo)
                        path.append(uc2.lpszExtraInfo, uc2.dwExtraInfoLength);
                    if (path.empty()) path = L"/";
                    if (const size_t hf = path.find(L'#');
                        hf != std::wstring::npos) path.resize(hf);
                    WinHttpCloseHandle(con);
                    con = WinHttpConnect(
                        ses, host.c_str(),
                        uc2.nPort ? uc2.nPort : (https ? 443 : 80), 0);
                }
                continue;
            }
            WinHttpCloseHandle(req);
            break;
        }
        out->status = status;
        if (status >= 200 && status < 300) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                std::vector<std::uint8_t> chunk(avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got)
                    break;
                chunk.resize(got);
                out->body.insert(out->body.end(), chunk.begin(), chunk.end());
            }
            out->ok = true;
            done = true;
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return out->ok;
}

// 探测分片大小：Range: bytes=0-0 → 206 时取 Content-Range 的 total；
// 服务器不支持 range（200）时取 Content-Length。
bool HttpProbeSize(const std::wstring& url, const std::wstring& user_agent,
                   const std::wstring& referer, int timeout_ms,
                   std::uint64_t* size) {
    *size = 0;
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = 1;
    uc.dwUrlPathLength = 1;
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    // 片段（#...）不能进请求行，否则服务端按字面路径找 → 404。
    if (const size_t hf = path.find(L'#'); hf != std::wstring::npos)
        path.resize(hf);

    HINTERNET ses = WinHttpOpen(
        // 真实 CDN 对非浏览器 UA 直接 403，默认必须是浏览器 UA。
        user_agent.empty()
            ? L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
            : user_agent.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    const int to = timeout_ms > 0 ? timeout_ms : 8000;
    WinHttpSetTimeouts(ses, to, to, to, to);
    HINTERNET con = WinHttpConnect(
        ses, host.c_str(), uc.nPort ? uc.nPort : (https ? 443 : 80), 0);
    HINTERNET req = con ? OpenGetRequest(con, path, https, "Range: bytes=0-0\r\n")
                        : nullptr;
    bool ok = false;
    if (req) {
        const wchar_t* ref =
            referer.empty() ? WINHTTP_NO_REFERER : referer.c_str();
        if (WinHttpSendRequest(req, ref, ref ? -1L : 0L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(
                req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                WINHTTP_NO_HEADER_INDEX);
            wchar_t cr[128]{};
            DWORD cs = sizeof(cr);
            if (status == 206 &&
                WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE,
                                    WINHTTP_HEADER_NAME_BY_INDEX, cr, &cs,
                                    WINHTTP_NO_HEADER_INDEX)) {
                // "bytes 0-0/TOTAL"
                const std::string s = WideToAscii(cr);
                const size_t slash = s.rfind('/');
                if (slash != std::string::npos) {
                    *size = std::strtoull(s.c_str() + slash + 1, nullptr, 10);
                    ok = *size > 0;
                }
            } else if (status == 200) {
                DWORD cl = 0, cls = sizeof(cl);
                if (WinHttpQueryHeaders(
                        req,
                        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &cl, &cls,
                        WINHTTP_NO_HEADER_INDEX) && cl > 0) {
                    *size = cl;
                    ok = true;
                }
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// ───────────────────────── AES-128-CBC（BCrypt） ─────────────────────────

bool Aes128CbcDecrypt(const std::uint8_t key[16], const std::uint8_t iv[16],
                      std::vector<std::uint8_t>* data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hkey = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) ==
            0 &&
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(
                              const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &hkey, nullptr, 0,
                                   const_cast<PUCHAR>(key), 16, 0) == 0) {
        ULONG len = static_cast<ULONG>(data->size() / 16 * 16);
        if (len > 0) {
            std::vector<std::uint8_t> ivb(iv, iv + 16);
            NTSTATUS st = BCryptDecrypt(hkey, data->data(), len, nullptr,
                                        ivb.data(), 16, data->data(), len,
                                        &len, 0);
            ok = st == 0;
            data->resize(len);
        } else {
            ok = true;
        }
    }
    if (hkey) BCryptDestroyKey(hkey);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ───────────────────────── 会话状态 ─────────────────────────

struct Segment {
    int index = 0;                 // 全局序号（MEDIA-SEQUENCE 对齐）
    std::string url;
    double duration = 0.0;
    std::atomic<int> state{0};     // 0 待取 1 下载中 2 就绪 3 失败
    int tries = 0;
    std::uint64_t size = 0;        // 探测出的原始分片大小（0 未知）
};

// 明文分片缓存（进程内，跨会话共用 index 命名空间，VPOpen 时清空）。
std::mutex g_cache_mtx;
std::map<int, std::shared_ptr<std::vector<std::uint8_t>>> g_cache;
constexpr size_t kCacheMaxSegments = 96;

void CacheReset() {
    std::lock_guard<std::mutex> lk(g_cache_mtx);
    g_cache.clear();
}

void CachePut(int index, std::shared_ptr<std::vector<std::uint8_t>> data) {
    std::lock_guard<std::mutex> lk(g_cache_mtx);
    g_cache[index] = std::move(data);
    if (g_cache.size() > kCacheMaxSegments) g_cache.erase(g_cache.begin());
}

std::shared_ptr<std::vector<std::uint8_t>> CacheGet(int index) {
    std::lock_guard<std::mutex> lk(g_cache_mtx);
    auto it = g_cache.find(index);
    return it == g_cache.end() ? nullptr : it->second;
}

struct HlsState : std::enable_shared_from_this<HlsState> {
    std::wstring origin;               // 原始 URL（master 或媒体列表）
    std::wstring media_url;            // 真实媒体列表地址
    std::wstring user_agent, referer;
    int timeout_ms = 8000;

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Segment>> segs;
    std::vector<std::uint8_t> aes_key, aes_iv;
    std::atomic<bool> live{false};
    std::atomic<bool> list_ready{false};
    bool list_failed = false;
    std::wstring error;

    std::atomic<bool> stop{false};
    std::atomic<int> served_index{-1};       // 播放前沿（最近被请求的分片）
    std::atomic<bool> sizes_probed{false};
    std::atomic<bool> sizes_ok{false};
    std::atomic<std::uint64_t> total_size{0};

    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    std::thread accept_th, setup_th, sched_th;
    std::vector<std::thread> workers;

    ~HlsState() {
        if (listener != INVALID_SOCKET) closesocket(listener);
    }

    std::shared_ptr<Segment> FindSeg(int index) {
        std::lock_guard<std::mutex> lk(mtx);
        if (index < 0 || index >= static_cast<int>(segs.size())) return nullptr;
        return segs[index];
    }

    void Fail(const wchar_t* msg) {
        std::lock_guard<std::mutex> lk(mtx);
        if (error.empty()) error = msg;
        list_failed = true;
        cv.notify_all();
    }
};

// 等待分片就绪（会驱动 served_index 提示下载优先级）。返回明文数据。
std::shared_ptr<std::vector<std::uint8_t>> WaitSegReady(
    const std::shared_ptr<HlsState>& st, int index, int timeout_ms) {
    st->served_index.store(std::max(st->served_index.load(), index));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (st->stop.load()) return nullptr;
        if (auto data = CacheGet(index)) return data;
        auto seg = st->FindSeg(index);
        if (!seg) {
            // 直播窗口滑动可能把该片裁掉：视为不再可用
            return nullptr;
        }
        if (seg->state.load() == 3 && seg->tries >= 3) return nullptr;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->cv.notify_all();   // 唤醒下载线程补活
        }
        if (std::chrono::steady_clock::now() >= deadline) return nullptr;
        std::unique_lock<std::mutex> lk(st->mtx);
        st->cv.wait_for(lk, std::chrono::milliseconds(120));
    }
}

// ───────────────────────── MPD 抓取与解析 ─────────────────────────

bool FetchPlaylist(const std::wstring& url, const HlsState* st,
                   std::string* out) {
    HttpResult r;
    if (!HttpGet(url, st->user_agent, st->referer, st->timeout_ms, &r))
        return false;
    if (r.body.size() > 4 * 1024 * 1024) return false;
    out->assign(r.body.begin(), r.body.end());
    return out->find("<MPD") != std::string::npos ||
           out->find("<mpd") != std::string::npos;
}

// MPD 是简单 XML：按字符串取标签属性即可，不引入 XML 库。
std::string Attr(const std::string& tag, const std::string& name) {
    const std::string pat = name + "=\"";
    const size_t p = tag.find(pat);
    if (p == std::string::npos) return {};
    const size_t b = p + pat.size();
    const size_t e = tag.find('"', b);
    return e == std::string::npos ? std::string() : tag.substr(b, e - b);
}

std::string FindTag(const std::string& xml, const std::string& name,
                    size_t from = 0) {
    const size_t p = xml.find("<" + name, from);
    if (p == std::string::npos) return {};
    const size_t e = xml.find('>', p);
    return e == std::string::npos ? std::string() : xml.substr(p, e - p);
}

void Subst(std::string* s, const std::string& from, const std::string& to) {
    size_t p = 0;
    while ((p = s->find(from, p)) != std::string::npos) {
        s->replace(p, from.size(), to);
        p += to.size();
    }
}

std::string SegmentUrl(const std::string& tmpl, const std::string& base,
                       int number, const std::string& rep_id,
                       const std::string& bandwidth, std::uint64_t time) {
    std::string s = tmpl;
    Subst(&s, "$RepresentationID$", rep_id);
    Subst(&s, "$Bandwidth$", bandwidth);
    char num[32]{}, tm[32]{};
    std::snprintf(num, sizeof(num), "%d", number);
    std::snprintf(tm, sizeof(tm), "%llu",
                  static_cast<unsigned long long>(time));
    Subst(&s, "$Number$", num);
    Subst(&s, "$Time$", tm);
    return JoinUrl(base, s);
}

// 解析 MPD → (序号, 绝对 URL, 时长) 列表。序号 0 固定是 init segment，
// 之后是 fMP4 分片；这样 HLS 骨架里的「顺序拼接 + Range 前缀和」可直接复用。
bool ParseMediaPlaylist(const std::string& text, const std::string& base_url,
                        std::uint64_t* media_seq, bool* is_live,
                        std::string* key_uri, std::vector<std::uint8_t>* key_iv,
                        bool* key_iv_explicit,
                        std::vector<std::tuple<int, std::string, double>>* out) {
    *media_seq = 0;
    key_uri->clear();
    key_iv->clear();
    *key_iv_explicit = false;
    // DRM：只传递合法授权，不绕过。明确拒绝，让上层换源。
    if (text.find("ContentProtection") != std::string::npos) return false;
    const std::string mpd = FindTag(text, "MPD");
    *is_live = mpd.find("type=\"dynamic\"") != std::string::npos;

    // BaseURL（若有）：相对地址基于它补全。
    std::string base = base_url;
    {
        const size_t p = text.find("<BaseURL");
        if (p != std::string::npos) {
            const size_t gt = text.find('>', p);
            const size_t end = text.find("</BaseURL>", gt == std::string::npos
                                                        ? p : gt);
            if (gt != std::string::npos && end != std::string::npos) {
                std::string b = text.substr(gt + 1, end - gt - 1);
                while (!b.empty() && (b.front() == ' ' || b.front() == '\r' ||
                                      b.front() == '\n')) b.erase(b.begin());
                while (!b.empty() && (b.back() == ' ' || b.back() == '\r' ||
                                      b.back() == '\n')) b.pop_back();
                if (!b.empty()) base = JoinUrl(base_url, b);
            }
        }
    }

    // 选码率最高的视频 Representation（DASH 常有 3~6 档清晰度）。
    std::string rep_id, bandwidth;
    {
        unsigned long long best = 0;
        size_t pos = 0;
        for (;;) {
            const size_t p = text.find("<Representation", pos);
            if (p == std::string::npos) break;
            const size_t e = text.find('>', p);
            const std::string tag = text.substr(p, e == std::string::npos
                                                        ? std::string::npos
                                                        : e - p);
            pos = e == std::string::npos ? text.size() : e + 1;
            const std::string mime = Attr(tag, "mimeType");
            const std::string codecs = Attr(tag, "codecs");
            if (!mime.empty() && mime.find("video") == std::string::npos &&
                codecs.find("avc") == std::string::npos &&
                codecs.find("hvc") == std::string::npos)
                continue;   // 音频 Representation
            const std::string bw = Attr(tag, "bandwidth");
            const unsigned long long v =
                std::strtoull(bw.c_str(), nullptr, 10);
            if (v >= best) {
                best = v;
                rep_id = Attr(tag, "id");
                bandwidth = bw;
            }
        }
    }

    // init segment：SegmentTemplate@initialization 或 <Initialization sourceURL>。
    std::string init_rel;
    const std::string tmpl_tag = FindTag(text, "SegmentTemplate");
    if (!tmpl_tag.empty()) init_rel = Attr(tmpl_tag, "initialization");
    if (init_rel.empty()) {
        const std::string ini = FindTag(text, "Initialization");
        if (!ini.empty()) init_rel = Attr(ini, "sourceURL");
    }
    if (init_rel.empty()) return false;   // 无 init 的 fMP4 无法播
    out->emplace_back(0, SegmentUrl(init_rel, base, 0, rep_id, bandwidth, 0),
                      0.0);

    const std::string media_tmpl = Attr(tmpl_tag, "media");
    if (!media_tmpl.empty()) {
        int start = 1;
        const std::string sn = Attr(tmpl_tag, "startNumber");
        if (!sn.empty()) start = std::atoi(sn.c_str());
        std::uint64_t timescale = 1;
        const std::string ts = Attr(tmpl_tag, "timescale");
        if (!ts.empty()) timescale = std::strtoull(ts.c_str(), nullptr, 10);
        std::uint64_t seg_duration = 0;
        const std::string dur = Attr(tmpl_tag, "duration");
        if (!dur.empty()) seg_duration = std::strtoull(dur.c_str(), nullptr, 10);

        int count = *is_live ? 30 : 60;   // 无 Timeline 时的保守窗口
        std::uint64_t time = 0;
        const size_t tp = text.find("<SegmentTimeline");
        if (tp != std::string::npos) {
            const size_t te = text.find("</SegmentTimeline>", tp);
            if (te != std::string::npos) {
                const std::string tl = text.substr(tp, te - tp);
                count = 0;
                size_t sp = 0;
                while ((sp = tl.find("<S", sp)) != std::string::npos) {
                    const size_t se = tl.find('>', sp);
                    if (se == std::string::npos) break;
                    const std::string s = tl.substr(sp, se - sp);
                    sp = se + 1;
                    const std::string r = Attr(s, "r");
                    count += r.empty() ? 1 : std::atoi(r.c_str()) + 1;
                }
            }
        } else if (seg_duration && !mpd.empty()) {
            // 点播 MPD 常给 mediaPresentationDuration="PTxxS"
            const std::string ptd = Attr(mpd, "mediaPresentationDuration");
            if (!ptd.empty() && ptd.find("PT") == 0) {
                const double sec = std::strtod(ptd.c_str() + 2, nullptr);
                if (sec > 0 && timescale > 0)
                    count = static_cast<int>(
                        sec * timescale / static_cast<double>(seg_duration)) + 1;
            }
        }
        count = std::max(1, std::min(count, 600));   // 防爆炸
        for (int i = 0; i < count; ++i) {
            out->emplace_back(i + 1,
                              SegmentUrl(media_tmpl, base, start + i, rep_id,
                                         bandwidth, time),
                              timescale ? static_cast<double>(seg_duration) /
                                              timescale : 0.0);
            time += seg_duration;
        }
        return out->size() > 1;
    }

    // SegmentList：<SegmentURL media="..."/>
    {
        size_t pos = 0;
        for (;;) {
            const size_t p = text.find("<SegmentURL", pos);
            if (p == std::string::npos) break;
            const size_t e = text.find('>', p);
            const std::string tag = text.substr(p, e == std::string::npos
                                                        ? std::string::npos
                                                        : e - p);
            pos = e == std::string::npos ? text.size() : e + 1;
            const std::string m = Attr(tag, "media");
            if (m.empty()) continue;
            out->emplace_back(static_cast<int>(out->size()),
                              JoinUrl(base, m), 0.0);
            if (out->size() > 600) break;
        }
    }
    return out->size() > 1;
}

// ───────────────────────── 下载 / 调度 / 探测线程 ─────────────────────────

void DownloadLoop(std::shared_ptr<HlsState> st) {
    while (!st->stop.load()) {
        std::shared_ptr<Segment> seg;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            const int served = st->served_index.load();
            std::shared_ptr<Segment> fallback;
            for (auto& s : st->segs) {
                const int state = s->state.load();
                if (state != 0 && !(state == 3 && s->tries < 3)) continue;
                // seek 优先：先补播放前沿之后的，再补更早的
                if (s->index >= served) { seg = s; break; }
                if (!fallback) fallback = s;
            }
            if (!seg) seg = fallback;
        }
        if (!seg) {
            std::unique_lock<std::mutex> lk(st->mtx);
            st->cv.wait_for(lk, std::chrono::milliseconds(200));
            continue;
        }
        int expect = seg->state.load() == 0 ? 0 : 3;
        if (!seg->state.compare_exchange_strong(expect, 1)) continue;
        seg->tries++;

        std::vector<std::uint8_t> data;
        bool ok = false;
        for (int attempt = 0; attempt < 2 && !st->stop.load(); ++attempt) {
            HttpResult r;
            if (HttpGet(AsciiToWide(seg->url), st->user_agent, st->referer,
                        st->timeout_ms, &r) &&
                !r.body.empty()) {
                data = std::move(r.body);
                ok = true;
                break;
            }
            Sleep(150);
        }
        // DASH 分片是明文 fMP4；DRM 已在 ParseMediaPlaylist 明确拒绝，
        // 这里不做 HLS 的 AES-128 解密。
        if (ok && !data.empty()) {
            CachePut(seg->index,
                     std::make_shared<std::vector<std::uint8_t>>(std::move(data)));
            seg->state.store(2);
        } else {
            seg->state.store(3);
        }
        st->cv.notify_all();
    }
}

void ScheduleLoop(std::shared_ptr<HlsState> st) {
    // DASH 动态 MPD 的分段 URL 由 $Number$/$Time$ 推导，不需要像 HLS 那样
    // 每 6 秒重拉清单；这里只保留唤醒泵，让 VpClose 的 join 语义与 HLS 一致。
    while (!st->stop.load()) {
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->cv.notify_all();
        }
        Sleep(120);
    }
}

// 点播：并发探测全部分片大小 → range 模式可用
void ProbeSizes(std::shared_ptr<HlsState> st) {
    std::vector<std::shared_ptr<Segment>> snapshot;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        snapshot = st->segs;
    }
    std::atomic<size_t> cursor{0};
    std::atomic<bool> any_fail{false};
    auto worker = [&]() {
        for (;;) {
            const size_t i = cursor.fetch_add(1);
            if (i >= snapshot.size() || st->stop.load() || any_fail.load())
                return;
            auto& seg = snapshot[i];
            if (auto d = CacheGet(seg->index)) {
                seg->size = d->size();
                continue;
            }
            std::uint64_t sz = 0;
            if (HttpProbeSize(AsciiToWide(seg->url), st->user_agent,
                              st->referer, st->timeout_ms, &sz)) {
                seg->size = sz;
            } else {
                any_fail.store(true);
                return;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < 6; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (!any_fail.load() && !st->stop.load()) {
        std::uint64_t total = 0;
        std::lock_guard<std::mutex> lk(st->mtx);
        for (auto& s : st->segs) total += s->size;
        st->total_size.store(total);
        st->sizes_ok.store(true);
    }
    st->sizes_probed.store(true);
    st->cv.notify_all();
}

// 初始化线程：抓 MPD → 解析 init + 分片 → 启动下载/调度/探测
void SetupThread(std::shared_ptr<HlsState> st) {
    std::string text;
    if (!FetchPlaylist(st->origin, st.get(), &text) || st->stop.load()) {
        st->Fail(L"拉取 MPD 清单失败（网络或地址无效）");
        return;
    }
    st->media_url = st->origin;

    std::vector<std::tuple<int, std::string, double>> items;
    std::uint64_t seq = 0;
    bool live = false, ivx = false;
    std::vector<std::uint8_t> iv;
    std::string key_uri;
    if (!ParseMediaPlaylist(text, WideToAscii(st->media_url), &seq, &live,
                            &key_uri, &iv, &ivx, &items)) {
        st->Fail(L"MPD 里没有可用的 fMP4 分片（或含 DRM，本程序不绕过授权）");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->segs.resize(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            auto s = std::make_shared<Segment>();
            s->index = std::get<0>(items[i]);
            s->url = std::get<1>(items[i]);
            s->duration = std::get<2>(items[i]);
            st->segs[i] = s;
        }
        st->live.store(live);
        st->cv.notify_all();
    }
    st->list_ready.store(true);
    st->cv.notify_all();

    for (int i = 0; i < 4; ++i) st->workers.emplace_back(DownloadLoop, st);
    st->sched_th = std::thread(ScheduleLoop, st);
    if (!live) ProbeSizes(st);
}

// ───────────────────────── 本地代理 ─────────────────────────

bool SendAll(SOCKET c, const char* p, int n) {
    while (n > 0) {
        const int k = send(c, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

bool SendHead(SOCKET c, const char* head) {
    return SendAll(c, head, static_cast<int>(strlen(head)));
}

// 等待列表就绪（最长 25s）。false = 超时/失败/停止。
bool WaitForList(const std::shared_ptr<HlsState>& st) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(25);
    std::unique_lock<std::mutex> lk(st->mtx);
    while (!st->list_ready.load() && !st->list_failed && !st->stop.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        st->cv.wait_for(lk, std::chrono::milliseconds(100));
    }
    return st->list_ready.load();
}

// 点播 range 模式：把 [a, b]（含端点）按分片边界映射写出。
bool ServeRange(std::shared_ptr<HlsState> st, SOCKET c, std::uint64_t a,
                std::uint64_t b) {
    const std::uint64_t total = st->total_size.load();
    if (total == 0 || a >= total) return false;
    if (b == UINT64_MAX || b >= total) b = total - 1;
    char head[256];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 206 Partial Content\r\n"
                  "Content-Type: video/mp4\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Range: bytes %llu-%llu/%llu\r\n"
                  "Content-Length: %llu\r\n"
                  "Connection: close\r\n\r\n",
                  static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b),
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(b - a + 1));
    if (!SendHead(c, head)) return false;

    std::uint64_t pos = a;
    while (pos <= b && !st->stop.load()) {
        // 定位分片（前缀和）
        std::uint64_t prefix = 0;
        int seg_index = -1;
        std::shared_ptr<Segment> seg;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            for (auto& s : st->segs) {
                const std::uint64_t sz =
                    s->size ? s->size : (CacheGet(s->index) ? CacheGet(s->index)->size() : 0);
                if (pos < prefix + sz) { seg_index = s->index; seg = s; break; }
                prefix += sz;
            }
        }
        if (!seg) return false;
        const std::uint64_t in_seg = pos - prefix;
        auto data = WaitSegReady(st, seg_index, 30000);
        if (!data || data->empty()) return false;
        const std::uint64_t take =
            std::min<std::uint64_t>(b - pos + 1, data->size() - in_seg);
        if (!SendAll(c, reinterpret_cast<const char*>(data->data()) + in_seg,
                     static_cast<int>(take)))
            return false;
        pos += take;
    }
    return pos > b;
}

// 直播/未知大小：chunked 顺序流。
bool ServeChunked(std::shared_ptr<HlsState> st, SOCKET c) {
    if (!SendHead(c, "HTTP/1.1 200 OK\r\n"
                     "Content-Type: video/mp4\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "Connection: close\r\n\r\n"))
        return false;
    int index = 0;
    while (!st->stop.load()) {
        auto seg = st->FindSeg(index);
        if (!seg) {
            if (st->live) {   // 等列表刷新出更多分片
                Sleep(250);
                continue;
            }
            break;   // 点播顺序到头
        }
        auto data = WaitSegReady(st, index, 30000);
        if (!data || data->empty()) break;
        char sz[32];
        std::snprintf(sz, sizeof(sz), "%zx\r\n", data->size());
        if (!SendHead(c, sz) ||
            !SendAll(c, reinterpret_cast<const char*>(data->data()),
                     static_cast<int>(data->size())) ||
            !SendHead(c, "\r\n"))
            return false;
        ++index;
    }
    return SendHead(c, "0\r\n\r\n");
}

void HandleConn(std::shared_ptr<HlsState> st, SOCKET c) {
    char req[8192];
    int total = 0;
    while (total < static_cast<int>(sizeof(req)) - 1) {
        const int k = recv(c, req + total,
                           static_cast<int>(sizeof(req)) - 1 - total, 0);
        if (k <= 0) { closesocket(c); return; }
        total += k;
        req[total] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[total] = '\0';
    char method[16] = {}, path[512] = {};
    bool ok = false;
    if (sscanf(req, "%15s %511s", method, path) == 2 &&
        _stricmp(method, "GET") == 0) {
        const char* range = strstr(req, "Range:");
        if (path == std::string("/stream.mp4")) {
            if (!WaitForList(st)) {
                SendHead(c, "HTTP/1.1 503 Service Unavailable\r\n"
                            "Connection: close\r\n\r\n");
            } else if (st->sizes_ok.load()) {
                // 点播：Range: bytes=A-B | A- | -N(忽略后缀语义，按 A=0)
                std::uint64_t a = 0, b = UINT64_MAX;
                if (range) {
                    const char* p = strstr(range, "bytes=");
                    if (p) {
                        unsigned long long va = 0, vb = 0;
                        if (sscanf(p, "bytes=%llu-%llu", &va, &vb) >= 1) {
                            a = va;
                            b = vb ? vb : UINT64_MAX;
                        }
                    }
                }
                ok = ServeRange(std::move(st), c, a, b);
            } else {
                ok = ServeChunked(std::move(st), c);
            }
        } else {
            SendHead(c, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
        }
    }
    if (!ok) { /* 已尽力输出，失败静默 */ }
    shutdown(c, SD_SEND);
    closesocket(c);
}

void AcceptLoop(std::shared_ptr<HlsState> st) {
    while (!st->stop.load()) {
        sockaddr_in peer{};
        int len = sizeof(peer);
        const SOCKET c = accept(st->listener,
                                reinterpret_cast<sockaddr*>(&peer), &len);
        if (c == INVALID_SOCKET) break;   // listener 已关（VPClose）
        std::thread([st, c]() { HandleConn(st, c); }).detach();
    }
}

std::wstring DashProbe(const std::wstring& url) {
    std::wstring low = url;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    const size_t q = low.find(L'?');
    if (q != std::wstring::npos) low = low.substr(0, q);
    if (low.find(L".mpd") != std::wstring::npos ||
        low.find(L"dash") != std::wstring::npos ||
        low.find(L"/manifest") != std::wstring::npos)
        return url;
    return {};
}

int g_ws_ref = 0;

} // namespace

// ───────────────────────── 导出接口 ─────────────────────────
// 与音频插件 ttpGetSoundAddIn 同构的工厂模式：唯一导出 ttpGetVideoProtocol。

namespace {

int VpApiVersion() { return VP_PROTOCOL_API_VERSION; }

void VpInfo(VPProtocolInfo* out) {
    out->name = L"DASH";
    out->vendor = L"TTPlayer built-in (MPD -> fMP4 local stream proxy)";
    out->api_version = VP_PROTOCOL_API_VERSION;
}

int VpProbe(const wchar_t* url) {
    if (!url) return 0;
    std::wstring low = url;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    const size_t q = low.find(L'?');
    if (q != std::wstring::npos) low = low.substr(0, q);
    if (low.find(L".mpd") != std::wstring::npos)
        return 95;   // 高于 WEB 的 60，确保 .mpd 不被网页解析插件抢走
    if (low.find(L"dash") != std::wstring::npos) return 80;
    return 0;
}

VPSession* VpOpen(const wchar_t* url, const VPHttpConfig* cfg,
                  wchar_t* out_media_url, int cch_media_url) {
    if (!url || !out_media_url || cch_media_url <= 0) return nullptr;
    if (DashProbe(url).empty()) return nullptr;

    if (g_ws_ref == 0) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return nullptr;
    }
    g_ws_ref++;
    CacheReset();

    auto st = std::make_shared<HlsState>();
    st->origin = url;
    st->user_agent = cfg && cfg->user_agent ? cfg->user_agent : L"";
    st->referer = cfg && cfg->referer ? cfg->referer : L"";
    st->timeout_ms = cfg && cfg->timeout_ms > 0 ? cfg->timeout_ms : 8000;

    // 本地监听 socket（随机端口）
    st->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (st->listener == INVALID_SOCKET) {
        g_ws_ref--;
        return nullptr;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(st->listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(st->listener, 8) != 0) {
        closesocket(st->listener);
        st->listener = INVALID_SOCKET;
        g_ws_ref--;
        return nullptr;
    }
    sockaddr_in bound{};
    int blen = sizeof(bound);
    getsockname(st->listener, reinterpret_cast<sockaddr*>(&bound), &blen);
    st->port = ntohs(bound.sin_port);

    swprintf(out_media_url, static_cast<size_t>(cch_media_url),
             L"http://127.0.0.1:%d/stream.mp4", st->port);

    st->setup_th = std::thread(SetupThread, st);
    st->accept_th = std::thread(AcceptLoop, st);

    // 裸指针出接口；VPClose 时统一回收
    return reinterpret_cast<VPSession*>(new std::shared_ptr<HlsState>(st));
}

int VpPoll(VPSession* s, wchar_t* out_err, int cch_err) {
    if (!s) return VP_POLL_ERROR;
    auto st = *reinterpret_cast<std::shared_ptr<HlsState>*>(s);
    if (st->stop.load() || st->list_failed) {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (out_err && cch_err > 0) {
            wcsncpy(out_err, st->error.empty() ? L"HLS 会话已停止" : st->error.c_str(),
                    static_cast<size_t>(cch_err - 1));
            out_err[cch_err - 1] = L'\0';
        }
        return VP_POLL_ERROR;
    }
    if (!st->list_ready.load()) return VP_POLL_CONNECTING;
    // 首片就绪（或长度探测完成）即认为可开播
    auto seg0 = st->FindSeg(0);
    if ((seg0 && seg0->state.load() == 2) || st->sizes_probed.load())
        return VP_POLL_READY;
    return VP_POLL_CONNECTING;
}

void VpClose(VPSession* s) {
    if (!s) return;
    auto st = *reinterpret_cast<std::shared_ptr<HlsState>*>(s);
    delete reinterpret_cast<std::shared_ptr<HlsState>*>(s);
    st->stop.store(true);
    if (st->listener != INVALID_SOCKET) {
        closesocket(st->listener);
        st->listener = INVALID_SOCKET;
    }
    st->cv.notify_all();
    if (st->setup_th.joinable()) st->setup_th.join();
    if (st->sched_th.joinable()) st->sched_th.join();
    for (auto& t : st->workers) {
        if (t.joinable()) t.join();
    }
    if (st->accept_th.joinable()) st->accept_th.join();
    // 连接线程持 shared_ptr 自灭；最后一份引用释放即析构。
    if (--g_ws_ref == 0) WSACleanup();
}

} // namespace

// 唯一导出：工厂函数（对齐音频插件 ttpGetSoundAddIn）。
extern "C" __declspec(dllexport)
HRESULT WINAPI ttpGetVideoProtocol(VPProtocolApi* out) {
    if (!out) return E_POINTER;
    out->api_version = VpApiVersion;
    out->info = VpInfo;
    out->probe = VpProbe;
    out->open = VpOpen;
    out->poll = VpPoll;
    out->close = VpClose;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
