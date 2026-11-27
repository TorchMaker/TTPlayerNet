// HLS 协议插件（编译为 video\protocols\video_protocol_hls.dll）
//
// 把 m3u8（点播/直播，含 AES-128 与 fMP4/CMAF）摊平成 Media Foundation
// 能直接播的本地字节流：DLL 内起 127.0.0.1 随机端口代理，向播放器输出
// http://127.0.0.1:<port>/stream.ts ——
//   · 点播（有 #EXT-X-ENDLIST）：并发探测每片长度 → 输出带精确
//     Content-Length 的顺序流，支持 Range 请求（进度条可拖）；
//   · 直播 / 探测失败：chunked 无限流（等同网络广播流）；
//   · 分片后台并发预取（首片就绪即开播，边下边播），AES-128 在代理层
//     解密（BCrypt + PKCS7 去填充），播放器拿到的永远是明文；
//   · fMP4/CMAF（#EXT-X-MAP）：init 段 + 各分片（moof/mdat）原样顺序
//     拼接；fMP4 内容请求 /stream.ts 时 302 到 /stream.mp4，让 MF 按
//     扩展名/重定向目标选对解封装器，TS 路径保持原样不变；
//   · 诊断日志：环境变量 TTPLAYER_HLS_LOG 有值时，解析/取 key/分片校验
//     失败等诊断追加到 <插件所在目录>\..\video\hls_fail.log。
//
// 线程模型：accept 线程（listener 关闭即退）、连接线程（每连接一个，
// detach + shared_ptr 保活）、初始化线程、预取调度线程、下载线程×4。
// VPClose：置 stop → 关 listener → notify_all → join 全部可 join 线程。

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
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
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

// ───────────────────────── 运行期诊断日志 ─────────────────────────
// 环境变量 TTPLAYER_HLS_LOG 有值时启用：把诊断追加写到
// 「插件所在目录\..\video\hls_fail.log」（与 protocol_web.cpp 的
// AppendProtoFailLog 同族算法：按模块路径取目录再拼），无值时零开销直返。

HMODULE g_this_module = nullptr;   // DllMain 记录，用于定位插件目录

std::wstring HlsFailLogPath() {
    wchar_t mod[MAX_PATH]{};
    if (!g_this_module ||
        !GetModuleFileNameW(g_this_module, mod, MAX_PATH))
        return {};
    const std::wstring p(mod);
    const size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return {};
    return p.substr(0, s) + L"\\..\\video\\hls_fail.log";
}

bool HlsLogEnabled() {
    static const bool on = [] {
        wchar_t buf[8]{};
        // 返回 >0（含值过长）即视为环境变量存在
        return GetEnvironmentVariableW(L"TTPLAYER_HLS_LOG", buf,
                                       static_cast<DWORD>(sizeof(buf) /
                                                          sizeof(buf[0]))) > 0;
    }();
    return on;
}

std::mutex g_hls_log_mtx;

