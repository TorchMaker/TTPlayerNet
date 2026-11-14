// protocol_media.cpp
//
// 「直链媒体中转」协议插件（ttp_v_media.dll）。
//
// 背景：Media Foundation 的窗口化 MediaEngine 不能自定义请求头，B 站（bilivideo）
// 这类 CDN 强制校验 Referer/UA，直链交给 MF 直接 403；旧代码只能整段下载落盘，
// 大文件不可用、也不能拖动。
//
// 本插件把 http(s) 直链（mp4/flv/mkv/webm/mov/m4v/avi/ts…）接管为本地代理：
//   · VpOpen 立即返回 http://127.0.0.1:<port>/stream；
//   · 浏览器请求里带的 Range 原样转发给上游，206/Content-Range 原样回传；
//   · 请求头来自 VPHttpConfig（user_agent/referer）以及 options.headers
//     （主程序把播放地址内联头 {Referer/User-Agent/Cookie/Origin…} 序列化进来）。
//
// probe 只认「路径以媒体扩展名结尾」的地址并返回 30 分：网页（WEB 60）、
// m3u8（HLS 100）仍由各自插件优先接管，互不抢占。

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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ttplayer/video/video_protocol_api.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
// Win32/x86 下 WINAPI(stdcall) 导出会被修饰成 _ttpGetVideoProtocol@4，
// 用链接器别名额外导出契约要求的纯名（主程序按纯名 GetProcAddress）。
#pragma comment(linker, "/EXPORT:ttpGetVideoProtocol=_ttpGetVideoProtocol@4")

namespace {

HMODULE g_self = nullptr;
std::mutex g_ws_mtx;
int g_ws_ref = 0;

// ───────────────────────── 字符串 / URL ─────────────────────────

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                            &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            &s[0], n, nullptr, nullptr);
    return s;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// WinHTTP 请求行必须是 ASCII：URL 里的非 ASCII（未编码中文等）按 UTF-8 百分号编码。
std::wstring EscapeNonAsciiUrl(const std::wstring& url) {
    static const char* kHex = "0123456789ABCDEF";
    const std::string utf8 = WideToUtf8(url);
    std::string out;
    out.reserve(utf8.size() + 16);
    for (unsigned char c : utf8) {
        if (c >= 0x80) {
            out += '%';
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        } else {
            out += static_cast<char>(c);
        }
    }
    return Utf8ToWide(out);
}

bool StartsHttp(const std::wstring& url) {
    return url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0;
}

bool EndsWithNoCase(std::wstring value, const wchar_t* suffix) {
    const std::wstring tail(suffix);
    if (value.size() < tail.size()) return false;
    value = value.substr(value.size() - tail.size());
    return _wcsicmp(value.c_str(), tail.c_str()) == 0;
}

// 直链媒体：路径（去 query/fragment）以媒体扩展名结尾。m3u8/mpd 交给 HLS/DASH。
bool IsDirectMediaUrl(const std::wstring& url) {
    if (!StartsHttp(url)) return false;
    std::wstring path = url;
    const size_t cut = path.find_first_of(L"?#");
    if (cut != std::wstring::npos) path.resize(cut);
    static const wchar_t* kExts[] = {L".mp4", L".flv",  L".mkv", L".webm",
                                     L".mov", L".m4v",  L".avi", L".wmv",
                                     L".mpg", L".mpeg", L".ts"};
    for (const wchar_t* ext : kExts)
        if (EndsWithNoCase(path, ext)) return true;
    return false;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
};

bool Crack(const std::wstring& url, UrlParts* out) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = 1;
    uc.dwUrlPathLength = 1;
    uc.dwExtraInfoLength = 1;
    const std::wstring safe = EscapeNonAsciiUrl(url);
    if (!WinHttpCrackUrl(safe.c_str(), 0, 0, &uc)) return false;
    out->https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    out->host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out->path.assign(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        out->path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (out->path.empty()) out->path = L"/";
    // 片段属于客户端语义，不能进请求行。
    const size_t hash = out->path.find(L'#');
    if (hash != std::wstring::npos) out->path.resize(hash);
    out->port = uc.nPort ? uc.nPort : (out->https ? 443 : 80);
    return !out->host.empty();
}

// ───────────────────── options.headers（主程序传进来的请求头） ─────────────────────

