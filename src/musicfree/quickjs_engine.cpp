// MusicFree 兼容层 —— QuickJS 沙箱实现
//
// 复刻 MusicFree plugin.ts 的关键行为：
//  * mountPlugin：用 function(require,__musicfree_require,module,exports,
//    console,env,URL,process){ <code> } 包裹插件源码，读取 module.exports。
//  * _require：把固定的一组包名映射到内置实现（axios → WinHTTP；crypto-js 等
//    → 纯 JS 库；cheerio/webdav 等 → 尽力实现/置空）。

#include "ttplayer/musicfree/quickjs_engine.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <quickjs.h>
#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/musicfree_http.h"

namespace ttplayer::musicfree {

// 前向声明（定义见本文件末尾）
std::string Base64Encode(const uint8_t* data, size_t size);

namespace {

// 把 JS 异常转成可读字符串并清除。
std::string ExceptionText(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    std::string text;
    const char* c = JS_ToCString(ctx, exc);
    if (c) { text = c; JS_FreeCString(ctx, c); }
    // 附带堆栈（如存在）
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (JS_IsString(stack)) {
        const char* s = JS_ToCString(ctx, stack);
        if (s) { text += "\n" + std::string(s); JS_FreeCString(ctx, s); }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exc);
    return text;
}

void ThrowJs(JSContext* ctx, const char* where) {
    throw std::runtime_error(std::string(where) + ": " + ExceptionText(ctx));
}

// URL 全局的轻量 polyfill（Node 风格，够用即可）。
const char kUrlPolyfill[] = R"js(
(function () {
  function URL(input, base) {
    if (base) {
      try { input = new URL(base).toString().replace(/\/[^\/]*$/, '/') + input.replace(/^\//, ''); }
      catch (e) { /* ignore */ }
    }
    var m = /^([a-z]+:)\/\/([^@]+@)?([^:/?#]+)(:\d+)?(\/[^?#]*)?(\?[^#]*)?(#.*)?$/i.exec(input);
    if (!m) throw new Error('Invalid URL: ' + input);
    this.protocol = m[1];
    this.username = m[2] ? decodeURIComponent(m[2].split(':')[0]) : '';
    this.password = m[2] ? decodeURIComponent((m[2].split(':')[1] || '').replace('@', '')) : '';
    this.hostname = m[3] || '';
    this.port = m[4] ? m[4].slice(1) : '';
    this.host = (this.username || this.password ? this.username + ':' + this.password + '@' : '') + this.hostname + (this.port ? ':' + this.port : '');
    this.pathname = m[5] || '/';
    this.search = m[6] || '';
    this.hash = m[7] || '';
    this.origin = this.protocol + '//' + this.host;
  }
  URL.prototype.toString = function () { return this.protocol + '//' + this.host + this.pathname + this.search + this.hash; };
  Object.defineProperty(URL.prototype, 'searchParams', {
    get: function () {
      var sp = {};
      this.search.replace(/^\?/, '').split('&').forEach(function (p) {
        if (!p) return; var kv = p.split('='); sp[decodeURIComponent(kv[0])] = decodeURIComponent(kv[1] || '');
      });
      return { get: function (k) { return sp[k]; }, set: function (k, v) { sp[k] = v; }, has: function (k) { return k in sp; }, getAll: function (k) { return [sp[k]]; }, append: function (k, v) { sp[k] = v; } };
    }
  });
  return URL;
})()
)js";

// console.* 方法：把参数格式化为字符串后交给 LogSink。
JSValue ConsoleThunk(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv,
                     const char* level) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i) msg += ' ';
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, argv[i]);
        msg += j.is_string() ? j.get<std::string>() : j.dump();
    }
    if (eng) eng->Log(level, msg);
    return JS_UNDEFINED;
}
#define CONSOLE_FN(Name, Level)                                                 \
    JSValue Console##Name(JSContext* ctx, JSValueConst t, int a, JSValueConst* v) \
    { return ConsoleThunk(ctx, t, a, v, Level); }
CONSOLE_FN(Log, "log")
CONSOLE_FN(Info, "info")
CONSOLE_FN(Warn, "warn")
CONSOLE_FN(Error, "error")
CONSOLE_FN(Debug, "debug")
#undef CONSOLE_FN

// env.userVariables 等：返回空对象（TTPlayer 暂无用户变量概念）。
JSValue EmptyObjectFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewObject(ctx);
}

