// TVBox / catvod / drpy 自定义播放器执行器（逆向复刻）— 实现
//
// 见 video_player_js.h 头注。要点：TVBox 的 playerUrl 是**裸脚本**（不是
// CommonJS 模块），里面用全局函数声明 playerContent / 以 flag 命名的函数，
// 因此这里不能用 MusicFree 的 EvalPlugin 包裹（那会把函数变成局部的），必须
// 用 JS_Eval(... JS_EVAL_TYPE_GLOBAL) 直接求值到全局对象。宿主 API 则复用
// 同一个 QuickJsEngine 的 require/axios/crypto-js/console/URL 基础能力。

#include "ttplayer/video/video_player_js.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <quickjs.h>
#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/quickjs_engine.h"
#include "ttplayer/video/spider/spider_util.h"

namespace ttplayer::video {

namespace {

using nlohmann::json;
using ttplayer::musicfree::NetworkConfig;
using ttplayer::musicfree::QuickJsEngine;

// JS C 回调无法拿到调用方栈上的状态（quickjs 的 context opaque 已被
// QuickJsEngine 占用），用 thread_local 存本次执行的 HTTP 配置与站点头。
// RunVideoPlayerJs 全程在同一后台线程内完成，生命周期安全。
struct HostState {
    NetworkConfig cfg;
    std::map<std::string, std::string> headers;
    std::string log;
};
thread_local HostState* g_host = nullptr;

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(std::string s) {
    const auto ns = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
    s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
    return s;
}

std::string JsStr(JSContext* ctx, JSValueConst v) {
    const char* c = JS_ToCString(ctx, v);
    if (!c) return {};
    std::string s(c);
    JS_FreeCString(ctx, c);
    return s;
}

json JsJson(JSContext* ctx, JSValueConst v) {
    try { return QuickJsEngine::ValueToJson(ctx, v); } catch (...) { return json(); }
}

std::string JsonPick(const json& j, std::initializer_list<const char*> keys) {
    if (!j.is_object()) return {};
    for (const char* k : keys) {
        if (!j.contains(k) || j[k].is_null()) continue;
        const auto& v = j[k];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        if (v.is_number_float()) return std::to_string(v.get<double>());
        if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    }
    return {};
}

// 大小写不敏感地写入请求头，避免站点头与 JS 传入的头重复添加两行。
void PutHeader(std::map<std::string, std::string>& dst,
               const std::string& key, const std::string& value) {
    if (key.empty() || value.empty()) return;
    for (auto it = dst.begin(); it != dst.end();) {
        if (ToLower(it->first) == ToLower(key)) it = dst.erase(it);
        else ++it;
    }
    dst[key] = value;
}

void CollectHeaders(const json& j, std::map<std::string, std::string>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) PutHeader(out, it.key(), it.value().get<std::string>());
            else if (it.value().is_number()) PutHeader(out, it.key(), it.value().dump());
            else if (it.value().is_boolean())
                PutHeader(out, it.key(), it.value().get<bool>() ? "true" : "false");
        }
    } else if (j.is_string()) {
        // 也允许 header 是一段 JSON 字符串。
        try {
            const json inner = json::parse(j.get<std::string>());
            if (inner.is_object()) CollectHeaders(inner, out);
        } catch (...) {}
        // 或 "K: V, K2: V2" 文本
        std::string text = j.get<std::string>();
        if (text.find(':') != std::string::npos) {
            for (const auto& seg : spider_util::Split(text, ',')) {
                const size_t p = seg.find(':');
                if (p == std::string::npos) continue;
                PutHeader(out, Trim(seg.substr(0, p)), Trim(seg.substr(p + 1)));
            }
        }
    }
}