// 从 {"headers":{"Referer":"…","Cookie":"…"}} 里取出字符串键值对。
std::map<std::string, std::string> ParseHeaderOptions(const std::wstring& options) {
    std::map<std::string, std::string> out;
    if (options.empty()) return out;
    const std::string js = WideToUtf8(options);
    size_t pos = js.find("\"headers\"");
    if (pos == std::string::npos) return out;
    pos = js.find('{', pos);
    if (pos == std::string::npos) return out;

    size_t i = pos + 1;
    auto skip_ws = [&]() {
        while (i < js.size() &&
               (js[i] == ' ' || js[i] == '\t' || js[i] == '\r' || js[i] == '\n'))
            ++i;
    };
    while (i < js.size()) {
        skip_ws();
        if (i >= js.size() || js[i] == '}') break;
        if (js[i] != '"') {   // 非字符串键：跳过到下一个分隔符
            const size_t e = js.find_first_of(",}", i);
            if (e == std::string::npos) break;
            i = e;
            if (js[i] == ',') ++i;
            continue;
        }
        ++i;
        std::string key;
        bool esc = false;
        while (i < js.size()) {
            const char c = js[i];
            if (esc) { key += c; esc = false; ++i; continue; }
            if (c == '\\') { esc = true; ++i; continue; }
            if (c == '"') { ++i; break; }
            key += c;
            ++i;
        }
        skip_ws();
        if (i < js.size() && js[i] == ':') ++i;
        skip_ws();
        if (i < js.size() && js[i] == '"') {
            ++i;
            std::string value;
            bool e2 = false;
            while (i < js.size()) {
                const char c = js[i];
                if (e2) { value += c; e2 = false; ++i; continue; }
                if (c == '\\') { e2 = true; ++i; continue; }
                if (c == '"') { ++i; break; }
                value += c;
                ++i;
            }
            if (!key.empty()) out[key] = value;
        } else {
            const size_t e = js.find_first_of(",}", i);
            if (e == std::string::npos) break;
            i = e;
        }
        skip_ws();
        if (i < js.size() && js[i] == ',') ++i;
    }
    return out;
}

// ───────────────────────── HTTP / Socket 小工具 ─────────────────────────

std::wstring HeaderText(HINTERNET req, DWORD which) {
    wchar_t buf[1024]{};
    DWORD sz = sizeof(buf);
    if (!WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, buf,
                             &sz, WINHTTP_NO_HEADER_INDEX) ||
        sz == 0)
        return {};
    const size_t chars = sz / sizeof(wchar_t);
    return std::wstring(buf, chars > 0 && buf[chars - 1] == L'\0' ? chars - 1
                                                                  : chars);
}