// 落盘一行（带时间戳，追加）。任何等级都会调它；文件超过 256KB 时先滚一档
// 到 hls_fail.log.old，避免长时间运行无限增长。
void WriteHlsLogLine(const char* msg) {
    const std::wstring path = HlsFailLogPath();
    if (path.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fi{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fi) &&
        fi.nFileSizeHigh == 0 && fi.nFileSizeLow > 256 * 1024) {
        std::wstring old = path + L".old";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    char stamp[64]{};
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::lock_guard<std::mutex> lk(g_hls_log_mtx);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    std::fprintf(f, "%s %s", stamp, msg);
    std::fclose(f);
}

// 追加一行带时间戳的诊断（fmt 需自带换行）。仅在 TTPLAYER_HLS_LOG
// 有值时落盘；多线程各写各的整行，fopen-append 保证不交叉。
void DBG(const char* fmt, ...) {
    if (!HlsLogEnabled()) return;
    char msg[1024]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    WriteHlsLogLine(msg);
}

// 失败级日志：**不受开关限制、始终落盘**（用户要求「播不了的源要留 log 分析」）。
void ErrLog(const char* fmt, ...) {
    char msg[1024]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    WriteHlsLogLine(msg);
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

// 播放列表/分片地址里可能出现未编码的中文（如 .../hls/峡谷.m3u8）。WinHTTP
// 的请求行必须是 ASCII，直接把宽字符塞进去会失败（表现为「拉取 m3u8 播放列表
// 失败」）。这里按 UTF-8 把非 ASCII 字节转成 %XX，与浏览器行为一致。
std::wstring EscapeNonAsciiUrl(const std::wstring& url) {
    static const char* kHex = "0123456789ABCDEF";
    const std::string utf8 = WideToAscii(url);
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
    return AsciiToWide(out);
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
    if (!WinHttpCrackUrl(EscapeNonAsciiUrl(url).c_str(), 0, 0, &uc)) return false;
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    // TVBox 播放地址常带 `#集名` 片段（如 .../index.m3u8#第02集）。片段属于
    // 客户端语义，绝不能进请求行，否则服务端按字面路径找 → 404 播不了。
    if (const size_t hf = path.find(L'#'); hf != std::wstring::npos)
        path.resize(hf);

    HINTERNET ses = WinHttpOpen(
        // 真实 CDN（dytt/非凡/量子…）对非浏览器 UA 直接 403，默认必须是浏览器 UA。
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
                if (WinHttpCrackUrl(EscapeNonAsciiUrl(next).c_str(), 0, 0, &uc2)) {
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
    DBG("HTTPGET status=%d ok=%d bodysize=%zu urlend\n", (int)out->status, (int)out->ok, out->body.size());
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
    if (!WinHttpCrackUrl(EscapeNonAsciiUrl(url).c_str(), 0, 0, &uc)) return false;
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    // TVBox 播放地址常带 `#集名` 片段（如 .../index.m3u8#第02集）。片段属于
    // 客户端语义，绝不能进请求行，否则服务端按字面路径找 → 404 播不了。
    if (const size_t hf = path.find(L'#'); hf != std::wstring::npos)
        path.resize(hf);

    HINTERNET ses = WinHttpOpen(
        // 真实 CDN（dytt/非凡/量子…）对非浏览器 UA 直接 403，默认必须是浏览器 UA。
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

// AES IV 推导：显式 IV（EXT-X-KEY 的 IV=0x…，对其后所有分片生效）优先；
// 未给 IV 时按 RFC 8216 —— 该分片媒体序列号的 16 字节大端（低 8 字节）。
std::vector<std::uint8_t> DeriveIv(const std::vector<std::uint8_t>& explicit_iv,
                                   bool iv_explicit, int media_seq) {
    if (iv_explicit && explicit_iv.size() == 16) return explicit_iv;
    std::vector<std::uint8_t> iv(16, 0);
    const std::uint64_t s =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(media_seq));
    for (int b = 0; b < 8; ++b)
        iv[8 + b] = static_cast<std::uint8_t>((s >> (56 - b * 8)) & 0xFF);
    return iv;
}

// fMP4 特征：前 12 字节内 4 字节对齐位置出现 box 类型
// （ftyp/styp/moof/sidx/mdat/free/skip/prft/emsg/mfhd/moov）。
bool HasFmp4BoxSignature(const std::vector<std::uint8_t>& d) {
    static const char* kTypes[] = {"ftyp", "styp", "moof", "sidx", "mdat",
                                   "free", "skip", "prft", "emsg", "mfhd",
                                   "moov"};
    const size_t n = std::min<size_t>(d.size(), 12);
    for (size_t off = 0; off + 4 <= n; off += 4)
        for (const char* t : kTypes)
            if (std::memcmp(d.data() + off, t, 4) == 0) return true;
    return false;
}

// 部分站点给分片套一层 PNG/JPEG 外壳反盗链：文件是合法图片，真正的 TS 数据
// 追加在 IEND/EOI 之后（Content-Type 也是 image/png）。hls.js 不校验容器，
// 但我们的 0x47 校验会判失败 → 整片「响应体为空」。这里在解密后、校验前
// 找到 TS 同步起点并去掉外壳。
void StripImageWrapper(std::vector<std::uint8_t>* data) {
    if (!data || data->size() < 1024) return;
    const auto& d = *data;
    size_t from = 0;
    bool image = false;
    if (d.size() > 8 && d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E &&
        d[3] == 0x47) {   // PNG: 找 IEND 块（len4+type4+data+crc4）
        for (size_t i = 8; i + 12 <= d.size() && i < 65536; ++i) {
            if (d[i] == 'I' && d[i + 1] == 'E' && d[i + 2] == 'N' &&
                d[i + 3] == 'D') {
                from = i + 12;
                break;
            }
        }
        image = true;
    } else if (d[0] == 0xFF && d[1] == 0xD8) {   // JPEG: 找 EOI
        for (size_t i = 2; i + 2 <= d.size() && i < (1u << 20); ++i) {
            if (d[i] == 0xFF && d[i + 1] == 0xD9) {
                from = i + 2;
                break;
            }
        }
        image = true;
    }
    if (!image) return;
    // 在图片尾部 64KB 内找连续 3 个 188 字节对齐的 0x47。
    size_t ts = std::string::npos;
    const size_t limit = std::min(d.size(), from + 65536);
    for (size_t off = from; off + 188 * 3 < d.size() && off < limit; ++off) {
        if (d[off] != 0x47) continue;
        if (d[off + 188] == 0x47 && d[off + 376] == 0x47) {
            ts = off;
            break;
        }
    }
    if (ts == std::string::npos) return;
    data->erase(data->begin(), data->begin() + static_cast<std::ptrdiff_t>(ts));
    DBG("SEG 去掉图片外壳：offset=%zu 后按 TS 处理\n", ts);
}

// 分片合法性按容器分流：fMP4（有 #EXT-X-MAP）必须是 box 序列；
// 否则当 TS 处理 —— 首字节 0x47 同步；无 MAP 但内容是 fMP4 的放行并提示
// （缺 init 段大概率解不开，交由 MF 报错，日志里能看到）。
bool ValidateSegment(bool expect_fmp4, const std::vector<std::uint8_t>& d) {
    if (d.empty()) return false;
    if (d[0] == 0x47) return true;   // TS 同步字节优先，绝不误杀 TS
    const bool sig = HasFmp4BoxSignature(d);
    if (expect_fmp4) {
        if (!sig)
            ErrLog("SEG 校验失败：fMP4 流但首字节不是 box（head=%02X %02X %02X %02X %02X %02X）\n",
                d.size() > 0 ? d[0] : 0, d.size() > 1 ? d[1] : 0,
                d.size() > 2 ? d[2] : 0, d.size() > 3 ? d[3] : 0,
                d.size() > 4 ? d[4] : 0, d.size() > 5 ? d[5] : 0);
        return sig;
    }
    if (sig) {
        DBG("SEG 提示：无 #EXT-X-MAP 但内容是 fMP4（缺少 init 段，可能无法解码）\n");
        return true;
    }
    ErrLog("SEG 校验失败：非 0x47 同步且非 fMP4（head=%02X %02X %02X %02X）\n",
        d[0], d.size() > 1 ? d[1] : 0, d.size() > 2 ? d[2] : 0,
        d.size() > 3 ? d[3] : 0);
    return false;
}

// 解密（BCrypt AES-128-CBC）。密文长度非 16 倍数时只解 16 的整倍数部分；
// 成功后做 PKCS7 去填充（末字节 p∈[1,16] 且末尾 p 字节全为 p 才剥离，
// 填充非法则保留全部解密字节——宁可留尾部垃圾也不截断有效数据）。
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
            if (len != data->size())
                DBG("AES 输入长度 %zu 非 16 倍数，按 %u 字节解密（尾部丢弃）\n",
                    data->size(), static_cast<unsigned>(len));
            std::vector<std::uint8_t> ivb(iv, iv + 16);
            NTSTATUS st = BCryptDecrypt(hkey, data->data(), len, nullptr,
                                        ivb.data(), 16, data->data(), len,
                                        &len, 0);
            ok = st == 0;
            DBG("AES bcrypt st=0x%08X len=%u in_size=%zu\n",
                static_cast<unsigned>(st), static_cast<unsigned>(len),
                data->size());
            if (ok) {
                // PKCS7 去填充（HLS AES-128 分片末尾必带 1..16 字节填充）
                if (len > 0) {
                    const std::uint8_t p = (*data)[len - 1];
                    if (p >= 1 && p <= 16) {
                        bool valid = true;
                        for (std::uint8_t i = 1; i <= p; ++i)
                            if ((*data)[len - i] != p) { valid = false; break; }
                        if (valid) {
                            len -= p;
                        } else {
                            DBG("AES PKCS7 填充非法（尾字节=%u），保留全部解密字节\n",
                                static_cast<unsigned>(p));
                        }
                    } else {
                        DBG("AES PKCS7 填充非法（尾字节=%u），保留全部解密字节\n",
                            static_cast<unsigned>(p));
                    }
                }
                data->resize(len);
            } else {
                data->clear();   // 解密失败：丢弃密文，调用方按失败处理
            }
        } else {
            ok = true;
        }
    } else {
        DBG("AES 初始化失败（BCrypt provider/key）\n");
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
    // 该分片生效的 EXT-X-KEY 快照（解析时记录，支持直播刷新时的 key 轮换）
    std::string key_uri;           // 绝对 key 地址；空 = 明文
    std::vector<std::uint8_t> iv;  // 显式 IV（16 字节；空 = 按媒体序列号推导）
    bool iv_explicit = false;
};

// 明文分片缓存（进程内，跨会话共用 index 命名空间，VPOpen 时清空）。
std::mutex g_cache_mtx;
std::map<int, std::shared_ptr<std::vector<std::uint8_t>>> g_cache;
// 点播一集通常 400~600 个分片（6s/片 ≈ 40~60 分钟）：96 个的窗口播到
// 中段就把早期分片全部逐出，回看/续读时 state=2 却拿不到数据 → 30s 干等
// → 断流卡住（真机日志 SEG_WAIT 超时 state=2）。容量提到 384，并配合
// WaitSegReady 的 state 自愈（见下）双保险。
constexpr size_t kCacheMaxSegments = 384;

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
    // AES-128 key 缓存：key_uri(绝对) → 16 字节 key（受 mtx 保护）
    std::map<std::string, std::vector<std::uint8_t>> keys;
    std::atomic<bool> any_encrypted{false};    // 任一分片/init 受 AES 加密

    // fMP4/CMAF（#EXT-X-MAP）：init 段在 list_ready 前取好，输出时
    // init 段字节 + 各分片字节顺序拼接（ExoPlayer/CMAF 同款拼法）。
    std::atomic<bool> is_fmp4{false};
    std::string init_url;                      // 绝对地址（setup 线程写，之后只读）
    std::shared_ptr<std::vector<std::uint8_t>> init_data;  // 同上，list_ready 前就绪
    std::uint64_t init_size = 0;               // init 段字节数（计入总长前缀）

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

    // 按全局序号（而非向量位置）查找：MEDIA-SEQUENCE≠0 时位置与序号错位，
    // 缓存键与 served_index 都用全局序号，这里必须对齐。
    std::shared_ptr<Segment> FindSeg(int index) {
        std::lock_guard<std::mutex> lk(mtx);
        if (index < 0) return nullptr;
        for (auto& s : segs)
            if (s && s->index == index) return s;
        return nullptr;
    }

    void Fail(const wchar_t* msg) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (error.empty()) error = msg;
            list_failed = true;
            cv.notify_all();
        }
        ErrLog("FAIL %s\n",
               WideToAscii(msg).c_str());   // 解析/取 key/init 失败：始终落盘
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
            ErrLog("SEG_WAIT 下滑裁掉 index=%d\n", index);
            return nullptr;
        }
        if (seg->state.load() == 3 && seg->tries >= 3) {
            // 分片重试耗尽成永久空洞：这是「代理等不到数据」的一种，
            // 上层表现为 MF 饿死/卡住。必须留下证据（哪个分片、几次失败）。
            ErrLog("SEG_WAIT 分片失败耗尽 index=%d tries=%d url=%s\n", index,
                   seg->tries, seg->url.substr(0, 200).c_str());
            return nullptr;
        }
        // 自愈：state=2（已下载）但缓存里没有 = 数据已被逐出（或跨会话
        // CacheReset 清掉）。旧实现干等 30s 必超时 → 断流卡住（真机日志
        // SEG_WAIT 超时 state=2）。把状态打回「待下载」让 DownloadLoop 重取。
        if (seg->state.load() == 2) {
            ErrLog("SEG_WAIT 缓存逐出自愈 index=%d 重新下载\n", index);
            seg->state.store(0);
        }
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->cv.notify_all();   // 唤醒下载线程补活
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ErrLog("SEG_WAIT 超时(%dms) index=%d state=%d tries=%d\n",
                   timeout_ms, index, seg->state.load(), seg->tries);
            return nullptr;
        }
        std::unique_lock<std::mutex> lk(st->mtx);
        st->cv.wait_for(lk, std::chrono::milliseconds(120));
    }
}

