// 洛雪 / LX Music 自定义音源适配器实现
//
// 与 MusicFree 插件不同，洛雪脚本是「普通全局脚本」而非 CommonJS 模块：
// 不能用 EvalPlugin 的 function(require,module,exports,...) 包裹，必须直接在
// 全局作用域 JS_Eval，并预先把 globalThis.lx 搭好。脚本执行过程中就会调用
// on('request', handler) 与 send('inited', {...})，所以这两者都是同步可用的。

#include "ttplayer/musicfree/lx_source_adapter.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <quickjs.h>
#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/quickjs_engine.h"

namespace ttplayer::musicfree {

namespace detail {

struct LxBridge {
    std::unique_ptr<QuickJsEngine> engine;
    JSValue request_handler = JS_UNDEFINED;
    nlohmann::json inited;
    ProtocolSourceInfo info;
    NetworkConfig network;
    std::string update_log;
    std::string last_error;

    ~LxBridge() {
        if (engine && !JS_IsUndefined(request_handler))
            JS_FreeValue(engine->context(), request_handler);
        request_handler = JS_UNDEFINED;
    }
};

} // namespace detail

namespace {

std::mutex g_bridge_mutex;
std::unordered_map<JSContext*, detail::LxBridge*> g_bridges;

void RegisterBridge(JSContext* ctx, detail::LxBridge* bridge) {
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    g_bridges[ctx] = bridge;
}

void UnregisterBridge(JSContext* ctx) {
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    g_bridges.erase(ctx);
}

detail::LxBridge* BridgeOf(JSContext* ctx) {
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    auto it = g_bridges.find(ctx);
    return it == g_bridges.end() ? nullptr : it->second;
}

std::string ExceptionText(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    std::string text;
    if (const char* c = JS_ToCString(ctx, exc)) {
        text = c;
        JS_FreeCString(ctx, c);
    }
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (JS_IsString(stack)) {
        if (const char* s = JS_ToCString(ctx, stack)) {
            text += "\n" + std::string(s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
    return text;
}

std::string JsString(JSContext* ctx, JSValueConst v) {
    if (const char* c = JS_ToCString(ctx, v)) {
        std::string s(c);
        JS_FreeCString(ctx, c);
        return s;
    }
    return {};
}

// lx.request 的 resp.body：与 axios 的 transformResponse 一致，
// 无条件尝试 JSON.parse，失败才回退原始字符串。洛雪音源几乎都读 resp.body.xxx。
JSValue BodyValue(JSContext* ctx, const std::vector<std::uint8_t>& bytes) {
    std::string text(bytes.begin(), bytes.end());
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_discarded()) return QuickJsEngine::JsonToValue(ctx, j);
    return JS_NewString(ctx, text.c_str());
}

nlohmann::json PropertyJson(JSContext* ctx, JSValueConst obj, const char* name) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    nlohmann::json j = QuickJsEngine::ValueToJson(ctx, v);
    JS_FreeValue(ctx, v);
    return j;
}

// --------------------------- lx 运行时 C 回调 ---------------------------

JSValue LxNoop(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

JSValue LxOn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    detail::LxBridge* b = BridgeOf(ctx);
    if (!b || argc < 2) return JS_UNDEFINED;
    const std::string event = argc >= 1 ? JsString(ctx, argv[0]) : std::string{};
    if (event == "request" && JS_IsFunction(ctx, argv[1])) {
        if (!JS_IsUndefined(b->request_handler))
            JS_FreeValue(ctx, b->request_handler);
        b->request_handler = JS_DupValue(ctx, argv[1]);
    }
    return JS_UNDEFINED;
}

JSValue LxSend(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    detail::LxBridge* b = BridgeOf(ctx);
    if (!b || argc < 1) return JS_UNDEFINED;
    const std::string event = JsString(ctx, argv[0]);
    if (event == "inited") {
        b->inited = argc >= 2 ? QuickJsEngine::ValueToJson(ctx, argv[1])
                              : nlohmann::json::object();
    } else if (event == "updateAlert" && argc >= 2) {
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, argv[1]);
        if (j.is_object() && j.contains("log") && j["log"].is_string())
            b->update_log = j["log"].get<std::string>();
    }
    return JS_UNDEFINED;
}

JSValue LxRequest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    detail::LxBridge* b = BridgeOf(ctx);
    if (!b || argc < 3 || !JS_IsFunction(ctx, argv[2])) return JS_UNDEFINED;

