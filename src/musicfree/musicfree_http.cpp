// MusicFree 兼容层 —— axios 后端（WinHTTP）与 cheerio 垫片
//
// 复刻 MusicFree plugin.ts 中 axios 的用法：plugins 调用 axios.get/post，
// 读取 response.data / response.headers / response.status。

#include "ttplayer/musicfree/musicfree_http.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#include <quickjs.h>
#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/musicfree/quickjs_engine.h"

namespace ttplayer::musicfree {

namespace {

// ---------------------------------------------------------------------------
// WinHTTP 阻塞请求
// ---------------------------------------------------------------------------

struct UrlParts {
    std::wstring scheme, host, port, path;
};

UrlParts ParseUrl(const std::wstring& url) {
    UrlParts p;
    URL_COMPONENTS uc{sizeof(uc)};
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUserNameLength =
        uc.dwPasswordLength = uc.dwUrlPathLength = uc.dwExtraInfoLength =
            static_cast<DWORD>(-1);
    // 先探测 scheme
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return p;
    auto get = [&](DWORD len, LPCWSTR ptr) -> std::wstring {
        return len ? std::wstring(ptr, len) : std::wstring{};
    };
    p.scheme = get(uc.dwSchemeLength, uc.lpszScheme);
    p.host = get(uc.dwHostNameLength, uc.lpszHostName);
    if (uc.nPort) p.port = std::to_wstring(uc.nPort);
    p.path = get(uc.dwUrlPathLength, uc.lpszUrlPath) + get(uc.dwExtraInfoLength, uc.lpszExtraInfo);
    if (p.path.empty()) p.path = L"/";
    return p;
}

std::wstring ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct HttpResponse {
    DWORD status = 200;
    std::string status_text = "OK";
    std::map<std::string, std::vector<std::string>> headers; // 同名头保留多个
    std::vector<std::uint8_t> body;
};

// ---------------------------------------------------------------------------
// URL 百分号编码
//
// 两个必须编码的地方，缺一不可：
//  1) BuildQuery 把 config.params 拼进 query string 时，键值要按
//     encodeURIComponent 规则编码（axios 就是这么做的）。
//  2) WinHttpCrackUrl(url, 0, 0, ...) 会把 URL 中的非 ASCII 字符按 ANSI
//     代码页降级：中文会直接变成 '?'。于是「告白气球」在真正的请求行里成了
//     "????"，服务端收到 s=???? 自然搜不到东西，返回
//     {"result":{"songCount":0},"code":200}，音源据此抛出「网易云搜索失败」。
//     所以在交给 WinHTTP 之前，必须把 URL 里剩下的非 ASCII 字节先转成 %XX。
// ---------------------------------------------------------------------------

// encodeURIComponent：非 A-Za-z0-9-_.~ 一律编码。空格编码为 %20。
std::wstring PercentEncodeComponent(const std::string& utf8) {
    static const char* kHex = "0123456789ABCDEF";
    const auto unreserved = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    };
    std::wstring out;
    out.reserve(utf8.size());
    for (unsigned char c : utf8) {
        if (unreserved(c)) {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(static_cast<wchar_t>(kHex[c >> 4]));
            out.push_back(static_cast<wchar_t>(kHex[c & 0x0F]));
        }
    }
    return out;
}

// 安全网：把整条 URL 中的非 ASCII（及空格）按 UTF-8 做 %XX 编码。已是 ASCII 的
// 部分原样保留，因此已编码的 %XX 不会被二次编码。
std::wstring EncodeNonAsciiForUrl(const std::wstring& url) {
    if (url.empty()) return url;
    bool needs = false;
    for (wchar_t c : url) {
        if (c < 0x21 || c > 0x7E) { needs = true; break; }
    }
    if (!needs) return url;
    const int n = WideCharToMultiByte(CP_UTF8, 0, url.c_str(), (int)url.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return url;
    std::string bytes(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, url.c_str(), (int)url.size(), bytes.data(), n,
                        nullptr, nullptr);
    static const char* kHex = "0123456789ABCDEF";
    std::wstring out;
    out.reserve(bytes.size());
    for (unsigned char b : bytes) {
        if (b >= 0x21 && b < 0x7F) {
            out.push_back(static_cast<wchar_t>(b));
        } else {
            out.push_back(L'%');
            out.push_back(static_cast<wchar_t>(kHex[b >> 4]));
            out.push_back(static_cast<wchar_t>(kHex[b & 0x0F]));
        }
    }
    return out;
}

HttpResponse WinHttpBlocking(const std::wstring& url, bool is_post,
                             const std::vector<std::uint8_t>& post_body,
                             const std::map<std::string, std::string>& req_headers,
                             const NetworkConfig& cfg) {
    HttpResponse resp;
    // 必须在 ParseUrl 之前编码：WinHttpCrackUrl 会把非 ASCII 降级为 '?'。
    UrlParts parts = ParseUrl(EncodeNonAsciiForUrl(url));
    if (parts.host.empty()) throw std::runtime_error("非法 URL");

    // 用 RAII 包装 HINTERNET，离开作用域自动关闭。
    struct WinHttpHandleRAII {
        HINTERNET h{};
        ~WinHttpHandleRAII() { if (h) WinHttpCloseHandle(h); }
        operator HINTERNET() const { return h; }
    };
    WinHttpHandleRAII hSession, hConnect, hRequest;

    hSession.h = WinHttpOpen(L"TTPlayerMusicFree/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession.h) throw std::runtime_error("WinHttpOpen 失败");

    if (!cfg.proxy.empty()) {
        // NetworkConfig::proxy 已是 std::wstring，无需再转换。
        std::wstring p = cfg.proxy;
        WINHTTP_PROXY_INFO pi{};
        pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        pi.lpszProxy = const_cast<wchar_t*>(p.c_str());
        WinHttpSetOption(hSession.h, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
    }

    DWORD timeout = cfg.timeout_ms ? cfg.timeout_ms : 20000;
    WinHttpSetTimeouts(hSession.h, timeout, timeout, timeout, timeout);

    hConnect.h = WinHttpConnect(hSession.h, parts.host.c_str(),
                                parts.port.empty() ? (parts.scheme == L"https" ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT)
                                                   : (DWORD)_wtoi(parts.port.c_str()),
                                0);
    if (!hConnect.h) throw std::runtime_error("WinHttpConnect 失败");

    DWORD flags = (parts.scheme == L"https") ? WINHTTP_FLAG_SECURE : 0;
    LPCWSTR verb = is_post ? L"POST" : L"GET";
    hRequest.h = WinHttpOpenRequest(hConnect.h, verb, parts.path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest.h) throw std::runtime_error("WinHttpOpenRequest 失败 (错误码 " +
                                              std::to_string(GetLastError()) + ")");

    // 音源里的解析接口大多会先 301/302 跳到另一种协议或域名
    // （例如 http://yinyue.haitangw.net/... -> https://...）。
    // 明确设为「始终跟随」，否则 WinHttpReceiveResponse 会在重定向处直接失败。
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                     sizeof(redirect_policy));

    // 请求头
    std::wstring headerBlob;
    for (const auto& kv : req_headers) {
        headerBlob += ToWide(kv.first) + L": " + ToWide(kv.second) + L"\r\n";
    }
    if (!headerBlob.empty())
        WinHttpAddRequestHeaders(hRequest.h, headerBlob.c_str(), (DWORD)headerBlob.size(),
                                 WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            const_cast<LPVOID>(static_cast<LPCVOID>(post_body.data())),
                            (DWORD)post_body.size(), (DWORD)post_body.size(), 0))
        throw std::runtime_error("WinHttpSendRequest 失败 (错误码 " +
                                 std::to_string(GetLastError()) + ")");

    if (!WinHttpReceiveResponse(hRequest.h, nullptr))
        throw std::runtime_error("WinHttpReceiveResponse 失败 (错误码 " +
                                 std::to_string(GetLastError()) + ")");

    // 状态码
    DWORD s = 0, sLen = sizeof(s);
    WinHttpQueryHeaders(hRequest.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &s, &sLen, WINHTTP_NO_HEADER_INDEX);
    resp.status = s;

    // 原始头（逐行解析）。查询长度时缓冲区传 nullptr（不存在 WINHTTP_NO_BUFFER 宏）。
    DWORD hSize = 0;
    WinHttpQueryHeaders(hRequest.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &hSize, WINHTTP_NO_HEADER_INDEX);
    if (hSize) {
        std::wstring raw(hSize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(hRequest.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                raw.data(), &hSize, WINHTTP_NO_HEADER_INDEX)) {
            // 去掉结尾空字符
            while (!raw.empty() && raw.back() == L'\0') raw.pop_back();
            std::wstring lower = raw;
            std::wstringstream ss(lower);
            std::wstring line;
            while (std::getline(ss, line)) {
                auto colon = line.find(L':');
                if (colon == std::wstring::npos) continue;
                std::wstring name = line.substr(0, colon);
                std::wstring value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.erase(0, 1);
                while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r')) value.pop_back();
                std::string n8 = ToUtf8(name), v8 = ToUtf8(value);
                for (auto& c : n8) c = (char)::tolower((unsigned char)c);
                resp.headers[n8].push_back(v8);
            }
        }
    }

    // 响应体
    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest.h, &avail)) break;
        if (!avail) break;
        std::vector<std::uint8_t> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest.h, buf.data(), avail, &read)) break;
        if (!read) break;
        resp.body.insert(resp.body.end(), buf.begin(), buf.begin() + read);
    } while (avail > 0);

    return resp;
}