// ───────────────────────── 播放列表抓取与解析 ─────────────────────────

bool FetchPlaylist(const std::wstring& url, const HlsState* st,
                   std::string* out) {
    HttpResult r;
    const bool got = HttpGet(url, st->user_agent, st->referer,
                             st->timeout_ms, &r);
    // 解析失败的源必须能复盘：记下确实是哪一跳、什么状态码/字节数。
    if (!got || r.body.empty()) {
        ErrLog("PLAYLIST 抓取失败 url=%s got=%d status=%u bytes=%zu\n",
               WideToAscii(url).substr(0, 200).c_str(),
               got ? 1 : 0, static_cast<unsigned>(r.status),
               r.body.size());
        return false;
    }
    if (r.body.size() > 4 * 1024 * 1024) {
        ErrLog("PLAYLIST 过大 bytes=%zu url=%s\n", r.body.size(),
               WideToAscii(url).substr(0, 200).c_str());
        return false;
    }
    out->assign(r.body.begin(), r.body.end());
    if (out->find("#EXTM3U") == std::string::npos) {
        ErrLog("PLAYLIST 非 m3u8 status=%u bytes=%zu head=%.120s\n",
               static_cast<unsigned>(r.status), r.body.size(),
               r.body.data());
        return false;
    }
    return true;
}