struct HttpResult {
    long code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

HttpResult DoHttp(const std::string& url, std::string method,
                  const std::map<std::string, std::string>& extra,
                  const std::string& body) {
    HttpResult r;
    if (!g_host || url.empty()) return r;
    if (method.empty()) method = "GET";
    std::transform(method.begin(), method.end(), method.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    auto hs = g_host->headers;
    for (const auto& kv : extra) PutHeader(hs, kv.first, kv.second);
    try {
        const std::vector<std::uint8_t> bytes(body.begin(), body.end());
        const auto ex = ttplayer::musicfree::PerformHttpRequest(
            url, method, hs, bytes, g_host->cfg);
        r.code = static_cast<long>(ex.status);
        r.body.assign(ex.body.begin(), ex.body.end());
        for (const auto& kv : ex.headers)
            if (!kv.second.empty()) PutHeader(r.headers, kv.first, kv.second.front());
    } catch (const std::exception& e) {
        r.body = std::string("[http error] ") + e.what();
    } catch (...) {
        r.body = "[http error]";
    }
    return r;
}

json HttpResultJson(const HttpResult& r) {
    json h = json::object();
    for (const auto& kv : r.headers) h[kv.first] = kv.second;
    return json{{"code", r.code}, {"body", r.body}, {"headers", h},
                {"status", r.code}, {"data", r.body}};
}

// java.ajax(url | {url,method,headers,data,body}, headers?) → catvod 返回 JSON 字符串。
struct AjaxArgs {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
};

AjaxArgs ParseAjax(JSContext* ctx, int argc, JSValueConst* argv) {
    AjaxArgs a;
    if (argc >= 1) {
        if (JS_IsString(argv[0])) {
            a.url = JsStr(ctx, argv[0]);
        } else if (JS_IsObject(argv[0])) {
            const json j = JsJson(ctx, argv[0]);
            a.url = JsonPick(j, {"url", "uri", "href"});
            const std::string m = JsonPick(j, {"method", "type"});
            if (!m.empty()) a.method = ToLower(m) == "post" ? "POST" : m;
            if (j.contains("headers")) CollectHeaders(j["headers"], a.headers);
            if (j.contains("header")) CollectHeaders(j["header"], a.headers);
            for (const char* k : {"data", "body", "params", "postData"}) {
                if (!j.contains(k) || j[k].is_null()) continue;
                const auto& v = j[k];
                if (v.is_string()) a.body = v.get<std::string>();
                else a.body = v.dump();
                if (!a.body.empty()) {
                    if (ToLower(a.method) == "get") a.method = "POST";
                    break;
                }
            }
        }
    }
    // java.ajax(url, {headers}) / java.ajax(url, data, headers)
    if (argc >= 2 && JS_IsObject(argv[1])) {
        const json j = JsJson(ctx, argv[1]);
        if (j.is_object() &&
            (j.contains("headers") || j.contains("header") || j.contains("method"))) {
            if (j.contains("headers")) CollectHeaders(j.at("headers"), a.headers);
            if (j.contains("header")) CollectHeaders(j.at("header"), a.headers);
            const std::string m = JsonPick(j, {"method"});
            if (!m.empty()) a.method = m;
        } else {
            CollectHeaders(j, a.headers);
        }
    }
    if (argc >= 3 && JS_IsObject(argv[2])) CollectHeaders(JsJson(ctx, argv[2]), a.headers);
    return a;
}

JSValue JavaAjax(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const AjaxArgs a = ParseAjax(ctx, argc, argv);
    const HttpResult r = DoHttp(a.url, a.method, a.headers, a.body);
    const std::string text = HttpResultJson(r).dump();
    return JS_NewString(ctx, text.c_str());
}

JSValue FetchObject(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const AjaxArgs a = ParseAjax(ctx, argc, argv);
    const HttpResult r = DoHttp(a.url, a.method, a.headers, a.body);
    return QuickJsEngine::JsonToValue(ctx, HttpResultJson(r));
}

// 简单 GET/POST：返回 body 字符串。
JSValue SimpleGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string url = argc >= 1 ? JsStr(ctx, argv[0]) : std::string();
    std::map<std::string, std::string> h;
    if (argc >= 2 && JS_IsObject(argv[1])) CollectHeaders(JsJson(ctx, argv[1]), h);
    const HttpResult r = DoHttp(url, "GET", h, {});
    return JS_NewString(ctx, r.body.c_str());
}

JSValue SimplePost(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string url = argc >= 1 ? JsStr(ctx, argv[0]) : std::string();
    std::string body;
    if (argc >= 2 && JS_IsString(argv[1])) body = JsStr(ctx, argv[1]);
    else if (argc >= 2 && JS_IsObject(argv[1])) body = JsJson(ctx, argv[1]).dump();
    std::map<std::string, std::string> h;
    if (argc >= 3 && JS_IsObject(argv[2])) CollectHeaders(JsJson(ctx, argv[2]), h);
    const HttpResult r = DoHttp(url, "POST", h, body);
    return JS_NewString(ctx, r.body.c_str());
}

JSValue FnBase64Encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string s = argc >= 1 ? JsStr(ctx, argv[0]) : std::string();
    return JS_NewString(ctx, spider_util::Base64Encode(s).c_str());
}