// 从 headers map 取主 content-type（小写，去参数）
std::string ContentType(const std::map<std::string, std::vector<std::string>>& h) {
    auto it = h.find("content-type");
    if (it == h.end() || it->second.empty()) return {};
    std::string ct = it->second.front();
    auto sc = ct.find(';');
    if (sc != std::string::npos) ct = ct.substr(0, sc);
    for (auto& c : ct) c = (char)::tolower((unsigned char)c);
    while (!ct.empty() && ct.back() == ' ') ct.pop_back();
    return ct;
}

// 把 config.headers（JS 对象）合并进 req_headers。
std::map<std::string, std::string> MergeHeaders(JSContext* ctx, JSValueConst config) {
    std::map<std::string, std::string> out;
    if (JS_IsObject(config)) {
        JSValue hdrs = JS_GetPropertyStr(ctx, config, "headers");
        if (JS_IsObject(hdrs)) {
            JSPropertyEnum* props = nullptr; uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &count, hdrs,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < count; ++i) {
                    const char* k = JS_AtomToCString(ctx, props[i].atom);
                    if (!k) continue;
                    JSValue v = JS_GetProperty(ctx, hdrs, props[i].atom);
                    const char* cv = JS_ToCString(ctx, v);
                    if (cv) out[k] = cv;
                    if (cv) JS_FreeCString(ctx, cv);
                    JS_FreeValue(ctx, v);
                    JS_FreeCString(ctx, k);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, hdrs);
    }
    return out;
}