bool SendAll(SOCKET c, const char* p, int n) {
    int sent = 0;
    while (sent < n) {
        const int r = send(c, p + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

void SendSimple(SOCKET c, const char* status, const std::string& text) {
    char head[512]{};
    _snprintf_s(head, _TRUNCATE,
                "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: %u\r\nConnection: close\r\n\r\n",
                status, static_cast<unsigned>(text.size()));
    SendAll(c, head, static_cast<int>(strlen(head)));
    if (!text.empty()) SendAll(c, text.data(), static_cast<int>(text.size()));
}

// 失败诊断：<插件目录>\..\video\media_fail.log（与 WEB/HLS 插件同族）。
void AppendMediaFailLog(const std::string& text) {
    wchar_t mod[MAX_PATH]{};
    if (!g_self || !GetModuleFileNameW(g_self, mod, MAX_PATH)) return;
    const std::wstring p(mod);
    const size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return;
    const std::wstring path = p.substr(0, s) + L"\\..\\video\\media_fail.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char stamp[64]{};
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::fprintf(f, "\n==== %s ====\n%s\n", stamp, text.c_str());
    std::fclose(f);
}

bool IEQual(const std::string& a, const char* b) {
    return _stricmp(a.c_str(), b) == 0;
}

std::wstring PickUserAgent(const std::map<std::string, std::string>& headers,
                           const std::wstring& cfg_ua) {
    for (const auto& kv : headers)
        if (IEQual(kv.first, "user-agent")) return Utf8ToWide(kv.second);
    if (!cfg_ua.empty()) return cfg_ua;
    return L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
           L"(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";
}

// 上游请求头：options.headers + Referer + Range；Host/UA/Length 由 WinHTTP 自己管。
std::wstring BuildUpstreamHeaders(const std::map<std::string, std::string>& headers,
                                  const std::wstring& referer,
                                  const std::wstring& range) {
    std::wstring blob;
    bool has_referer = false;
    for (const auto& kv : headers) {
        if (IEQual(kv.first, "user-agent") || IEQual(kv.first, "host") ||
            IEQual(kv.first, "range") || IEQual(kv.first, "content-length") ||
            IEQual(kv.first, "connection"))
            continue;
        if (IEQual(kv.first, "referer")) has_referer = true;
        blob += Utf8ToWide(kv.first) + L": " + Utf8ToWide(kv.second) + L"\r\n";
    }
    if (!referer.empty() && !has_referer)
        blob += L"Referer: " + referer + L"\r\n";
    if (!range.empty()) blob += L"Range: " + range + L"\r\n";
    return blob;
}

// ───────────────────────── 会话状态 ─────────────────────────

struct MediaState {
    std::wstring url;
    std::wstring ua;
    std::wstring referer;
    int timeout_ms = 8000;
    std::map<std::string, std::string> headers;

    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    std::thread accept_th;
    std::thread probe_th;

    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::mutex mtx;
    std::condition_variable cv;
    std::wstring error;
};

struct Upstream {
    HINTERNET ses = nullptr;
    HINTERNET con = nullptr;
    HINTERNET req = nullptr;
    ~Upstream() {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
};

// 建立上游请求（自动跟随 3xx；Range 与自定义头原样带上）。
bool BeginUpstream(const MediaState& st, const std::wstring& range,
                   bool head_only, Upstream* up, DWORD* status,
                   std::string* error) {
    *status = 0;
    UrlParts u{};
    if (!Crack(st.url, &u)) {
        *error = "直链地址无法解析";
        return false;
    }
    const std::wstring ua = PickUserAgent(st.headers, st.ua);
    up->ses = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!up->ses) {
        *error = "WinHttpOpen 失败";
        return false;
    }
    const int to = st.timeout_ms > 0 ? st.timeout_ms : 8000;
    WinHttpSetTimeouts(up->ses, to, to, to, to);
    up->con = WinHttpConnect(up->ses, u.host.c_str(), u.port, 0);
    if (!up->con) {
        *error = "WinHttpConnect 失败";
        return false;
    }
    up->req = WinHttpOpenRequest(up->con, head_only ? L"HEAD" : L"GET",
                                 u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!up->req) {
        *error = "WinHttpOpenRequest 失败";
        return false;
    }
    if (u.https) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(up->req, WINHTTP_OPTION_SECURITY_FLAGS, &flags,
                         sizeof(flags));
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(up->req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect,
                     sizeof(redirect));

    const std::wstring blob =
        BuildUpstreamHeaders(st.headers, st.referer, range);
    if (!WinHttpSendRequest(
            up->req,
            blob.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : blob.c_str(),
            blob.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA,
            0, 0, 0) ||
        !WinHttpReceiveResponse(up->req, nullptr)) {
        *error = "上游请求失败（网络或地址无效）";
        return false;
    }
    DWORD sz = sizeof(*status);
    WinHttpQueryHeaders(up->req,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    return true;
}

// 预检：GET Range:0-0 判断上游是否接受（403/404 提前报错，避免打开空画面）。
void ProbeThread(std::shared_ptr<MediaState> st) {
    Upstream up;
    DWORD status = 0;
    std::string error;
    const bool ok = BeginUpstream(*st, L"bytes=0-0", false, &up, &status, &error);
    if (ok && (status == 200 || status == 206)) {
        st->ready.store(true);
        st->cv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->error = Utf8ToWide(
            ok ? ("上游 HTTP " + std::to_string(status) + "（防盗链/请求头或地址失效）")
               : error);
    }
    st->failed.store(true);
    st->cv.notify_all();
    if (!st->stop.load()) {
        AppendMediaFailLog("直链媒体预检失败: HTTP " + std::to_string(status) +
                           " url=" + WideToUtf8(st->url).substr(0, 200));
    }
}

// 本地请求 → 上游转发（Range/206 原样透传，流式，不落盘）。
void StreamForward(std::shared_ptr<MediaState> st, SOCKET c,
                   const std::wstring& range, bool head_only) {
    Upstream up;
    DWORD status = 0;
    std::string error;
    if (!BeginUpstream(*st, range, head_only, &up, &status, &error)) {
        SendSimple(c, "502 Bad Gateway", error);
        return;
    }
    if (status != 200 && status != 206) {
        if (status == 416) {
            const std::string head =
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            SendAll(c, head.data(), static_cast<int>(head.size()));
            return;
        }
        SendSimple(c, "502 Bad Gateway",
                   "上游 HTTP " + std::to_string(status));
        return;
    }
    const std::string ctype =
        WideToUtf8(HeaderText(up.req, WINHTTP_QUERY_CONTENT_TYPE));
    const std::string clen =
        WideToUtf8(HeaderText(up.req, WINHTTP_QUERY_CONTENT_LENGTH));
    const std::string crange =
        WideToUtf8(HeaderText(up.req, WINHTTP_QUERY_CONTENT_RANGE));
    const char* phrase = status == 206 ? "206 Partial Content" : "200 OK";
    std::string h = std::string("HTTP/1.1 ") + phrase + "\r\n";
    h += "Content-Type: " + (ctype.empty() ? "video/mp4" : ctype) + "\r\n";
    h += "Accept-Ranges: bytes\r\n";
    if (!crange.empty()) h += "Content-Range: " + crange + "\r\n";
    if (!clen.empty()) h += "Content-Length: " + clen + "\r\n";
    h += "Connection: close\r\n\r\n";
    if (!SendAll(c, h.data(), static_cast<int>(h.size()))) return;
    if (head_only) return;
    for (;;) {
        if (st->stop.load()) break;
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(up.req, &avail) || !avail) break;
        const DWORD want = std::min<DWORD>(avail, 64 * 1024);
        std::vector<char> chunk(want);
        DWORD got = 0;
        if (!WinHttpReadData(up.req, chunk.data(), want, &got) || !got) break;
        if (!SendAll(c, chunk.data(), static_cast<int>(got))) break;
    }
}

// 读取本地请求头（≤16KB），返回首行方法与 Range 头。
void HandleConn(std::shared_ptr<MediaState> st, SOCKET c) {
    DWORD rcv = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&rcv), sizeof(rcv));

    std::string req;
    char buf[2048];
    while (req.size() < 16 * 1024 &&
           req.find("\r\n\r\n") == std::string::npos) {
        const int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
    }
    if (req.empty()) {
        closesocket(c);
        return;
    }
    bool head_only = false;
    {
        const size_t eol = req.find("\r\n");
        const std::string first = req.substr(0, eol);
        if (first.rfind("HEAD ", 0) == 0) head_only = true;
    }
    std::wstring range;
    {
        std::string low = Lower(req);
        const size_t p = low.find("\r\nrange:");
        if (p != std::string::npos) {
            const size_t b = p + 8;
            const size_t e = req.find("\r\n", b);
            std::string value = req.substr(b, e == std::string::npos ? std::string::npos : e - b);
            const size_t s = value.find_first_not_of(" \t");
            if (s != std::string::npos) value = value.substr(s);
            range = Utf8ToWide(value);
        }
    }
    if (st->failed.load()) {
        std::wstring error;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            error = st->error;
        }
        SendSimple(c, "502 Bad Gateway", WideToUtf8(error));
        closesocket(c);
        return;
    }
    if (!st->ready.load()) {
        std::unique_lock<std::mutex> lk(st->mtx);
        st->cv.wait_for(lk, std::chrono::seconds(10), [&st] {
            return st->ready.load() || st->failed.load() || st->stop.load();
        });
        if (st->failed.load()) {
            SendSimple(c, "502 Bad Gateway", WideToUtf8(st->error));
            closesocket(c);
            return;
        }
        if (!st->ready.load()) {
            SendSimple(c, "504 Gateway Timeout", "上游媒体响应超时");
            closesocket(c);
            return;
        }
    }
    StreamForward(st, c, range, head_only);
    shutdown(c, SD_SEND);
    closesocket(c);
}

