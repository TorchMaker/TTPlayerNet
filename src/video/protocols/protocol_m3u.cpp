// 直播列表协议插件（编译为 <exe>\AddIn\ttp_v_m3u.dll）
//
// TVBox 的 lives 段（16 份影视仓配置里实测 37 个）给的是**频道列表**：
//   · M3U / #EXTM3U：`#EXTINF:-1 tvg-name="CCTV1",CCTV1` 下一行是频道 URL
//   · TXT：`频道名,http://.../xxx.m3u8`
//   · JSON：`[{"name":"CCTV1","url":"http://.../1.m3u8"}, ...]`
//   · 有的直播源本身就是一条 m3u8（那归 ttp_v_hls.dll，本插件不认领）
//
// 播放器拿到的应该是「某一个频道的媒体地址」，不是整个列表文件。本 DLL：
//   · VpProbe 认领 .m3u/.txt / 含 live|lives|tvlist 的列表 URL（90 分）；
//   · VpOpen 立即返回本地代理 URL，后台线程抓列表并选出频道
//     （options 可用 {"channel":N} 或 {"prefer":"CCTV1"} 指定）；
//   · 频道是 m3u8 → 委托同目录 ttp_v_hls.dll 摊平成 TS 本地流；
//     频道是网页播放页 → 委托 ttp_v_web.dll 走 parses 解析；
//     频道是直链 → 直接 302；
//   · 代理 302 到委托结果，VpPoll / VpClose 转发状态并回收委托会话。
//
// 纯 C ABI、零第三方依赖（WinHTTP + WinSock），与音乐格式插件同族。

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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ttplayer/video/video_protocol_api.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
// x86 下 WINAPI(stdcall) 会被修饰成 _ttpGetVideoProtocol@4，
// 用链接器别名额外导出契约要求的纯名（主程序 GetProcAddress 按纯名找）。
#pragma comment(linker, "/EXPORT:ttpGetVideoProtocol=_ttpGetVideoProtocol@4")

