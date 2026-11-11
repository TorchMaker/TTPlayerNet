// 音乐窗 WebView2 JSON 双向桥 —— 协议层实现
//
// 职责边界：只做「解析 → 注册表分发 → 信封回执 / 事件推送（含节流）」，
// 不含任何具体业务能力（播放、视频、搜索等由后续任务注册 handler 接入）。
// 协议语义、线程假设、日志约定见 musicfree_bridge.h 文件头注释。

#include "musicfree_bridge.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttplayer::musicfree {

namespace {

using json = nlohmann::json;

// ───────────────────────── 工具 ─────────────────────────

// 高频事件名单：命中才做 100ms 节流合并（当前只有播放状态刷得最勤；
// 其余事件一律直发，保证业务时序直观）。
bool IsHighFreqEvt(std::string_view evt) { return evt == "player.state"; }

// 单调递增毫秒 tick，用于节流窗口计算（不受系统时间调整影响）。
long long NowTickMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

// 日志里截断超长原文，避免把整页消息灌进日志。
std::string TruncForLog(const std::string& s, size_t max_len = 512) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len) +
           "...(len=" + std::to_string(s.size()) + ")";
}

// 安全序列化：CMS 站点可能返回 GBK 文本（不是合法 UTF-8），nlohmann 默认
// dump 会抛 type_error.316；若这发生在 detached 工作线程的回执里，异常会
// 一路冒到 std::terminate → 进程闪退（0xc0000409）。这里统一用 replace
// 策略：非法字节替换为 U+FFFD，保证桥永远能发出 JSON。
std::string DumpJsonSafe(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// 读 JSON 对象里的字符串字段：缺失/非字符串一律给空串，绝不抛异常
//（handler 入参来自页面，不可信）。
std::string JsonStr(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();  // 数字等标量退化为字面量文本
}

// ───────────────────────── 日志 ─────────────────────────
// 与 protocol_web.cpp 的 AppendProtoFailLog 同一算法：GetModuleFileNameW(nullptr)
// 取 exe 所在目录再拼路径。本模块编入 exe，落点为
//「<exe 目录>\video\bridge.log」—— 与 play_fail.log / hls_fail.log 同处，
// 也等价于「<模块目录>\..\video\bridge.log」约定在模块位于 exe 子目录时的落点
//（两种取法最终都指向 exe 的 video 目录）。失败级始终写；
// debug 级需环境变量 TTPLAYER_BRIDGE_LOG 有值（首个调用时缓存）才写。

std::wstring BridgeLogPath() {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    const std::wstring p(exe);
    const size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return {};
    return p.substr(0, s) + L"\\video\\bridge.log";
}

bool BridgeDebugLogEnabled() {
    static const bool on = [] {
        wchar_t buf[8]{};
        // 返回 >0（含值过长）即视为环境变量存在
        return GetEnvironmentVariableW(L"TTPLAYER_BRIDGE_LOG", buf,
                                       static_cast<DWORD>(sizeof(buf) /
                                                          sizeof(buf[0]))) > 0;
    }();
    return on;
}

// 追加一个带时间戳的日志块（格式与 AppendProtoFailLog 一致）。
void AppendBridgeLog(const std::string& text, bool debug_only) {
    if (debug_only && !BridgeDebugLogEnabled()) return;
    const std::wstring path = BridgeLogPath();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    char stamp[64]{};
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::fprintf(f, "\n==== %s ====\n%s\n", stamp, text.c_str());
    std::fclose(f);
}

// 失败级：协议容错路径必写（解析失败 / 缺字段 / 未知 cmd / handler 异常）。
void BridgeFailLog(const std::string& text) { AppendBridgeLog(text, false); }
// debug 级：被忽略的重复回执、节流挂起等诊断，需 TTPLAYER_BRIDGE_LOG 门控。
void BridgeDebugLog(const std::string& text) { AppendBridgeLog(text, true); }

}  // namespace

// ───────────────────────── 契约清单 ─────────────────────────
// 首批 cmd 清单。tools/bridge_selftest.py 会用正则抽取本表与
// webui/app.js 的 SUPPORTED_CMDS 比对，改协议必须两边同步。

const char* const MusicFreeBridge::kSupportedCmds[] = {
    "app.init",     "ping",         "tab.switch",
    "player.play",  "player.pause", "player.next",
    "player.prev",  "player.seek",  "player.volume",
    "video.home",   "video.open",   "video.sources", "video.search",
    "music.import",
    "music.discover", "music.sheet",
    "search.query", "fav.set",
    "cfg.get",      "cfg.set",
};

// 首批 evt 清单（同上，与 app.js 的 SUPPORTED_EVTS 比对）。
const char* const MusicFreeBridge::kSupportedEvts[] = {
    "bridge.ready", "player.state", "video.home",
    "video.status", "video.search", "search.result", "source.result",
    "music.discover", "music.sheet",
};

// ───────────────────────── 构造 / 内置命令 ─────────────────────────

MusicFreeBridge::MusicFreeBridge() { RegisterBuiltins(); }