JSValue FnBase64Decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string s = argc >= 1 ? JsStr(ctx, argv[0]) : std::string();
    std::string out;
    spider_util::Base64Decode(s, &out);
    return JS_NewString(ctx, out.c_str());
}

JSValue FnMd5(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string s = argc >= 1 ? JsStr(ctx, argv[0]) : std::string();
    return JS_NewString(ctx, spider_util::Md5Hex(s).c_str());
}

JSValue FnSha256(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string s = argc >= 1 ? JsStr(ctx, argv[0]) : std::string();
    return JS_NewString(ctx, spider_util::Sha256Hex(s).c_str());
}

JSValue FnPrint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i) msg += ' ';
        msg += JsStr(ctx, argv[i]);
    }
    if (g_host) {
        if (g_host->log.size() < 4096) g_host->log += msg + "\n";
    }
    return JS_UNDEFINED;
}

JSValue FnUndefined(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

// localStorage / 全局 getItem-setItem（drpy 常用 getString/setString）。
std::map<std::string, std::string>& KvStore() {
    static std::map<std::string, std::string> m;
    return m;
}
std::mutex& KvMutex() {
    static std::mutex mu;
    return mu;
}

JSValue FnGetItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string k = argc >= 1 ? JsStr(ctx, argv[0]) : std::string();
    std::lock_guard<std::mutex> lock(KvMutex());
    auto it = KvStore().find(k);
    const std::string v = it == KvStore().end() ? std::string() : it->second;
    return JS_NewString(ctx, v.c_str());
}

JSValue FnSetItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 2) {
        const std::string k = JsStr(ctx, argv[0]);
        const std::string v = JsStr(ctx, argv[1]);
        std::lock_guard<std::mutex> lock(KvMutex());
        KvStore()[k] = v;
    }
    return JS_UNDEFINED;
}

JSValue FnSleep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::int32_t ms = 0;
    if (argc >= 1) JS_ToInt32(ctx, &ms, argv[0]);
    // 播放器脚本偶尔 sleep 等接口，硬上限 1s 免得拖死解析线程。
    if (ms > 0 && ms <= 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return JS_UNDEFINED;
}

JSValue FnTimestamp(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt64(ctx, spider_util::UnixMilliseconds());
}

void AddFn(JSContext* ctx, JSValue obj, const char* name, JSCFunction* fn) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, 1));
}