// require / __musicfree_require 全局函数。
JSValue RequireThunk(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    const char* name = JS_ToCString(ctx, argv[0]);
    std::string nm = name ? name : "";
    if (name) JS_FreeCString(ctx, name);
    try {
        return eng->ResolveRequire(nm);
    } catch (const std::exception& e) {
        return JS_Throw(ctx, JS_NewString(ctx, e.what()));
    }
}

// Await 用的微任务回调：把结果/错误写入 await_state_。
JSValue AwaitFulfilled(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    if (auto* st = eng->await_state()) {
        st->done = true;
        st->rejected = false;
        st->result = JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    }
    return JS_UNDEFINED;
}
// 把 Promise 的拒绝原因转成可读文本。
// Error 对象取 message（必要时附加 stack 首行），字符串原样返回，其余退化为 JSON。
// 若统一走 JSON 序列化，Error 对象会变成 "{}"（message/stack 均不可枚举），
// 用户只会看到「插件调用被拒绝: {}」，完全无法定位问题。
std::string DescribeRejection(JSContext* ctx, JSValueConst value) {
    if (JS_IsString(value)) {
        const char* text = JS_ToCString(ctx, value);
        std::string out = text ? text : "";
        if (text) JS_FreeCString(ctx, text);
        return out;
    }
    if (JS_IsObject(value)) {
        std::string out;
        JSValue message = JS_GetPropertyStr(ctx, value, "message");
        if (JS_IsString(message)) {
            const char* text = JS_ToCString(ctx, message);
            if (text) { out = text; JS_FreeCString(ctx, text); }
        }
        JS_FreeValue(ctx, message);
        JSValue stack = JS_GetPropertyStr(ctx, value, "stack");
        if (JS_IsString(stack)) {
            const char* text = JS_ToCString(ctx, stack);
            if (text) {
                std::string first_line = text;
                const size_t newline = first_line.find('\n');
                if (newline != std::string::npos) first_line.resize(newline);
                if (!first_line.empty() && first_line != out)
                    out += out.empty() ? first_line : (" | " + first_line);
                JS_FreeCString(ctx, text);
            }
        }
        JS_FreeValue(ctx, stack);
        if (!out.empty()) return out;
    }
    try {
        nlohmann::json j = QuickJsEngine::ValueToJson(ctx, value);
        if (j.is_string()) return j.get<std::string>();
        if (j.is_null()) return "null";
        return j.dump();
    } catch (...) {
        return "(无法序列化拒绝原因)";
    }
}

