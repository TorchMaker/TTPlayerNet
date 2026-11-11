#pragma once

// 音乐窗 WebView2 JSON 双向桥 —— 协议层与命令注册表
//
// 【命名说明】类名取 MusicFreeBridge（简称「桥」）：本模块专属于音乐窗
// （musicfree_* 系列）承载页面的通信协议层，取全名是为了避免与其它模块
// 未来可能出现的同名 Bridge 类冲突；对外语义即 spec 中的「JSON 双向桥」。
//
// 【协议（已定，不许改语义）】
//   页面 → C++（命令）：{"cmd":"<name>","id":<int>,"payload":{...}}
//   C++ → 页面（三类信封）：
//     事件       ：{"evt":"<name>","payload":{...}}
//     成功响应   ：{"id":N,"ok":true,"result":{...}}
//     失败响应   ：{"id":N,"ok":false,"error":"<原因>"}
//
// 【容错约定（只落日志不崩）】
//   · JSON 解析失败 / 消息不是对象 → 无 id 可回，只写失败日志；
//   · 缺 id → 只写失败日志，不回执；
//   · 缺 cmd（id 有）→ 回 {"id":N,"ok":false,"error":"..."}；
//   · 未知 cmd → 回 ok:false 并写失败日志；
//   · handler 抛异常 / 重复回执 → 捕获并归一为一次 ok:false 回执（重复的忽略）。
//
// 【日志】
//   与 protocol_web.cpp 的 AppendProtoFailLog 同一算法：按 GetModuleFileNameW
//   （nullptr = exe 模块）取所在目录，拼 "\video\bridge.log" 落盘（即与
//   play_fail.log 同一 video 目录；本模块编入 exe，等价于
//   「<模块目录>\..\video\bridge.log」在模块位于 exe 子目录时的落点约定）。
//   失败级日志始终写；debug 级需环境变量 TTPLAYER_BRIDGE_LOG 有值才写。
//
// 【线程模型假设（宿主注入时必须遵守）】
//   1. OnWebMessage 只在 UI 线程调用（WebView2 WebMessageReceived 回调线程）。
//   2. Register / PushEvent / FlushPending 可从任意线程调用：注册表与节流
//      状态由内部互斥锁保护；回调与发送均在锁外执行，允许 handler 内再入
//      Register/PushEvent。
//   3. sender 是唯一发送出口：接收一个完整 JSON 字符串，宿主拿去
//      PostWebMessageAsJson 给页面。**sender 必须在 UI 线程执行**。跨线程
//      发送由二选一保证：
//        a)（推荐）宿主注入 poster：模块内所有发送先经 poster 投递回 UI
//           线程再调 sender；宿主须保证桥析 poster 队列已排空前不析构本对象；
//        b) 不注入 poster：宿主须保证 sender 自身线程安全（如内部自行投递）。
//   4. 本模块不 include 任何 WebView2 头文件，可独立编译与无 GUI 自测
//      （tools/bridge_selftest.py 只做静态契约比对，不需要链接本文件）。
//
// 【节流】PushEvent 对高频事件（当前仅 "player.state"）内置 100ms 节流：
//   窗口内重复推送不再直接发送，只把最新一份 payload 合并挂起（旧份作废），
//   由 FlushPending / 下一次同名事件发送 / OnWebMessage 到达时补发。

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace ttplayer::musicfree {

class MusicFreeBridge {
public:
    // 回执：handler 处理完成后必须恰好调用一次（可同步、也可在工作线程异步调；
    // 异步回执要求桥实例届时仍存活 —— 销毁前等待/作废在途回执由宿主负责）。
    //   ok=true  ：result 作为成功响应的 result（null 归一为 {}）；
    //   ok=false ：error 作为失败响应的 error 文本（空则归一为 "unknown error"）。
    using ReplyFn =
        std::function<void(bool ok, nlohmann::json result, std::string error)>;

    // 命令处理器：payload 为页面消息的 payload 字段（缺失时为 {}）；
    // reply 由模块封装成协议信封并经 sender 发出，handler 不必自己拼 JSON。
    using Handler =
        std::function<void(const nlohmann::json& payload, ReplyFn reply)>;

    MusicFreeBridge();
    ~MusicFreeBridge() = default;

    MusicFreeBridge(const MusicFreeBridge&) = delete;
    MusicFreeBridge& operator=(const MusicFreeBridge&) = delete;

    // —— 宿主注入的发送出口（启动时赋值一次，运行期不要更换）——
    // 收到的字符串是要 PostWebMessageAsJson 给页面的完整 JSON。
    std::function<void(const std::string& json)> sender;
    // 可选：把一个任务投递回 UI 线程执行（宿主可注入 PostMessage / SendMessage
    // 之类的投递器）。注入后所有发送都走它；不注入则按「调用线程即 UI 线程」
    // 的约定直接调 sender（见文件头线程模型假设第 3 条）。
    std::function<void(std::function<void()> task)> poster;

    // 注册命令处理器。重复注册以后者为准并落一条 debug 日志。
    // 注册表不要求 cmd 必须在 kSupportedCmds 内（后续任务可增量注册）；
    // 页面发来未注册的 cmd 一律按「未知 cmd」回 ok:false。
    void Register(const std::string& cmd, Handler h);

    // 收页面消息：解析 → 查注册表 → 调 handler → 经 sender 回响应。
    // 仅在 UI 线程调用。任何输入错误都只落日志 + 尽量回 ok:false，不抛出。
    void OnWebMessage(const std::string& json);

    // 推事件到页面（任意线程）。高频事件（player.state）内置 100ms 节流合并；
    // "bridge.ready" 全程只发一次（先到者生效，重复推送忽略并落 debug 日志），
    // 以免宿主与 app.init 握手各推一次造成页面重复初始化。
    void PushEvent(const std::string& evt, const nlohmann::json& payload);

    // 把处于节流窗口内、被挂起（合并）的高频事件立即补发。
    // 宿主可用约 100ms 的定时器周期调用；OnWebMessage 每次处理消息时也会调用。
    void FlushPending();

    // 首批 cmd / evt 契约清单（定义在 .cpp）：给 app.init 回显能力表，
    // 同时是 tools/bridge_selftest.py 的抽取锚点 —— 需与 webui/app.js 中的
    // SUPPORTED_CMDS / SUPPORTED_EVTS 保持一致（自测会正则抽取两边比对）。
    static const char* const kSupportedCmds[];
    static const char* const kSupportedEvts[];

    // 高频事件节流窗口（毫秒）。见 .cpp 中 IsHighFreqEvt 的高频名单。
    static constexpr int kHighFreqThrottleMs = 100;

private:
    // 注册内置命令（ping / app.init），构造时调用。
    void RegisterBuiltins();
    // 经 poster（若有）投递后调用 sender；sender 未注入时只落 debug 日志。
    void SendJson(const std::string& json_text);

    std::mutex mtx_;  // 保护下面全部成员

    std::unordered_map<std::string, Handler> handlers_;             // cmd → 处理器
    std::unordered_map<std::string, long long> last_sent_ms_;       // 高频 evt 上次真正发送的 tick
    std::unordered_map<std::string, nlohmann::json> pending_;       // 节流窗口内挂起的最新 payload（合并）
    bool ready_sent_ = false;  // bridge.ready 是否已发过（只发一次）
};

}  // namespace ttplayer::musicfree