// config.params（对象）拼到 query string。
std::wstring BuildQuery(JSContext* ctx, JSValueConst config, const std::wstring& path) {
    std::wstring result = path;
    if (JS_IsObject(config)) {
        JSValue params = JS_GetPropertyStr(ctx, config, "params");
        if (JS_IsObject(params)) {
            bool first = path.find(L'?') == std::wstring::npos;
            JSPropertyEnum* props = nullptr; uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &count, params,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < count; ++i) {
                    const char* k = JS_AtomToCString(ctx, props[i].atom);
                    if (!k) continue;
                    JSValue v = JS_GetProperty(ctx, params, props[i].atom);
                    const char* cv = JS_ToCString(ctx, v);
                    // 与 axios 一致：键和值都按 encodeURIComponent 编码。
                    // 关键词里的中文/空格/&/= 若不编码，服务端收到的参数就是错的
                    // （WinHTTP 还会把中文降级成 '?'，见 EncodeNonAsciiForUrl）。
                    const std::wstring kv = PercentEncodeComponent(k) + L"=" +
                                            PercentEncodeComponent(cv ? cv : "");
                    result += (first ? L"?" : L"&") + kv;
                    first = false;
                    if (cv) JS_FreeCString(ctx, cv);
                    JS_FreeValue(ctx, v);
                    JS_FreeCString(ctx, k);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, params);
    }
    return result;
}