JSValue AwaitRejected(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* eng = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
    if (auto* st = eng->await_state()) {
        st->done = true;
        st->rejected = true;
        try {
            st->error = DescribeRejection(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        } catch (...) {
            st->error = "(无法序列化拒绝原因)";
        }
        st->result = JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

QuickJsEngine::QuickJsEngine() {
    rt_ = JS_NewRuntime();
    ctx_ = JS_NewContext(rt_);
    if (!rt_ || !ctx_) throw std::runtime_error("QuickJsEngine: 无法创建 runtime/context");
    JS_SetContextOpaque(ctx_, this);
    // 注意：JS_NewContext() 内部已经依次调用
    //   JS_AddIntrinsicBaseObjects / Date / Eval / RegExp / JSON /
    //   Proxy / MapSet / TypedArrays / Promise / BigInt / WeakRef，
    // 并执行 JS_AddPerformance()。此处**不能**重复添加，否则会重复
    // 初始化 class_proto 并导致进程 abort（exit 3）。
    // 若将来确需手工挑选内建对象，应改用 JS_NewContextRaw()。
    InitRequireRegistry();
    InitGlobals();
}

QuickJsEngine::~QuickJsEngine() {
    for (auto& kv : resolved_) JS_FreeValue(ctx_, kv.second);
    resolved_.clear();
    if (ctx_) JS_FreeContext(ctx_);
    if (rt_) JS_FreeRuntime(rt_);
}

void QuickJsEngine::SetGlobal(std::string_view name, JSValue value) {
    // JS_GetGlobalObject() 返回**新的引用**（refcount++），必须显式释放，
    // 否则全局对象永远带着外部引用活着，JS_FreeRuntime() 会在
    // assert(list_empty(&rt->gc_obj_list)) 处 abort（退出码 3）。
    JSValue global = JS_GetGlobalObject(ctx_);
    JS_SetPropertyStr(ctx_, global, std::string(name).c_str(), value);
    JS_FreeValue(ctx_, global);
}

JSValue QuickJsEngine::EvalPlugin(std::string_view code, std::string_view filename) {
    RefreshStackTop();  // 挂载通常在后台线程，换线程后必须刷新栈基准
    // 与 plugin.ts 完全一致：用 Function('return function(...){ ... }')() 包裹，
    // 使 JS_Eval 返回内部函数（裸 function 声明在 script 中求值结果为 undefined）。
    std::string src = std::string(
        "(function(){ return function(require,__musicfree_require,module,exports,console,env,URL,process){\n")
        + std::string(code) + "\n}; })()";
    JSValue func = JS_Eval(ctx_, src.c_str(), src.size(),
                            std::string(filename).c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(func)) ThrowJs(ctx_, "EvalPlugin.compile");

    JSValue module = JS_NewObject(ctx_);
    JSValue exports = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, module, "exports", JS_DupValue(ctx_, exports));

    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue req = JS_GetPropertyStr(ctx_, global, "require");
    JSValue cons = JS_GetPropertyStr(ctx_, global, "console");
    JSValue env = JS_GetPropertyStr(ctx_, global, "env");
    JSValue url = JS_GetPropertyStr(ctx_, global, "URL");
    JSValue proc = JS_GetPropertyStr(ctx_, global, "process");
    JS_FreeValue(ctx_, global);  // 取出属性后立即释放全局对象引用

    JSValue args[8] = { req, req, module, exports, cons, env, url, proc };
    JSValue ret = JS_Call(ctx_, func, JS_UNDEFINED, 8, args);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx_, func); JS_FreeValue(ctx_, module); JS_FreeValue(ctx_, exports);
        JS_FreeValue(ctx_, req); JS_FreeValue(ctx_, cons); JS_FreeValue(ctx_, env);
        JS_FreeValue(ctx_, url); JS_FreeValue(ctx_, proc); JS_FreeValue(ctx_, ret);
        ThrowJs(ctx_, "EvalPlugin.run");
    }
    JS_FreeValue(ctx_, ret);
    JS_FreeValue(ctx_, func);

    JSValue mod_exports = JS_GetPropertyStr(ctx_, module, "exports");
    JSValue def = JS_GetPropertyStr(ctx_, mod_exports, "default");
    JSValue result = JS_IsObject(def) ? JS_DupValue(ctx_, def) : JS_DupValue(ctx_, mod_exports);

    JS_FreeValue(ctx_, def);
    JS_FreeValue(ctx_, mod_exports);
    JS_FreeValue(ctx_, module);
    JS_FreeValue(ctx_, exports);
    JS_FreeValue(ctx_, req); JS_FreeValue(ctx_, cons); JS_FreeValue(ctx_, env);
    JS_FreeValue(ctx_, url); JS_FreeValue(ctx_, proc);
    return result; // 调用方负责释放
}

JSValue QuickJsEngine::EvalLibrary(std::string_view code, std::string_view filename) {
    RefreshStackTop();
    // UMD 库检测 module.exports / exports / define。提供 module+exports+require。
    // 用 IIFE 返回内部函数，避免裸 function 声明求值为 undefined。
    std::string src = std::string(
        "(function(){ return function(module,exports,require,define){\n")
        + std::string(code) + "\n}; })()";
    JSValue func = JS_Eval(ctx_, src.c_str(), src.size(),
                            std::string(filename).c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(func)) ThrowJs(ctx_, "EvalLibrary.compile");

    JSValue module = JS_NewObject(ctx_);
    JSValue exports = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, module, "exports", JS_DupValue(ctx_, exports));
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue req = JS_GetPropertyStr(ctx_, global, "require");
    JS_FreeValue(ctx_, global);  // 取出属性后立即释放全局对象引用

    JSValue args[4] = { module, exports, req, JS_UNDEFINED };
    JSValue ret = JS_Call(ctx_, func, JS_UNDEFINED, 4, args);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx_, func); JS_FreeValue(ctx_, module); JS_FreeValue(ctx_, exports);
        JS_FreeValue(ctx_, req); JS_FreeValue(ctx_, ret);
        ThrowJs(ctx_, "EvalLibrary.run");
    }
    JS_FreeValue(ctx_, ret);
    JS_FreeValue(ctx_, func);

    JSValue mod_exports = JS_GetPropertyStr(ctx_, module, "exports");
    JS_FreeValue(ctx_, module);
    JS_FreeValue(ctx_, exports);
    JS_FreeValue(ctx_, req);
    return mod_exports; // 调用方负责释放
}