    const std::string url = JsString(ctx, argv[0]);
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;

    if (JS_IsObject(argv[1])) {
        nlohmann::json o = QuickJsEngine::ValueToJson(ctx, argv[1]);
        if (o.is_object()) {
            if (o.contains("method") && o["method"].is_string())
                method = o["method"].get<std::string>();
            if (o.contains("headers") && o["headers"].is_object()) {
                for (auto it = o["headers"].begin(); it != o["headers"].end(); ++it) {
                    if (it.value().is_string())
                        headers[it.key()] = it.value().get<std::string>();
                    else if (it.value().is_array() && !it.value().empty() &&
                             it.value()[0].is_string())
                        headers[it.key()] = it.value()[0].get<std::string>();
                    else if (!it.value().is_null())
                        headers[it.key()] = it.value().dump();
                }
            }
            if (o.contains("body")) {
                const auto& v = o["body"];
                std::string s = v.is_string() ? v.get<std::string>() : v.dump();
                body.assign(s.begin(), s.end());
            } else if (o.contains("form") && o["form"].is_object()) {
                std::string s;
                for (auto it = o["form"].begin(); it != o["form"].end(); ++it) {
                    if (!s.empty()) s += '&';
                    s += it.key() + "=" +
                         (it.value().is_string() ? it.value().get<std::string>()
                                                 : it.value().dump());
                }
                body.assign(s.begin(), s.end());
                if (headers.find("content-type") == headers.end())
                    headers["content-type"] = "application/x-www-form-urlencoded";
            }
        }
    }

    JSValue callback = argv[2];
    try {
        HttpExchange r = PerformHttpRequest(url, method, headers, body, b->network);
        JSValue resp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, resp, "statusCode",
                          JS_NewInt32(ctx, static_cast<int>(r.status)));
        JS_SetPropertyStr(ctx, resp, "statusText",
                          JS_NewString(ctx, r.status_text.c_str()));
        JSValue hdrs = JS_NewObject(ctx);
        for (const auto& kv : r.headers) {
            const std::string joined =
                kv.second.empty() ? std::string{} : kv.second.front();
            JS_SetPropertyStr(ctx, hdrs, kv.first.c_str(),
                              JS_NewString(ctx, joined.c_str()));
        }
        JS_SetPropertyStr(ctx, resp, "headers", hdrs);
        JS_SetPropertyStr(ctx, resp, "body", BodyValue(ctx, r.body));

        std::string raw(r.body.begin(), r.body.end());
        JSValue args[3] = {JS_NULL, resp, JS_NewString(ctx, raw.c_str())};
        JSValue ret = JS_Call(ctx, callback, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, resp);  // JS_Call 不接管 argv 的所有权
        if (JS_IsException(ret)) {
            b->last_error = "lx.request 回调异常: " + ExceptionText(ctx);
        }
        JS_FreeValue(ctx, ret);
    } catch (const std::exception& e) {
        std::string message = e.what();
        JSValue args[3] = {JS_NewString(ctx, message.c_str()), JS_NULL,
                           JS_UNDEFINED};
        JSValue ret = JS_Call(ctx, callback, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, args[0]);
        if (JS_IsException(ret)) {
            b->last_error = "lx.request 失败: " + ExceptionText(ctx);
        }
        JS_FreeValue(ctx, ret);
    }
    // 文档要求返回一个「可调用以终止请求」的方法；本实现是阻塞同步请求，
    // 返回空操作保持接口形状。
    return JS_NewCFunction(ctx, &LxNoop, "cancelHttp", 0);
}

// --------------------------- lx.utils / 宿主垫片 ---------------------------