void AcceptLoop(std::shared_ptr<MediaState> st) {
    while (!st->stop.load()) {
        SOCKADDR_IN addr{};
        int len = sizeof(addr);
        SOCKET c = accept(st->listener, reinterpret_cast<sockaddr*>(&addr), &len);
        if (c == INVALID_SOCKET) break;
        std::thread([st, c]() { HandleConn(st, c); }).detach();
    }
}

// ───────────────────────── 导出接口 ─────────────────────────

int VpApiVersion() { return VP_PROTOCOL_API_VERSION; }

void VpInfo(VPProtocolInfo* out) {
    out->name = L"MEDIA";
    out->vendor = L"TTPlayer built-in (direct media relay with headers)";
    out->api_version = VP_PROTOCOL_API_VERSION;
}

int VpProbe(const wchar_t* url) {
    if (!url || !*url) return 0;
    // 30 分：只认直链媒体；HLS(100)/DASH/WEB(60) 优先。
    return IsDirectMediaUrl(url) ? 30 : 0;
}

VPSession* VpOpen(const wchar_t* url, const VPHttpConfig* cfg,
                  wchar_t* out_media_url, int cch_media_url) {
    if (!url || !out_media_url || cch_media_url <= 0) return nullptr;
    if (VpProbe(url) <= 0) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        if (g_ws_ref == 0) {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return nullptr;
        }
        ++g_ws_ref;
    }

    auto st = std::make_shared<MediaState>();
    st->url = url;
    st->ua = cfg && cfg->user_agent ? cfg->user_agent : L"";
    st->referer = cfg && cfg->referer ? cfg->referer : L"";
    st->timeout_ms = cfg && cfg->timeout_ms > 0 ? cfg->timeout_ms : 8000;
    if (cfg && cfg->options) st->headers = ParseHeaderOptions(cfg->options);

    st->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (st->listener == INVALID_SOCKET) {
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        --g_ws_ref;
        return nullptr;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(st->listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(st->listener, 8) != 0) {
        closesocket(st->listener);
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        --g_ws_ref;
        return nullptr;
    }
    sockaddr_in bound{};
    int blen = sizeof(bound);
    getsockname(st->listener, reinterpret_cast<sockaddr*>(&bound), &blen);
    st->port = ntohs(bound.sin_port);

    _snwprintf_s(out_media_url, static_cast<size_t>(cch_media_url), _TRUNCATE,
                 L"http://127.0.0.1:%d/stream", st->port);

    st->accept_th = std::thread(AcceptLoop, st);
    st->probe_th = std::thread(ProbeThread, st);
    return reinterpret_cast<VPSession*>(new std::shared_ptr<MediaState>(st));
}

