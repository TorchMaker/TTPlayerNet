// 网页播放地址协议插件（编译为 <exe>\AddIn\ttp_v_web.dll）
//
// 影视仓 / TVBox 的线路里，除了 m3u8/mp4 直链，还有大量「网页播放地址」：
//   https://v.youku.com/v_show/id_xxx.html、https://host/share/xxx …
// 这类地址不是传输协议，而是「内容寻址」——播放器必须先把网页换成真实
// 媒体直链。影视仓靠 parses（解析接口）+ spider 完成这一步。
//
// 本 DLL 把这一层做成协议插件，与 ttp_v_hls.dll 同一套工厂契约：
//   · VpProbe 认领 http(s) 且不带媒体扩展名的地址；
//   · VpOpen 立即返回一个本地代理 URL，后台线程按
//       ① 配置 parses 解析接口（options JSON 传入）
//       ② 播放页 HTML / iframe 里的内嵌直链
//     解析出 m3u8 / mp4 / flv；
//   · 解析结果是 m3u8 → 委托同目录 ttp_v_hls.dll 摊平成 TS 本地流，
//     代理 302 过去（Media Foundation 没有内置 HLS 源）；
//   · 解析结果是 mp4/flv → 本地代理用 WinHTTP 带 UA/Referer/Range 转发，
//     解决「MF 窗口化后端不能自定义请求头」导致的直链播放失败；
//   · VpPoll / VpClose 转发状态并回收线程。
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

// ───────────────────── 相对媒体地址（分享页 JS 变量） ─────────────────────

// 把 "/xxx/index.m3u8?sign=…" 这类相对路径按页面 URL 补成绝对地址。
std::string JoinUrl(const std::string& base, const std::string& ref) {
    if (StartsHttp(ref)) return ref;
    if (ref.rfind("//", 0) == 0) {
        const size_t p = base.find("://");
        return p == std::string::npos ? "https:" + ref : base.substr(0, p) + ":" + ref;
    }
    const size_t scheme = base.find("://");
    if (!ref.empty() && ref[0] == '/') {
        const size_t slash = scheme == std::string::npos
                                 ? std::string::npos
                                 : base.find('/', scheme + 3);
        return slash == std::string::npos ? base + ref : base.substr(0, slash) + ref;
    }
    const size_t slash = base.rfind('/');
    return slash == std::string::npos ? ref : base.substr(0, slash + 1) + ref;
}

// 影视仓分享页常见写法：
//   const url = "/20261004/59072_72c88840/index.m3u8?sign=xxx";
//   var playurl = '/xxx/1.mp4';
// 这些是**相对路径**，ExtractMediaUrl（只认 http(s)）找不到，必须单独扫。
std::string ExtractRelativeMediaUrl(const std::string& text) {
    static const char* kExts[] = {".m3u8", ".mp4", ".flv", ".ts", ".mkv",
                                  ".m4v", ".mov", ".webm"};
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"' && text[i] != '\'') continue;
        const char quote = text[i];
        const size_t b = i + 1;
        const size_t e = text.find(quote, b);
        if (e == std::string::npos) break;
        i = e;
        if (e - b < 5 || e - b > 2048) continue;
        std::string s = text.substr(b, e - b);
        if (s.rfind("./", 0) == 0) s = s.substr(1);
        if (s.empty() || s[0] != '/') continue;   // 只要根相对路径
        const std::string low = Lower(s);
        const size_t q = low.find('?');
        const std::string head = q == std::string::npos ? low : low.substr(0, q);
        bool media = false;
        for (const char* ext : kExts)
            if (head.find(ext) != std::string::npos) { media = true; break; }
        if (media) return s;
    }
    return {};
}

// ───────────── TVBox 风格：从 JSON 响应取直链（type 1） ─────────────
// 扫描 "url" / "data":{"url"} 等键值，优先带媒体后缀者。很多解析接口返回
// {"url":"https://...m3u8"} 或 {"data":{"url":"..."}}，纯子串扫描容易漏/拿错。
std::string ExtractJsonMediaUrl(const std::string& body) {
    const std::string low = Lower(body);
    std::vector<std::string> cands;
    size_t pos = 0;
    while ((pos = low.find("\"url\"", pos)) != std::string::npos) {
        const size_t colon = body.find(':', pos + 5);
        if (colon == std::string::npos) break;
        const size_t q = body.find('"', colon + 1);
        if (q == std::string::npos) break;
        const size_t q2 = body.find('"', q + 1);
        if (q2 == std::string::npos) break;
        std::string v = body.substr(q + 1, q2 - q - 1);
        ReplaceAll(&v, "\\/", "/");
        ReplaceAll(&v, "\\u002F", "/");
        ReplaceAll(&v, "\\u002f", "/");              // 编码器输出小写 hex 也认
        if (v.rfind("//", 0) == 0) v = "https:" + v; // 协议相对直链 //host/x.m3u8
        if (StartsHttp(v)) cands.push_back(v);
        pos = q2 + 1;
    }
    static const char* kExt[] = {".m3u8", ".mp4", ".flv", ".ts", ".m4v",
                                 ".mov", ".webm", ".m3u"};
    for (const char* e : kExt)
        for (const auto& c : cands)
            if (Lower(c).find(e) != std::string::npos) return c;
    for (const auto& c : cands)
        if (c.size() > 20) return c;
    return {};
}