const char kLxHostShim[] = R"js(
(function () {
  var C = globalThis.__lxCrypto || {};
  var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

  function b64encode(bytes) {
    var out = '', i = 0;
    for (; i + 2 < bytes.length; i += 3) {
      var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
      out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] +
             B64[(n >> 6) & 63] + B64[n & 63];
    }
    var rest = bytes.length - i;
    if (rest === 1) {
      var n1 = bytes[i] << 16;
      out += B64[(n1 >> 18) & 63] + B64[(n1 >> 12) & 63] + '==';
    } else if (rest === 2) {
      var n2 = (bytes[i] << 16) | (bytes[i + 1] << 8);
      out += B64[(n2 >> 18) & 63] + B64[(n2 >> 12) & 63] +
             B64[(n2 >> 6) & 63] + '=';
    }
    return out;
  }
  function b64decode(s) {
    s = String(s).replace(/=+$/, '');
    var out = [], i = 0;
    for (; i + 3 < s.length; i += 4) {
      var a = B64.indexOf(s[i]), b = B64.indexOf(s[i + 1]),
          c = B64.indexOf(s[i + 2]), d = B64.indexOf(s[i + 3]);
      if (a < 0 || b < 0) continue;
      var n = (a << 18) | (b << 12) | ((c < 0 ? 0 : c) << 6) | (d < 0 ? 0 : d);
      out.push((n >> 16) & 255);
      if (c >= 0) out.push((n >> 8) & 255);
      if (d >= 0) out.push(n & 255);
    }
    return new Uint8Array(out);
  }
  function utf8Bytes(s) {
    var out = [];
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
        var c2 = s.charCodeAt(++i);
        var cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
        out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63),
                 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
      } else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
  }
  function utf8String(bytes) {
    var out = '', i = 0;
    while (i < bytes.length) {
      var c = bytes[i++];
      if (c < 0x80) out += String.fromCharCode(c);
      else if (c < 0xe0) out += String.fromCharCode(((c & 31) << 6) | (bytes[i++] & 63));
      else if (c < 0xf0) out += String.fromCharCode(((c & 15) << 12) |
                                ((bytes[i++] & 63) << 6) | (bytes[i++] & 63));
      else {
        var cp = ((c & 7) << 18) | ((bytes[i++] & 63) << 12) |
                 ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
        cp -= 0x10000;
        out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 1023));
      }
    }
    return out;
  }
  function toU8(input, format) {
    format = format || 'utf8';
    if (input instanceof Uint8Array) return input;
    if (input && input.buffer instanceof ArrayBuffer) return new Uint8Array(input.buffer);
    if (Array.isArray(input)) return new Uint8Array(input);
    var s = String(input === undefined || input === null ? '' : input);
    if (format === 'base64') return b64decode(s);
    if (format === 'hex') {
      var out = [];
      for (var i = 0; i + 1 < s.length; i += 2) out.push(parseInt(s.substr(i, 2), 16));
      return new Uint8Array(out);
    }
    return utf8Bytes(s);
  }
  function fromU8(bytes, format) {
    format = format || 'utf8';
    if (format === 'base64') return b64encode(bytes);
    if (format === 'hex') {
      var out = '';
      for (var i = 0; i < bytes.length; i++)
        out += ('0' + bytes[i].toString(16)).slice(-2);
      return out;
    }
    return utf8String(bytes);
  }
  function wa(bytes) {
    var words = [], i = 0;
    for (; i + 3 < bytes.length; i += 4)
      words.push((bytes[i] << 24) | (bytes[i + 1] << 16) |
                 (bytes[i + 2] << 8) | bytes[i + 3]);
    var tail = bytes.length - i, last = 0;
    for (var k = 0; k < tail; k++) last |= bytes[i + k] << (24 - k * 8);
    if (tail) words.push(last);
    return C.lib.WordArray.create(words, bytes.length);
  }
  function u8(wordArray) {
    var words = wordArray.words, sig = wordArray.sigBytes, out = [];
    for (var i = 0; i < sig; i++) {
      var w = words[i >>> 2];
      out.push((w >>> (24 - (i % 4) * 8)) & 255);
    }
    return new Uint8Array(out);
  }
  function parseKey(k, len) {
    if (k instanceof Uint8Array || Array.isArray(k)) return wa(toU8(k));
    var s = String(k || '');
    if (len && s.length === len) return C.enc.Utf8.parse(s);
    try { return C.enc.Base64.parse(s); } catch (e) { return C.enc.Utf8.parse(s); }
  }

  var utils = {
    buffer: {
      from: function (input, format) { return toU8(input, format); },
      bufToString: function (buffer, format) { return fromU8(toU8(buffer), format); }
    },
    crypto: {
      md5: function (s) {
        return C.MD5(String(s)).toString(C.enc.Hex);
      },
      randomBytes: function (size) {
        var out = new Uint8Array(size || 16);
        for (var i = 0; i < out.length; i++) out[i] = Math.floor(Math.random() * 256);
        return out;
      },
      aesEncrypt: function (buffer, mode, key, iv) {
        if (!C.AES) throw new Error('crypto-js AES 不可用');
        var cfg = { mode: /ecb/i.test(String(mode)) ? C.mode.ECB : C.mode.CBC,
                    padding: C.pad.Pkcs7 };
        if (iv) cfg.iv = parseKey(iv);
        var enc = C.AES.encrypt(wa(toU8(buffer)), parseKey(key), cfg);
        return u8(enc.ciphertext);
      },
      rsaEncrypt: function () {
        throw new Error('lx.utils.crypto.rsaEncrypt 尚未在 TTPlayer 中实现');
      }
    },
    zlib: {
      inflate: function () { return Promise.reject(new Error('zlib.inflate 不可用')); },
      deflate: function () { return Promise.reject(new Error('zlib.deflate 不可用')); }
    }
  };
  lx.utils = utils;

  // 宿主环境：QuickJS 没有真实定时器，而音源仅用它做重试/串行化。
  // 同步立即执行可以让 Promise 链继续推进；加次数上限避免失控递归。
  var timerCount = 0;
  globalThis.setTimeout = function (fn) {
    if (++timerCount > 1000) throw new Error('setTimeout 调用次数超过上限');
    if (typeof fn === 'function') fn();
    return timerCount;
  };
  globalThis.clearTimeout = function () {};
  globalThis.setInterval = function () { return 0; };
  globalThis.clearInterval = function () {};
  // 兼容旧版 window.lx 写法。刻意不写 window = globalThis：全局对象持有
  // 自身会在 JS_FreeRuntime 的 gc_obj_list 断言上 abort。
  globalThis.window = { lx: lx,
                        setTimeout: globalThis.setTimeout,
                        clearTimeout: globalThis.clearTimeout };
})()
)js";