namespace {

HMODULE g_self = nullptr;
std::mutex g_ws_mtx;
int g_ws_ref = 0;

// 定义在文件末尾「导出接口」段，频道分流时要先用。
bool LooksLikeDirectMedia(const std::wstring& url);

// ───────────────────────── 字符串工具 ─────────────────────────

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

void ReplaceAll(std::string* s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s->find(from, pos)) != std::string::npos) {
        s->replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool IsUrlTerm(unsigned char c) {
    return c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' ||
           c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ')' ||
           c == ']' || c == '}' || c == ';' || c == ',' || c == '`' ||
           c == '\x01' || c == '\0';
}

// 候选 URL 里的媒体扩展名（去掉 query 后判断）。
bool IsMediaUrl(const std::string& u) {
    std::string low = Lower(u);
    const size_t q = low.find('?');
    if (q != std::string::npos) low = low.substr(0, q);
    static const char* kExts[] = {".m3u8", ".mp4", ".flv", ".ts", ".mkv",
                                  ".m4v", ".mov", ".webm", ".avi", ".wmv"};
    for (const char* e : kExts) {
        const std::string ext(e);
        if (low.size() >= ext.size() &&
            low.compare(low.size() - ext.size(), ext.size(), ext) == 0)
            return true;
        // 形如 .m3u8?token=… 已在上面截断；形如 /play.m3u8/seg 也算。
        if (low.find(ext + "/") != std::string::npos) return true;
    }
    return false;
}

bool StartsHttp(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

// 从任意文本（JSON / HTML / JS / 解析接口返回）里挖出第一条媒体直链。
// 优酷/腾讯这类解析接口常见的转义（\/、&amp;、%3A%2F%2F）先归一化。
std::string ExtractMediaUrl(const std::string& raw) {
    std::string text = raw;
    ReplaceAll(&text, "\\/", "/");
    ReplaceAll(&text, "\\u002F", "/");
    ReplaceAll(&text, "\\u002f", "/");
    ReplaceAll(&text, "&amp;", "&");
    ReplaceAll(&text, "%3A", ":");
    ReplaceAll(&text, "%2F", "/");
    ReplaceAll(&text, "%3F", "?");
    ReplaceAll(&text, "%3D", "=");
    ReplaceAll(&text, "%26", "&");
    ReplaceAll(&text, "%3a", ":");
    ReplaceAll(&text, "%2f", "/");

    std::vector<std::string> found;
    for (size_t i = 0; i + 4 < text.size(); ++i) {
        size_t start = std::string::npos;
        bool relative = false;
        if (text.compare(i, 7, "http://") == 0 ||
            text.compare(i, 8, "https://") == 0) {
            start = i;
        } else if (text.compare(i, 2, "//") == 0 &&
                   (i == 0 || text[i - 1] != ':')) {
            start = i;
            relative = true;
        }
        if (start == std::string::npos) continue;
        size_t end = start;
        while (end < text.size() &&
               !IsUrlTerm(static_cast<unsigned char>(text[end])))
            ++end;
        std::string cand = text.substr(start, end - start);
        while (!cand.empty() && (cand.back() == '.' || cand.back() == ':'))
            cand.pop_back();
        if (relative) cand = "https:" + cand;
        if (StartsHttp(cand)) found.push_back(cand);
        i = end == start ? start : end - 1;
    }
    // m3u8 优先（HLS 可边下边播），其次 mp4/flv，最后其它。
    static const char* kRank[] = {".m3u8", ".mp4", ".flv", ".ts"};
    for (const char* e : kRank) {
        for (const auto& u : found) {
            if (Lower(u).find(e) != std::string::npos && IsMediaUrl(u)) return u;
        }
    }
    for (const auto& u : found)
        if (IsMediaUrl(u)) return u;
    return {};
}

// ───────────────────────── options JSON ─────────────────────────

struct ParseEndpoint {
    std::string name;
    std::string url;
    int type = 0;   // 0=嗅探（跳过）1=JSON 2=HTML 3=直链前缀
};

// 读取 JSON 对象片段里的字符串字段（只处理常见转义，够 parses 用）。
std::string JsonStringField(const std::string& obj, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return {};
    p = obj.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
    if (p >= obj.size() || obj[p] != '"') return {};
    ++p;
    std::string out;
    while (p < obj.size()) {
        if (obj[p] == '"') break;
        if (obj[p] == '\\' && p + 1 < obj.size()) {
            const char c = obj[p + 1];
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else if (c == 'u' && p + 5 < obj.size()) {
                // 解析接口 URL 里几乎不会出现 \u；按字面保留 4 位hex。
                out.append(obj, p, 6);
                p += 5;
            } else out.push_back(c);
            p += 2;
            continue;
        }
        out.push_back(obj[p++]);
    }
    return out;
}

int JsonIntField(const std::string& obj, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return 0;
    p = obj.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    ++p;
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
    int v = 0;
    while (p < obj.size() && obj[p] >= '0' && obj[p] <= '9')
        v = v * 10 + (obj[p++] - '0');
    return v;
}

void ParseOptions(const wchar_t* options, std::vector<ParseEndpoint>* out) {
    if (!options || !*options) return;
    const std::string j = WideToUtf8(options);
    size_t p = j.find("\"parses\"");
    if (p == std::string::npos) return;
    p = j.find('[', p);
    if (p == std::string::npos) return;
    const size_t array_end = j.find(']', p);
    if (array_end == std::string::npos) return;

    bool in_str = false, esc = false;
    int depth = 0;
    size_t obj_begin = std::string::npos;
    for (size_t i = p + 1; i < array_end; ++i) {
        const char c = j[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') {
            if (depth == 0) obj_begin = i;
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 0 && obj_begin != std::string::npos) {
                const std::string obj = j.substr(obj_begin, i - obj_begin + 1);
                ParseEndpoint ep;
                ep.url = JsonStringField(obj, "url");
                ep.name = JsonStringField(obj, "name");
                ep.type = JsonIntField(obj, "type");
                if (!ep.url.empty() && ep.type != 0) out->push_back(std::move(ep));
                obj_begin = std::string::npos;
            }
        }
    }
}

// 按 TVBox 约定拼解析接口请求地址。
std::string BuildParseRequest(const ParseEndpoint& ep, const std::string& raw) {
    std::string t = ep.url;
    const size_t brace = t.find("{url}");
    if (brace != std::string::npos) {
        t.replace(brace, 5, raw);
        return t;
    }
    const size_t pct = t.find("%s");
    if (pct != std::string::npos) {
        t.replace(pct, 2, raw);
        return t;
    }
    if (ep.type == 3) return t + raw;       // 直链前缀
    const char sep = t.find('?') == std::string::npos ? '?' : '&';
    return t + sep + "url=" + raw;
}

// ───────────────────────── WinHTTP ─────────────────────────

bool IsRedirect(DWORD s) {
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
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
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    out->https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    out->host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out->path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        out->path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (out->path.empty()) out->path = L"/";
    // 片段（#...）不能进请求行，否则服务端按字面路径找 → 404。
    if (const size_t hf = out->path.find(L'#');
        hf != std::wstring::npos) out->path.resize(hf);
    out->port = uc.nPort ? uc.nPort : (out->https ? 443 : 80);
    return !out->host.empty();
}

HINTERNET MakeRequest(HINTERNET con, const std::wstring& path, bool https,
                      const std::wstring& range) {
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
    if (!range.empty()) {
        WinHttpAddRequestHeaders(req, range.c_str(),
                                 static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }
    return req;
}

// 抓文本（解析接口返回 / 播放页 HTML），最多 max_bytes，跟随 ≤4 跳重定向。
bool HttpGetText(const std::wstring& url, const std::wstring& ua,
                 const std::wstring& ref, int timeout_ms, size_t max_bytes,
                 std::string* body, DWORD* out_status) {
    body->clear();
    if (out_status) *out_status = 0;
    UrlParts u{};
    if (!Crack(url, &u)) return false;
    // 真实源对非浏览器 UA 直接 403，默认必须是浏览器 UA。
    HINTERNET ses = WinHttpOpen(
        ua.empty() ? L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                     L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
                   : ua.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    const int to = timeout_ms > 0 ? timeout_ms : 8000;
    WinHttpSetTimeouts(ses, to, to, to, to);
    bool ok = false;
    for (int hop = 0; hop < 5; ++hop) {
        HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0);
        if (!con) break;
        HINTERNET req = MakeRequest(con, u.path, u.https, L"");
        if (!req) { WinHttpCloseHandle(con); break; }
        const wchar_t* r = ref.empty() ? WINHTTP_NO_REFERER : ref.c_str();
        if (!WinHttpSendRequest(req, r, r ? -1L : 0L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            break;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        if (out_status) *out_status = status;
        if (IsRedirect(status)) {
            wchar_t loc[4096]{};
            DWORD ls = sizeof(loc);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &ls,
                                    WINHTTP_NO_HEADER_INDEX)) {
                std::wstring next = loc;
                if (next.find(L"http") != 0) {
                    const size_t slash = url.find(L'/', url.find(L"://") + 3);
                    next = url.substr(0, slash) + next;
                }
                UrlParts nu{};
                if (Crack(next, &nu)) u = nu;
            }
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            continue;
        }
        if (status >= 200 && status < 300) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                std::vector<char> chunk(avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got)
                    break;
                if (body->size() + got > max_bytes) got = static_cast<DWORD>(
                    max_bytes > body->size() ? max_bytes - body->size() : 0);
                body->append(chunk.data(), got);
                if (body->size() >= max_bytes) break;
            }
            ok = true;
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        break;
    }
    WinHttpCloseHandle(ses);
    return ok;
}

std::wstring HeaderText(HINTERNET req, DWORD which) {
    wchar_t buf[1024]{};
    DWORD sz = sizeof(buf);
    if (!WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, buf,
                             &sz, WINHTTP_NO_HEADER_INDEX) || sz == 0)
        return {};
    // sz 是字节数，结尾自带一个 L'\0'。
    const size_t chars = sz / sizeof(wchar_t);
    return std::wstring(buf, chars > 0 && buf[chars - 1] == L'\0'
                                 ? chars - 1 : chars);
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

// 带 UA / Referer / Range 的流式转发（解决 MF 窗口化后端不能带请求头）。
void StreamForward(SOCKET c, const std::wstring& url, const std::wstring& ua,
                   const std::wstring& ref, int timeout_ms,
                   const std::wstring& range, const std::atomic<bool>& stop) {
    UrlParts u{};
    if (!Crack(url, &u)) { SendSimple(c, "502 Bad Gateway", "bad media url"); return; }
    // 真实源对非浏览器 UA 直接 403，默认必须是浏览器 UA。
    HINTERNET ses = WinHttpOpen(
        ua.empty() ? L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                     L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
                   : ua.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { SendSimple(c, "502 Bad Gateway", "WinHttpOpen failed"); return; }
    const int to = timeout_ms > 0 ? timeout_ms : 8000;
    WinHttpSetTimeouts(ses, to, to, to, to);
    bool done = false;
    for (int hop = 0; hop < 5 && !done && !stop.load(); ++hop) {
        HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0);
        if (!con) break;
        HINTERNET req = MakeRequest(con, u.path, u.https, range);
        if (!req) { WinHttpCloseHandle(con); break; }
        const wchar_t* r = ref.empty() ? WINHTTP_NO_REFERER : ref.c_str();
        if (!WinHttpSendRequest(req, r, r ? -1L : 0L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            break;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
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
                    const size_t slash = url.find(L'/', url.find(L"://") + 3);
                    next = url.substr(0, slash) + next;
                }
                UrlParts nu{};
                if (Crack(next, &nu)) u = nu;
            }
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            continue;
        }
        const std::string ctype = WideToUtf8(
            HeaderText(req, WINHTTP_QUERY_CONTENT_TYPE));
        const std::string clen = WideToUtf8(
            HeaderText(req, WINHTTP_QUERY_CONTENT_LENGTH));
        const std::string crange = WideToUtf8(
            HeaderText(req, WINHTTP_QUERY_CONTENT_RANGE));
        const char* phrase = status == 206 ? "206 Partial Content"
                                           : "200 OK";
        std::string h = std::string("HTTP/1.1 ") + phrase + "\r\n";
        h += "Content-Type: " + (ctype.empty() ? "video/mp4" : ctype) + "\r\n";
        h += "Accept-Ranges: bytes\r\n";
        if (!crange.empty()) h += "Content-Range: " + crange + "\r\n";
        if (!clen.empty()) h += "Content-Length: " + clen + "\r\n";
        h += "Connection: close\r\n\r\n";
        if (!SendAll(c, h.data(), static_cast<int>(h.size()))) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            break;
        }
        for (;;) {
            if (stop.load()) break;
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
            DWORD want = std::min<DWORD>(avail, 64 * 1024);
            std::vector<char> chunk(want);
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), want, &got) || !got) break;
            if (!SendAll(c, chunk.data(), static_cast<int>(got))) break;
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        done = true;
    }
    if (!done && !stop.load()) SendSimple(c, "502 Bad Gateway",
                                          "upstream media request failed");
    WinHttpCloseHandle(ses);
}