// 构造 axios 响应对象（作为 Promise 的 resolve 值）。
JSValue BuildResponse(JSContext* ctx, const HttpResponse& r, const NetworkConfig& cfg,
                      const std::string& response_type) {
    JSValue obj = JS_NewObject(ctx);

    // data
    // 复刻 axios 的默认 transformResponse：只要 responseType 不是 arraybuffer
    // （即空或 "json"），就对字符串 body **无条件** 尝试 JSON.parse，解析失败时
    // 静默回退为原始字符串。这里的判定**不能依赖 Content-Type**：网易云
    // music.163.com 的接口返回的确实是 JSON，但响应头是
    // `Content-Type: text/plain;charset=UTF-8`。真实 axios 不看响应头也能解析，
    // 只看响应头会让 .data 变成字符串，音源里 `L.code`/`L.result` 全部取不到，
    // 直接抛出「网易云搜索失败」。
    std::string ct = ContentType(r.headers);   // 仅用于诊断，不参与解析决策
    static_cast<void>(ct);
    if (response_type == "arraybuffer") {
        // 与浏览器一致：responseType:'arraybuffer' 得到 ArrayBuffer（QuickJS 托管其内存）。
        JSValue ab = JS_NewArrayBufferCopy(ctx, r.body.data(), r.body.size());
        JS_SetPropertyStr(ctx, obj, "data", ab);
    } else {
        std::string body_str(r.body.begin(), r.body.end());
        nlohmann::json parsed;
        bool parsed_ok = false;
        if (!body_str.empty()) {
            try {
                parsed = nlohmann::json::parse(body_str, nullptr, false);
                parsed_ok = !parsed.is_discarded();
            } catch (...) {
                parsed_ok = false;
            }
        }
        JS_SetPropertyStr(ctx, obj, "data",
            parsed_ok ? QuickJsEngine::JsonToValue(ctx, parsed)
                      : JS_NewString(ctx, body_str.c_str()));
    }

    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, (int)r.status));
    JS_SetPropertyStr(ctx, obj, "statusText", JS_NewString(ctx, r.status_text.c_str()));

    // headers 对象（set-cookie 等同名头保留数组）
    JSValue hdrs = JS_NewObject(ctx);
    for (const auto& kv : r.headers) {
        if (kv.second.size() == 1)
            JS_SetPropertyStr(ctx, hdrs, kv.first.c_str(), JS_NewString(ctx, kv.second.front().c_str()));
        else {
            JSValue arr = JS_NewArray(ctx);
            for (size_t i = 0; i < kv.second.size(); ++i)
                JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, kv.second[i].c_str()));
            JS_SetPropertyStr(ctx, hdrs, kv.first.c_str(), arr);
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", hdrs);
    JS_SetPropertyStr(ctx, obj, "config", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, obj, "request", JS_UNDEFINED);
    return obj;
}

// 诊断开关：设置环境变量 TTPLAYER_MF_TRACE=1 后，每次 axios 请求都会把
// 方法 / URL / 请求体 / 响应状态 / Content-Type / 响应体前若干字节追加到
// %TEMP%\ttplayer_mf_http.log。用于定位音源插件在沙箱内的真实网络行为
// （例如某音源只报「搜索失败」却看不出上游到底返回了什么）。
void TraceHttpRequest(const std::string& method, const std::string& url,
                      const std::wstring& final_url, bool is_post,
                      const std::map<std::string, std::string>& request_headers,
                      const std::vector<std::uint8_t>& request_body,
                      const HttpResponse& r) {
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"TTPLAYER_MF_TRACE", flag, 8) == 0) return;
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0) return;
    const std::wstring path = std::wstring(temp, length) + L"ttplayer_mf_http.log";
    FILE* file = _wfopen(path.c_str(), L"ab");
    if (!file) return;
    const auto clip = [](const std::vector<std::uint8_t>& data, size_t limit) {
        return std::string(data.begin(),
                           data.begin() + std::min<size_t>(data.size(), limit));
    };
    std::fprintf(file, "----\nmethod=%s is_post=%d\nurl=%s\nfinal=%s\nstatus=%d ct=%s\n",
                 method.c_str(), is_post ? 1 : 0, url.c_str(),
                 ToUtf8(final_url).c_str(), r.status,
                 ContentType(r.headers).c_str());
    // 请求头：音源动辄自带 Referer/Cookie，必须原样看到，才能判断服务端为何
    // 返回空结果（真实 axios 与这里的差异往往就在这一层）。
    for (const auto& kv : request_headers)
        std::fprintf(file, "hdr %s: %s\n", kv.first.c_str(), kv.second.c_str());
    std::fprintf(file, "req=%s\nresp=%s\n", clip(request_body, 400).c_str(),
                 clip(r.body, 600).c_str());
    std::fclose(file);
}

// 同一开关下的失败记录：请求在 WinHttp 层直接抛异常时不会走到
// TraceHttpRequest，必须单独留一条，否则「解析失败」看不到是哪个 URL 挂了。
void TraceHttpFailure(const std::string& method, const std::string& url,
                      const std::string& reason) {
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"TTPLAYER_MF_TRACE", flag, 8) == 0) return;
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0) return;
    const std::wstring path = std::wstring(temp, length) + L"ttplayer_mf_http.log";
    FILE* file = _wfopen(path.c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "----\nFAILED method=%s\nurl=%s\nreason=%s\n", method.c_str(),
                 url.c_str(), reason.c_str());
    std::fclose(file);
}