void SetString(JSContext* ctx, JSValue obj, const char* key,
               const std::string& value) {
    JS_SetPropertyStr(ctx, obj, key, JS_NewString(ctx, value.c_str()));
}

// 把音源平台名/搜索结果 raw 组装成洛雪脚本期待的 musicInfo。
// 洛雪脚本按平台读不同的主键：kw/tx 读 songmid、kg 读 hash、
// wy 读 id、mg 读 copyrightId / contentId。这里全部补齐，避免
// 「解析时只用 id 重建对象 → 插件私有字段丢失」这类问题。
nlohmann::json BuildMusicInfo(const ProtocolTrack& track,
                              const std::string& source) {
    nlohmann::json info = track.raw.is_object() ? track.raw
                                                : nlohmann::json::object();
    auto put = [&](const char* key, const std::string& value) {
        if (!value.empty() && (!info.contains(key) || !info[key].is_string() ||
                               info[key].get<std::string>().empty()))
            info[key] = value;
    };
    put("id", track.id);
    put("name", track.title);
    put("title", track.title);
    put("singer", track.artist);
    put("artist", track.artist);
    put("album", track.album);
    put("source", track.platform);
    if (!track.artwork.empty()) put("artwork", track.artwork);
    if (track.duration_ms > 0 && !info.contains("interval"))
        info["interval"] = track.duration_ms / 1000;
    const std::string id = track.id;
    if (source == "kw" || source == "tx") {
        put("songmid", id);
        put("mid", id);
    } else if (source == "kg") {
        put("hash", id);
        put("songmid", id);
    } else if (source == "wy") {
        put("songId", id);
    } else if (source == "mg") {
        put("copyrightId", id);
        put("contentId", id);
        put("songId", id);
    }
    return info;
}

std::string SourceDisplayName(std::string_view key) {
    if (key == "kw") return "酷我";
    if (key == "kg") return "酷狗";
    if (key == "tx") return "QQ 音乐";
    if (key == "wy") return "网易云";
    if (key == "mg") return "咪咕";
    if (key == "local") return "本地";
    return std::string(key);
}

} // namespace