void InstallHost(QuickJsEngine& eng) {
    JSContext* ctx = eng.context();

    // java：catvod Java spider 暴露给 JS 的宿主对象
    JSValue java = JS_NewObject(ctx);
    AddFn(ctx, java, "ajax", &JavaAjax);
    AddFn(ctx, java, "get", &SimpleGet);
    AddFn(ctx, java, "post", &SimplePost);
    AddFn(ctx, java, "md5", &FnMd5);
    AddFn(ctx, java, "base64Encode", &FnBase64Encode);
    AddFn(ctx, java, "base64Decode", &FnBase64Decode);
    AddFn(ctx, java, "startProxy", &FnUndefined);
    AddFn(ctx, java, "stopProxy", &FnUndefined);
    eng.SetGlobal("java", java);

    // $：drpy/hipy 的工具对象
    JSValue dollar = JS_NewObject(ctx);
    AddFn(ctx, dollar, "fetch", &FetchObject);
    AddFn(ctx, dollar, "get", &FetchObject);
    AddFn(ctx, dollar, "post", &FetchObject);
    AddFn(ctx, dollar, "request", &SimpleGet);
    AddFn(ctx, dollar, "md5", &FnMd5);
    AddFn(ctx, dollar, "base64Encode", &FnBase64Encode);
    AddFn(ctx, dollar, "base64Decode", &FnBase64Decode);
    eng.SetGlobal("$", dollar);

    // 全局函数
    eng.SetGlobal("request", JS_NewCFunction(ctx, &SimpleGet, "request", 1));
    eng.SetGlobal("print", JS_NewCFunction(ctx, &FnPrint, "print", 1));
    eng.SetGlobal("log", JS_NewCFunction(ctx, &FnPrint, "log", 1));
    eng.SetGlobal("base64Encode", JS_NewCFunction(ctx, &FnBase64Encode, "base64Encode", 1));
    eng.SetGlobal("base64Decode", JS_NewCFunction(ctx, &FnBase64Decode, "base64Decode", 1));
    eng.SetGlobal("atob", JS_NewCFunction(ctx, &FnBase64Decode, "atob", 1));
    eng.SetGlobal("btoa", JS_NewCFunction(ctx, &FnBase64Encode, "btoa", 1));
    eng.SetGlobal("md5", JS_NewCFunction(ctx, &FnMd5, "md5", 1));
    eng.SetGlobal("MD5", JS_NewCFunction(ctx, &FnMd5, "MD5", 1));
    eng.SetGlobal("sha256", JS_NewCFunction(ctx, &FnSha256, "sha256", 1));
    eng.SetGlobal("sleep", JS_NewCFunction(ctx, &FnSleep, "sleep", 1));
    eng.SetGlobal("getTimestamp", JS_NewCFunction(ctx, &FnTimestamp, "getTimestamp", 0));
    eng.SetGlobal("getTime", JS_NewCFunction(ctx, &FnTimestamp, "getTime", 0));
    eng.SetGlobal("getItem", JS_NewCFunction(ctx, &FnGetItem, "getItem", 1));
    eng.SetGlobal("setItem", JS_NewCFunction(ctx, &FnSetItem, "setItem", 2));
    eng.SetGlobal("getString", JS_NewCFunction(ctx, &FnGetItem, "getString", 1));
    eng.SetGlobal("setString", JS_NewCFunction(ctx, &FnSetItem, "setString", 2));

    JSValue storage = JS_NewObject(ctx);
    AddFn(ctx, storage, "getItem", &FnGetItem);
    AddFn(ctx, storage, "setItem", &FnSetItem);
    AddFn(ctx, storage, "removeItem", &FnUndefined);
    AddFn(ctx, storage, "clear", &FnUndefined);
    eng.SetGlobal("localStorage", storage);

    if (g_host && !g_host->headers.empty()) {
        std::string ua;
        for (const auto& kv : g_host->headers)
            if (ToLower(kv.first) == "user-agent") ua = kv.second;
        if (!ua.empty()) eng.SetGlobal("UA", JS_NewString(ctx, ua.c_str()));
    }
}