// 按需获取并缓存 AES-128 key。key 文件常见尾部换行（先裁空白）；也有源
// 直接给 32 位十六进制文本 key（解成 16 字节）。false = 获取/格式失败。
bool EnsureKey(const std::shared_ptr<HlsState>& st,
               const std::string& key_uri) {
    if (key_uri.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (st->keys.count(key_uri)) return true;
    }
    HttpResult r;
    if (!HttpGet(AsciiToWide(key_uri), st->user_agent, st->referer,
                 st->timeout_ms, &r)) {
        ErrLog("KEY 获取失败 url=%s status=%u\n", key_uri.c_str(),
            static_cast<unsigned>(r.status));
        return false;
    }
    auto is_ws = [](std::uint8_t c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
               c == '\f' || c == '\v';
    };
    size_t b = 0, e = r.body.size();
    while (b < e && is_ws(r.body[b])) ++b;
    while (e > b && is_ws(r.body[e - 1])) --e;
    if (b || e != r.body.size()) r.body.erase(r.body.begin(),
                                              r.body.begin() + b);
    if (e - b != r.body.size()) r.body.resize(e - b);
    std::vector<std::uint8_t> key;
    if (r.body.size() == 16) {
        key = std::move(r.body);
    } else if (r.body.size() == 32) {
        auto hex = [](std::uint8_t c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        key.reserve(16);
        bool good = true;
        for (size_t i = 0; good && i < 32; i += 2) {
            const int hi = hex(r.body[i]), lo = hex(r.body[i + 1]);
            if (hi < 0 || lo < 0) good = false;
            else key.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        }
        if (!good) key.clear();
    }
    if (key.size() != 16) {
        DBG("KEY 长度/格式非法 size=%zu url=%s\n", r.body.size(),
            key_uri.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->keys[key_uri] = key;
    }
    DBG("KEY ok url=%s\n", key_uri.c_str());
    return true;
}

// 解析出的单个分片，携带其生效的加密状态快照。
struct SegItem {
    int index = 0;
    std::string url;
    double duration = 0.0;
    std::string key_uri;              // 绝对 URI；空 = 明文
    std::vector<std::uint8_t> iv;     // 显式 16 字节 IV；空 = 按序号推导
    bool iv_explicit = false;
};

// #EXT-X-MAP（fMP4/CMAF 的 init 段）；key_* 为 MAP 标签处生效的加密状态。
struct MapInfo {
    bool present = false;
    std::string uri;                  // 绝对 URI
    std::string key_uri;
    std::vector<std::uint8_t> iv;
    bool iv_explicit = false;
};

// 取 EXT-X-* 行里的属性值：支持 URI="…" 引号形式与裸值形式。
std::string TagAttr(const std::string& ln, const std::string& name) {
    const size_t p = ln.find(name + "=");
    if (p == std::string::npos) return {};
    size_t v = p + name.size() + 1;
    if (v < ln.size() && ln[v] == '"') {
        const size_t e = ln.find('"', v + 1);
        return e == std::string::npos ? ln.substr(v + 1)
                                      : ln.substr(v + 1, e - v - 1);
    }
    const size_t e = ln.find(',', v);
    return e == std::string::npos ? ln.substr(v) : ln.substr(v, e - v);
}

// 解析媒体播放列表 → 分片列表（含每片的 key/IV 快照）+ #EXT-X-MAP 信息。
// EXT-X-KEY 随列表滚动更新（支持直播轮换与 METHOD=NONE 解除）。
bool ParseMediaPlaylist(const std::string& text, const std::string& base_url,
                        std::uint64_t* media_seq, bool* is_live,
                        MapInfo* map, std::vector<SegItem>* out) {
    *is_live = text.find("#EXT-X-ENDLIST") == std::string::npos;
    *map = MapInfo{};
    {
        const std::string tag = "#EXT-X-MEDIA-SEQUENCE:";
        const size_t p = text.find(tag);
        *media_seq = p == std::string::npos
                         ? 0
                         : std::strtoull(text.c_str() + p + tag.size(), nullptr, 10);
    }
    // 当前生效的 EXT-X-KEY 状态（其后的分片继承；METHOD=NONE 清除）
    std::string key_uri;
    std::vector<std::uint8_t> key_iv;
    bool key_iv_explicit = false;
    double pending = 0.0;
    std::istringstream ss(text);
    std::string ln;
    while (std::getline(ss, ln)) {
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == '\n'))
            ln.pop_back();
        if (ln.empty()) continue;
        if (ln.rfind("#EXTINF:", 0) == 0) {
            const size_t comma = ln.find(',');
            const std::string num = ln.substr(
                8, comma == std::string::npos ? std::string::npos : comma - 8);
            pending = std::strtod(num.c_str(), nullptr);
            continue;
        }
        if (ln.rfind("#EXT-X-KEY:", 0) == 0) {
            const std::string method = TagAttr(ln, "METHOD");
            if (method.empty()) continue;   // 无 METHOD：按旧行为忽略本条
            if (method == "AES-128") {
                const std::string uri = TagAttr(ln, "URI");
                if (uri.empty()) {
                    DBG("EXT-X-KEY 缺少 URI，忽略本条：%s\n", ln.c_str());
                } else {
                    key_uri = JoinUrl(base_url, uri);   // 相对/绝对均在此补全
                }
                key_iv.clear();
                key_iv_explicit = false;
                const std::string iv = TagAttr(ln, "IV");
                if (iv.size() >= 34 && iv[0] == '0' &&
                    (iv[1] == 'x' || iv[1] == 'X')) {
                    key_iv.resize(16);
                    for (int i = 0; i < 16; ++i)
                        key_iv[i] = static_cast<std::uint8_t>(
                            std::strtoul(iv.substr(2 + i * 2, 2).c_str(),
                                         nullptr, 16));
                    key_iv_explicit = true;
                }
            } else if (method == "NONE") {
                key_uri.clear();
                key_iv.clear();
                key_iv_explicit = false;
            } else {
                // SAMPLE-AES 等不支持的方法：按明文走，分片校验会拦下并记日志
                DBG("不支持的加密方法 METHOD=%s，按明文处理\n", method.c_str());
                key_uri.clear();
                key_iv.clear();
                key_iv_explicit = false;
            }
            continue;
        }
        if (ln.rfind("#EXT-X-MAP:", 0) == 0) {
            const std::string uri = TagAttr(ln, "URI");
            if (uri.empty()) {
                DBG("EXT-X-MAP 缺少 URI：%s\n", ln.c_str());
                continue;
            }
            if (map->present) {
                // 直播中 init 切换（不支持）：仍用首个，日志留痕
                DBG("EXT-X-MAP 出现多次，仅用首个 init 段（新=%s 旧=%s）\n",
                    uri.c_str(), map->uri.c_str());
                continue;
            }
            if (ln.find("BYTERANGE=") != std::string::npos)
                DBG("EXT-X-MAP 带 BYTERANGE，按整个 init 文件下载：%s\n",
                    ln.c_str());
            map->present = true;
            map->uri = JoinUrl(base_url, uri);
            map->key_uri = key_uri;        // MAP 处生效的加密状态快照
            map->iv = key_iv;
            map->iv_explicit = key_iv_explicit;
            continue;
        }
        if (ln[0] == '#') continue;
        SegItem it;
        it.index = static_cast<int>(*media_seq + out->size());
        it.url = JoinUrl(base_url, ln);
        it.duration = pending;
        it.key_uri = key_uri;             // 每片继承当前 key/IV 快照
        it.iv = key_iv;
        it.iv_explicit = key_iv_explicit;
        out->push_back(std::move(it));
        pending = 0.0;
    }
    return !out->empty();
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
        DBG("DL pre seg=%d httpgot=%d bodysize=%zu key=%s url=%s\n",
            seg->index, (int)ok, data.size(),
            seg->key_uri.empty() ? "-" : seg->key_uri.c_str(),
            seg->url.c_str());
        if (ok && !seg->key_uri.empty()) {
            // AES-128：按分片解析时的 key/IV 快照解密（支持直播 key 轮换；
            // 显式 IV 优先，否则 = 该片媒体序列号 16 字节大端）
            if (!EnsureKey(st, seg->key_uri)) {
                ok = false;
            } else {
                std::vector<std::uint8_t> key;
                {
                    std::lock_guard<std::mutex> lk(st->mtx);
                    auto it = st->keys.find(seg->key_uri);
                    if (it != st->keys.end()) key = it->second;
                }
                if (key.size() != 16) {
                    ok = false;
                } else {
                    const std::vector<std::uint8_t> iv =
                        DeriveIv(seg->iv, seg->iv_explicit, seg->index);
                    ok = Aes128CbcDecrypt(key.data(), iv.data(), &data);
                }
            }
            DBG("DL seg=%d decrypt_ok=%d out=%zu ivx=%d\n",
                seg->index, (int)ok, data.size(), (int)seg->iv_explicit);
        }
        // 容器分流校验（解密之后）：fMP4 看 box 特征，TS 看 0x47 同步
        if (ok) StripImageWrapper(&data);
        if (ok && !ValidateSegment(st->is_fmp4.load(), data)) ok = false;
        if (ok && !data.empty()) {
            // 记录解密去填充后的真实长度，供本地代理按字节范围拼接。
            // 否则段边界字节错位 → TS 失同步（解码错误 / 周期性闪烁）。
            seg->size = data.size();
            CachePut(seg->index,
                     std::make_shared<std::vector<std::uint8_t>>(std::move(data)));
            seg->state.store(2);
            {  // 维护点播总大小（去填充后；fMP4 的 init 段计入首片之前）
                std::lock_guard<std::mutex> lk(st->mtx);
                std::uint64_t t = st->init_size;
                for (auto& s : st->segs)
                    if (s) t += s->size;
                st->total_size.store(t);
            }
        } else {
            DBG("DL seg=%d 下载/解密/校验失败（tries=%d）\n",
                seg->index, seg->tries);
            seg->state.store(3);
        }
        st->cv.notify_all();
    }
}