// ───────────────────────── 直播列表解析 ─────────────────────────

struct Channel {
    std::string name;
    std::string url;
};

std::string TrimStr(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

// 相对频道地址补全（列表里常见 ./xxx.m3u8 或 //cdn/xxx.m3u8）。
std::string JoinChannelUrl(const std::string& base, const std::string& ref) {
    if (StartsHttp(ref)) return ref;
    if (ref.rfind("//", 0) == 0) {
        const size_t p = base.find("://");
        return p == std::string::npos ? "https:" + ref : base.substr(0, p) + ":" + ref;
    }
    const size_t slash = base.rfind('/');
    return slash == std::string::npos ? ref : base.substr(0, slash + 1) + ref;
}

void ParseChannelList(const std::string& text, const std::string& base_url,
                      std::vector<Channel>* out) {
    // ① JSON 频道数组（含 lives / channels / list 等包装）。
    if (text.find('{') != std::string::npos &&
        text.find("\"url\"") != std::string::npos) {
        bool in_str = false, esc = false;
        int depth = 0;
        size_t begin = std::string::npos;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{') {
                if (depth == 0) begin = i;
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0 && begin != std::string::npos) {
                    const std::string obj = text.substr(begin, i - begin + 1);
                    const std::string u = JsonStringField(obj, "url");
                    if (StartsHttp(u))
                        out->push_back({JsonStringField(obj, "name"), u});
                    begin = std::string::npos;
                }
            }
        }
        if (!out->empty()) return;
    }
    // ② M3U / TXT / 纯 URL 行。
    std::istringstream ss(text);
    std::string ln;
    std::string pending;
    while (std::getline(ss, ln)) {
        ln = TrimStr(ln);
        if (ln.empty()) continue;
        if (ln.rfind("#EXTINF", 0) == 0) {
            const size_t comma = ln.find(',');
            pending = comma == std::string::npos
                          ? std::string()
                          : TrimStr(ln.substr(comma + 1));
            if (pending.empty()) {   // 试着取 tvg-name="..."
                const size_t p = ln.find("tvg-name=\"");
                if (p != std::string::npos) {
                    const size_t b = p + 10;
                    const size_t e = ln.find('"', b);
                    if (e != std::string::npos)
                        pending = ln.substr(b, e - b);
                }
            }
            continue;
        }
        if (ln[0] == '#') continue;
        std::string url = ln;
        const size_t comma = ln.find(',');
        if (comma != std::string::npos) {
            const std::string tail = TrimStr(ln.substr(comma + 1));
            if (StartsHttp(tail)) {
                if (pending.empty()) pending = TrimStr(ln.substr(0, comma));
                url = tail;
            }
        }
        url = JoinChannelUrl(base_url, url);
        if (!StartsHttp(url)) continue;
        out->push_back({pending.empty()
                            ? "频道 " + std::to_string(out->size() + 1)
                            : pending,
                        url});
        pending.clear();
    }
}