JSValue QuickJsEngine::Await(JSValue promise) {
    RefreshStackTop();
    JSValue then = JS_GetPropertyStr(ctx_, promise, "then");
    if (!JS_IsFunction(ctx_, then)) {
        JS_FreeValue(ctx_, then);
        return promise; // 不是 Promise，原样返回（调用方释放）
    }
    AwaitState state{};
    await_state_ = &state;

    JSValue on_fulfill = JS_NewCFunction(ctx_, &AwaitFulfilled, "onFulfilled", 1);
    JSValue on_reject = JS_NewCFunction(ctx_, &AwaitRejected, "onRejected", 1);
    JSValue cb_args[2] = { on_fulfill, on_reject };
    JSValue call = JS_Call(ctx_, then, promise, 2, cb_args);
    const bool then_failed = JS_IsException(call);
    JS_FreeValue(ctx_, call);
    JS_FreeValue(ctx_, then);
    JS_FreeValue(ctx_, on_fulfill);
    JS_FreeValue(ctx_, on_reject);
    if (then_failed) {
        // 「Promise 未决 (jobs=0)」有一部分是这里抛出的 JS 异常被静默丢弃：
        // then 调用失败后队列自然是空的，上层只看到一句没有线索的超时。
        const std::string text = ExceptionText(ctx_);
        await_state_ = nullptr;
        throw std::runtime_error("MusicFree 插件 Promise.then 调用异常: " + text);
    }

    JSContext* pctx = nullptr;
    int jobs = 0;
    // 微任务队列暂时为空不等于调用失败：插件刚挂载时，宿主侧的异步回调
    // （网络 / 定时器 / 分片解析）可能稍后才把 job 排进来。以前这里一见到
    // rc==0 就 break，导致冷启动后第一次打开歌单必定报「Promise 未决(jobs=0)」，
    // 而第二次点同一张卡片又正常。这里给一个短的泵送窗口，仍不决才算超时。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(1500);
    while (!state.done) {
        int rc = JS_ExecutePendingJob(rt_, &pctx);
        if (rc > 0) { ++jobs; continue; }
        // rc < 0 表示某个微任务抛了未捕获异常：把异常文本取出来报告，
        // 否则 state.result 会一直保持初值 JS_UNDEFINED，上层只看到「返回 null」。
        if (rc < 0) {
            JSContext* ectx = pctx ? pctx : ctx_;
            JSValue exc = JS_GetException(ectx);
            const char* c = JS_ToCString(ectx, exc);
            std::string text = c ? c : "(无法读取异常)";
            if (c) JS_FreeCString(ectx, c);
            JS_FreeValue(ectx, exc);
            await_state_ = nullptr;
            if (!JS_IsUndefined(state.result)) JS_FreeValue(ctx_, state.result);
            throw std::runtime_error("MusicFree 插件异步任务异常 (jobs=" +
                                     std::to_string(jobs) + "): " + text);
        }
        // rc == 0：队列已空。让出 CPU 再泵一次，而不是立刻判超时。
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    await_state_ = nullptr;

    if (state.rejected) {
        if (!JS_IsUndefined(state.result)) JS_FreeValue(ctx_, state.result);
        throw std::runtime_error("MusicFree 插件调用被拒绝: " + state.error);
    }
    if (!state.done) {
        // 以前这里直接返回初值 JS_UNDEFINED，调用方拿到 null 且拿不到任何线索。
        if (!JS_IsUndefined(state.result)) JS_FreeValue(ctx_, state.result);
        throw std::runtime_error("MusicFree 插件调用超时：Promise 未决 (jobs=" +
                                 std::to_string(jobs) + ")");
    }
    return state.result; // 调用方负责释放
}

void QuickJsEngine::Pump() {
    JSContext* pctx = nullptr;
    while (JS_ExecutePendingJob(rt_, &pctx) > 0) {}
}

void QuickJsEngine::RegisterRequire(std::string name, RequireFactory factory) {
    factories_[name] = std::move(factory);
}

JSValue QuickJsEngine::ResolveRequire(std::string_view name) {
    std::string nm(name);
    auto cached = resolved_.find(nm);
    if (cached != resolved_.end()) return JS_DupValue(ctx_, cached->second);
    auto it = factories_.find(nm);
    if (it == factories_.end())
        throw std::runtime_error("无法解析模块: " + nm + "（该 require 包未提供）");
    JSValue v = it->second(ctx_); // 工厂负责产生 module.exports
    resolved_[nm] = JS_DupValue(ctx_, v); // 缓存（引擎托管释放）
    return v;
}

void QuickJsEngine::InitGlobals() {
    // require / __musicfree_require
    // 注意：JS_SetPropertyStr() **接管** value 的所有权（相当于消费一个引用），
    // 因此第一次 SetGlobal 之后 req 已被消费，不能再对它调用 JS_FreeValue，
    // 否则引用计数下溢 → JS_FreeRuntime 时 assert(p->ref_count > 0) abort。
    JSValue req = JS_NewCFunction(ctx_, &RequireThunk, "require", 1);
    SetGlobal("require", JS_DupValue(ctx_, req));
    SetGlobal("__musicfree_require", req);  // 移交本函数持有的原始引用

    // console
    JSValue console = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, console, "log", JS_NewCFunction(ctx_, &ConsoleLog, "log", 0));
    JS_SetPropertyStr(ctx_, console, "info", JS_NewCFunction(ctx_, &ConsoleInfo, "info", 0));
    JS_SetPropertyStr(ctx_, console, "warn", JS_NewCFunction(ctx_, &ConsoleWarn, "warn", 0));
    JS_SetPropertyStr(ctx_, console, "error", JS_NewCFunction(ctx_, &ConsoleError, "error", 0));
    JS_SetPropertyStr(ctx_, console, "debug", JS_NewCFunction(ctx_, &ConsoleDebug, "debug", 0));
    SetGlobal("console", console);

    // URL
    JSValue url = JS_Eval(ctx_, kUrlPolyfill, sizeof(kUrlPolyfill) - 1, "url-polyfill",
                          JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(url)) SetGlobal("URL", url);
    else { JS_FreeValue(ctx_, url); SetGlobal("URL", JS_NewObject(ctx_)); }

    // fetch 全局（部分插件直接用 fetch 而非 axios）
    SetGlobal("fetch", CreateFetchGlobal(ctx_));

    // env
    JSValue env = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, env, "appVersion", JS_NewString(ctx_, app_version_.c_str()));
    JS_SetPropertyStr(ctx_, env, "os", JS_NewString(ctx_, "android"));
    JS_SetPropertyStr(ctx_, env, "lang", JS_NewString(ctx_, "zh-CN"));
    JS_SetPropertyStr(ctx_, env, "getUserVariables",
                      JS_NewCFunction(ctx_, &EmptyObjectFn, "getUserVariables", 0));
    JS_SetPropertyStr(ctx_, env, "userVariables", JS_NewObject(ctx_));
    SetGlobal("env", env);

    // process
    JSValue process = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, process, "platform", JS_NewString(ctx_, "android"));
    JS_SetPropertyStr(ctx_, process, "version", JS_NewString(ctx_, app_version_.c_str()));
    JS_SetPropertyStr(ctx_, process, "env", JS_DupValue(ctx_, env));
    SetGlobal("process", process);
}