void ScheduleLoop(std::shared_ptr<HlsState> st) {
    DWORD last_refresh = 0;
    while (!st->stop.load()) {
        if (st->live && st->list_ready.load() &&
            GetTickCount() - last_refresh > 6000) {
            last_refresh = GetTickCount();
            std::string text;
            if (FetchPlaylist(st->media_url, st.get(), &text)) {
                std::vector<SegItem> items;
                std::uint64_t seq = 0;
                bool live = false;
                MapInfo map;
                if (ParseMediaPlaylist(text, WideToAscii(st->media_url), &seq,
                                       &live, &map, &items)) {
                    {
                        std::lock_guard<std::mutex> lk(st->mtx);
                        for (auto& it : items) {
                            bool exists = false;
                            for (auto& s : st->segs)
                                if (s && s->index == it.index) {
                                    exists = true;
                                    break;
                                }
                            if (exists) continue;
                            // 按序追加：绝不按序号 resize 开洞（MEDIA-SEQUENCE≠0
                            // 时按位置开洞会产生空指针并错位缓存键）
                            auto s = std::make_shared<Segment>();
                            s->index = it.index;
                            s->url = it.url;
                            s->duration = it.duration;
                            s->key_uri = it.key_uri;      // 新片带自己的 key 快照
                            s->iv = it.iv;
                            s->iv_explicit = it.iv_explicit;
                            st->segs.push_back(std::move(s));
                        }
                        st->cv.notify_all();
                    }
                    if (map.present && st->is_fmp4.load() &&
                        map.uri != st->init_url)
                        DBG("直播刷新：EXT-X-MAP 变化，仍用首个 init（%s）\n",
                            st->init_url.c_str());
                }
            }
        }
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
                DBG("PROBE fail url=%s（回退 chunked 顺序流）\n",
                    seg->url.c_str());
                any_fail.store(true);
                return;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < 6; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (!any_fail.load() && !st->stop.load()) {
        // fMP4 的 init 段在 setup 已下载（init_size 就绪），计入首片之前
        std::uint64_t total = st->init_size;
        std::lock_guard<std::mutex> lk(st->mtx);
        for (auto& s : st->segs)
            if (s) total += s->size;
        st->total_size.store(total);
        st->sizes_ok.store(true);
    }
    st->sizes_probed.store(true);
    st->cv.notify_all();
}

// 初始化线程：master → 媒体列表 → key → 启动下载/调度/探测
void SetupThread(std::shared_ptr<HlsState> st) {
    std::string text;
    std::wstring cur = st->origin;
    bool ok = false;
    for (int hop = 0; hop < 2; ++hop) {
        if (!FetchPlaylist(cur, st.get(), &text)) break;
        if (text.find("#EXT-X-STREAM-INF") == std::string::npos) {
            st->media_url = cur;
            ok = true;
            break;
        }
        // master：取第一个子列表
        std::string child;
        bool take_next = false;
        std::istringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln)) {
            while (!ln.empty() && (ln.back() == '\r' || ln.back() == '\n'))
                ln.pop_back();
            if (ln.empty()) continue;
            if (take_next) { child = ln; break; }
            if (ln.rfind("#EXT-X-STREAM-INF", 0) == 0) take_next = true;
        }
        if (child.empty()) break;
        cur = AsciiToWide(JoinUrl(WideToAscii(cur), child));
    }
    if (!ok || st->stop.load()) {
        st->Fail(L"拉取 m3u8 播放列表失败（网络或地址无效）");
        return;
    }
    if (text.find("#EXT-X-STREAM-INF") != std::string::npos) {
        // 两层后仍是 master（罕见）→ 当作失败
        st->Fail(L"m3u8 主列表没有子列表");
        return;
    }

    std::vector<SegItem> items;
    std::uint64_t seq = 0;
    bool live = false;
    MapInfo map;
    if (!ParseMediaPlaylist(text, WideToAscii(st->media_url), &seq, &live,
                            &map, &items)) {
        st->Fail(L"m3u8 里没有可用的分段");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->segs.resize(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            auto s = std::make_shared<Segment>();
            s->index = items[i].index;
            s->url = items[i].url;
            s->duration = items[i].duration;
            s->key_uri = items[i].key_uri;
            s->iv = items[i].iv;
            s->iv_explicit = items[i].iv_explicit;
            st->segs[i] = s;
        }
        st->live.store(live);
        st->cv.notify_all();
    }

    // ---- fMP4/CMAF：#EXT-X-MAP 的 init 段（list_ready 前取好）----
    // 输出 = init 字节 + 各分片字节顺序拼接（moof/mdat 本身可拼接）。
    if (map.present) {
        st->is_fmp4.store(true);
        st->init_url = map.uri;
        HttpResult r;
        if (!HttpGet(AsciiToWide(map.uri), st->user_agent, st->referer,
                     st->timeout_ms, &r) || r.body.empty()) {
            st->Fail(L"init 段（#EXT-X-MAP）下载失败");
            return;
        }
        std::vector<std::uint8_t> data = std::move(r.body);
        if (!map.key_uri.empty()) {
            // init 段可能被 AES 加密（IV 按 RFC 应显式给出，缺失时退化用
            // 首个媒体分片的序号）；解完不是 fMP4 再试按明文保留——部分源
            // 只加密分片、init 本身明文。
            if (!EnsureKey(st, map.key_uri)) {
                st->Fail(L"解密 key 获取失败（HLS AES-128）");
                return;
            }
            std::vector<std::uint8_t> key;
            {
                std::lock_guard<std::mutex> lk(st->mtx);
                auto it = st->keys.find(map.key_uri);
                if (it != st->keys.end()) key = it->second;
            }
            const int first_seq = items.empty() ? 0 : items[0].index;
            const std::vector<std::uint8_t> iv =
                DeriveIv(map.iv, map.iv_explicit, first_seq);
            std::vector<std::uint8_t> plain = data;
            if (key.size() == 16 &&
                Aes128CbcDecrypt(key.data(), iv.data(), &plain) &&
                HasFmp4BoxSignature(plain)) {
                data = std::move(plain);
            } else if (HasFmp4BoxSignature(data)) {
                DBG("SETUP init 段实为明文（EXT-X-KEY 不适用），按明文使用\n");
            } else {
                st->Fail(L"init 段解密失败（HLS AES-128）");
                return;
            }
        }
        if (!HasFmp4BoxSignature(data)) {
            st->Fail(L"init 段（#EXT-X-MAP）不是 fMP4 格式");
            return;
        }
        st->init_data =
            std::make_shared<std::vector<std::uint8_t>>(std::move(data));
        st->init_size = st->init_data->size();
        DBG("SETUP init url=%s size=%llu\n", map.uri.c_str(),
            static_cast<unsigned long long>(st->init_size));
    }

    // ---- AES-128 key：每个不同 URI 取一次（分片与 init 共用缓存）----
    bool any_enc = !map.key_uri.empty();
    std::vector<std::string> key_uris;
    for (const auto& it : items) {
        if (it.key_uri.empty()) continue;
        any_enc = true;
        if (std::find(key_uris.begin(), key_uris.end(), it.key_uri) ==
            key_uris.end())
            key_uris.push_back(it.key_uri);
    }
    for (const auto& k : key_uris) {
        if (!EnsureKey(st, k)) {
            st->Fail(L"解密 key 获取失败（HLS AES-128）");
            return;
        }
    }
    st->any_encrypted.store(any_enc);
    DBG("SETUP segs=%zu media_seq=%llu live=%d fmp4=%d keys=%zu enc=%d\n",
        items.size(), static_cast<unsigned long long>(seq), (int)live,
        (int)map.present, key_uris.size(), (int)any_enc);

    st->list_ready.store(true);
    st->cv.notify_all();

    for (int i = 0; i < 4; ++i) st->workers.emplace_back(DownloadLoop, st);
    st->sched_th = std::thread(ScheduleLoop, st);
    // AES-128 内容解密后每段比原分片短 1..16 字节（去 PKCS7 填充）。若仍按加密
    // 分片大小做 Range 探测，ServeRange 的字节映射会与「实际外发的解密大小」错位、
    // 总长对不上 → 解码错误或死循环发空字节。AES 内容不走 Range 探测，改用
    // ServeChunked 顺序流式（按真实解密字节外发），从根本上消除大小不一致。
    if (!live && !any_enc) ProbeSizes(st);
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