// ───────────────────────── 会话状态 ─────────────────────────

struct WebState {
    std::wstring origin;
    std::wstring ua;
    std::wstring referer;
    std::wstring options;
    int timeout_ms = 8000;
    std::vector<ParseEndpoint> parses;
    // 频道选择：options 里的 {"channel":N}（0 起）或 {"prefer":"CCTV1"}。
    int pref_channel = -1;
    std::wstring pref_name;

    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    std::thread resolve_th;
    std::thread accept_th;

    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::mutex mtx;
    std::condition_variable cv;

    std::wstring resolved;     // 解析出的真实媒体直链
    std::wstring final_media;  // 交给播放器/代理的最终地址
    std::wstring error;
    bool via_hls = false;      // 是否委托了 ttp_v_hls.dll

    HMODULE hls_mod = nullptr;
    VPProtocolApi hls_api{};
    VPSession* hls_session = nullptr;
};

// 从本 DLL 所在目录加载另一个协议 DLL，把选中的频道交给它处理。
// dll_name：ttp_v_hls.dll（m3u8 频道）/ ttp_v_web.dll（网页频道）。
// pass_options：WEB 需要主程序传下来的 parses 解析接口。
bool DelegateTo(const std::shared_ptr<WebState>& st,
                const std::wstring& target, const wchar_t* dll_name,
                bool pass_options) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    full = full.substr(0, slash + 1) + dll_name;
    HMODULE mod = LoadLibraryW(full.c_str());
    if (!mod) return false;
    FARPROC proc = GetProcAddress(mod, "ttpGetVideoProtocol");
    if (!proc) proc = GetProcAddress(mod, "_ttpGetVideoProtocol@4");
    const auto factory =
        reinterpret_cast<HRESULT(WINAPI*)(VPProtocolApi*)>(proc);
    VPProtocolApi api{};
    if (!factory || FAILED(factory(&api)) || !api.open || !api.poll ||
        !api.close || !api.api_version ||
        api.api_version() != VP_PROTOCOL_API_VERSION) {
        FreeLibrary(mod);
        return false;
    }
    VPHttpConfig cfg{};
    cfg.user_agent = st->ua.empty() ? nullptr : st->ua.c_str();
    cfg.referer = st->referer.empty() ? nullptr : st->referer.c_str();
    cfg.timeout_ms = st->timeout_ms;
    cfg.options = pass_options && !st->options.empty()
                      ? st->options.c_str()
                      : nullptr;
    wchar_t media[1024]{};
    VPSession* s = api.open(target.c_str(), &cfg, media, 1024);
    if (!s || !media[0]) {
        if (s) api.close(s);
        FreeLibrary(mod);
        return false;
    }
    st->hls_mod = mod;
    st->hls_api = api;
    st->hls_session = s;
    st->final_media = media;
    st->via_hls = true;   // 语义：已委托给另一个协议 DLL
    return true;
}