// 把 player_spec 变成 JS 源码。
std::string LoadPlayerCode(const std::string& spec, const std::string& base_url,
                           const NetworkConfig& cfg,
                           const std::map<std::string, std::string>& headers,
                           std::string* note) {
    std::string s = Trim(spec);
    if (s.empty()) return {};
    // js:// 前缀
    if (s.rfind("js://", 0) == 0) s = Trim(s.substr(5));
    // 直接就是 JS 源码（含函数声明 / 顶层表达式）
    const bool looks_code =
        s.find("function ") != std::string::npos ||
        s.find("=>") != std::string::npos ||
        s.find("playerContent") != std::string::npos ||
        s.find("globalThis") != std::string::npos ||
        (s.find('\n') != std::string::npos && s.find('{') != std::string::npos);
    if (looks_code) {
        if (note) *note = "inline-js";
        return s;
    }
    // 本地文件
    std::string local = s;
    if (local.rfind("file://", 0) == 0) local = local.substr(7);
    {
        std::ifstream f(local, std::ios::binary);
        if (f.good()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            if (note) *note = "local-file";
            return ss.str();
        }
    }
    // URL / 相对路径
    std::string url = s;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0 &&
        !base_url.empty()) {
        url = spider_util::JoinUrl(base_url, url);
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        // 最后尝试 base64 解码（TVBox 有的把 JS 直接 base64 塞进 playerUrl）
        std::string decoded;
        if (spider_util::Base64Decode(s, &decoded) &&
            (decoded.find("function") != std::string::npos ||
             decoded.find("playerContent") != std::string::npos)) {
            if (note) *note = "base64-js";
            return decoded;
        }
        return {};
    }
    try {
        if (note) *note = "download";
        return ttplayer::musicfree::DownloadText(url, cfg, headers);
    } catch (const std::exception& e) {
        if (note) *note = std::string("download-failed: ") + e.what();
        return {};
    }
}

// 解析播放器返回值：对象 / JSON 字符串 / 数组 / 纯 URL。
VideoPlayerJsResult InterpretResult(const json& j, const std::string& which) {
    VideoPlayerJsResult out;
    out.detail = which;
    if (j.is_null() || j.is_discarded() || (j.is_string() && j.get<std::string>().empty())) {
        out.detail += " -> 返回空";
        return out;
    }
    json v = j;
    if (v.is_string()) {
        const std::string text = Trim(v.get<std::string>());
        if (text.size() > 1 && (text[0] == '{' || text[0] == '[')) {
            try { v = json::parse(text); } catch (...) { out.url = text; return out; }
        } else {
            out.url = text;
            return out;
        }
    }
    if (v.is_array()) {
        // catvod Java spider 的 [parse, url, header] 三元组
        std::string parse_v, url_v;
        json hdr = json::object();
        for (const auto& e : v) {
            if (e.is_number_integer() && parse_v.empty())
                parse_v = std::to_string(e.get<long long>());
            else if (e.is_string() && url_v.empty()) url_v = e.get<std::string>();
            else if (e.is_object()) hdr = e;
        }
        out.url = url_v;
        out.parse = (parse_v == "1" || parse_v == "2") ? 1 : 0;
        CollectHeaders(hdr, out.headers);
        return out;
    }
    if (!v.is_object()) return out;
    out.url = JsonPick(v, {"url", "playUrl", "play_url", "jxUrl", "jx_url",
                           "link", "mediaUrl", "realUrl", "m3u8", "play"});
    if (out.url.empty() && v.contains("data")) {
        const auto& d = v["data"];
        if (d.is_object()) out.url = JsonPick(d, {"url", "playUrl", "link"});
        else if (d.is_string()) out.url = d.get<std::string>();
    }
    const std::string parse_v = JsonPick(v, {"parse", "jx", "isParse"});
    if (parse_v == "1" || parse_v == "2" || parse_v == "true") out.parse = 1;
    if (v.contains("header")) CollectHeaders(v["header"], out.headers);
    if (v.contains("headers")) CollectHeaders(v["headers"], out.headers);
    const std::string ua = JsonPick(v, {"ua", "userAgent", "user-agent", "User-Agent"});
    if (!ua.empty()) PutHeader(out.headers, "User-Agent", ua);
    return out;
}