// 输出流的 Content-Type（MF 嗅探/选解封装器的依据）：fMP4 → mp4，否则 TS。
const char* StreamContentType(const HlsState& st) {
    return st.is_fmp4.load() ? "video/mp4" : "video/mp2t";
}

// 点播 range 模式：把 [a, b]（含端点）按字节边界映射写出。
// fMP4：init 段占据 [0, init_size)，其后才是各分片的前缀和。
bool ServeRange(std::shared_ptr<HlsState> st, SOCKET c, std::uint64_t a,
                std::uint64_t b) {
    const std::uint64_t total = st->total_size.load();
    if (total == 0 || a >= total) return false;
    if (b == UINT64_MAX || b >= total) b = total - 1;
    char head[320];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 206 Partial Content\r\n"
                  "Content-Type: %s\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Content-Range: bytes %llu-%llu/%llu\r\n"
                  "Content-Length: %llu\r\n"
                  "Connection: close\r\n\r\n",
                  StreamContentType(*st),
                  static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b),
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(b - a + 1));
    if (!SendHead(c, head)) return false;

    // init 段在 list_ready 前已取好，这里只读快照
    const auto init = st->is_fmp4.load() ? st->init_data : nullptr;
    const std::uint64_t init_sz = init ? init->size() : 0;
    std::uint64_t pos = a;
    while (pos <= b && !st->stop.load()) {
        if (pos < init_sz) {   // 先发 init 段字节
            const std::uint64_t take =
                std::min<std::uint64_t>(b - pos + 1, init_sz - pos);
            if (!SendAll(c, reinterpret_cast<const char*>(init->data()) + pos,
                         static_cast<int>(take)))
                return false;
            pos += take;
            continue;
        }
        // 定位分片（前缀和，从 init 段之后起算）
        std::uint64_t prefix = init_sz;
        int seg_index = -1;
        std::shared_ptr<Segment> seg;
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            for (auto& s : st->segs) {
                if (!s) continue;
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

// 直播/未知大小：chunked 顺序流（fMP4 先发 init 段，再按序发分片）。
// start_sec > 0：按 #EXTINF 累加定位到目标时间所在分片，从该分片起输出。
// 这是「拖动进度条」在无 Range / AES-128 流上唯一可行的实现：MF 对
// chunked 流 SetCurrentTime 完全无效，只能让宿主用带起点的 URL 重开会话。
bool ServeChunked(std::shared_ptr<HlsState> st, SOCKET c, double start_sec) {
    char head[192];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: %s\r\n"
                  "Transfer-Encoding: chunked\r\n"
                  "Connection: close\r\n\r\n",
                  StreamContentType(*st));
    if (!SendHead(c, head)) return false;
    if (st->is_fmp4.load()) {
        const auto init = st->init_data;
        if (!init || init->empty()) return false;
        char sz[32];
        std::snprintf(sz, sizeof(sz), "%zx\r\n", init->size());
        if (!SendHead(c, sz) ||
            !SendAll(c, reinterpret_cast<const char*>(init->data()),
                     static_cast<int>(init->size())) ||
            !SendHead(c, "\r\n"))
            return false;
    }
    // 起始序号 = 首片全局序号（MEDIA-SEQUENCE 可能不为 0）；
    // start_sec > 0 时按 #EXTINF 累加跳过前面的分片。
    int index = 0;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (!st->segs.empty() && st->segs.front()) index = st->segs.front()->index;
        if (start_sec > 0.0) {
            double acc = 0.0;
            for (const auto& s : st->segs) {
                if (!s) continue;
                if (acc + s->duration > start_sec) {
                    index = s->index;
                    break;
                }
                acc += s->duration;
            }
        }
    }
    // 只在「按起点跳转」的连接上把 DownloadLoop 的下载前沿推到目标分片。
    // ★ 不能每个连接都 store：MF 之外还会有其它 GET（旧的 RANGE probe、
    //   播放器重连、外部探测），它们 start_sec=0 会把 played frontier 重置回
    //   首片，让正在播放的连接等不到后续分片 → 缓冲耗尽 → 解码/网络错误。
    if (start_sec > 0.0) st->served_index.store(index);
    DBG("SERVE chunked start=%.2f first_seg=%d\n", start_sec, index);
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
    char method[16] = {}, raw_path[512] = {};
    bool ok = false;
    if (sscanf(req, "%15s %511s", method, raw_path) == 2 &&
        _stricmp(method, "GET") == 0) {
        const char* range = strstr(req, "Range:");
        if (char* q = strchr(raw_path, '?')) *q = '\0';
        const std::string path(raw_path);
        const size_t n = path.size();
        // 允许带起点前缀：/s/<秒>/stream.ts（仍以 .ts 结尾，MF 才能按扩展名
        // 选对解封装器；用 ?start= 会让 MF 嗅探不到 .ts）。
        const bool req_ts = n >= 10 && path.compare(n - 10, 10, "/stream.ts") == 0;
        const bool req_mp4 = n >= 11 && path.compare(n - 11, 11, "/stream.mp4") == 0;
        double start_sec = 0.0;
        std::string start_prefix;
        if (n > 3 && path.compare(0, 3, "/s/") == 0) {
            const size_t slash = path.find('/', 3);
            if (slash != std::string::npos && slash > 3) {
                start_sec = atof(path.substr(3, slash - 3).c_str());
                if (start_sec > 0.0) start_prefix = path.substr(0, slash);
                else start_sec = 0.0;
            }
        }
        if (req_ts || req_mp4) {
            if (!WaitForList(st)) {
                SendHead(c, "HTTP/1.1 503 Service Unavailable\r\n"
                            "Connection: close\r\n\r\n");
            } else if (req_ts && st->is_fmp4.load()) {
                // fMP4 内容改走 .mp4 路径：MF 按 URL 扩展名/重定向后的
                // 地址选解封装器，fMP4 字节若停在 .ts 上会被交给 TS 解析。
                // 重定向必须保留起点前缀，否则跳转信息在第二次请求里丢失。
                char loc[256];
                std::snprintf(loc, sizeof(loc),
                              "HTTP/1.1 302 Found\r\n"
                              "Location: http://127.0.0.1:%d%s/stream.mp4\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n",
                              st->port, start_prefix.c_str());
                ok = SendHead(c, loc);
                DBG("REDIRECT /stream.ts -> %s/stream.mp4 (fMP4 init=%llu)\n",
                    start_prefix.c_str(),
                    static_cast<unsigned long long>(st->init_size));
            } else if (start_sec > 0.0) {
                // 拖动进度条：从目标时间所在分片开始输出（含 fMP4 init 段）。
                // 带起点时一律 chunked：MF 对无 Content-Length 的流无法
                // SetCurrentTime，但重开会话后画面本身就从目标处开始。
                ok = ServeChunked(std::move(st), c, start_sec);
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
                ok = ServeChunked(std::move(st), c, 0.0);
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

std::wstring HlsProbe(const std::wstring& url) {
    std::wstring low = url;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    const size_t q = low.find(L'?');
    if (q != std::wstring::npos) low = low.substr(0, q);
    if (low.size() >= 5 && low.rfind(L".m3u8") == low.size() - 5) return url;
    if (low.find(L"m3u8") != std::wstring::npos) return url;
    return {};
}

int g_ws_ref = 0;

} // namespace

// ───────────────────────── 导出接口 ─────────────────────────
// 与音频插件 ttpGetSoundAddIn 同构的工厂模式：唯一导出 ttpGetVideoProtocol。

namespace {

int VpApiVersion() { return VP_PROTOCOL_API_VERSION; }

void VpInfo(VPProtocolInfo* out) {
    out->name = L"HLS";
    out->vendor = L"TTPlayer built-in (m3u8 -> local stream proxy)";
    out->api_version = VP_PROTOCOL_API_VERSION;
}

int VpProbe(const wchar_t* url) {
    if (!url) return 0;
    std::wstring low = url;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    const size_t q = low.find(L'?');
    if (q != std::wstring::npos) low = low.substr(0, q);
    if (low.size() >= 5 && low.rfind(L".m3u8") == low.size() - 5) return 100;
    if (low.find(L"m3u8") != std::wstring::npos) return 90;
    return 0;
}

VPSession* VpOpen(const wchar_t* url, const VPHttpConfig* cfg,
                  wchar_t* out_media_url, int cch_media_url) {
    if (!url || !out_media_url || cch_media_url <= 0) return nullptr;
    if (HlsProbe(url).empty()) return nullptr;

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
             L"http://127.0.0.1:%d/stream.ts", st->port);

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
    // 首片就绪（或长度探测完成）即认为可开播。首片取位置 0 —— 全局序号
    // （MEDIA-SEQUENCE）未必从 0 开始，不能拿 FindSeg(0) 判断。
    std::shared_ptr<Segment> first;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (!st->segs.empty()) first = st->segs.front();
    }
    if ((first && first->state.load() == 2) || st->sizes_probed.load())
        return VP_POLL_READY;
    return VP_POLL_CONNECTING;
}

// 播放前沿起连续就绪的秒数（state=2 且缓存命中；缓存逐出即断点）。
double AheadReadySeconds(const std::shared_ptr<HlsState>& st, double cap) {
    const int served = st->served_index.load();
    double ready_sec = 0.0;
    std::lock_guard<std::mutex> lk(st->mtx);
    for (const auto& seg : st->segs) {
        if (!seg || seg->index < served) continue;
        if (seg->state.load() != 2) break;          // 前沿断点：后续未下
        if (!CacheGet(seg->index)) break;           // 已逐出（自愈重下中）
        ready_sec += seg->duration > 0.0 ? seg->duration : 6.0;
        if (ready_sec >= cap) break;
    }
    return ready_sec;
}

// 缓冲进度 0-100：播放前沿（served_index）起**连续就绪**的分片时长占
// 15 秒目标缓冲的比例。宿主用它把「正在缓冲」提示升级为「正在缓冲 n%」。
// 未就绪 / 列表未就绪返回 0（正在缓冲且前方无数据），会话不可用返回 -1。
int VpPollBuffering(VPSession* s) {
    if (!s) return -1;
    auto st = *reinterpret_cast<std::shared_ptr<HlsState>*>(s);
    if (st->stop.load() || st->list_failed || !st->list_ready.load())
        return -1;
    constexpr double kTargetSeconds = 15.0;
    const double ready_sec = AheadReadySeconds(st, kTargetSeconds);
    if (ready_sec <= 0.0) return 0;
    const double ratio = ready_sec / kTargetSeconds * 100.0;
    return ratio >= 100.0 ? 100 : static_cast<int>(ratio);
}

// 前方连续就绪秒数（cap 300）：进度条浅色缓冲段用——15s 百分比目标在
// 长片上只有几像素，看不出预加载进度（用户反馈）。
int VpPollBuffered(VPSession* s) {
    if (!s) return -1;
    auto st = *reinterpret_cast<std::shared_ptr<HlsState>*>(s);
    if (st->stop.load() || st->list_failed || !st->list_ready.load())
        return -1;
    const double ready_sec = AheadReadySeconds(st, 300.0);
    return static_cast<int>(ready_sec);
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
    out->poll_buffering = VpPollBuffering;
    out->poll_buffered = VpPollBuffered;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_this_module = h;   // 供日志定位插件目录
    return TRUE;
}