void ResolveThread(const std::shared_ptr<WebState>& st) {
    std::string body;
    DWORD status = 0;
    std::string detail;
    std::vector<Channel> channels;
    if (HttpGetText(st->origin, st->ua, st->referer, st->timeout_ms,
                    2 * 1024 * 1024, &body, &status)) {
        ParseChannelList(body, WideToUtf8(st->origin), &channels);
        if (channels.empty())
            detail = "列表里没解析出频道（不是 M3U/TXT/JSON 频道列表？）";
    } else {
        detail = "直播列表抓取失败(HTTP " + std::to_string(status) + ")";
    }

    // 选频道：优先按名称匹配，其次 options 的 channel 序号，最后取第一个。
    int pick = -1;
    if (!st->pref_name.empty()) {
        for (size_t i = 0; i < channels.size(); ++i) {
            if (Utf8ToWide(channels[i].name).find(st->pref_name) !=
                std::wstring::npos) {
                pick = static_cast<int>(i);
                break;
            }
        }
    }
    if (pick < 0 && st->pref_channel >= 0 &&
        st->pref_channel < static_cast<int>(channels.size()))
        pick = st->pref_channel;
    if (pick < 0 && !channels.empty()) pick = 0;

    if (pick < 0) {
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->failed.store(true);
            st->error = L"直播列表协议没能选出可播频道。\r\n" +
                        Utf8ToWide(detail) +
                        L"\r\n请用配置里的 lives 源，或直接打开单条 m3u8 频道。";
        }
        st->cv.notify_all();
        return;
    }

    const Channel& ch = channels[static_cast<size_t>(pick)];
    const std::wstring wurl = Utf8ToWide(ch.url);
    const std::string low = Lower(ch.url);
    bool ok = false;
    if (low.find(".m3u8") != std::string::npos) {
        ok = DelegateTo(st, wurl, L"ttp_v_hls.dll", false);
        if (!ok) {   // 少数系统 MF 自带 HLS 源，退回直连
            std::lock_guard<std::mutex> lk(st->mtx);
            st->final_media = wurl;
            ok = true;
        }
    } else if (!IsMediaUrl(ch.url) && !LooksLikeDirectMedia(wurl)) {
        // 网页频道（解析接口线路）→ 交给 WEB 协议插件，options 带 parses。
        ok = DelegateTo(st, wurl, L"ttp_v_web.dll", true);
    } else {
        ok = true;   // 直链频道：代理直接 302 过去
    }
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->resolved = wurl;
        if (ok && st->final_media.empty()) st->final_media = wurl;
        if (ok) {
            st->error = Utf8ToWide("频道[" + ch.name + "] / 共 " +
                                   std::to_string(channels.size()) + " 个频道");
            st->ready.store(true);
        } else {
            st->failed.store(true);
            st->error = L"频道 [" + Utf8ToWide(ch.name) +
                        L"] 无法交给 HLS/WEB 协议插件处理（DLL 缺失或打开失败）。";
        }
    }
    st->cv.notify_all();
}