// 依次尝试 TVBox / catvod / drpy 的播放器入口约定。
bool CallPlayer(QuickJsEngine& eng, const std::string& flag,
                const std::string& play_url, VideoPlayerJsResult* out) {
    JSContext* ctx = eng.context();
    JSValue global = JS_GetGlobalObject(ctx);
    std::vector<std::string> names = {
        "playerContent", "getPlayer", "player", "play", "parse", "playerUrlCall"};
    // TVBox 自定义播放器常见写法：函数名就是 flag（如 function 直连(...){}）。
    if (!flag.empty() && std::find(names.begin(), names.end(), flag) == names.end())
        names.insert(names.begin(), flag);

    for (const auto& name : names) {
        JSValue fn = JS_GetPropertyStr(ctx, global, name.c_str());
        if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); continue; }
        JSValue argv[3] = {
            JS_NewString(ctx, flag.c_str()),
            JS_NewString(ctx, play_url.c_str()),
            JS_NewArray(ctx),
        };
        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 3, argv);
        JS_FreeValue(ctx, argv[0]);
        JS_FreeValue(ctx, argv[1]);
        JS_FreeValue(ctx, argv[2]);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(ret)) {
            JS_FreeValue(ctx, ret);
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
            continue;
        }
        // 播放器也可能是 async 函数 → 泵微任务等 Promise。
        JSValue then = JS_GetPropertyStr(ctx, ret, "then");
        const bool is_promise = JS_IsFunction(ctx, then);
        JS_FreeValue(ctx, then);
        if (is_promise) {
            try {
                JSValue resolved = eng.Await(ret);
                JS_FreeValue(ctx, ret);
                ret = resolved;
            } catch (...) {
                JS_FreeValue(ctx, ret);
                continue;
            }
        }
        const json j = JsJson(ctx, ret);
        JS_FreeValue(ctx, ret);
        VideoPlayerJsResult r = InterpretResult(j, name);
        if (r.ok()) {
            JS_FreeValue(ctx, global);
            *out = std::move(r);
            return true;
        }
        out->detail += std::string(" | ") + r.detail;
    }
    JS_FreeValue(ctx, global);
    return false;
}

} // namespace

std::map<std::string, std::string> ParseVideoSiteHeaders(const VideoSite& site) {
    std::map<std::string, std::string> out;
    const std::string h = Trim(site.header);
    if (!h.empty()) {
        if (h[0] == '{') {
            try { CollectHeaders(json::parse(h), out); } catch (...) {}
        } else {
            for (const auto& seg : spider_util::Split(h, ',')) {
                const size_t p = seg.find(':');
                if (p == std::string::npos) continue;
                PutHeader(out, Trim(seg.substr(0, p)), Trim(seg.substr(p + 1)));
            }
            // 有些配置是换行分隔而不是逗号
            if (out.empty()) {
                for (const auto& seg : spider_util::Split(h, '\n')) {
                    const size_t p = seg.find(':');
                    if (p == std::string::npos) continue;
                    PutHeader(out, Trim(seg.substr(0, p)), Trim(seg.substr(p + 1)));
                }
            }
        }
    }
    if (!site.ua.empty()) PutHeader(out, "User-Agent", site.ua);
    return out;
}

bool IsBuiltinPlayerFlag(const std::string& flag) {
    const std::string f = ToLower(Trim(flag));
    if (f.empty()) return true;
    static const char* const kBuiltin[] = {
        "m3u8", "hls", "mp4", "flv", "direct", "default", "直连", "默认",
        "线路1", "线路2", "线路3", "hd", "高清", "超清", "蓝光", "标清",
        "普通", "原画", "1080p", "720p", "4k", "youku", "iqiyi", "qq",
        "web", "download", "down", "ts", "jsonapi", "dash",
    };
    for (const char* b : kBuiltin)
        if (f == b || f.find(b) != std::string::npos) return true;
    return false;
}