// 解析出的直链是否可当作最终媒体地址（TVBox 用长度阈值判定有效）。
bool IsValidResolvedUrl(const std::string& u) {
    if (!StartsHttp(u)) return false;
    static const char* kExt[] = {".m3u8", ".mp4", ".flv", ".ts", ".m4v",
                                 ".mov", ".webm", ".m3u"};
    const std::string low = Lower(u);
    for (const char* e : kExt)
        if (low.find(e) != std::string::npos) return true;
    return u.size() > 40;  // 部分接口返回无后缀的 CDN 直链
}

// ───────────────────────── 失败源诊断日志 ─────────────────────────
// 与主程序共用一个 log（<exe>\video\play_fail.log）；测试时可用环境变量
// TTPLAYER_PLAY_FAIL_LOG 指定路径，便于直接对真实源做无 GUI 测试。
std::wstring PlayFailLogPath() {
    wchar_t custom[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"TTPLAYER_PLAY_FAIL_LOG",
                                            custom, 1024);
    if (n > 0 && n < 1024) return custom;
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    const std::wstring p(exe);
    const size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return {};
    return p.substr(0, s) + L"\\video\\play_fail.log";
}

void AppendProtoFailLog(const std::string& text) {
    const std::wstring path = PlayFailLogPath();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    char stamp[64]{};
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::snprintf(stamp, sizeof(stamp),
                  "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth,
                  t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::fprintf(f, "\n==== %s ====\n%s\n", stamp, text.c_str());
    std::fclose(f);
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

// 解析 302/301 的 Location 头，按相对引用规则补全成绝对地址。
// base 必须传「当前跳转后的 url」（不是最初的 url），否则多跳重定向会拼错：
//   · http(s)://host/path   绝对地址，原样返回（大小写不敏感）；
//   · //other.host/path     协议相对，继承 base 的 scheme；
//   · /path                 根相对，继承 base 的 scheme + host[:port]；
//   · path                  相对路径，基于 base 所在目录拼接。
std::wstring ResolveLocation(const std::wstring& base, const std::wstring& loc) {
    // 去掉首尾空白（个别服务器的 Location 会带空格 / CR / LF）
    const size_t b = loc.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    const size_t e = loc.find_last_not_of(L" \t\r\n");
    const std::wstring l = loc.substr(b, e - b + 1);
    // 绝对 URL（HTTP scheme 官方写法可能是大写，比较要忽略大小写）
    if (_wcsnicmp(l.c_str(), L"http://", 7) == 0 ||
        _wcsnicmp(l.c_str(), L"https://", 8) == 0)
        return l;
    UrlParts bp{};
    if (!Crack(base, &bp)) return {};    // base 不合法，相对地址无从拼起
    const std::wstring scheme = bp.https ? L"https" : L"http";
    std::wstring authority = scheme + L"://" + bp.host;
    if (bp.port != (bp.https ? 443 : 80))   // 非默认端口必须带上
        authority += L":" + std::to_wstring(bp.port);
    if (l.rfind(L"//", 0) == 0) return scheme + L":" + l;   // 协议相对
    if (l[0] == L'/') return authority + l;                  // 根相对
    // 相对路径：去掉 base 自身的 query/fragment，取目录再拼
    std::wstring path = bp.path;
    const size_t q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path = path.substr(0, q);
    const size_t slash = path.find_last_of(L'/');
    return authority +
           (slash == std::wstring::npos ? L"/" : path.substr(0, slash + 1)) + l;
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
    std::wstring cur = url;   // 当前实际请求的地址（每跳更新，供相对 Location 拼接）
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
            // Location 取不到（或拼不出下一跳）时不再原地重试同一地址——
            // 那样只会空转满 5 跳；直接退出并保留已拿到的 body/status。
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &ls,
                                    WINHTTP_NO_HEADER_INDEX)) {
                const std::wstring next = ResolveLocation(cur, loc);
                UrlParts nu{};
                if (!next.empty() && Crack(next, &nu)) {
                    u = nu;
                    cur = next;
                    WinHttpCloseHandle(req);
                    WinHttpCloseHandle(con);
                    continue;
                }
            }
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            break;
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
    std::wstring cur = url;   // 当前实际请求的地址（每跳更新，供相对 Location 拼接）
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
            // Location 取不到（或拼不出下一跳）时不再原地重试同一地址——
            // 那样只会空转满 5 跳；直接退出，由外层回 502。
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &ls,
                                    WINHTTP_NO_HEADER_INDEX)) {
                const std::wstring next = ResolveLocation(cur, loc);
                UrlParts nu{};
                if (!next.empty() && Crack(next, &nu)) {
                    u = nu;
                    cur = next;
                    WinHttpCloseHandle(req);
                    WinHttpCloseHandle(con);
                    continue;
                }
            }
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);
            break;
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