// 核心实现：url 字符串，config 对象（可为 undefined），data 作为可选请求体。
// 注意 axios 的调用约定差异：
//   get(url, config)              -> data = undefined
//   post(url, data, config)       -> data 为 argv[1]
//   request(config)               -> url/data 都从 config 里取
JSValue AxiosCore(JSContext* ctx, std::string url, JSValueConst config,
                  JSValueConst data, const NetworkConfig& cfg, bool is_post) {
    if (url.empty()) return JS_Throw(ctx, JS_NewString(ctx, "axios: url 为空"));

    std::map<std::string, std::string> headers = MergeHeaders(ctx, config);
    if (cfg.user_agent.empty() && headers.find("user-agent") == headers.end())
        headers["user-agent"] = "Mozilla/5.0 (compatible; TTPlayerMusicFree/0.1)";
    else if (!cfg.user_agent.empty())
        headers["user-agent"] = ToUtf8(cfg.user_agent);

    // POST body（来自 data 参数；get 时 data 为 undefined）
    std::vector<std::uint8_t> post_body;
    if (is_post && !JS_IsUndefined(data)) {
        if (JS_IsString(data)) {
            const char* d = JS_ToCString(ctx, data);
            std::string s = d ? d : "";
            if (d) JS_FreeCString(ctx, d);
            post_body.assign(s.begin(), s.end());
            if (headers.find("content-type") == headers.end())
                headers["content-type"] = "application/x-www-form-urlencoded";
        } else {
            nlohmann::json j = QuickJsEngine::ValueToJson(ctx, data);
            std::string s = j.dump();
            post_body.assign(s.begin(), s.end());
            if (headers.find("content-type") == headers.end())
                headers["content-type"] = "application/json";
        }
    }

    std::wstring wurl = BuildQuery(ctx, config, ToWide(url));

    std::string response_type;
    if (JS_IsObject(config)) {
        JSValue rt = JS_GetPropertyStr(ctx, config, "responseType");
        if (JS_IsString(rt)) {
            const char* c = JS_ToCString(ctx, rt);
            if (c) response_type = c;
            if (c) JS_FreeCString(ctx, c);
        }
        JS_FreeValue(ctx, rt);
    }

    // 执行同步请求
    HttpResponse r;
    try {
        r = WinHttpBlocking(wurl, is_post, post_body, headers, cfg);
    } catch (const std::exception& e) {
        TraceHttpFailure(is_post ? "POST" : "GET", ToUtf8(wurl), e.what());
        return JS_Throw(ctx, JS_NewString(ctx, ("axios 请求失败: " + std::string(e.what())).c_str()));
    }

    TraceHttpRequest(is_post ? "POST" : "GET", url, wurl, is_post, headers, post_body, r);

    // 构造 Promise 并同步 resolve（让 Await 用微任务泵出 await 链）
    JSValue resolve[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, resolve[0]); JS_FreeValue(ctx, resolve[1]);
        return promise;
    }
    JSValue resp = BuildResponse(ctx, r, cfg, response_type);
    JSValue arg = resp;
    JS_Call(ctx, resolve[0], JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, resp);
    JS_FreeValue(ctx, resolve[0]);
    JS_FreeValue(ctx, resolve[1]);
    return promise;
}

// axios.get(url, config)
JSValue AxiosGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    const char* url_c = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
    std::string url = url_c ? url_c : "";
    if (url_c) JS_FreeCString(ctx, url_c);
    JSValueConst config = (argc >= 2 && !JS_IsUndefined(argv[1])) ? argv[1] : JS_UNDEFINED;
    return AxiosCore(ctx, url, config, JS_UNDEFINED, eng ? eng->network() : NetworkConfig{}, false);
}

// axios.post(url, data, config)
JSValue AxiosPost(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    const char* url_c = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
    std::string url = url_c ? url_c : "";
    if (url_c) JS_FreeCString(ctx, url_c);
    JSValueConst data = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    JSValueConst config = (argc >= 3 && !JS_IsUndefined(argv[2])) ? argv[2] : JS_UNDEFINED;
    return AxiosCore(ctx, url, config, data, eng ? eng->network() : NetworkConfig{}, true);
}