int VpPoll(VPSession* s, wchar_t* out_err, int cch_err) {
    if (!s) return VP_POLL_ERROR;
    auto st = *reinterpret_cast<std::shared_ptr<MediaState>*>(s);
    if (st->stop.load()) return VP_POLL_ERROR;
    if (st->failed.load()) {
        if (out_err && cch_err > 0) {
            std::lock_guard<std::mutex> lk(st->mtx);
            wcsncpy(out_err, st->error.empty() ? L"直链媒体连接失败"
                                               : st->error.c_str(),
                    static_cast<size_t>(cch_err - 1));
            out_err[cch_err - 1] = L'\0';
        }
        return VP_POLL_ERROR;
    }
    return st->ready.load() ? VP_POLL_READY : VP_POLL_CONNECTING;
}

void VpClose(VPSession* s) {
    if (!s) return;
    auto st = *reinterpret_cast<std::shared_ptr<MediaState>*>(s);
    delete reinterpret_cast<std::shared_ptr<MediaState>*>(s);
    st->stop.store(true);
    if (st->listener != INVALID_SOCKET) {
        closesocket(st->listener);
        st->listener = INVALID_SOCKET;
    }
    st->cv.notify_all();
    if (st->accept_th.joinable()) st->accept_th.join();
    if (st->probe_th.joinable()) st->probe_th.join();
    std::lock_guard<std::mutex> lk(g_ws_mtx);
    if (g_ws_ref > 0) {
        --g_ws_ref;
        if (g_ws_ref == 0) WSACleanup();
    }
}

} // namespace

extern "C" __declspec(dllexport)
HRESULT WINAPI ttpGetVideoProtocol(VPProtocolApi* out) {
    if (!out) return E_FAIL;
    out->api_version = VpApiVersion;
    out->info = VpInfo;
    out->probe = VpProbe;
    out->open = VpOpen;
    out->poll = VpPoll;
    out->close = VpClose;
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
    }
    return TRUE;
}