// ───────────────────────── 会话状态 ─────────────────────────

struct WebState {
    std::wstring origin;
    std::wstring ua;
    std::wstring referer;
    std::wstring options;
    int timeout_ms = 8000;
    std::vector<ParseEndpoint> parses;

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

// 从本 DLL 所在目录加载 ttp_v_hls.dll，把 m3u8 摊平成 TS 本地流。
bool DelegateToHls(const std::shared_ptr<WebState>& st,
                   const std::wstring& m3u8) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    full = full.substr(0, slash + 1) + L"ttp_v_hls.dll";
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
    cfg.options = nullptr;
    wchar_t media[1024]{};
    VPSession* s = api.open(m3u8.c_str(), &cfg, media, 1024);
    if (!s || !media[0]) {
        if (s) api.close(s);
        FreeLibrary(mod);
        return false;
    }
    st->hls_mod = mod;
    st->hls_api = api;
    st->hls_session = s;
    st->final_media = media;
    st->via_hls = true;
    return true;
}

void ResolveThread(const std::shared_ptr<WebState>& st) {
    const std::string raw = WideToUtf8(st->origin);
    std::string media;
    std::string detail;
    std::string page_info;   // 失败时落日志的播放页诊断
    // 总解析预算：MF 会在代理连接上等数据；超级解析并行竞速，预算放宽到 18s。
    const DWORD deadline = GetTickCount() + 18000;
    const int per_req = std::min(st->timeout_ms, 8000);
    // 解析起点：成功日志里记总耗时用
    const auto t_start = std::chrono::steady_clock::now();

    // ① 配置里的 parses（影视仓解析接口）+ 内置兜底，按 TVBox「超级解析」策略：
    //    所有接口并行竞速，先返回有效直链者胜（不再串行挨个试到第 6 个就放弃）。
    std::vector<ParseEndpoint> all = st->parses;
    {
        std::vector<std::string> seen;
        std::vector<ParseEndpoint> dedup;
        for (auto& ep : all) {
            if (ep.url.empty()) continue;
            const std::string key = std::to_string(ep.type) + "|" + ep.url;
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            dedup.push_back(std::move(ep));
        }
        all = std::move(dedup);
    }

    std::atomic<bool> done{false};
    std::mutex pmtx;
    std::string pdetail;
    std::string pmedia;
    std::string pwho;
    const size_t total = all.size();
    std::atomic<int> tried{0};      // 已真正发起请求的接口数
    std::atomic<int> skipped{0};    // 因 done/stop 没发起请求的接口数（区别于「试了失败」）
    const std::wstring kBrowserUA =
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

    auto try_one = [&](const ParseEndpoint& ep, size_t idx) {
        if (st->stop.load() || done.load()) {
            skipped.fetch_add(1);   // 已有结果/已停止：这条没试过，不是试了失败
            return;
        }
        tried.fetch_add(1);
        std::string body;
        DWORD status = 0;
        const std::wstring req = Utf8ToWide(BuildParseRequest(ep, raw));
        const bool ok = HttpGetText(req, kBrowserUA, st->referer, per_req,
                                    2 * 1024 * 1024, &body, &status);
        const std::string tag =
            "接口[" + (ep.name.empty() ? std::to_string(idx + 1) : ep.name) + "]";
        if (ok && status >= 200 && status < 300) {
            // type1 先按 JSON 取 url；type2/HTML 再退化到全文扫描。
            std::string found = ExtractJsonMediaUrl(body);
            if (found.empty()) found = ExtractMediaUrl(body);
            if (!found.empty() && found != raw && IsValidResolvedUrl(found)) {
                std::lock_guard<std::mutex> lk(pmtx);
                if (!done.exchange(true)) {
                    pmedia = found;
                    pwho = tag;
                }
                return;
            }
            std::lock_guard<std::mutex> lk(pmtx);
            pdetail += tag + "未返回直链; ";
        } else {
            std::lock_guard<std::mutex> lk(pmtx);
            pdetail += tag + "请求失败(HTTP " + std::to_string(status) + "); ";
        }
    };

    std::vector<std::thread> workers;
    for (size_t i = 0; i < total; ++i) {
        if (st->stop.load() || done.load()) {
            skipped.fetch_add(static_cast<int>(total - i));   // 剩余接口没发起
            break;
        }
        workers.emplace_back(try_one, all[i], i);
        if (workers.size() >= 16) {
            workers.front().join();
            workers.erase(workers.begin());
        }
    }
    for (auto& w : workers)
        if (w.joinable()) w.join();

    if (!pmedia.empty()) {
        media = pmedia;
        detail = pwho;
    } else {
        detail = pdetail;
    }

    // ② 直接抓播放页 / 分享页 HTML 里的内嵌直链。
    if (media.empty() && !st->stop.load() && GetTickCount() <= deadline) {
        std::string page;
        DWORD status = 0;
        if (HttpGetText(st->origin, st->ua, st->referer, per_req,
                        1024 * 1024, &page, &status)) {
            {
                const std::string low = Lower(page);
                std::string preview = page.substr(0, 2048);
                ReplaceAll(&preview, "\r", " ");
                ReplaceAll(&preview, "\n", " ");
                page_info = "播放页 HTTP " + std::to_string(status) +
                            "，字节 " + std::to_string(page.size()) +
                            "，含 m3u8 字样=" +
                            (low.find("m3u8") != std::string::npos ? "是" : "否") +
                            "，含 mp4 字样=" +
                            (low.find("mp4") != std::string::npos ? "是" : "否") +
                            "\n页面前 2048 字符: " + preview + "\n";
            }
            media = ExtractMediaUrl(page);
            // 影视仓分享页常见根相对媒体地址（如 const url =
            // "/20261004/59072_xxx/index.m3u8?sign=…"）。ExtractMediaUrl 只认
            // http(s)，会漏掉这种相对路径，必须单独扫并按播放页 URL 补成绝对地址。
            if (media.empty()) {
                std::string rel = ExtractRelativeMediaUrl(page);
                if (!rel.empty()) media = JoinUrl(raw, rel);
            }
            // 播放页常见 iframe 播放器：再抓一层（最多 2 个）。
            if (media.empty()) {
                std::string low = Lower(page);
                size_t pos = 0;
                int taken = 0;
                while (taken < 2 &&
                       (pos = low.find("iframe", pos)) != std::string::npos) {
                    const size_t src = low.find("src", pos);
                    if (src == std::string::npos) break;
                    const size_t q1 = page.find('"', src);
                    const size_t q2 = q1 == std::string::npos
                                          ? std::string::npos
                                          : page.find('"', q1 + 1);
                    pos = src + 3;
                    if (q1 == std::string::npos || q2 == std::string::npos)
                        continue;
                    std::string frame = page.substr(q1 + 1, q2 - q1 - 1);
                    if (frame.rfind("//", 0) == 0) frame = "https:" + frame;
                    if (!StartsHttp(frame)) continue;
                    ++taken;
                    std::string inner;
                    if (GetTickCount() > deadline) break;
                    if (HttpGetText(Utf8ToWide(frame), st->ua, st->referer,
                                    per_req, 1024 * 1024, &inner,
                                    nullptr)) {
                        media = ExtractMediaUrl(inner);
                        if (media.empty()) {
                            std::string rel = ExtractRelativeMediaUrl(inner);
                            if (!rel.empty()) media = JoinUrl(frame, rel);
                        }
                        if (!media.empty()) break;
                    }
                }
            }
            if (media.empty())
                detail += "播放页(HTTP " + std::to_string(status) +
                          ")里没有内嵌直链";
        } else {
            detail += "播放页抓取失败(HTTP " + std::to_string(status) + ")";
        }
    }

    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (!media.empty()) {
            st->resolved = Utf8ToWide(media);
            // m3u8 → 交给 HLS 协议 DLL；其余（mp4/flv…）走本地转发代理。
            if (Lower(media).find(".m3u8") != std::string::npos &&
                !DelegateToHls(st, st->resolved)) {
                // 委托失败仍可试着让 MF 直连 m3u8（少数系统有 HLS 源）。
                st->final_media = st->resolved;
            }
            if (st->final_media.empty()) st->final_media = st->resolved;
            if (st->error.empty()) st->error = Utf8ToWide(detail);
            st->ready.store(true);
            // 成功也落一条：哪个接口赢、直链前 120 字符、耗时（同一日志文件，
            // AppendProtoFailLog 只是追加带时间戳的块，成功/失败共用）。
            const auto cost_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_start)
                    .count();
            std::string slog = "类型: WEB 协议解析成功\n";
            slog += "源地址: " + raw + "\n";
            slog += "成功来源: " +
                    (pmedia.empty() ? std::string("播放页/iframe 内嵌直链")
                                    : pwho) +
                    "\n";
            slog += "媒体地址: " + media.substr(0, 120) +
                    (media.size() > 120 ? "..." : "") + "\n";
            slog += "耗时: " + std::to_string(cost_ms) + " ms\n";
            slog += "接口统计: 共 " + std::to_string(total) + " 个，已尝试 " +
                    std::to_string(tried.load()) + " 个，跳过 " +
                    std::to_string(skipped.load()) + " 个\n";
            AppendProtoFailLog(slog);
        } else {
            st->failed.store(true);
            st->error = L"网页协议插件没能解析出媒体直链。\r\n" +
                        Utf8ToWide(detail) +
                        L"\r\n这是网页线路（如优酷/分享页），需要在「视频源」里"
                        L"导入带 parses 解析接口的影视仓配置，或点「切换源」换"
                        L"一条 m3u8/mp4 直链线路。";
            // 把「播不了的那条源」完整落盘：源地址、parses、接口尝试结果、
            // 播放页 HTTP 状态/长度/内容预览，之后不必反复点程序复现。
            std::string log = "类型: WEB 协议解析失败（页面/接口都没拿到直链）\n";
            log += "源地址: " + WideToUtf8(st->origin) + "\n";
            log += "请求头 UA: " + WideToUtf8(st->ua) + "\n";
            log += "请求头 Referer: " + WideToUtf8(st->referer) + "\n";
            log += "配置解析接口(parses): " +
                   std::to_string(st->parses.size()) + " 个\n";
            for (const auto& ep : st->parses)
                log += "  type=" + std::to_string(ep.type) + " name=" +
                       ep.name + " url=" + ep.url + "\n";
            log += "接口尝试: " + detail + "\n";
            log += "接口统计: 共 " + std::to_string(total) + " 个，已尝试 " +
                   std::to_string(tried.load()) + " 个，跳过 " +
                   std::to_string(skipped.load()) + " 个\n";
            log += page_info;
            log += "结论: " + WideToUtf8(st->error) + "\n";
            AppendProtoFailLog(log);
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
    bool via_hls = false;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        final_media = st->final_media;
        via_hls = st->via_hls;
    }
    if (via_hls) {
        // HLS 协议 DLL 已经把 m3u8 摊平成明文 TS 本地流，302 过去即可。
        std::string h = "HTTP/1.1 302 Found\r\nLocation: " +
                        WideToUtf8(final_media) +
                        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        SendAll(c, h.data(), static_cast<int>(h.size()));
    } else {
        StreamForward(c, final_media, st->ua, st->referer, st->timeout_ms,
                      range, st->stop);
    }
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
    // .mpd / .m3u / .txt 有各自的协议插件（DASH / M3U，probe 分数更高），
    // WEB 不认领它们，避免专用插件缺失时把清单文本当网页去跑解析爬虫。
    static const char* kExts[] = {".m3u8", ".mp4", ".flv", ".ts", ".mkv",
                                  ".m4v", ".mov", ".webm", ".avi", ".wmv",
                                  ".mp3", ".m4a", ".aac",
                                  ".mpd", ".m3u", ".txt", ".m4s"};
    for (const char* e : kExts)
        if (head.find(e) != std::string::npos) return true;
    return false;
}

int VpApiVersion() { return VP_PROTOCOL_API_VERSION; }

void VpInfo(VPProtocolInfo* out) {
    out->name = L"WEB";
    out->vendor = L"TTPlayer built-in (web page -> direct media / parses)";
    out->api_version = VP_PROTOCOL_API_VERSION;
}

int VpProbe(const wchar_t* url) {
    if (!url || !*url) return 0;
    const std::wstring u(url);
    const std::string low = Lower(WideToUtf8(u));
    if (low.rfind("http://", 0) != 0 && low.rfind("https://", 0) != 0)
        return 0;
    if (LooksLikeDirectMedia(u)) return 0;   // 直链交给其它协议 / MF
    return 60;                               // 网页播放地址
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
                      st->error.empty() ? L"网页协议会话已停止"
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