void QuickJsEngine::InitRequireRegistry() {
    // axios —— 唯一必须落到 C++ 的包（真实网络）。
    RegisterRequire("axios", [this](JSContext* c) {
        return CreateAxiosModule(c, network_, this);
    });

    // 纯 JS 库：首次 require 时按需从 lib_dir_ 读取并求值（module.exports 缓存）。
    auto js_lib = [this](const std::string& file) -> RequireFactory {
        return [this, file](JSContext* c) -> JSValue {
            if (lib_dir_.empty())
                throw std::runtime_error("MusicFree lib 目录未设置，无法加载 " + file);
            std::filesystem::path p = lib_dir_ / file;
            if (!std::filesystem::exists(p))
                throw std::runtime_error("缺少 MusicFree 依赖库: " + p.string());
            JSValue v = EvalLibrary(ReadFile(p), file);
            if (JS_IsException(v)) ThrowJs(c, ("加载库 " + file).c_str());
            return v;
        };
    };
    // 注意：这里只登记“已知包名 -> 文件名”的映射；若某插件 require 了未被
    // 登记或文件缺失的包，会以“无法解析模块”失败（与 MusicFree 行为一致）。
    RegisterRequire("crypto-js", js_lib("crypto-js.js"));
    RegisterRequire("dayjs", js_lib("dayjs.js"));
    RegisterRequire("qs", js_lib("qs.js"));
    RegisterRequire("big-integer", js_lib("big-integer.js"));
    RegisterRequire("he", js_lib("he.js"));

    // cheerio —— 完整实现需 parse5 等依赖，单个 UMD 不可得；提供尽力 shim：
    // 支持 cheerio.load(html) 返回带 .text()/.attr()/.find()/.each() 的 $，
    // 但仅做极简解析（够多数“取文本/属性”场景；复杂选择器可能不准确）。
    RegisterRequire("cheerio", [this](JSContext* c) -> JSValue {
        return CreateCheerioShim(c);
    });

    // 以下包在音乐源中几乎不用，且依赖原生/网络，置为空对象（与 MusicFree 的
    // @react-native-cookies/cookies 一致，均为 no-op）。
    RegisterRequire("@react-native-cookies/cookies", [](JSContext* c) -> JSValue {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "get", JS_NewCFunction(c, &EmptyObjectFn, "get", 0));
        JS_SetPropertyStr(c, o, "set", JS_NewCFunction(c, &EmptyObjectFn, "set", 0));
        JS_SetPropertyStr(c, o, "flush", JS_NewCFunction(c, &EmptyObjectFn, "flush", 0));
        return o;
    });
    RegisterRequire("webdav", [](JSContext* c) -> JSValue { return JS_NewObject(c); });
}