// --------------------------- LxSourceAdapter ---------------------------

LxSourceAdapter::LxSourceAdapter() = default;
LxSourceAdapter::~LxSourceAdapter() {
    if (bridge_ && bridge_->engine) UnregisterBridge(bridge_->engine->context());
}

const ProtocolSourceInfo& LxSourceAdapter::info() const {
    static const ProtocolSourceInfo kEmpty{};
    return bridge_ ? bridge_->info : kEmpty;
}

bool LxSourceAdapter::IsMounted() const {
    return bridge_ && bridge_->engine && !JS_IsUndefined(bridge_->request_handler);
}

bool LxSourceAdapter::Mount(std::string_view code, const NetworkConfig& network,
                            const std::filesystem::path& lib_dir,
                            std::string_view source_url, std::string* error) {
    auto bridge = std::make_unique<detail::LxBridge>();
    bridge->network = network;
    bridge->engine = std::make_unique<QuickJsEngine>();
    bridge->engine->SetNetworkConfig(network);
    bridge->engine->SetLibDir(lib_dir);
    bridge->engine->SetAppVersion("2.0.0");
    bridge->engine->SetLogSink([bridge_ptr = bridge.get()](std::string_view level,
                                                          std::string_view msg) {
        if (level == "error" && bridge_ptr->last_error.empty())
            bridge_ptr->last_error = std::string(msg);
    });

    JSContext* ctx = bridge->engine->context();
    RegisterBridge(ctx, bridge.get());

    try {
        // crypto-js 供 lx.utils.crypto 垫片使用（与 MusicFree 插件共用一份）。
        try {
            JSValue crypto = bridge->engine->ResolveRequire("crypto-js");
            bridge->engine->SetGlobal("__lxCrypto", crypto);
        } catch (...) {
            // 缺库时垫片里的 md5/aes 会抛明确错误，但纯网络音源仍能工作。
        }

        const nlohmann::json header = ParseScriptHeader(code);
        auto hs = [&](const char* key) -> std::string {
            return header.contains(key) && header[key].is_string()
                       ? header[key].get<std::string>()
                       : std::string{};
        };

        JSValue lx = JS_NewObject(ctx);
        SetString(ctx, lx, "version", "2.0.0");
        SetString(ctx, lx, "env", "desktop");
        JSValue script_info = QuickJsEngine::JsonToValue(ctx, header);
        JS_SetPropertyStr(ctx, script_info, "rawScript",
                          JS_NewString(ctx, std::string(code).c_str()));
        JS_SetPropertyStr(ctx, lx, "currentScriptInfo", script_info);

        JSValue events = JS_NewObject(ctx);
        SetString(ctx, events, "inited", "inited");
        SetString(ctx, events, "request", "request");
        SetString(ctx, events, "updateAlert", "updateAlert");
        JS_SetPropertyStr(ctx, lx, "EVENT_NAMES", events);
        JS_SetPropertyStr(ctx, lx, "on", JS_NewCFunction(ctx, &LxOn, "on", 2));
        JS_SetPropertyStr(ctx, lx, "send",
                          JS_NewCFunction(ctx, &LxSend, "send", 2));
        JS_SetPropertyStr(ctx, lx, "request",
                          JS_NewCFunction(ctx, &LxRequest, "request", 3));
        bridge->engine->SetGlobal("lx", lx);

        JSValue shim = JS_Eval(ctx, kLxHostShim, sizeof(kLxHostShim) - 1,
                               "<lx-host-shim>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(shim)) {
            const std::string why = ExceptionText(ctx);
            JS_FreeValue(ctx, shim);
            throw std::runtime_error("宿主垫片初始化失败: " + why);
        }
        JS_FreeValue(ctx, shim);

        // 洛雪脚本是普通全局脚本，不能套 CommonJS 包装。
        const std::string filename =
            source_url.empty() ? std::string("<lx-source>") : std::string(source_url);
        JSValue evaluated = JS_Eval(ctx, code.data(), code.size(), filename.c_str(),
                                    JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(evaluated)) {
            const std::string why = ExceptionText(ctx);
            JS_FreeValue(ctx, evaluated);
            throw std::runtime_error("脚本执行失败: " + why);
        }
        JS_FreeValue(ctx, evaluated);
        bridge->engine->Pump();

        if (!bridge->inited.is_object())
            throw std::runtime_error("脚本未发送 inited 事件（不是有效的洛雪音源）");
        if (JS_IsUndefined(bridge->request_handler))
            throw std::runtime_error("脚本未用 on('request', handler) 注册处理器");

        auto& info = bridge->info;
        info.kind = SourceProtocol::kLxMusic;
        info.name = hs("name");
        info.version = hs("version");
        info.author = hs("author");
        info.description = hs("description");
        info.homepage = hs("homepage");
        info.source_url = std::string(source_url);
        if (info.name.empty()) info.name = "洛雪音源";
        if (bridge->inited.contains("sources") &&
            bridge->inited["sources"].is_object()) {
            for (auto it = bridge->inited["sources"].begin();
                 it != bridge->inited["sources"].end(); ++it) {
                info.sources.push_back(it.key());
                const auto& src = it.value();
                if (!src.is_object() || !src.contains("actions")) continue;
                const auto& actions = src["actions"];
                if (!actions.is_array()) continue;
                for (const auto& a : actions) {
                    if (!a.is_string()) continue;
                    const std::string act = a.get<std::string>();
                    if (act == "musicUrl") info.can_resolve = true;
                    if (act == "lyric") info.can_lyric = true;
                }
            }
        }
        // 即便脚本没声明 actions，只要注册了 request 处理器就允许尝试 musicUrl。
        if (!info.can_resolve) info.can_resolve = true;

        bridge_ = std::move(bridge);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        UnregisterBridge(ctx);
        return false;
    }
}

