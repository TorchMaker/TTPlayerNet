// 音乐窗 WebView2 JSON 桥 —— 音乐能力命令实现（Task 6）
//
// 职责边界：只做「参数校验 → 复用既有播放链路 → 拼 JSON 回执 / 推事件」。
// 硬约束（对应 spec「功能不减」与 tasks.md Task 6）：
//   1. 暂停 / 播放 / 上一首 / 下一首 / 定位 / 音量 / 模式读写全部走主播放窗
//      PlayerWindow 的既有成员实现（与 WM_COMMAND 的 kCmdPlay(32000) /
//      kCmdPause(32001) / kCmdPrevious(32005) / kCmdNext(32006) 分支、
//      皮肤进度条松手提交、皮肤音量拖动是同一份代码），绝不自造播放器；
//   2. 带曲目的播放复用「音乐窗播放所选」同一条链：ResolvePlayUrl 解析地址
//      （musicfree_app.cpp，工作线程 + g_plugin_io 串行锁，与 StartResolve 一致）
//      → PlayerWindow::AddOnlineTrack 入列起播（即 ShowMusicFreeBrowserWindow
//      给音乐窗接的 on_track 回调本体，player_window.cpp:5245-5252）；
//   3. 不新造任何存储文件；handler 不抛异常，失败统一回 ok:false + 中文 error。
//
// 【为何能拿到 PlayerWindow 实例】主窗 GWLP_USERDATA 存着 PlayerWindow*
// （src/ui/wtl_window.h WindowBinding::Start L42），按窗口类名
// L"TTPlayer_PlayerWnd"（src/ui/player_window_internal.h L47）找到窗即可取。
// 私有成员访问经 player_window.h / audio_engine.h 预留的 friend
// ttplayer::testing::ProgressSeekAccess（原为测试保留；全工程此前无定义，
// 唯一定义在本文件）——不修改任何既有文件。
//
// 【player.state 推送】定时器挂在音乐窗窗口的 SetWindowSubclass 子类化过程
// 上（500ms 轮询 + 签名去重）：播放中 pos 每秒进位 → 正好每秒一推；播放状态 /
// 模式 / 音量变化 ≤500ms 到达。注册后前 kWarmupPushes 次无条件推送做初始同步
//（sender 注入前的丢包由重推弥补）。桥内 100ms 节流照常生效，本模块只管触发。
//
// 线程模型（见 musicfree_bridge.h 头注）：
//   · handler 在 UI 线程被调；g_w（桥指针 / 窗口句柄 / 上次签名 / 封面记忆）
//     只在 UI 线程（handler、子类窗口过程、定时器）读写，不加锁；
//   · 只有 ResolvePlayUrl 在工作线程（插件 QuickJS 不能阻塞 UI），解析完成后
//     SendMessage 回音乐窗窗口（子类化过程在 UI 线程执行入列），再异步 reply；
//   · 回推前复核音乐窗还在（窗口销毁 = 桥随 BrowserState 一起销毁，
//     与 video 域 BridgeAlive 同一判据）；窗口已消失则不再回执，
//     残余竞态由宿主按桥头文件线程模型第 3 条兜底。

#include "bridge_cmds_music.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_browser_window.h"
#include "ttplayer/ui/player_window.h"   // 同时带入 windows.h
#include <commctrl.h>                    // SetWindowSubclass / TBM_SETPOS