// axios.request(config)：从 config.url / config.method / config.data 取参数
JSValue AxiosRequest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    JSValueConst config = (argc >= 1 && !JS_IsUndefined(argv[0])) ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(config)) return JS_Throw(ctx, JS_NewString(ctx, "axios.request: 缺少 config"));
    const char* url_c = nullptr;
    std::string url;
    JSValue uv = JS_GetPropertyStr(ctx, config, "url");
    if (JS_IsString(uv)) { url_c = JS_ToCString(ctx, uv); url = url_c ? url_c : ""; }
    if (url_c) JS_FreeCString(ctx, url_c);
    JS_FreeValue(ctx, uv);
    JSValue mv = JS_GetPropertyStr(ctx, config, "method");
    std::string method;
    if (JS_IsString(mv)) { const char* m = JS_ToCString(ctx, mv); if (m) method = m; if (m) JS_FreeCString(ctx, m); }
    JS_FreeValue(ctx, mv);
    bool is_post = !method.empty() &&
        (method == "post" || method == "POST" || method == "put" || method == "PUT" || method == "delete" || method == "DELETE");
    JSValue data = JS_GetPropertyStr(ctx, config, "data");
    JSValue result = AxiosCore(ctx, url, config, data, eng ? eng->network() : NetworkConfig{}, is_post);
    JS_FreeValue(ctx, data);
    return result;
}

// axios(config) 或 axios(url) —— 复刻 MusicFree：require('axios') 返回的对象本身
// 可调用，且其 .default 指向自身（插件经 _interopRequireDefault 后通过 .default 调用）。
JSValue AxiosBase(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 1 && JS_IsString(argv[0]))
        return AxiosGet(ctx, JS_UNDEFINED, argc, argv); // axios(url) -> GET
    return AxiosRequest(ctx, JS_UNDEFINED, argc, argv); // axios(config)
}

// 构造一个 axios 实例（本身可调用，并带 get/post/request/defaults/create/interceptors）。
JSValue MakeAxios(JSContext* ctx, const NetworkConfig& cfg) {
    JSValue axios = JS_NewCFunction(ctx, &AxiosBase, "axios", 1);
    JS_SetPropertyStr(ctx, axios, "get", JS_NewCFunction(ctx, &AxiosGet, "get", 2));
    JS_SetPropertyStr(ctx, axios, "post", JS_NewCFunction(ctx, &AxiosPost, "post", 3));
    JS_SetPropertyStr(ctx, axios, "request", JS_NewCFunction(ctx, &AxiosRequest, "request", 1));

    JSValue defaults = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, defaults, "timeout", JS_NewInt32(ctx, (int)cfg.timeout_ms));
    JSValue dheaders = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dheaders, "common", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, defaults, "headers", dheaders);
    JS_SetPropertyStr(ctx, axios, "defaults", defaults);

    // create(config) -> 新实例（此处忽略 config 差异，复用同一后端）
    JS_SetPropertyStr(ctx, axios, "create",
        JS_NewCFunction(ctx,
            [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
                auto* e = static_cast<QuickJsEngine*>(JS_GetContextOpaque(c));
                return MakeAxios(c, e ? e->network() : NetworkConfig{});
            }, "create", 1));

    // interceptors.response.use(onFulfilled) —— 仅记录第一个回调（尽力）
    JSValue interceptors = JS_NewObject(ctx);
    JSValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "use",
        JS_NewCFunction(ctx,
            [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue { return JS_UNDEFINED; },
            "use", 1));
    JS_SetPropertyStr(ctx, interceptors, "response", response);
    JS_SetPropertyStr(ctx, axios, "interceptors", interceptors);

    // MusicFree：require('axios').default 才是可调用的实例本体
    JS_SetPropertyStr(ctx, axios, "default", JS_DupValue(ctx, axios));
    return axios;
}