void MusicFreeBridge::RegisterBuiltins() {
    // ping：回 {"pong":true,"t":<unix 毫秒>}，页面可据此测往返延迟。
    Register("ping", [](const json&, ReplyFn reply) {
        const auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
        json result;
        result["pong"] = true;
        result["t"] = t;
        reply(true, std::move(result), {});
    });

    // app.init：页面握手。记录页面 UA / 版本，回显桥能力表
    //（协议版本 + cmds + events），随后补推一次 bridge.ready。
    // 宿主若在 NavigationCompleted 也推过 bridge.ready：PushEvent 内部
    //「只发一次」去重，先到者生效，页面不会收到两遍。
    Register("app.init", [this](const json& payload, ReplyFn reply) {
        json result;
        result["ua"] = JsonStr(payload, "ua");
        result["version"] = JsonStr(payload, "version");
        result["bridge"] = "1.0";
        json cmds = json::array();
        for (const char* c : kSupportedCmds) cmds.push_back(c);
        json evts = json::array();
        for (const char* e : kSupportedEvts) evts.push_back(e);
        result["cmds"] = std::move(cmds);
        result["events"] = std::move(evts);
        reply(true, std::move(result), {});

        json ready;
        ready["bridge"] = "1.0";
        PushEvent("bridge.ready", ready);
    });
}

// ───────────────────────── 注册表 ─────────────────────────

void MusicFreeBridge::Register(const std::string& cmd, Handler h) {
    if (cmd.empty() || !h) {
        BridgeDebugLog("Register 拒绝：cmd 为空或 handler 为空");
        return;
    }
    bool replaced = false;
    {
        std::lock_guard lk(mtx_);
        replaced = handlers_.contains(cmd);
        handlers_[cmd] = std::move(h);
    }
    if (replaced) BridgeDebugLog("cmd 重复注册（以后者为准）: " + cmd);
}

// ───────────────────────── 页面 → C++ ─────────────────────────

void MusicFreeBridge::OnWebMessage(const std::string& json_text) {
    json msg;
    try {
        msg = json::parse(json_text);
        // 容错：页面若 postMessage 直接发字符串且宿主整串转发，这里会拿到
        // 一个 JSON 字符串值，再剥一层等价于对象消息。
        int depth = 0;
        while (msg.is_string() && depth++ < 2)
            msg = json::parse(msg.get<std::string>());
    } catch (const std::exception& e) {
        // 无 id 可回：只落日志不崩（协议容错要求）。
        BridgeFailLog(std::string("页面消息 JSON 解析失败: ") + e.what() +
                      " | raw=" + TruncForLog(json_text));
        return;
    }
    if (!msg.is_object()) {
        BridgeFailLog("页面消息不是 JSON 对象: " + TruncForLog(json_text));
        return;
    }

    // id 必须是数字才可回执；缺失/非法 → 只落日志不回。
    if (!msg.contains("id") || !msg["id"].is_number()) {
        BridgeFailLog("页面消息缺 id（不回执）: " + TruncForLog(json_text));
        return;
    }
    const json& idv = msg["id"];
    const std::int64_t id = idv.is_number_integer()
                                ? idv.get<std::int64_t>()
                                : static_cast<std::int64_t>(idv.get<double>());

    // cmd 缺失/非字符串：有 id 就回一条失败响应。
    if (!msg.contains("cmd") || !msg["cmd"].is_string() ||
        msg["cmd"].get_ref<const std::string&>().empty()) {
        BridgeFailLog("页面消息缺 cmd: " + TruncForLog(json_text));
        json resp;
        resp["id"] = id;
        resp["ok"] = false;
        resp["error"] = "missing cmd field";
        SendJson(DumpJsonSafe(resp));
        return;
    }
    const std::string cmd = msg["cmd"].get<std::string>();

    // 诊断专用通道：封面 <img> 加载失败时页面上报 URL（video.coverFail）。
    // 不进正常 cmd 分发、不污染协议清单，仅落盘 COVER_FAIL 行供排查
    // 「视频/音乐卡片封面不显示」是 mixed content / 防盗链 / 404。
    if (cmd == "video.coverFail") {
        const json pl = (msg.contains("payload") && msg["payload"].is_object())
                            ? msg["payload"]
                            : json::object();
        const std::string url =
            (pl.contains("url") && pl["url"].is_string())
                ? pl["url"].get<std::string>()
                : "";
        const bool wasHttp = pl.contains("wasHttp") && pl["wasHttp"].is_boolean()
                                 ? pl["wasHttp"].get<bool>()
                                 : false;
        const std::string stage = (pl.contains("stage") && pl["stage"].is_string())
                                      ? pl["stage"].get<std::string>()
                                      : "";
        BridgeFailLog("COVER_FAIL url=" + url + " wasHttp=" + (wasHttp ? "1" : "0") +
                      " stage=" + stage);
        json resp;
        resp["id"] = id;
        resp["ok"] = true;
        SendJson(DumpJsonSafe(resp));
        return;
    }

    Handler handler;
    {
        std::lock_guard lk(mtx_);
        auto it = handlers_.find(cmd);
        if (it != handlers_.end()) handler = it->second;
    }
    if (!handler) {
        BridgeFailLog("未知 cmd: " + cmd + " | raw=" + TruncForLog(json_text));
        json resp;
        resp["id"] = id;
        resp["ok"] = false;
        resp["error"] = "unknown cmd: " + cmd;
        SendJson(DumpJsonSafe(resp));
        return;
    }

    // payload 缺失/为 null → 交给 handler 一个空对象；其余类型原样透传
    //（handler 侧入参不可信，统一用 JsonStr / find 取值）。
    json payload = json::object();
    if (msg.contains("payload") && !msg["payload"].is_null())
        payload = msg["payload"];

    // 回执封套：只回一次（handler 可能同步回、也可能在工作线程稍后异步回，
    // 重复调用第二次起忽略并落 debug 日志）。
    auto replied = std::make_shared<std::atomic<bool>>(false);
    ReplyFn reply = [this, id, replied](bool ok, json result,
                                        std::string error) {
        if (replied->exchange(true)) {
            BridgeDebugLog("重复回执已忽略 id=" + std::to_string(id));
            return;
        }
        json resp;
        resp["id"] = id;
        resp["ok"] = ok;
        if (ok) {
            resp["result"] = result.is_null() ? json::object()
                                               : std::move(result);
        } else {
            resp["error"] =
                error.empty() ? std::string("unknown error") : std::move(error);
        }
        SendJson(DumpJsonSafe(resp));
    };

    try {
        handler(payload, reply);
    } catch (const std::exception& e) {
        BridgeFailLog("handler 异常 cmd=" + cmd + ": " + e.what());
        reply(false, json{}, std::string("handler exception: ") + e.what());
    } catch (...) {
        BridgeFailLog("handler 异常 cmd=" + cmd + ": unknown");
        reply(false, json{}, "handler exception: unknown");
    }

    // 页面正在交互是天然的刷新时机：补发节流窗口内挂起的事件。
    FlushPending();
}