// ─────────────────── PlayerWindow 访问通道（friend 预留，无既有文件改动） ──────────
//
// player_window.h L53/L113 与 audio_engine.h L23/L275 均 friend 声明了
// ttplayer::testing::ProgressSeekAccess，全工程没有任何定义（tests/ 目录已移除）。
// 本文件给出唯一定义：所有成员都只做「取既有私有成员 / 调既有私有函数」，
// 语义与对应 WM_COMMAND 分支逐行一致（行号为本次实现时的快照）。
namespace ttplayer::testing {

struct ProgressSeekAccess {
    // 主窗 HWND → PlayerWindow*（WindowBinding::Start 写入 GWLP_USERDATA）。
    static ui::PlayerWindow* FromWindow(HWND window) {
        if (!window || !::IsWindow(window)) return nullptr;
        return reinterpret_cast<ui::PlayerWindow*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    // —— 状态读取（player.state 的数据源）——
    static audio::PlaybackState State(const ui::PlayerWindow* player) {
        return player->audio_->State();
    }
    static std::int64_t PositionMs(const ui::PlayerWindow* player) {
        return player->audio_->Position().count();
    }
    static std::int64_t DurationMs(const ui::PlayerWindow* player) {
        return player->audio_->Duration().count();
    }
    static int Volume(const ui::PlayerWindow* player) {
        return player->settings_.player.volume;
    }
    static bool Mute(const ui::PlayerWindow* player) {
        return player->settings_.player.mute;
    }
    static int PlayMode(const ui::PlayerWindow* player) {
        return player->settings_.player.play_mode;
    }
    static const playlist::Track* CurrentTrack(const ui::PlayerWindow* player) {
        return player->PlaybackTrackForUi();
    }
    static std::wstring DisplayTitle(const ui::PlayerWindow* player) {
        return player->display_title_;
    }

    // —— 播放控制（分支口径 = HandleContextCommand 的 switch，player_window.cpp
    //    L5318-5338；直接调成员等价于投递同名 WM_COMMAND，且同步可回执）——
    // kCmdPause：audio_->Pause()（L5322）
    static void Pause(ui::PlayerWindow* player) { player->audio_->Pause(); }
    // kCmdPlay：暂停态 Resume，否则重起当前条目（L5319-5321）
    static bool Play(ui::PlayerWindow* player) {
        if (player->audio_->State() == audio::PlaybackState::paused) {
            player->audio_->Resume();
            return true;
        }
        return player->PlayCurrent();
    }
    // kCmdNext / kCmdPrevious（L5324-5325）
    static void Next(ui::PlayerWindow* player) { player->SelectRelative(true); }
    static void Prev(ui::PlayerWindow* player) { player->SelectRelative(false); }
    // 皮肤进度条松手提交：SeekWithoutFade（L2944），不继承通用淡出门槛
    static void Seek(ui::PlayerWindow* player, std::int64_t position_ms) {
        player->audio_->SeekWithoutFade(std::chrono::milliseconds(position_ms));
    }
    // 皮肤音量落值（L6008-6009：SetVolume + 清 mute）+ 两种形态的显示同步
    static void SetVolume(ui::PlayerWindow* player, int volume) {
        volume = std::clamp(volume, 0, 100);
        player->settings_.player.volume = volume;
        player->audio_->SetVolume(static_cast<float>(volume) / 100.0F);
        player->settings_.player.mute = false;
        if (player->skin_) {
            // 皮肤形态：只重画音量条本体（口径同 SetSkinVolumeFromPoint L6010）
            if (const auto* element = player->skin_->Find(L"volume"))
                ::InvalidateRect(player->window_, &element->bounds, FALSE);
        }
        // 经典形态：同步原生音量滑杆（kVolume=1103，player_window.cpp L3475-3479）
        if (const HWND bar = ::GetDlgItem(player->window_, 1103))
            ::SendMessageW(bar, TBM_SETPOS, TRUE, volume);
    }
    // 播放模式（菜单 kCmdPlayModeFirst..Last 同一入口，L5275-5276）
    static void SetPlayMode(ui::PlayerWindow* player, int mode) {
        player->SetPlaybackMode(std::clamp(mode, 0, 4));
    }

    // 在线条目入列 / 起播 —— 音乐窗「播放所选」on_track 的回调本体
    //（player_window_file_intake.cpp L1322：start=true 插表头并立即起播）
    static bool AddOnlineTrack(ui::PlayerWindow* player,
                               const musicfree::OnlineSearchHit& hit,
                               const std::wstring& url, bool start_playback) {
        if (url.empty()) return false;
        return player->AddOnlineTrack(hit, url, start_playback);
    }
};

}  // namespace ttplayer::testing

namespace ttplayer::musicfree {

namespace {

using json = nlohmann::json;

// AppendPlayLog 已提到 musicfree_app.cpp（公共），UI 播放链路也要写同一份日志。
// 日志只记阶段 / id / 长度，不落带签名的一次性直链。

// 主窗窗口类（player_window_internal.h L47，只取字符串不依赖其内部头）。
constexpr wchar_t kPlayerWndClass[] = L"TTPlayer_PlayerWnd";
// 本模块私有的跨线程消息：工作线程解析完地址后回 UI 线程执行入列。
// 挂在音乐窗窗口上（子类化过程先于窗口过程见到它）。
constexpr UINT kMsgPlayBatch = WM_APP + 0x5E7;
// 状态轮询定时器：500ms + 签名去重 → 播放中正好每秒一推（pos 每秒进位）。
constexpr UINT_PTR kStateTimerId = 0x6C01;
constexpr UINT kStatePollMs = 500;
// SetWindowSubclass 的子类标识（本模块唯一，避开宿主可能使用的 id）。
constexpr UINT_PTR kSubclassId = 0x6D01;
// 注册后无条件推送的次数：页面 bridge.ready/app.init 握手期间 sender 可能
// 尚未注入（静默丢弃），连推几次保证页面一挂监听就能拿到初始状态。
constexpr int kWarmupPushes = 5;

// 子类化过程的前置声明（EnsureAttached 需要取其地址；定义在文件后段）。
LRESULT CALLBACK MusicBridgeSubclass(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR subclass_id,
                                     DWORD_PTR ref_data);

// ───────────────────────── 模块状态（只在 UI 线程读写） ─────────────────────────
struct MusicBridgeWindowState {
    MusicFreeBridge* bridge{};   // 随 BrowserState 创建 / 销毁
    HWND browser{};              // 音乐窗 HWND（子类化 + 定时器宿主）
    UINT_PTR timer_id{};
    json last_sig;               // 上次已推送的签名（不含 pos_ms/dur_ms）
    int warmup_pushes{};
    std::string art_key;         // 封面记忆："title\x1fartist" → 封面 URL
    std::string art_url;
};
MusicBridgeWindowState g_w;

// ───────────────────────── 参数取值（页面入参不可信，一律不抛） ─────────────────────────

std::string JsonStr(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

// 数字（或数字串）→ double；不是有限数字返回 nullopt（NaN/inf 会把
// static_cast 和 std::clamp 搞成未定义值，入口直接拒）。
std::optional<double> AsNumber(const json& j, const char* key) {
    if (!j.is_object()) return std::nullopt;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (it->is_number()) {
        const double v = it->get<double>();
        return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
    }
    if (it->is_string()) {
        try {
            const std::string& s = it->get_ref<const std::string&>();
            size_t pos = 0;
            const double v = std::stod(s, &pos);
            if (pos == s.size() && std::isfinite(v)) return v;
        } catch (...) {
        }
    }
    return std::nullopt;
}

// ───────────────────────── 主窗定位与状态快照 ─────────────────────────

// 找主播放窗 HWND：优先音乐窗的 owner（ShowMusicFreeBrowser 传的就是主窗），
// 类名不符或没有 owner 再按窗口类全局找（单实例进程只会有一个）。
HWND PlayerHwnd(HWND browser) {
    HWND owner = (browser && ::IsWindow(browser)) ? ::GetWindow(browser, GW_OWNER)
                                                  : nullptr;
    if (owner) {
        wchar_t cls[64] = {};
        const int n = ::GetClassNameW(owner, cls, 64);
        if (n > 0 && wcscmp(cls, kPlayerWndClass) == 0) return owner;
    }
    return ::FindWindowW(kPlayerWndClass, nullptr);
}

ui::PlayerWindow* CurrentPlayer() {
    return ttplayer::testing::ProgressSeekAccess::FromWindow(
        PlayerHwnd(g_w.browser));
}

const char* StateName(audio::PlaybackState state) {
    switch (state) {
        case audio::PlaybackState::playing:  return "playing";
        case audio::PlaybackState::paused:   return "paused";
        case audio::PlaybackState::opening:  return "opening";
        case audio::PlaybackState::failed:   return "failed";
        case audio::PlaybackState::stopped:  return "stopped";
        default:                             return "stopped";
    }
}

// player.state 的完整载荷（字段契约见 bridge_cmds_music.h 头注）。
// 皮肤 / 经典两种形态都只从 audio 引擎与 settings 取真值，不猜界面。
json BuildPlayerState(ui::PlayerWindow* player) {
    using ttplayer::testing::ProgressSeekAccess;
    json s;
    if (!player) {
        s["playing"] = false;
        s["state"] = "stopped";
        s["title"] = "";
        s["artist"] = "";
        s["art"] = "";
        s["pos"] = 0;
        s["dur"] = 0;
        s["pos_ms"] = 0;
        s["dur_ms"] = 0;
        s["mode"] = 0;
        s["vol"] = 0;
        s["mute"] = false;
        return s;
    }
    const auto state = ProgressSeekAccess::State(player);
    s["playing"] = state == audio::PlaybackState::playing ||
                   state == audio::PlaybackState::opening;
    s["state"] = StateName(state);

    std::string title;
    std::string artist;
    if (const playlist::Track* track = ProgressSeekAccess::CurrentTrack(player)) {
        title = track->title;
        artist = track->artist;
    }
    if (title.empty()) title = core::WideToUtf8(ProgressSeekAccess::DisplayTitle(player));
    s["title"] = title;
    s["artist"] = artist;
    // 封面只有本模块起播时才记得住（payload 带 cover/art）；其它途径起播
    // 的曲目回空串，页面走占位样式。
    const std::string key = title + "\x1f" + artist;
    s["art"] = (key == g_w.art_key) ? g_w.art_url : std::string();

    const std::int64_t pos_ms = ProgressSeekAccess::PositionMs(player);
    const std::int64_t dur_ms = ProgressSeekAccess::DurationMs(player);
    s["pos"] = static_cast<std::int64_t>(pos_ms / 1000);
    s["dur"] = static_cast<std::int64_t>(dur_ms / 1000);
    s["pos_ms"] = pos_ms;
    s["dur_ms"] = dur_ms;

    s["mode"] = std::clamp(ProgressSeekAccess::PlayMode(player), 0, 4);
    s["vol"] = std::clamp(ProgressSeekAccess::Volume(player), 0, 100);
    s["mute"] = ProgressSeekAccess::Mute(player);
    return s;
}

// 签名 = 载荷去掉 pos_ms/dur_ms：播放中毫秒每 500ms 必变，含它会把推送频率
// 翻倍（违反「进度每秒一次」）；去掉后播放中正好随 pos 秒进位每秒一推。
json StateSignature(const json& state) {
    json sig = state;
    sig.erase("pos_ms");
    sig.erase("dur_ms");
    return sig;
}

// 惰性挂载状态推送的宿主（子类化 + 定时器）：RegisterMusicCommands 若在音乐窗
// 窗口建好之前被调用（时序异常），第一次 tick 时自动补挂；已挂则零开销返回。
void EnsureAttached() {
    if (!g_w.bridge || (g_w.browser && ::IsWindow(g_w.browser))) return;
    HWND browser = MusicFreeBrowserWindow();
    if (!browser || !::IsWindow(browser)) return;
    g_w.browser = browser;
    ::SetWindowSubclass(browser, MusicBridgeSubclass, kSubclassId, 0);
    g_w.timer_id = ::SetTimer(browser, kStateTimerId, kStatePollMs, nullptr);
}

// 一次推送（UI 线程）。warmup 阶段无条件推，之后签名有变化才推。
void TickState() {
    if (!g_w.bridge) return;
    EnsureAttached();
    if (g_w.browser && !::IsWindow(g_w.browser)) return;  // 销毁途中：等 NCDESTROY 收尾
    json state = BuildPlayerState(CurrentPlayer());
    json sig = StateSignature(state);
    const bool warm = g_w.warmup_pushes < kWarmupPushes;
    if (!warm && sig == g_w.last_sig) return;
    if (warm) ++g_w.warmup_pushes;
    g_w.last_sig = std::move(sig);
    g_w.bridge->PushEvent("player.state", state);
    // 自触发的推送不等页面下一条消息来补发（桥内节流窗口照常生效）。
    g_w.bridge->FlushPending();
}

// ───────────────────────── 带曲目播放：入列执行体（必须在 UI 线程） ─────────────────────────

struct PlayBatchRequest {
    std::vector<OnlineSearchHit> hits;
    std::vector<std::wstring> urls;
    std::size_t play_index = 0;   // 先起播的曲目下标（默认首条）
    bool start_playback = true;   // 点中那首解析失败时改 false：只入列不起播
    bool applied = false;         // 出参：至少一首已成功入列
    std::size_t added = 0;        // 出参：实际入列条数
    std::string error;            // 出参：失败原因（中文）
    std::string warning;          // 出参：已入列但没起播的原因
};

// 复用音乐窗「播放所选」的既有链：AddOnlineTrack（on_track 回调本体）。
// play_index 先以 start=true 入列（插表头并立即起播），其余按「添加到列表」
// 追加——与页面「播放列表中第 N 首、其余排队」的期望一致。
// ★ 2026-11-10 修复：整批入列不再因为点中那首解析失败而整批放弃；
//   start_playback=false 时按原顺序全部入列、不触发任何起播。
void ExecutePlayBatch(PlayBatchRequest& req) {
    req.applied = false;
    req.added = 0;
    if (req.hits.empty() || req.hits.size() != req.urls.size()) {
        req.error = "播放数据不完整（曲目与地址数量不符）";
        return;
    }
    ui::PlayerWindow* player = CurrentPlayer();
    if (!player) {
        req.error = "播放器主窗口不可用，无法入列播放";
        return;
    }
    try {
        const std::size_t chosen =
            req.play_index < req.hits.size() ? req.play_index : 0;
        const auto add = [&](std::size_t i, bool play) -> bool {
            try {
                return ttplayer::testing::ProgressSeekAccess::AddOnlineTrack(
                    player, req.hits[i], req.urls[i], play);
            } catch (...) {
                return false;
            }
        };
        if (req.start_playback) {
            if (!add(chosen, true)) {
                req.error = "加入播放列表失败（播放列表不可用）";
                return;
            }
            ++req.added;
            // 记住封面，供 player.state 的 art 字段回吐。
            const OnlineSearchHit& hit = req.hits[chosen];
            if (!hit.artwork.empty()) {
                g_w.art_key = hit.title + "\x1f" + hit.artist;
                g_w.art_url = hit.artwork;
            }
            req.applied = true;   // 目标曲目已起播：后续曲目入列失败不推翻本回执
            TickState();
            for (std::size_t i = 0; i < req.hits.size(); ++i) {
                if (i == chosen) continue;
                if (!add(i, false)) break;  // 排队失败不推翻已在播的目标曲
                ++req.added;
            }
        } else {
            for (std::size_t i = 0; i < req.hits.size(); ++i) {
                if (!add(i, false)) break;
                ++req.added;
            }
            req.applied = req.added > 0;
        }
    } catch (const std::exception& e) {
        req.error = std::string("入列播放失败: ") + e.what();
    } catch (...) {
        req.error = "入列播放失败: 未知错误";
    }
}

// ───────────────────────── 曲目 payload → 在线条目 ─────────────────────────
//
// payload 形态（三种任选）：
//   { tracks: [track, ...], index?: n }   列表，起播第 index 条（默认 0）
//   { track: track }                      单条
//   { url 或 platform/id, title?, ... }   单条平铺
// track = { url?, platform?, id?, title?, artist?, album?, duration_ms?,
//           cover?/art?, raw? }：有 url 直接入列；无 url 必须有 platform+id
//（由工作线程 ResolvePlayUrl 解析，raw 原样回传给插件）。
struct TrackSpec {
    OnlineSearchHit hit;
    std::string url;        // 空 = 需要解析
    bool needs_resolve() const {
        return url.empty() && !hit.platform.empty() && !hit.id.empty();
    }
};

// ---------------------------------------------------------------- 双击短时拦截
// 真机 play.log 21:29:57 同一秒出现两条完全相同的
//   PLAY entry ... id=qq_0003lSMc4KXPxX → added=1
//   PLAY entry ... id=qq_0003lSMc4KXPxX → added=1
// 于是播放列表里多出一首。页面侧榜单双击有去重，但搜索结果是 click 监听
// （双击 = 两次 click）且没有去重；宿主是最终入列点，在这里按曲目身份做
// 短时窗口拦截，可兜住任何页面路径 / 未来新入口的重复投递。
constexpr DWORD kPlayDedupWindowMs = 1500;
std::mutex g_play_dedup_mutex;
std::map<std::string, DWORD> g_play_dedup_last;  // 曲目 key → 上次入列 tick

std::string PlayDedupKey(const std::vector<TrackSpec>& specs) {
    if (specs.empty()) return {};
    if (specs.size() > 1) {
        // 整单（兼容旧页面）：首曲 + 数量，避免整批被重复入列。
        return "batch:" + std::to_string(specs.size()) + ":" +
               specs.front().hit.platform + ":" + specs.front().hit.id;
    }
    const auto& h = specs.front().hit;
    if (!h.platform.empty() && !h.id.empty())
        return h.platform + "\x1f" + h.id;
    if (!h.id.empty()) return h.id;
    if (!specs.front().url.empty()) return specs.front().url;  // url 在 TrackSpec 上
    return h.title + "\x1f" + h.artist;
}

// 返回 true = 窗口内重复，调用方应跳过 ExecutePlayBatch。
bool PlayDedupBlock(const std::string& key, DWORD now, DWORD* elapsed) {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(g_play_dedup_mutex);
    for (auto it = g_play_dedup_last.begin(); it != g_play_dedup_last.end();) {
        if (now - it->second > kPlayDedupWindowMs * 4)
            it = g_play_dedup_last.erase(it);
        else ++it;
    }
    const auto it = g_play_dedup_last.find(key);
    if (it != g_play_dedup_last.end()) {
        const DWORD d = now - it->second;
        if (d < kPlayDedupWindowMs) {
            if (elapsed) *elapsed = d;
            return true;
        }
    }
    g_play_dedup_last[key] = now;
    return false;
}

// 入列失败时清掉记录，允许用户立刻重试（不被去重窗口误挡）。
void PlayDedupForget(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(g_play_dedup_mutex);
    g_play_dedup_last.erase(key);
}

bool BuildTrackSpec(const json& item, TrackSpec& out, std::string& error) {
    if (!item.is_object()) {
        error = "曲目条目必须是对象";
        return false;
    }
    out = TrackSpec{};
    out.url = JsonStr(item, "url");
    out.hit.platform = JsonStr(item, "platform");
    if (out.hit.platform.empty()) out.hit.platform = JsonStr(item, "source");
    if (out.hit.platform.empty() && item.is_object() &&
        item.contains("raw") && item.at("raw").is_object())
        out.hit.platform = JsonStr(item.at("raw"), "platform");
    out.hit.id = JsonStr(item, "id");
    out.hit.title = JsonStr(item, "title");
    out.hit.artist = JsonStr(item, "artist");
    out.hit.album = JsonStr(item, "album");
    // ★ 2026-11-10 用户实测「不如之前，直接报错：曲目缺少 url，也缺少
    //   platform+id」。搜索结果 / 旧 payload 常只带 id=qq_003Kcm8v3QZOkI，
    //   id 前缀本身就是音源标识；旧代码直接整单拒收，连占位入列都不做。
    //   这里按已知前缀补 platform（与 source_protocol.cpp 的平台标识一致），
    //   让这类条目也能走「立即入列 + 后台解析」。
    if (!out.hit.id.empty()) {
        static const char* const kPrefixes[] = {
            "netease", "bilibili", "cloudmusic", "kugou", "kuwo", "migu",
            "qq", "kg", "kw", "wy", "mg", "tx"};
        std::string lower = out.hit.id;
        for (char& ch : lower)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        for (const char* p : kPrefixes) {
            const std::string prefix(p);
            const std::size_t n = prefix.size();
            if (lower.size() > n && lower.compare(0, n, prefix) == 0 &&
                (lower[n] == '_' || lower[n] == '-' || lower[n] == ':')) {
                // id 前缀比页面字段可靠：分组名可能是「QQ音乐」这类显示名，
                // 也可能缺 platform，一律以真实音源标识为准。
                out.hit.platform = prefix;
                break;
            }
        }
    }
    const auto duration = AsNumber(item, "duration_ms");
    if (duration) out.hit.duration_ms = static_cast<std::int64_t>(*duration);
    std::string art = JsonStr(item, "cover");
    if (art.empty()) art = JsonStr(item, "art");
    out.hit.artwork = art;
    if (item.is_object() && item.contains("raw") && item.at("raw").is_object())
        out.hit.raw = item.at("raw");
    else {
        // 没带音源原始对象：按 musicfree_app.h RestoreSourceRaw 的口径拼一个
        // 最小对象，洛雪 / REST 音源至少能按歌名+歌手兜底解析。
        out.hit.raw = json{{"platform", out.hit.platform},
                           {"id", out.hit.id},
                           {"title", out.hit.title},
                           {"artist", out.hit.artist},
                           {"album", out.hit.album}};
    }
    if (out.url.empty() && (out.hit.platform.empty() || out.hit.id.empty())) {
        error = "曲目缺少 url，也缺少 platform+id（无法解析播放地址）";
        return false;
    }
    if (out.url.empty() && out.hit.title.empty()) {
        error = "待解析曲目缺少 title（入列后无法命名）";
        return false;
    }
    return true;
}

// 懒解析曲目在播放列表里的占位地址。
// 必须以 http 开头：PlayerWindow::PlayCurrent 只对「path 以 http 开头」的条目
// 走在线双路播放，并按 metadata 的 Platform/SongId 重新解析
//（player_window.cpp:6492）。这个占位地址本身永远不会被下载/打开——解析成功后
// play_path 会被真实直链覆盖。这样整张榜单可以一次入列，只解析点中的那一首。
std::wstring LazyPlaceholderUrl(const TrackSpec& spec) {
    const auto sanitize = [](const std::string& s) {
        std::string r;
        for (const unsigned char ch : s) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
                r.push_back(static_cast<char>(ch));
            else
                r.push_back('_');
        }
        if (r.empty()) r = "x";
        return r;
    };
    return core::Utf8ToWide("https://ttplayer.invalid/online/" +
                            sanitize(spec.hit.platform) + "/" +
                            sanitize(spec.hit.id));
}

// 从 payload 收集曲目列表与起播下标；空列表 = 只做「恢复播放」。
// ★ 整单里个别条目缺字段时不再整批拒收（以前 100 首里有 1 首缺 id 就一首不加），
//   只跳过坏条目并重映射起播下标；warning 回带给页面提示。
// 返回 false + 中文 error 表示 payload 形态非法（或整批都不可用）。
bool CollectTrackSpecs(const json& payload, std::vector<TrackSpec>& out,
                       std::size_t& play_index, std::string& error,
                       std::string* warning = nullptr) {
    json items = json::array();
    if (payload.is_object()) {
        if (payload.contains("tracks") && payload.at("tracks").is_array()) {
            items = payload.at("tracks");
        } else if (payload.contains("track") && payload.at("track").is_object()) {
            items = json::array({payload.at("track")});
        } else if (payload.contains("url") || payload.contains("platform") ||
                   payload.contains("id") || payload.contains("title") ||
                   payload.contains("mid")) {
            // 单条平铺（含只有 mid 的误用形态 —— 交给 BuildTrackSpec 报缺参）
            items = json::array({payload});
        }
    }
    if (items.empty()) return true;  // 无曲目：走「恢复/起播当前列表」分支

    double index = 0;
    if (payload.is_object()) {
        if (const auto v = AsNumber(payload, "index")) index = *v;
    }
    const std::size_t wanted =
        index < 0 ? 0 : static_cast<std::size_t>(index);

    out.reserve(items.size());
    std::vector<std::size_t> origins;
    origins.reserve(items.size());
    std::size_t skipped = 0;
    std::string first_reject;
    for (std::size_t k = 0; k < items.size(); ++k) {
        TrackSpec spec;
        std::string why;
        if (!BuildTrackSpec(items[k], spec, why)) {
            if (items.size() == 1) {
                error = why;
                return false;
            }
            ++skipped;
            if (first_reject.empty()) first_reject = why;
            continue;
        }
        origins.push_back(k);
        out.push_back(std::move(spec));
    }
    if (out.empty()) {
        error = first_reject.empty() ? "曲目列表为空或全部字段不完整"
                                     : first_reject;
        return false;
    }
    // 起播下标重映射：优先取原下标 >= wanted 的第一条有效曲目。
    std::size_t mapped = 0;
    for (std::size_t j = 0; j < out.size(); ++j) {
        mapped = j;
        if (origins[j] >= wanted) break;
    }
    play_index = mapped;
    if (skipped && warning) {
        *warning = "已跳过 " + std::to_string(skipped) +
                   " 首字段不完整的曲目（" + first_reject + "）";
    }
    return true;
}

// ───────────────────────── 音乐窗窗口子类化过程 ─────────────────────────
//
// 承担两件事（运行期注入，不改音乐窗实现文件）：
//   1. kMsgPlayBatch：工作线程解析完地址 → SendMessage 回 UI 线程入列；
//   2. kStateTimerId：player.state 的 500ms 轮询。
// 其余消息原样放行；WM_NCDESTROY 时摘掉本模块的全部状态。
LRESULT CALLBACK MusicBridgeSubclass(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR /*subclass_id*/,
                                     DWORD_PTR /*ref_data*/) {
    if (message == kMsgPlayBatch && lparam) {
        // SendMessageTimeout 同步执行：worker 用的是栈上请求，返回即可见出参。
        // 返回 1（而非 0）：调用方以返回值区分「已处理」与「超时/失败」。
        auto* req = reinterpret_cast<PlayBatchRequest*>(lparam);
        ExecutePlayBatch(*req);
        if (req)
            AppendPlayLog("PLAY apply(resolve) applied=" +
                          std::string(req->applied ? "1" : "0") +
                          " added=" + std::to_string(req->added) +
                          " started=" +
                          std::string(req->start_playback ? "1" : "0") +
                          " err=" + req->error);
        return 1;
    }
    if (message == WM_TIMER && wparam == kStateTimerId) {
        TickState();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        if (g_w.browser == hwnd) {
            if (g_w.timer_id) ::KillTimer(hwnd, g_w.timer_id);
            g_w = MusicBridgeWindowState{};
        }
        ::RemoveWindowSubclass(hwnd, MusicBridgeSubclass, kSubclassId);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    return ::DefSubclassProc(hwnd, message, wparam, lparam);
}

// 回执里附一份最新状态快照（页面可直接用，不等下一次 evt）。
json StateResult(ui::PlayerWindow* player, json extra = json::object()) {
    extra.update(BuildPlayerState(player));
    return extra;
}

}  // namespace

// ───────────────────────── 发现页磁盘缓存（与 Win32 发现页共用一份） ─────────────
//
// Win32 音乐窗早已把「各音源榜单 / 推荐」落盘到 <exe>\musicfree\discover_cache.json
//（12 小时 TTL，见 musicfree_browser_window.cpp 的 DiscoverCache* 系列）。WebView2
// 音乐窗此前完全绕过它：每开一次窗就把 6 个音源逐个联网重取，页面要十几秒才出
// 卡片。这里直接复用同一个文件、同一份格式（version=1），两条 UI 互相加热缓存：
//   · 命中且未过期（12h）→ 只推缓存、完全不联网；
//   · 命中但已过期      → 先推缓存（页面秒出），再后台静默刷新并回写；
//   · 无缓存            → 保持原有「立即回执 + 事件推结果」的行为。
namespace {

constexpr std::int64_t kDiscoverCacheTtlSeconds = 12 * 3600;  // 与 Win32 一致
constexpr std::uintmax_t kDiscoverCacheMaxBytes = 8 * 1024 * 1024;

struct DiscoverCacheGroup {
    std::string platform;
    std::vector<MusicSheet> sheets;
};

struct DiscoverCacheData {
    std::int64_t saved_at = 0;
    std::vector<DiscoverCacheGroup> groups;
};

std::int64_t NowUnixSeconds() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::filesystem::path DiscoverCachePath() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(),
                                       static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) return {};
    exe.resize(n);
    return std::filesystem::path(exe).parent_path() / L"musicfree" /
           L"discover_cache.json";
}

template <typename T>
T JsonValue(const json& j, const char* key, T fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    try {
        return j.at(key).get<T>();
    } catch (...) {
        return fallback;
    }
}

// 磁盘格式与 Win32 的 SheetToJson 逐字段一致：bridge 页面的 cover 字段不写回，
// 免得 Win32 卡片丢掉 artwork / work_id / play_count 这些只有它用的信息。
json SheetToDiskJson(const MusicSheet& s) {
    json j = json::object();
    j["id"] = s.id;
    j["platform"] = s.platform;
    j["title"] = s.title;
    j["description"] = s.description;
    j["artwork"] = s.artwork;
    j["work_id"] = s.work_id;
    j["play_count"] = s.play_count;
    j["create_at"] = s.create_at;
    j["group"] = s.group;
    // raw 必须原样保留：咪咕 / 酷狗的详情调用依赖 contentId 等私有字段。
    j["raw"] = s.raw.is_null() ? json::object() : s.raw;
    return j;
}

MusicSheet SheetFromDiskJson(const json& j) {
    MusicSheet s;
    s.id = JsonValue<std::string>(j, "id", "");
    s.platform = JsonValue<std::string>(j, "platform", "");
    s.title = JsonValue<std::string>(j, "title", "");
    s.description = JsonValue<std::string>(j, "description", "");
    s.artwork = JsonValue<std::string>(j, "artwork", "");
    s.work_id = JsonValue<std::string>(j, "work_id", "");
    s.play_count = JsonValue<std::int64_t>(j, "play_count", 0);
    s.create_at = JsonValue<std::int64_t>(j, "create_at", 0);
    s.group = JsonValue<std::string>(j, "group", "");
    if (j.is_object() && j.contains("raw")) s.raw = j["raw"];
    if (!s.raw.is_object()) s.raw = json::object();
    if (s.raw.find("platform") == s.raw.end()) s.raw["platform"] = s.platform;
    if (s.raw.find("id") == s.raw.end()) s.raw["id"] = s.id;
    return s;
}

// 卡片 → 页面事件载荷（字段与既有 handler 完全一致，kind 判定也照旧）。
json SheetToPayloadJson(const MusicSheet& s, const std::string& group_platform) {
    const bool is_sheet = s.group.rfind("推荐", 0) == 0;
    return json{{"id", s.id},
                {"platform", s.platform.empty() ? group_platform : s.platform},
                {"title", s.title},
                {"description", s.description},
                {"cover", s.artwork},
                {"group", s.group},
                {"kind", is_sheet ? "sheet" : "top"},
                {"raw", s.raw.is_null() ? json::object() : s.raw}};
}

json DiscoverGroupsPayload(const std::vector<DiscoverCacheGroup>& groups,
                           const std::string& error) {
    json arr = json::array();
    for (const auto& g : groups) {
        json sheets = json::array();
        for (const auto& s : g.sheets)
            sheets.push_back(SheetToPayloadJson(s, g.platform));
        arr.push_back(json{{"platform", g.platform}, {"sheets", std::move(sheets)}});
    }
    return json{{"groups", std::move(arr)}, {"error", error}};
}

bool ReadDiscoverCacheFile(const std::filesystem::path& path,
                           DiscoverCacheData* out) {
    if (!out || path.empty()) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kDiscoverCacheMaxBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    json root;
    try {
        in >> root;
    } catch (...) {
        return false;
    }
    if (!root.is_object() || JsonValue<int>(root, "version", 0) != 1) return false;
    DiscoverCacheData data;
    data.saved_at = JsonValue<std::int64_t>(root, "saved_at", 0);
    if (data.saved_at <= 0 || !root.contains("groups") ||
        !root["groups"].is_array())
        return false;
    for (const auto& g : root["groups"]) {
        if (!g.is_object()) continue;
        DiscoverCacheGroup group;
        group.platform = JsonValue<std::string>(g, "platform", "");
        if (group.platform.empty() || !g.contains("sheets") ||
            !g["sheets"].is_array())
            continue;
        for (const auto& sj : g["sheets"]) {
            if (!sj.is_object()) continue;
            group.sheets.push_back(SheetFromDiskJson(sj));
        }
        if (!group.sheets.empty()) data.groups.push_back(std::move(group));
    }
    if (data.groups.empty()) return false;
    *out = std::move(data);
    return true;
}

// 先写 .tmp 再原子替换（与 Win32 同一做法），避免中途退出留下半个 JSON。
void WriteDiscoverCacheFile(const std::filesystem::path& path,
                            const DiscoverCacheData& data) {
    if (path.empty() || data.groups.empty() || data.saved_at <= 0) return;
    try {
        json groups = json::array();
        for (const auto& g : data.groups) {
            json jg = json::object();
            jg["platform"] = g.platform;
            json sheets = json::array();
            for (const auto& s : g.sheets) sheets.push_back(SheetToDiskJson(s));
            jg["sheets"] = std::move(sheets);
            groups.push_back(std::move(jg));
        }
        json root = json::object();
        root["version"] = 1;
        root["saved_at"] = data.saved_at;
        root["groups"] = std::move(groups);
        const std::string text = root.dump();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path tmp(path.wstring() + L".tmp");
        {
            std::ofstream fout(tmp, std::ios::binary | std::ios::trunc);
            if (!fout) return;
            fout.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            std::filesystem::remove(tmp, ec);
    } catch (...) {
        // 缓存写失败不影响页面（下次仍可联网取）。
    }
}

// 本轮结果按音源替换、旧分组补齐（对齐 Win32 UpsertDiscoverGroup 的语义：
// 某个音源本轮失败时，页面上继续显示它上一次的卡片，不清空）。
std::vector<DiscoverCacheGroup> MergeDiscoverGroups(
    const std::vector<DiscoverCacheGroup>& fresh,
    const std::vector<DiscoverCacheGroup>& cached) {
    std::vector<DiscoverCacheGroup> merged = fresh;
    for (const auto& old : cached) {
        bool has = false;
        for (const auto& g : fresh) {
            if (g.platform == old.platform) {
                has = true;
                break;
            }
        }
        if (!has) merged.push_back(old);
    }
    return merged;
}

bool DiscoverPageAlive() {
    return !IsMusicFreeShuttingDown() && MusicFreeBrowserWindow() != nullptr;
}

// 同一时刻只跑一轮刷新；后到的 music.discover 由已回执的缓存与在途线程满足。
std::atomic<bool> g_discover_refreshing{false};

struct RefreshingReset {
    ~RefreshingReset() { g_discover_refreshing.store(false); }
};

}  // namespace

// ───────────────────────── 命令注册 ─────────────────────────

void RegisterMusicCommands(MusicFreeBridge& bridge) {
    // —— 挂状态推送的宿主：音乐窗窗口（子类化 + 定时器）——
    g_w = MusicBridgeWindowState{};
    g_w.bridge = &bridge;
    g_w.browser = MusicFreeBrowserWindow();
    if (g_w.browser && ::IsWindow(g_w.browser)) {
        // 重复注册同一窗口时 SetWindowSubclass / SetTimer 都按 id 替换，幂等。
        ::SetWindowSubclass(g_w.browser, MusicBridgeSubclass, kSubclassId, 0);
        g_w.timer_id = ::SetTimer(g_w.browser, kStateTimerId, kStatePollMs, nullptr);
        TickState();  // 初始同步（warmup 第 1 次）
    }

    // ── player.play：播放 ──
    // payload: {} → 恢复/起播当前列表（kCmdPlay 同一实现）；
    //          带曲目/列表 → ResolvePlayUrl → AddOnlineTrack（见头注）。
    bridge.Register("player.play",
                    [](const json& payload, MusicFreeBridge::ReplyFn reply) {
        const DWORD play_t0 = GetTickCount();
        {
            std::string intro = "PLAY entry";
            if (payload.is_object()) {
                if (payload.contains("tracks") && payload.at("tracks").is_array())
                    intro += " tracks=" +
                             std::to_string(payload.at("tracks").size());
                // 页面现在统一用 {track: 单曲}，日志必须跟着读 track 里的字段，
                // 否则 PLAY entry 全空，排查「点播失败」时看不到 id/platform。
                const json& one = (payload.contains("track") &&
                                   payload.at("track").is_object())
                                      ? payload.at("track") : payload;
                intro += " index=" + JsonStr(payload, "index");
                intro += " id=" + JsonStr(one, "id");
                intro += " platform=" + JsonStr(one, "platform");
                intro += " title=" + JsonStr(one, "title");
            }
            AppendPlayLog(intro);
        }
        try {
            std::vector<TrackSpec> specs;
            std::size_t play_index = 0;
            std::string error;
            std::string collect_warning;
            if (!CollectTrackSpecs(payload, specs, play_index, error,
                                  &collect_warning)) {
                AppendPlayLog("PLAY reject: " + error);
                reply(false, json{}, error);
                return;
            }
            ui::PlayerWindow* player = CurrentPlayer();
            if (!player) {
                AppendPlayLog("PLAY fail: player window unavailable");
                reply(false, json{}, "播放器主窗口不可用，无法播放");
                return;
            }

            // 无曲目：与主窗「播放」按钮同一实现（暂停态续播，否则重起当前条目）。
            if (specs.empty()) {
                AppendPlayLog("PLAY resume-only (no tracks)");
                static_cast<void>(ttplayer::testing::ProgressSeekAccess::Play(player));
                reply(true, StateResult(player), {});
                TickState();
                return;
            }

            // ★ 双击短时拦截（2026-11-10）：真机日志同一秒两条 PLAY entry
            //   都 added=1，列表加了两首。宿主是最终入列点，这里按曲目身份
            //   做 1.5s 窗口去重，兜住页面任何路径的重复投递。
            const std::string dedup_key = PlayDedupKey(specs);
            DWORD dedup_elapsed = 0;
            if (PlayDedupBlock(dedup_key, GetTickCount(), &dedup_elapsed)) {
                AppendPlayLog("PLAY dedup skipped key=" + dedup_key +
                              " within_ms=" + std::to_string(dedup_elapsed));
                json result = StateResult(CurrentPlayer(),
                                          json{{"accepted", true},
                                               {"tracks", specs.size()},
                                               {"added", 0},
                                               {"dedup", true}});
                reply(true, std::move(result), {});
                return;
            }

            // ★ 2026-11-10 用户反馈「双击很久才进播放列表 / 是不是完整下载」。
            //   旧实现在桥侧同步 ResolvePlayUrl（play.log 实测 28.7s）之后才
            //   ExecutePlayBatch，列表因此要等半分钟才出现；备用播放路径还会
            //   DownloadTrackFile 完整下载后再播。现在：
            //     1) 立即用 http 占位地址入列并起播，列表毫秒级出现；
            //     2) 播放窗 PlayCurrent 只读 resolve cache，未命中交后台异步
            //        解析；解析完重选本行走 HttpStreamSource 边下边播。
            //   桥侧不再联网取链，也不做任何完整下载。
            PlayBatchRequest req;
            req.hits.reserve(specs.size());
            req.urls.reserve(specs.size());
            std::size_t lazy = 0;
            for (const auto& spec : specs) {
                req.hits.push_back(spec.hit);
                if (!spec.url.empty()) {
                    req.urls.push_back(core::Utf8ToWide(spec.url));
                } else {
                    req.urls.push_back(LazyPlaceholderUrl(spec));
                    ++lazy;
                }
            }
            req.play_index = play_index;
            req.warning = collect_warning;
            AppendPlayLog("PLAY specs=" + std::to_string(specs.size()) +
                          " play_index=" + std::to_string(play_index) +
                          " immediate=1 lazy_enqueue=" + std::to_string(lazy));
            ExecutePlayBatch(req);
            AppendPlayLog("PLAY apply(immediate) applied=" +
                          std::string(req.applied ? "1" : "0") +
                          " added=" + std::to_string(req.added) +
                          " elapsed_ms=" +
                          std::to_string(GetTickCount() - play_t0));
            if (!req.applied) {
                PlayDedupForget(dedup_key);   // 失败可立刻重试，不被窗口误挡
                reply(false, json{}, req.error);
                return;
            }
            json result = StateResult(CurrentPlayer(),
                                      json{{"accepted", true},
                                           {"tracks", req.hits.size()},
                                           {"added", req.added},
                                           {"play_index", play_index},
                                           {"pending", lazy > 0}});
            if (!req.warning.empty()) result["warning"] = req.warning;
            reply(true, std::move(result), {});
            TickState();
            return;
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("播放失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "播放失败: 未知错误");
        }
    });

    // ── player.pause：暂停（kCmdPause 同一实现，player_window.cpp L5322）──
    bridge.Register("player.pause",
                    [](const json&, MusicFreeBridge::ReplyFn reply) {
        try {
            ui::PlayerWindow* player = CurrentPlayer();
            if (!player) {
                reply(false, json{}, "播放器主窗口不可用，无法暂停");
                return;
            }
            ttplayer::testing::ProgressSeekAccess::Pause(player);
            reply(true, StateResult(player), {});
            TickState();
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("暂停失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "暂停失败: 未知错误");
        }
    });

    // ── player.next：下一首（SelectRelative(true)，L5325）──
    bridge.Register("player.next",
                    [](const json&, MusicFreeBridge::ReplyFn reply) {
        try {
            ui::PlayerWindow* player = CurrentPlayer();
            if (!player) {
                reply(false, json{}, "播放器主窗口不可用，无法切歌");
                return;
            }
            ttplayer::testing::ProgressSeekAccess::Next(player);
            reply(true, StateResult(player), {});
            TickState();
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("下一首失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "下一首失败: 未知错误");
        }
    });

    // ── player.prev：上一首（SelectRelative(false)，L5324）──
    bridge.Register("player.prev",
                    [](const json&, MusicFreeBridge::ReplyFn reply) {
        try {
            ui::PlayerWindow* player = CurrentPlayer();
            if (!player) {
                reply(false, json{}, "播放器主窗口不可用，无法切歌");
                return;
            }
            ttplayer::testing::ProgressSeekAccess::Prev(player);
            reply(true, StateResult(player), {});
            TickState();
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("上一首失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "上一首失败: 未知错误");
        }
    });

    // ── player.seek：定位 ──
    // payload: { pos: 秒 } 或 { pos_ms: 毫秒 }（pos 可为小数）。
    // 走皮肤进度条松手提交的同一实现（SeekWithoutFade，L2944）。
    bridge.Register("player.seek",
                    [](const json& payload, MusicFreeBridge::ReplyFn reply) {
        try {
            std::optional<double> ms;
            if (payload.is_object()) {
                if (const auto by_ms = AsNumber(payload, "pos_ms")) ms = *by_ms;
                else if (const auto by_sec = AsNumber(payload, "pos"))
                    ms = *by_sec * 1000.0;
            }
            if (!ms) {
                reply(false, json{}, "缺少定位参数（payload.pos 秒或 payload.pos_ms 毫秒）");
                return;
            }
            ui::PlayerWindow* player = CurrentPlayer();
            if (!player) {
                reply(false, json{}, "播放器主窗口不可用，无法定位");
                return;
            }
            const auto duration =
                ttplayer::testing::ProgressSeekAccess::DurationMs(player);
            if (duration <= 0) {
                reply(false, json{}, "当前没有可定位的播放进度（未在播放）");
                return;
            }
            const auto target = static_cast<std::int64_t>(
                std::clamp(*ms, 0.0, static_cast<double>(duration)));
            ttplayer::testing::ProgressSeekAccess::Seek(player, target);
            reply(true, StateResult(player), {});
            TickState();  // 定位后 pos 立即可见（Position 先返回 pending 目标）
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("定位失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "定位失败: 未知错误");
        }
    });

    // ── player.volume：音量 ──
    // payload: { vol: 0..100 }。落值口径 = 皮肤音量松手（SetVolumeFromPoint
    // L6008-6009：写 settings + 引擎音量 + 清 mute）。
    bridge.Register("player.volume",
                    [](const json& payload, MusicFreeBridge::ReplyFn reply) {
        try {
            const auto vol = AsNumber(payload, "vol");
            if (!vol) {
                reply(false, json{}, "缺少音量参数（payload.vol 取值 0..100）");
                return;
            }
            ui::PlayerWindow* player = CurrentPlayer();
            if (!player) {
                reply(false, json{}, "播放器主窗口不可用，无法设置音量");
                return;
            }
            ttplayer::testing::ProgressSeekAccess::SetVolume(
                player, static_cast<int>(*vol));
            reply(true, StateResult(player), {});
            TickState();
        } catch (const std::exception& e) {
            reply(false, json{}, std::string("设置音量失败: ") + e.what());
        } catch (...) {
            reply(false, json{}, "设置音量失败: 未知错误");
        }
    });

    // ── music.discover：音乐首页「榜单 / 推荐」 ──
    // payload: {} 或 { platform?: "网易" }（限定单个音源）
    // 走 Win32 发现页那份磁盘缓存（<exe>\musicfree\discover_cache.json，12h TTL）：
    //   命中 → 立即推 evt（页面秒出卡片）；
    //   未过期 → 只推缓存、完全不联网；
    //   过期 / 无缓存 → 回 accepted 后起后台线程逐音源刷新（既有 GetDiscoverSheets，
    //                   与 Win32 发现页同一实现），成功后回写缓存再推更新。
    bridge.Register("music.discover",
                    [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
        const std::string only = JsonStr(payload, "platform");
        // 限定单音源时不吃全量缓存（只推部分分组会把页面其它音源清掉）。
        DiscoverCacheData cached;
        bool served = false;
        if (only.empty() && DiscoverPageAlive() &&
            ReadDiscoverCacheFile(DiscoverCachePath(), &cached)) {
            bridge.PushEvent("music.discover",
                             DiscoverGroupsPayload(cached.groups, ""));
            served = true;
        }
        if (served) {
            const std::int64_t now = NowUnixSeconds();
            if (cached.saved_at <= now &&
                now - cached.saved_at < kDiscoverCacheTtlSeconds) {
                reply(true, json{{"accepted", true}, {"cached", true}}, {});
                return;
            }
        }
        if (g_discover_refreshing.exchange(true)) {
            reply(true,
                  json{{"accepted", true},
                       {"cached", served},
                       {"already_running", true}},
                  {});
            return;
        }
        reply(true, json{{"accepted", true}, {"cached", served}}, {});
        try {
            std::thread([&bridge, only, served, cached = std::move(cached)] {
                BgThreadGuard guard;   // 退出时由 ShutdownMusicFree 等待
                RefreshingReset reset;
                std::vector<DiscoverCacheGroup> fresh;
                std::string err;
                std::vector<std::string> platforms;
                try {
                    if (IsMusicFreeShuttingDown()) return;
                    {
                        std::lock_guard<std::mutex> lk(g_plugin_io);
                        platforms = ListDiscoverPlatforms();
                    }
                    for (const auto& p : platforms) {
                        if (!only.empty() && p != only) continue;
                        DiscoverOutcome oc;
                        {
                            std::lock_guard<std::mutex> lk(g_plugin_io);
                            oc = GetDiscoverSheets(p);
                        }
                        if (oc.sheets.empty()) {
                            if (!oc.error.empty())
                                err += p + ": " + oc.error + "; ";
                            continue;
                        }
                        fresh.push_back({p, oc.sheets});
                    }
                } catch (const std::exception& e) {
                    err = e.what();
                } catch (...) {
                    err = "榜单读取异常";
                }
                if (!DiscoverPageAlive()) return;
                if (fresh.empty()) {
                    // 本轮一无所获：已显示缓存的页面保持原样，绝不用空数据覆盖；
                    // 本来就没缓存时维持旧行为（推空 groups + error）。
                    if (!served) {
                        bridge.PushEvent(
                            "music.discover",
                            json{{"groups", json::array()}, {"error", err}});
                    }
                    return;
                }
                // 部分音源失败时补上缓存里的旧分组（不清空这些音源的卡片）；
                // 已卸载音源的旧分组丢掉（对齐 Win32 按 platforms 过滤的收尾）。
                std::vector<DiscoverCacheGroup> merged = fresh;
                if (only.empty()) {
                    std::vector<DiscoverCacheGroup> keep;
                    for (const auto& old : cached.groups) {
                        if (std::find(platforms.begin(), platforms.end(),
                                      old.platform) != platforms.end())
                            keep.push_back(old);
                    }
                    merged = MergeDiscoverGroups(fresh, keep);
                    DiscoverCacheData snapshot;
                    snapshot.saved_at = NowUnixSeconds();
                    snapshot.groups = merged;
                    // 写缓存放在刷新线程收尾，进程退出前可能丢一轮（可接受）。
                    WriteDiscoverCacheFile(DiscoverCachePath(), snapshot);
                }
                json evt = DiscoverGroupsPayload(merged, err);
                evt["cached"] = false;
                bridge.PushEvent("music.discover", std::move(evt));
            }).detach();
        } catch (...) {
            // 线程创建失败：已回 accepted，页面保持缓存内容 / 加载失败态。
            g_discover_refreshing.store(false);
        }
    });

    // ── music.sheet：榜单 / 歌单详情 → 曲目列表 ──
    // payload: { platform, raw, kind: "top"|"sheet" }
    //（raw / kind 均来自 music.discover 的卡片，原样回传即可）
    // 结果走 evt music.sheet。
    bridge.Register("music.sheet",
                    [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
        const std::string platform = JsonStr(payload, "platform");
        if (platform.empty() || !payload.contains("raw") ||
            !payload["raw"].is_object()) {
            reply(false, json{},
                  "缺少 payload.platform / payload.raw（请从 music.discover "
                  "返回的卡片原样回传）");
            return;
        }
        const json raw = payload["raw"];
        const bool is_sheet = JsonStr(payload, "kind") == "sheet";
        reply(true, json{{"accepted", true}}, {});
        try {
            std::thread([&bridge, platform, raw, is_sheet] {
                BgThreadGuard guard;
                json items = json::array();
                std::string err;
                std::string sheet_title;
                try {
                    if (IsMusicFreeShuttingDown()) return;
                    sheet_title = raw.value("title", "");
                    SheetTracksOutcome oc;
                    {
                        std::lock_guard<std::mutex> lk(g_plugin_io);
                        oc = GetSheetTracks(platform, raw,
                                            is_sheet ? SheetKind::kSheet
                                                     : SheetKind::kTopList,
                                            1);
                    }
                    err = oc.error;
                    for (const auto& it : oc.items) {
                        items.push_back(json{
                            {"id", it.id},
                            {"platform", it.platform.empty() ? platform
                                                             : it.platform},
                            {"title", it.title},
                            {"artist", it.artist},
                            {"album", it.album},
                            {"duration",
                             it.duration_ms > 0
                                 ? std::to_string(it.duration_ms / 1000 / 60) +
                                       ":" +
                                       (it.duration_ms / 1000 % 60 < 10 ? "0" : "") +
                                       std::to_string(it.duration_ms / 1000 % 60)
                                 : std::string()},
                            {"duration_ms", it.duration_ms},
                            {"cover", it.artwork},
                            {"raw", it.raw}});
                    }
                } catch (const std::exception& e) {
                    err = e.what();
                } catch (...) {
                    err = "曲目读取异常";
                }
                bridge.PushEvent(
                    "music.sheet",
                    json{{"platform", platform},
                         {"title", sheet_title},
                         {"items", items},
                         {"error", err}});
            }).detach();
        } catch (...) {
        }
    });
}

}  // namespace ttplayer::musicfree

// ───────────────────────── 依赖既有符号清单（均无需改既有文件） ─────────────────────────
// 1) ttplayer::testing::ProgressSeekAccess      —— 本文件唯一定义（friend 预留口）
// 2) GWLP_USERDATA → PlayerWindow*              —— wtl_window.h WindowBinding::Start 写入
// 3) g_plugin_io / MusicFreeBrowserWindow()     —— musicfree_browser_window.h 已导出
// 4) ResolvePlayUrl / OnlineSearchHit           —— ttplayer/musicfree/musicfree_app.h
// 5) CMakeLists.txt 已登记本 .cpp（src/musicfree/bridge_cmds_music.cpp）；
//    宿主只需在音乐窗创建桥后调用一次 RegisterMusicCommands(*state->bridge)。
// 若未来 player_window.h 移除 ProgressSeekAccess 的 friend 声明，本文件需要
// 宿主改为提供公开的状态快照入口（届时在汇报清单中同步更新）。