// ---------------------------------------------------------------------------
// JSON <-> JSValue
// ---------------------------------------------------------------------------

JSValue QuickJsEngine::JsonToValue(JSContext* ctx, const nlohmann::json& j) {
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    if (j.is_null()) return JS_NULL;
    if (j.is_boolean()) return JS_NewBool(ctx, j.get<bool>());
    if (j.is_number()) {
        // nlohmann 3.11 无 is_integer()；整数需分别处理有/无符号。
        // quickjs-ng 没有 JS_NewUint64：超出 int64 范围的 uint64 走 BigInt，
        // 否则用 JS_NewInt64（能放 int32 时会自动收窄为 int32）。
        if (j.is_number_unsigned()) {
            std::uint64_t u = j.get<std::uint64_t>();
            if (u <= static_cast<std::uint64_t>(INT64_MAX))
                return JS_NewInt64(ctx, static_cast<std::int64_t>(u));
            return JS_NewBigUint64(ctx, u);
        }
        if (j.is_number_integer()) return JS_NewInt64(ctx, j.get<std::int64_t>());
        return JS_NewFloat64(ctx, j.get<double>());
    }
    if (j.is_string()) return JS_NewString(ctx, j.get<std::string>().c_str());
    if (j.is_array()) {
        JSValue arr = JS_NewArray(ctx);
        uint32_t i = 0;
        for (const auto& e : j) {
            JS_SetPropertyUint32(ctx, arr, i++, JsonToValue(ctx, e));
        }
        return arr;
    }
    if (j.is_object()) {
        JSValue obj = JS_NewObject(ctx);
        for (auto it = j.begin(); it != j.end(); ++it) {
            JS_SetPropertyStr(ctx, obj, it.key().c_str(), JsonToValue(ctx, it.value()));
        }
        return obj;
    }
    return JS_NULL;
}