ProtocolResolveResult LxSourceAdapter::ResolveUrl(const ProtocolTrack& track) {
    ProtocolResolveResult out;
    out.via = "洛雪: " + info().name;
    if (!IsMounted()) {
        out.error = "洛雪音源未挂载";
        return out;
    }
    detail::LxBridge& b = *bridge_;
    JSContext* ctx = b.engine->context();

    std::vector<std::string> candidates;
    const std::string mapped = MapToLxSource(track.platform);
    const auto& declared = b.info.sources;
    auto has = [&](const std::string& s) {
        return std::find(declared.begin(), declared.end(), s) != declared.end();
    };
    if (!mapped.empty() && has(mapped)) candidates.push_back(mapped);
    for (const auto& s : declared)
        if (s != "local" && std::find(candidates.begin(), candidates.end(), s) ==
                                candidates.end())
            candidates.push_back(s);
    if (candidates.empty()) {
        out.error = "音源未声明任何可用的 musicUrl 源";
        return out;
    }

    std::string quality = track.quality.empty() ? std::string("128k") : track.quality;
    for (const auto& source : candidates) {
        b.last_error.clear();
        try {
            nlohmann::json arg = nlohmann::json::object();
            arg["source"] = source;
            arg["action"] = "musicUrl";
            arg["info"]["type"] = quality;
            arg["info"]["musicInfo"] = BuildMusicInfo(track, source);

            JSValue js_arg = QuickJsEngine::JsonToValue(ctx, arg);
            JSValue promise = JS_Call(ctx, b.request_handler, JS_UNDEFINED, 1,
                                      &js_arg);
            JS_FreeValue(ctx, js_arg);
            if (JS_IsException(promise)) {
                out.error = "request 调用异常: " + ExceptionText(ctx);
                JS_FreeValue(ctx, promise);
                continue;
            }
            JSValue value = JS_UNDEFINED;
            try {
                value = b.engine->Await(promise);
            } catch (const std::exception& e) {
                out.error = std::string(e.what());
            }
            JS_FreeValue(ctx, promise);
            if (JS_IsUndefined(value)) continue;

            nlohmann::json j = QuickJsEngine::ValueToJson(ctx, value);
            JS_FreeValue(ctx, value);
            if (j.is_string()) {
                const std::string url = j.get<std::string>();
                if (url.rfind("http", 0) == 0) {
                    out.url = url;
                    out.error.clear();
                    out.via += " (" + SourceDisplayName(source) + ")";
                    return out;
                }
            }
            out.error = "音源返回的不是 HTTP 直链";
        } catch (const std::exception& e) {
            out.error = e.what();
        }
    }
    if (!b.last_error.empty()) out.error += "；" + b.last_error;
    return out;
}