// cheerio 垫片（极小实现）
const char kCheerioShim[] = R"js(
(function () {
  function stripTags(s) {
    return s.replace(/<[^>]*>/g, ' ')
            .replace(/&nbsp;/g, ' ').replace(/&amp;/g, '&')
            .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
            .replace(/&quot;/g, '"').replace(/\s+/g, ' ').trim();
  }
  function load(html) {
    html = html || '';
    function $(root) {
      root = root || html;
      return {
        _root: root,
        text: function () { return stripTags(root); },
        html: function () { return root; },
        length: 0,
        attr: function (name) {
          var m = new RegExp(name + '\\s*=\\s*"([^"]*)"', 'i').exec(root);
          if (m) return m[1];
          m = new RegExp(name + '\\s*=\\s*\'([^\']*)\'', 'i').exec(root);
          return m ? m[1] : null;
        },
        find: function (sel) {
          var tag = (sel.split(/[.#:]/)[0] || '*');
          if (tag === '*') return $(root);
          var re = new RegExp('<' + tag + '[^>]*>([\\s\\S]*?)<\\/' + tag + '>', 'i');
          var m = re.exec(root);
          return $(m ? ('<' + tag + '>' + m[1] + '</' + tag + '>') : '');
        },
        each: function (cb) { if (cb) cb({}, this); return this; }
      };
    }
    return $(html);
  }
  return { load: load };
})()
)js";

// ---------------------------------------------------------------------------
// fetch 全局（复刻 RN/浏览器 fetch；部分插件直接用 fetch 而非 axios）
// ---------------------------------------------------------------------------
// 把 data 包进一个已 resolve 的 Promise（供 json/text/arrayBuffer 等方法返回）。
JSValue ResolveWith(JSContext* c, JSValue data) {
    JSValue resolve[2];
    JSValue promise = JS_NewPromiseCapability(c, resolve);
    if (JS_IsException(promise)) {
        JS_FreeValue(c, resolve[0]); JS_FreeValue(c, resolve[1]); JS_FreeValue(c, data);
        return promise;
    }
    JSValue a = data;
    JS_Call(c, resolve[0], JS_UNDEFINED, 1, &a);
    JS_FreeValue(c, resolve[0]); JS_FreeValue(c, resolve[1]);
    return promise;
}

// response.headers.get(name)
JSValue FetchHeadersGet(JSContext* c, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* k = JS_ToCString(c, argv[0]);
    std::string ks = k ? k : "";
    if (k) JS_FreeCString(c, k);
    for (auto& ch : ks) ch = (char)::tolower((unsigned char)ch);
    return JS_GetPropertyStr(c, this_val, ks.c_str()); // 调用方释放
}

JSValue FetchJson(JSContext* c, JSValueConst this_val, int, JSValueConst*) {
    JSValue j = JS_GetPropertyStr(c, this_val, "__json");
    JSValue data = JS_IsNull(j) ? JS_GetPropertyStr(c, this_val, "__body") : j;
    JSValue ret = ResolveWith(c, data);
    JS_FreeValue(c, j); JS_FreeValue(c, data);
    return ret;
}
JSValue FetchText(JSContext* c, JSValueConst this_val, int, JSValueConst*) {
    JSValue b = JS_GetPropertyStr(c, this_val, "__body");
    JSValue ret = ResolveWith(c, b);
    JS_FreeValue(c, b);
    return ret;
}
JSValue FetchArrayBuffer(JSContext* c, JSValueConst this_val, int, JSValueConst*) {
    JSValue ab = JS_GetPropertyStr(c, this_val, "__ab");
    JSValue ret = ResolveWith(c, ab);
    JS_FreeValue(c, ab);
    return ret;
}
JSValue FetchClone(JSContext* c, JSValueConst this_val, int, JSValueConst*) {
    return JS_DupValue(c, this_val);
}

JSValue FetchCall(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    const NetworkConfig cfg = eng ? eng->network() : NetworkConfig{};
    if (argc < 1) return JS_Throw(ctx, JS_NewString(ctx, "fetch: 缺少 url"));
    const char* url_c = JS_ToCString(ctx, argv[0]);
    std::string url = url_c ? url_c : "";
    if (url_c) JS_FreeCString(ctx, url_c);
    if (url.empty()) return JS_Throw(ctx, JS_NewString(ctx, "fetch: url 为空"));

    JSValueConst options = (argc >= 2 && !JS_IsUndefined(argv[1])) ? argv[1] : JS_UNDEFINED;
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
    bool is_post = false;
    if (JS_IsObject(options)) {
        JSValue mv = JS_GetPropertyStr(ctx, options, "method");
        if (JS_IsString(mv)) { const char* m = JS_ToCString(ctx, mv); if (m) method = m; if (m) JS_FreeCString(ctx, m); }
        JS_FreeValue(ctx, mv);
        headers = MergeHeaders(ctx, options);
        JSValue bv = JS_GetPropertyStr(ctx, options, "body");
        if (!JS_IsUndefined(bv) && !JS_IsNull(bv)) {
            if (JS_IsString(bv)) {
                const char* d = JS_ToCString(ctx, bv); std::string s = d ? d : "";
                if (d) JS_FreeCString(ctx, d); body.assign(s.begin(), s.end());
            } else {
                nlohmann::json j = QuickJsEngine::ValueToJson(ctx, bv); std::string s = j.dump();
                body.assign(s.begin(), s.end());
            }
        }
        JS_FreeValue(ctx, bv);
    }
    is_post = (method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH");
    if (cfg.user_agent.empty() && headers.find("user-agent") == headers.end())
        headers["user-agent"] = "Mozilla/5.0 (compatible; TTPlayerMusicFree/0.1)";

    std::wstring wurl = BuildQuery(ctx, options, ToWide(url));

    HttpResponse r;
    try { r = WinHttpBlocking(wurl, is_post, body, headers, cfg); }
    catch (const std::exception& e) {
        return JS_Throw(ctx, JS_NewString(ctx, ("fetch 失败: " + std::string(e.what())).c_str()));
    }

    std::string body_str(r.body.begin(), r.body.end());
    nlohmann::json parsed; bool parsed_ok = false;
    try { parsed = nlohmann::json::parse(body_str, nullptr, false); parsed_ok = !parsed.is_discarded(); }
    catch (...) {}

    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, (int)r.status));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, r.status_text.c_str()));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, (r.status >= 200 && r.status < 300)));
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url.c_str()));
    JSValue hdrs = JS_NewObject(ctx);
    for (const auto& kv : r.headers) {
        if (kv.second.size() == 1)
            JS_SetPropertyStr(ctx, hdrs, kv.first.c_str(), JS_NewString(ctx, kv.second.front().c_str()));
        else {
            JSValue arr = JS_NewArray(ctx);
            for (size_t i = 0; i < kv.second.size(); ++i)
                JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, kv.second[i].c_str()));
            JS_SetPropertyStr(ctx, hdrs, kv.first.c_str(), arr);
        }
    }
    JS_SetPropertyStr(ctx, hdrs, "get", JS_NewCFunction(ctx, &FetchHeadersGet, "get", 1));
    JS_SetPropertyStr(ctx, resp, "headers", hdrs);
    // 预存数据，供 json/text/arrayBuffer 以 this_val 读取
    JS_SetPropertyStr(ctx, resp, "__body", JS_NewString(ctx, body_str.c_str()));
    JS_SetPropertyStr(ctx, resp, "__json", parsed_ok ? QuickJsEngine::JsonToValue(ctx, parsed) : JS_NULL);
    JS_SetPropertyStr(ctx, resp, "__ab", JS_NewArrayBufferCopy(ctx, r.body.data(), r.body.size()));
    JS_SetPropertyStr(ctx, resp, "json", JS_NewCFunction(ctx, &FetchJson, "json", 0));
    JS_SetPropertyStr(ctx, resp, "text", JS_NewCFunction(ctx, &FetchText, "text", 0));
    JS_SetPropertyStr(ctx, resp, "arrayBuffer", JS_NewCFunction(ctx, &FetchArrayBuffer, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, resp, "clone", JS_NewCFunction(ctx, &FetchClone, "clone", 0));

    return ResolveWith(ctx, resp);
}

} // namespace