VideoPlayerJsResult RunVideoPlayerJs(
    const std::string& player_spec, const std::string& base_url,
    const std::string& flag, const std::string& play_url,
    const NetworkConfig& cfg,
    const std::map<std::string, std::string>& headers) {
    VideoPlayerJsResult result;
    if (Trim(player_spec).empty()) { result.detail = "未提供 playerUrl"; return result; }
    if (Trim(play_url).empty()) { result.detail = "播放地址为空"; return result; }

    HostState host;
    host.cfg = cfg;
    host.headers = headers;
    HostState* prev = g_host;
    g_host = &host;
    struct HostGuard {
        HostState* prev;
        ~HostGuard() { g_host = prev; }
    } guard{prev};

    // 多个候选 playerUrl（逗号分隔）逐个尝试；内联 JS 源码整体作为一份。
    const std::string ps = Trim(player_spec);
    const bool inline_js =
        ps.find('\n') != std::string::npos || ps.find("function ") != std::string::npos;
    const std::vector<std::string> specs =
        inline_js ? std::vector<std::string>{ps} : spider_util::Split(ps, ',');
    for (const auto& raw_spec : specs) {
        const std::string spec = Trim(raw_spec);
        if (spec.empty()) continue;
        std::string note;
        const std::string code = LoadPlayerCode(spec, base_url, cfg, headers, &note);
        if (code.empty()) {
            result.detail += std::string(" | 加载播放器脚本失败(") + note + ")";
            continue;
        }
        try {
            QuickJsEngine eng;
            eng.RefreshStackTop();
            eng.SetNetworkConfig(cfg);
            InstallHost(eng);
            JSContext* ctx = eng.context();

            // 部分脚本直接读全局变量而不是收参数。
            JSValue global = JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx, global, "flag", JS_NewString(ctx, flag.c_str()));
            JS_SetPropertyStr(ctx, global, "id", JS_NewString(ctx, play_url.c_str()));
            JS_SetPropertyStr(ctx, global, "url", JS_NewString(ctx, play_url.c_str()));
            JS_SetPropertyStr(ctx, global, "input", JS_NewString(ctx, play_url.c_str()));
            JS_SetPropertyStr(ctx, global, "vipFlags", JS_NewArray(ctx));
            JSValue params = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, params, "flag", JS_NewString(ctx, flag.c_str()));
            JS_SetPropertyStr(ctx, params, "url", JS_NewString(ctx, play_url.c_str()));
            JS_SetPropertyStr(ctx, global, "PARAMS", params);
            JS_FreeValue(ctx, global);

            JSValue eval = JS_Eval(ctx, code.c_str(), code.size(), "tvbox-player.js",
                                   JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(eval)) {
                JSValue exc = JS_GetException(ctx);
                const std::string err = JsStr(ctx, exc);
                JS_FreeValue(ctx, exc);
                result.detail += std::string(" | 脚本执行异常: ") + err;
                JS_FreeValue(ctx, eval);
                continue;
            }
            JS_FreeValue(ctx, eval);

            VideoPlayerJsResult one;
            if (CallPlayer(eng, flag, play_url, &one)) {
                one.detail += " | " + note;
                return one;
            }
            result.detail += std::string(" | 未命中播放器入口(") + note + ")" + one.detail;
        } catch (const std::exception& e) {
            result.detail += std::string(" | 宿主异常: ") + e.what();
        } catch (...) {
            result.detail += " | 宿主未知异常";
        }
    }
    if (!host.log.empty()) result.detail += " | js: " + host.log.substr(0, 256);
    return result;
}

} // namespace ttplayer::video
