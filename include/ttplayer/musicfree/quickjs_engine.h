#pragma once

// MusicFree 兼容层 —— QuickJS 沙箱封装
//
// 职责：
//  * 创建/销毁 QuickJS runtime + context（每个插件一个，避免互相污染全局）。
//  * 以 MusicFree 的 CommonJS 方式挂载插件：
//        function(require, __musicfree_require, module, exports, console, env, URL, process){ <code> }
//    并读取 module.exports（优先 .default）—— 完全复刻 plugin.ts 的 mountPlugin。
//  * 提供 require() 解析：axios(→WinHTTP 后端) 与若干纯 JS 库(crypto-js/dayjs/qs/...)。
//  * 调用插件的 async 方法并用 JS_ExecutePendingJob 泵微任务来 await Promise。
//
// 关键设计：axios 后端执行“阻塞式”HTTP，在 C++ 侧拿到响应后再同步 resolve Promise。
// 这样整个 await 链（插件 await axios → 继续 async 函数 → resolve 顶层 Promise）
// 都只依赖微任务队列，无需外部事件循环即可在 Await() 内被泵完。

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

// quickjs-ng 将 JSValue 定义为 typedef struct JSValue *JSValue（指针类型），
// 单纯的前向声明会得到“不完整类型”。这里直接包含真实头文件。
#include <quickjs.h>

#include "ttplayer/musicfree/musicfree_types.h"

namespace ttplayer::musicfree {

class QuickJsEngine {
public:
    QuickJsEngine();
    ~QuickJsEngine();
    QuickJsEngine(const QuickJsEngine&) = delete;
    QuickJsEngine& operator=(const QuickJsEngine&) = delete;

    // 供匿名命名空间中的 C 回调使用。
    // 注：这些回调定义在 .cpp 的匿名命名空间内，具有内部链接，
    // 无法用 friend 声明匹配（friend 只在所属命名空间内查找名字），
    // 因此改为暴露最小访问器。
    void Log(std::string_view level, std::string_view message) const {
        if (log_sink_) log_sink_(level, message);
    }

    // Await 状态（微任务回调写入）。await_state() 为 nullptr 时表示当前无等待。
    struct AwaitState {
        JSValue result = JS_UNDEFINED;
        bool done = false;
        bool rejected = false;
        std::string error;
    };
    AwaitState* await_state() const { return await_state_; }

    // 以 MusicFree 的 CommonJS 包裹方式求值插件源码，返回 module.exports
    //（调用方负责 JS_FreeValue）。语法错误抛 std::runtime_error。
    JSValue EvalPlugin(std::string_view code, std::string_view filename);

    // 求值一个纯 JS 库（UMD，使用 module/exports），返回 module.exports。
    JSValue EvalLibrary(std::string_view code, std::string_view filename);

    // 调用一个 async JS 函数（已压好参数）并 await 其 Promise，泵微任务。
    // 返回已 resolve 的值（调用方负责 JS_FreeValue），reject 时抛异常。
    JSValue Await(JSValue promise);

    // 泵空微任务队列（jobs）。
    void Pump();

    JSRuntime* runtime() const { return rt_; }
    JSContext* context() const { return ctx_; }

    // QuickJS 在 JS_NewRuntime() 所在线程记录栈顶，用它检查 JS 栈溢出。
    // 插件挂载 / 歌单调用跑在后台线程，线程栈地址不同会让检查把「已用栈」
    // 算成一个巨大的值，随机报 Maximum call stack size exceeded（表现为同一张
    // 歌单第一次点没曲目、后面又正常）。官方要求换线程后调用下面这个 API。
    void RefreshStackTop() { if (rt_) JS_UpdateStackTop(rt_); }

    // 注册原生 require 包：name -> factory(ctx) 返回 module.exports（引擎托管释放）。
    using RequireFactory = std::function<JSValue(JSContext*)>;
    void RegisterRequire(std::string name, RequireFactory factory);

    // 设置全局对象（console / env / URL / process 等）。值被引擎接管。
    void SetGlobal(std::string_view name, JSValue value);

    // 注入控制台回调（默认打到 OutputDebugString / 忽略）。
    using LogSink = std::function<void(std::string_view level, std::string_view message)>;
    void SetLogSink(LogSink sink) { log_sink_ = std::move(sink); }

    // JSON <-> JSValue 互转（静态工具）
    static JSValue JsonToValue(JSContext* ctx, const nlohmann::json& j);
    static nlohmann::json ValueToJson(JSContext* ctx, JSValueConst v);

    // 取已注册 require 模块（带缓存），供 require 全局函数使用。
    JSValue ResolveRequire(std::string_view name);

    // 设置网络配置（axios 后端读取）。
    void SetNetworkConfig(NetworkConfig cfg) { network_ = std::move(cfg); }
    const NetworkConfig& network() const { return network_; }

    // 设置 app 版本（写入 env.appVersion / process.version）。
    void SetAppVersion(std::string_view v) { app_version_ = std::string(v); }

    // 设置纯 JS 依赖库目录（crypto-js/dayjs/qs/... 的 .js 文件所在处）。
    void SetLibDir(std::filesystem::path dir) { lib_dir_ = std::move(dir); }

private:
    void InitGlobals();
    void InitRequireRegistry();

    JSRuntime* rt_{};
    JSContext* ctx_{};

    AwaitState* await_state_ = nullptr;

    std::unordered_map<std::string, RequireFactory> factories_;
    std::unordered_map<std::string, JSValue> resolved_; // 缓存的 require 模块
    LogSink log_sink_;
    NetworkConfig network_;
    std::string app_version_ = "0.1.0";
    std::filesystem::path lib_dir_;
};

} // namespace ttplayer::musicfree