JSValue CreateAxiosModule(JSContext* ctx, const NetworkConfig& cfg, QuickJsEngine*) {
    return MakeAxios(ctx, cfg);
}

JSValue CreateCheerioShim(JSContext* ctx) {
    JSValue v = JS_Eval(ctx, kCheerioShim, sizeof(kCheerioShim) - 1, "cheerio-shim",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, v);
        return JS_NewObject(ctx); // 最坏情况返回空对象
    }
    return v;
}

JSValue CreateFetchGlobal(JSContext* ctx) {
    return JS_NewCFunction(ctx, &FetchCall, "fetch", 2);
}

std::string DownloadText(const std::string& url, const NetworkConfig& cfg) {
    std::wstring wurl = ToWide(url);
    HttpResponse r = WinHttpBlocking(wurl, false, {}, {}, cfg);
    if (r.status < 200 || r.status >= 300)
        throw std::runtime_error("下载失败，HTTP " + std::to_string(r.status));
    return std::string(r.body.begin(), r.body.end());
}

std::vector<std::uint8_t> DownloadBinary(
    const std::string& url, const NetworkConfig& cfg,
    const std::map<std::string, std::string>& headers) {
    std::wstring wurl = ToWide(url);
    HttpResponse r = WinHttpBlocking(wurl, false, {}, headers, cfg);
    if (r.status < 200 || r.status >= 300)
        throw std::runtime_error("下载失败，HTTP " + std::to_string(r.status));
    if (r.body.empty())
        throw std::runtime_error("下载结果为空（服务器返回 0 字节）");
    return r.body;
}

} // namespace ttplayer::musicfree