void HandleConn(const std::shared_ptr<WebState>& st, SOCKET c) {
    char buf[8193]{};
    int n = 0;
    while (n < 8192) {
        const int r = recv(c, buf + n, 8192 - n, 0);
        if (r <= 0) break;
        n += r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[n] = '\0';
    std::wstring range;
    {
        const std::string req(buf, static_cast<size_t>(n));
        const std::string low = Lower(req);
        const size_t p = low.find("range:");
        if (p != std::string::npos) {
            size_t e = req.find("\r\n", p);
            if (e == std::string::npos) e = req.size();
            std::string v = req.substr(p, e - p);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
                v.pop_back();
            range = Utf8ToWide(v);
        }
    }
    {
        std::unique_lock<std::mutex> lk(st->mtx);
        st->cv.wait_for(lk,
                        std::chrono::milliseconds(st->timeout_ms + 5000),
                        [&] {
                            return st->ready.load() || st->failed.load() ||
                                   st->stop.load();
                        });
    }
    if (st->stop.load()) { closesocket(c); return; }
    if (!st->ready.load()) {   // 解析失败或超时：把详细原因回给播放器
        std::wstring err;
        { std::lock_guard<std::mutex> lk(st->mtx); err = st->error; }
        SendSimple(c, "503 Service Unavailable",
                   err.empty() ? "解析播放地址超时" : WideToUtf8(err));
        closesocket(c);
        return;
    }
    std::wstring final_media;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        final_media = st->final_media;
    }
    // 频道解析结果统一 302：可能指向 HLS/WEB 插件的本地代理，
    // 也可能就是一条 mp4 直链（MF 跟随重定向即可）。
    (void)range;   // 列表插件本身不转发字节，Range 由目标代理处理
    std::string h = "HTTP/1.1 302 Found\r\nLocation: " +
                    WideToUtf8(final_media) +
                    "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    SendAll(c, h.data(), static_cast<int>(h.size()));
    closesocket(c);
}