// ───────────────────────── C++ → 页面 ─────────────────────────

void MusicFreeBridge::PushEvent(const std::string& evt,
                                const nlohmann::json& payload) {
    if (evt.empty()) {
        BridgeDebugLog("PushEvent 忽略空 evt");
        return;
    }

    // bridge.ready 全程只发一次（见头文件注释：避免宿主与 app.init 各推一次）。
    if (evt == "bridge.ready") {
        bool first = false;
        {
            std::lock_guard lk(mtx_);
            if (!ready_sent_) {
                ready_sent_ = true;
                first = true;
            }
        }
        if (!first) {
            BridgeDebugLog("bridge.ready 已发过，忽略重复推送");
            return;
        }
        json out;
        out["evt"] = evt;
        out["payload"] = payload.is_null() ? json::object() : payload;
        SendJson(DumpJsonSafe(out));
        return;
    }

    json out;
    out["evt"] = evt;
    out["payload"] = payload.is_null() ? json::object() : payload;

    bool send_now = true;
    {
        std::lock_guard lk(mtx_);
        if (IsHighFreqEvt(evt)) {
            const long long now = NowTickMs();
            auto it = last_sent_ms_.find(evt);
            if (it != last_sent_ms_.end() &&
                now - it->second < kHighFreqThrottleMs) {
                // 节流窗口内：不直发，把最新一份挂起（旧挂起被覆盖 = 合并），
                // 等 FlushPending / 下一次同名事件 / OnWebMessage 补发。
                pending_[evt] = out["payload"];
                send_now = false;
            } else {
                pending_.erase(evt);  // 即将发送的就是最新份，挂起份作废
            }
        }
        if (send_now) last_sent_ms_[evt] = NowTickMs();
    }

    if (send_now) {
        SendJson(DumpJsonSafe(out));
    } else {
        BridgeDebugLog("事件节流挂起（合并为最新一份）: " + evt);
    }
}

void MusicFreeBridge::FlushPending() {
    std::vector<std::string> ready;
    {
        std::lock_guard lk(mtx_);
        if (pending_.empty()) return;
        const long long now = NowTickMs();
        for (auto it = pending_.begin(); it != pending_.end();) {
            const auto ls = last_sent_ms_.find(it->first);
            const long long last = (ls == last_sent_ms_.end()) ? 0 : ls->second;
            if (now - last >= kHighFreqThrottleMs) {
                json out;
                out["evt"] = it->first;
                out["payload"] = it->second;
                ready.push_back(DumpJsonSafe(out));
                last_sent_ms_[it->first] = now;
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& s : ready) SendJson(s);
}

// ───────────────────────── 发送出口 ─────────────────────────

void MusicFreeBridge::SendJson(const std::string& json_text) {
    if (!sender) {
        BridgeDebugLog("sender 未注入，丢弃: " + TruncForLog(json_text));
        return;
    }
    if (poster) {
        // 宿主注入了「投递回 UI 线程」的钩子：所有发送统一经它执行，
        // 以满足「sender 只在 UI 线程跑」的约定（见头文件线程模型假设）。
        poster([this, json_text] {
            if (sender) sender(json_text);
        });
        return;
    }
    sender(json_text);
}

}  // namespace ttplayer::musicfree