nlohmann::json QuickJsEngine::ValueToJson(JSContext* ctx, JSValueConst v) {
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    // 注意：quickjs-ng 的 JS_VALUE_GET_TAG 对引用类型返回的是“加掩码后的值”
    //（对象 -1 → 15、字符串 -7 → 9），因此不能用 switch(JS_TAG_OBJECT) 判断。
    // 这里改用谓词函数，保证正确性。
    if (JS_IsNull(v) || JS_IsUndefined(v)) return nullptr;
    if (JS_IsBool(v)) return JS_ToBool(ctx, v) != 0;
    if (JS_IsBigInt(ctx, v)) {
        // 超过 2^53 的大整数（如音源 ID）走 BigInt，必须原样保留为整型，
        // 不能经 double 回传（否则丢精度）。先试无符号（覆盖正值与大 uint64），
        // 失败（负值）再试有符号。
        uint64_t u = 0;
        if (JS_ToBigUint64(ctx, &u, v) == 0) return u;
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, v) == 0) return i;
        return nullptr;
    }
    if (JS_IsNumber(v)) {
        int tag = JS_VALUE_GET_TAG(v);
        if (tag == JS_TAG_INT) {
            // 普通整型：非负值按 uint64 输出（保留无符号语义，且 2^53+1
            // 这类值能无损回传），负值按 int64。
            int64_t i = 0;
            JS_ToInt64(ctx, &i, v);
            return i >= 0 ? static_cast<uint64_t>(i) : i;
        }
        // 浮点：整数值必须输出成整数。JS 只有 double，但 JSON.stringify 不会把
        // 2716020167 写成 "2716020167.0"。歌曲 id 普遍大于 int32，QuickJS 会把
        // 它存成 float64；若原样当 double 交给 nlohmann，dump() 就会带上 ".0"，
        // 音源拼出来的解析地址变成 `...?id=2716020167.0`，服务端直接查不到歌，
        // 表现成「解析不出播放地址」。非整数仍然原样输出（nlohmann 区分 123.5）。
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v) == 0) {
            constexpr double kInt64Min = -9223372036854775808.0;
            constexpr double kInt64Max = 9223372036854775807.0;
            if (d >= kInt64Min && d <= kInt64Max) {
                const int64_t integral = static_cast<int64_t>(d);
                if (static_cast<double>(integral) == d) return integral;
            }
            return d;
        }
        return nullptr;
    }
    if (JS_IsString(v)) {
        const char* c = JS_ToCString(ctx, v);
        std::string s = c ? c : "";
        if (c) JS_FreeCString(ctx, c);
        return s;
    }
    // quickjs-ng : JS_IsArray(val) 只接受值本身（旧版 quickjs 才需要 ctx）。
    if (JS_IsArray(v)) {
        nlohmann::json arr = nlohmann::json::array();
        int64_t len = 0; JS_GetLength(ctx, v, &len);
        for (int64_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            arr.push_back(ValueToJson(ctx, e));
            JS_FreeValue(ctx, e);
        }
        return arr;
    }
    if (JS_IsArrayBuffer(v)) {
        size_t size = 0; uint8_t* ptr = JS_GetArrayBuffer(ctx, &size, v);
        if (ptr) return Base64Encode(ptr, size); // 以 base64 表示二进制
        return nullptr;
    }
    // TypedArray（如 responseType:'arraybuffer' 时插件可能拿到 Uint8Array）。
    // quickjs-ng 的 JS_GetArrayBuffer 只接受 (ctx, &size, arraybuffer)，
    // 要拿 TypedArray 底层缓冲需走 JS_GetTypedArrayBuffer 再取 .buffer。
    if (JS_IsObject(v) && JS_GetTypedArrayType(v) > 0) {
        size_t byte_offset = 0, byte_length = 0, bytes_per_element = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &byte_offset, &byte_length, &bytes_per_element);
        if (!JS_IsException(buf) && JS_IsArrayBuffer(buf)) {
            uint8_t* ptr = JS_GetArrayBuffer(ctx, &byte_length, buf);
            if (ptr) {
                // 尊重子数组视图的偏移，否则 subarray() 会读到错误数据。
                std::string b64 = Base64Encode(ptr + byte_offset,
                                               byte_length - byte_offset);
                JS_FreeValue(ctx, buf);
                return b64;
            }
        }
        JS_FreeValue(ctx, buf);
    }
    if (JS_IsObject(v)) {
        nlohmann::json obj = nlohmann::json::object();
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                const char* key = JS_AtomToCString(ctx, props[i].atom);
                if (!key) continue;
                JSValue val = JS_GetProperty(ctx, v, props[i].atom);
                obj[key] = ValueToJson(ctx, val);
                JS_FreeValue(ctx, val);
                JS_FreeCString(ctx, key);
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
        return obj;
    }
    return nullptr;
}

// Base64 编码（供 TypedArray 转字符串）。
std::string Base64Encode(const uint8_t* data, size_t size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < size) n |= data[i + 1] << 8;
        if (i + 2 < size) n |= data[i + 2];
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back((i + 1 < size) ? tbl[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < size) ? tbl[n & 63] : '=');
    }
    return out;
}

} // namespace ttplayer::musicfree