void AcceptLoop(const std::shared_ptr<WebState>& st) {
    while (!st->stop.load()) {
        SOCKET c = accept(st->listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        std::thread([st, c] { HandleConn(st, c); }).detach();
    }
}

// ───────────────────────── 导出接口 ─────────────────────────

bool LooksLikeDirectMedia(const std::wstring& url) {
    const std::string low = Lower(WideToUtf8(url));
    const size_t q = low.find('?');
    const std::string head = q == std::string::npos ? low : low.substr(0, q);
    static const char* kExts[] = {".m3u8", ".mp4", ".flv", ".ts", ".mkv",
                                  ".m4v", ".mov", ".webm", ".avi", ".wmv",
                                  ".mp3", ".m4a", ".aac"};
    for (const char* e : kExts)
        if (head.find(e) != std::string::npos) return true;
    return false;
}

int VpApiVersion() { return VP_PROTOCOL_API_VERSION; }

void VpInfo(VPProtocolInfo* out) {
    out->name = L"M3U";
    out->vendor = L"TTPlayer built-in (live channel list -> HLS/WEB delegate)";
    out->api_version = VP_PROTOCOL_API_VERSION;
}

int VpProbe(const wchar_t* url) {
    if (!url || !*url) return 0;
    const std::wstring u(url);
    std::string low = Lower(WideToUtf8(u));
    if (low.rfind("http://", 0) != 0 && low.rfind("https://", 0) != 0)
        return 0;
    const size_t q = low.find('?');
    if (q != std::string::npos) low = low.substr(0, q);
    // m3u8 是 HLS 媒体列表（不是频道列表），mpd 归 DASH，直链归 MF。
    if (low.find(".m3u8") != std::string::npos ||
        low.find(".mpd") != std::string::npos ||
        LooksLikeDirectMedia(u))
        return 0;
    // 明确的直播列表特征。普通网页留给 WEB（60 分），本插件不抢。
    if (low.size() >= 4 && low.substr(low.size() - 4) == ".m3u") return 90;
    if (low.size() >= 4 && low.substr(low.size() - 4) == ".txt") return 90;
    if (low.find("/lives") != std::string::npos ||
        low.find("/live/") != std::string::npos ||
        low.find("tvlist") != std::string::npos ||
        low.find("channel") != std::string::npos ||
        low.find("m3u?") != std::string::npos)
        return 90;
    return 0;
}

VPSession* VpOpen(const wchar_t* url, const VPHttpConfig* cfg,
                  wchar_t* out_media_url, int cch_media_url) {
    if (!url || !*url || !out_media_url || cch_media_url <= 0) return nullptr;
    if (VpProbe(url) <= 0) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        if (g_ws_ref == 0) {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return nullptr;
        }
        ++g_ws_ref;
    }
    auto st = std::make_shared<WebState>();
    st->origin = url;
    st->ua = cfg && cfg->user_agent ? cfg->user_agent : L"";
    st->referer = cfg && cfg->referer ? cfg->referer : L"";
    st->timeout_ms = cfg && cfg->timeout_ms > 0 ? cfg->timeout_ms : 8000;
    st->options = cfg && cfg->options ? cfg->options : L"";
    ParseOptions(st->options.c_str(), &st->parses);
    {
        const std::string oj = WideToUtf8(st->options);
        st->pref_channel = oj.find("\"channel\"") == std::string::npos
                               ? -1
                               : JsonIntField(oj, "channel");
        st->pref_name = Utf8ToWide(JsonStringField(oj, "prefer"));
    }

    st->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (st->listener == INVALID_SOCKET) {
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        if (--g_ws_ref == 0) WSACleanup();
        return nullptr;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(st->listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
        listen(st->listener, 8)) {
        closesocket(st->listener);
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        if (--g_ws_ref == 0) WSACleanup();
        return nullptr;
    }
    sockaddr_in bound{};
    int blen = sizeof(bound);
    getsockname(st->listener, reinterpret_cast<sockaddr*>(&bound), &blen);
    st->port = ntohs(bound.sin_port);
    // 不带扩展名：MF 按响应 Content-Type / 内容嗅探；m3u8 会 302 到 .ts。
    swprintf_s(out_media_url, static_cast<size_t>(cch_media_url),
               L"http://127.0.0.1:%d/stream", st->port);

    st->resolve_th = std::thread(ResolveThread, st);
    st->accept_th = std::thread(AcceptLoop, st);
    return reinterpret_cast<VPSession*>(new std::shared_ptr<WebState>(st));
}

int VpPoll(VPSession* s, wchar_t* out_err, int cch_err) {
    if (!s) return VP_POLL_ERROR;
    const auto st = *reinterpret_cast<std::shared_ptr<WebState>*>(s);
    if (st->failed.load() || st->stop.load()) {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (out_err && cch_err > 0) {
            wcsncpy_s(out_err, static_cast<size_t>(cch_err),
                      st->error.empty() ? L"直播列表协议会话已停止"
                                        : st->error.c_str(),
                      static_cast<size_t>(cch_err - 1));
        }
        return VP_POLL_ERROR;
    }
    if (!st->ready.load()) return VP_POLL_CONNECTING;
    if (st->via_hls && st->hls_api.poll && st->hls_session) {
        wchar_t err[512]{};
        const int state = st->hls_api.poll(st->hls_session, err, 512);
        if (state == VP_POLL_ERROR && out_err && cch_err > 0)
            wcsncpy_s(out_err, static_cast<size_t>(cch_err), err,
                      static_cast<size_t>(cch_err - 1));
        return state;
    }
    return VP_POLL_READY;
}

void VpClose(VPSession* s) {
    if (!s) return;
    const auto st = *reinterpret_cast<std::shared_ptr<WebState>*>(s);
    delete reinterpret_cast<std::shared_ptr<WebState>*>(s);
    st->stop.store(true);
    if (st->listener != INVALID_SOCKET) {
        closesocket(st->listener);
        st->listener = INVALID_SOCKET;
    }
    st->cv.notify_all();
    if (st->resolve_th.joinable()) st->resolve_th.join();
    if (st->accept_th.joinable()) st->accept_th.join();
    if (st->hls_session && st->hls_api.close)
        st->hls_api.close(st->hls_session);
    if (st->hls_mod) FreeLibrary(st->hls_mod);
    std::lock_guard<std::mutex> lk(g_ws_mtx);
    if (--g_ws_ref == 0) WSACleanup();
}

} // namespace

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

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_self = reinterpret_cast<HMODULE>(hinst);
    return TRUE;
}