std::optional<LyricResult> LxSourceAdapter::ResolveLyric(const ProtocolTrack& track) {
    if (!IsMounted() || !info().can_lyric) return std::nullopt;
    detail::LxBridge& b = *bridge_;
    JSContext* ctx = b.engine->context();
    try {
        nlohmann::json arg = nlohmann::json::object();
        arg["source"] = "local";
        arg["action"] = "lyric";
        arg["info"]["musicInfo"] = BuildMusicInfo(track, "local");
        JSValue js_arg = QuickJsEngine::JsonToValue(ctx, arg);
        JSValue promise = JS_Call(ctx, b.request_handler, JS_UNDEFINED, 1, &js_arg);
        JS_FreeValue(ctx, js_arg);
        if (JS_IsException(promise)) {
            ExceptionText(ctx);
            JS_FreeValue(ctx, promise);
            return std::nullopt;
        }
        JSValue value = b.engine->Await(promise);
        JS_FreeValue(ctx, promise);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, value);
        JS_FreeValue(ctx, value);
        if (!j.is_object()) return std::nullopt;
        LyricResult lyric;
        if (j.contains("lyric") && j["lyric"].is_string())
            lyric.raw_lrc = j["lyric"].get<std::string>();
        if (j.contains("tlyric") && j["tlyric"].is_string())
            lyric.translation = j["tlyric"].get<std::string>();
        if (j.contains("tlryic") && lyric.translation.empty() &&
            j["tlryic"].is_string())
            lyric.translation = j["tlryic"].get<std::string>();
        return lyric;
    } catch (...) {
        return std::nullopt;
    }
}

std::string LxSourceAdapter::ResolvePic(const ProtocolTrack& track) {
    if (!IsMounted()) return {};
    detail::LxBridge& b = *bridge_;
    JSContext* ctx = b.engine->context();
    try {
        nlohmann::json arg = nlohmann::json::object();
        arg["source"] = "local";
        arg["action"] = "pic";
        arg["info"]["musicInfo"] = BuildMusicInfo(track, "local");
        JSValue js_arg = QuickJsEngine::JsonToValue(ctx, arg);
        JSValue promise = JS_Call(ctx, b.request_handler, JS_UNDEFINED, 1, &js_arg);
        JS_FreeValue(ctx, js_arg);
        if (JS_IsException(promise)) {
            ExceptionText(ctx);
            JS_FreeValue(ctx, promise);
            return {};
        }
        JSValue value = b.engine->Await(promise);
        JS_FreeValue(ctx, promise);
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, value);
        JS_FreeValue(ctx, value);
        return j.is_string() ? j.get<std::string>() : std::string{};
    } catch (...) {
        return {};
    }
}

std::string LxSourceAdapter::Describe() const {
    const auto& i = info();
    std::string text = "协议：洛雪 / LX Music 自定义音源 (v2.0.0)\n";
    text += "名称：" + i.name + "\n";
    if (!i.version.empty()) text += "版本：" + i.version + "\n";
    if (!i.author.empty()) text += "作者：" + i.author + "\n";
    if (!i.description.empty()) text += "说明：" + i.description + "\n";
    if (!i.homepage.empty()) text += "主页：" + i.homepage + "\n";
    if (!i.source_url.empty()) text += "来源：" + i.source_url + "\n";
    text += "支持源：";
    for (size_t n = 0; n < i.sources.size(); ++n) {
        if (n) text += "、";
        text += SourceDisplayName(i.sources[n]) + "(" + i.sources[n] + ")";
    }
    text += "\n能力：播放直链" + std::string(i.can_resolve ? " ✓" : " ✗") +
            "　歌词" + std::string(i.can_lyric ? " ✓" : " ✗") + "\n";
    if (bridge_ && !bridge_->update_log.empty())
        text += "更新提示：" + bridge_->update_log + "\n";
    text += "注意：洛雪音源不提供搜索，搜索结果仍来自 MusicFree / REST 音源；"
            "这里只负责把歌曲信息解析成直链。";
    return text;
}

} // namespace ttplayer::musicfree
