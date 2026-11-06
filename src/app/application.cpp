#include "ttplayer/app/application.h"

#include "ttplayer/app/startup_trace.h"
#include "ttplayer/app/runtime.h"
#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "ttplayer/ui/wtl_runtime.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <memory>
#include <utility>
#include <windows.h>
#include <shellapi.h>

namespace ttplayer::app {
namespace {
// 后台音源重建完成通知。收到后才做依赖音源列表的启动收尾（恢复音乐窗、
// 恢复播放），主窗口不必等 1.9s 的插件挂载就能响应。
constexpr UINT kMsgStartupSourcesReady = WM_APP + 0x51;
// AddIn 音效库在窗口显示后的空闲时段加载：一次性定时器，用户提前操作时
// 由消息过滤器兜底同步加载。定时器 id 选一个不与播放器自有定时器冲突的值。
constexpr UINT_PTR kDeferredStartupTimer = 0x5454;
constexpr UINT kDeferredStartupDelayMs = 350;

bool IsUserInputMessage(UINT message) noexcept {
    switch (message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: case WM_SYSCHAR:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_COMMAND: case WM_SYSCOMMAND: case WM_COPYDATA:
    case WM_DROPFILES: case WM_HOTKEY: case WM_APPCOMMAND:
        return true;
    default:
        return false;
    }
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
settings::Settings LoadSettingsIfPresent() {
    return settings::LoadRuntimeSettings(RuntimePath({}));
}
} // namespace

bool ParsedCommandLine::HasSwitch(std::wstring_view name) const {
    const auto expected = Lower(std::wstring(name));
    return std::find(switches.begin(), switches.end(), expected) != switches.end();
}

ULONG_PTR ParsedCommandLine::FileMode() const {
    return static_cast<ULONG_PTR>(ui::ResolveCommandLineFileMode(
        HasSwitch(L"a"), HasSwitch(L"e")));
}

ParsedCommandLine ParseCommandLine() {
    ParsedCommandLine parsed;
    int count{};
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(GetCommandLineW(), &count), &LocalFree);
    for (int index = 1; arguments && index < count; ++index) {
        std::wstring argument(arguments.get()[index]);
        if (argument == L"--smoke-test") { parsed.smoke_test = true; continue; }
        if (argument.rfind(L"--mf-selftest=", 0) == 0) {
            parsed.mf_selftest = core::WideToUtf8(
                std::wstring_view(argument).substr(14));
            parsed.smoke_test = true;
            continue;
        }
        if (argument == L"--dpi-selftest") {
            parsed.dpi_selftest = true;
            parsed.smoke_test = true;
            continue;
        }
        if (!argument.empty() && (argument.front() == L'/' || argument.front() == L'-')) {
            argument.erase(argument.begin());
            parsed.switches.push_back(Lower(std::move(argument)));
        } else if (parsed.file.empty()) {
            parsed.file = std::move(argument);
        }
    }
    return parsed;
}

int TTPlayer_RunApplicationSession(HINSTANCE instance, int show_command,
                                   SingleInstanceIpc& single_instance,
                                   HMODULE resource_module,
                                   HMODULE ttpcomm_module,
    const plugins::PluginManager& sound_library,
    const std::function<void()>& ensure_sound_library) {
    TT_STARTUP_TRACE("session_enter");
    if (FAILED(ui::EnsureWtlRuntime())) return -1;
    const auto command_line = ParseCommandLine();
    auto settings = LoadSettingsIfPresent();
    TT_STARTUP_TRACE("settings_loaded");
    if (command_line.HasSwitch(L"reg") || command_line.HasSwitch(L"unreg")) {
        ui::PlayerWindow association(std::move(settings));
        association.SetSkinResourceModule(resource_module);
        association.SetTtpCommModule(ttpcomm_module);
        association.SetSoundLibrary(&sound_library);
        if (command_line.HasSwitch(L"reg")) {
            // 004C0900 constructs the reduced About + System Association
            // property sheet; it does not silently register every format.
            static_cast<void>(association.ShowRegistrationOptions(instance));
        } else {
            // FUN_0049CD41 removes registered reader/playlist patterns plus
            // the AudioCD and Directory shell verbs, then notifies Explorer.
            static_cast<void>(association.UnregisterAllAssociations());
        }
        return 0;
    }

    ui::PlayerWindow player(std::move(settings));
    player.SetSkinResourceModule(resource_module);
    player.SetTtpCommModule(ttpcomm_module);
    player.SetSoundLibrary(&sound_library);
    static_cast<void>(player.LoadStartupSkin(resource_module));
    TT_STARTUP_TRACE("skin_loaded");
    if (!player.Create(instance, show_command)) return 0;
    TT_STARTUP_TRACE("window_created");
    // AddIn 音效库（几十个 DLL 逐个 LoadLibrary）和文件关联检查都延后到窗口
    // 显示后的空闲时段。Create() 内已 UpdateWindow 完成首帧绘制，这里只需排
    // 一个短的一次性定时器；用户若提前操作，PlayerFilter 会在放行输入前兜底。
    if (!command_line.smoke_test)
        SetTimer(player.Handle(), kDeferredStartupTimer,
                 kDeferredStartupDelayMs, nullptr);

    // 音源重建（部署内置 JS 依赖库 + 为每个缓存插件建 QuickJS runtime）实测
    // ~1.9s。以前放在这里同步执行，窗口虽已 ShowWindow 却进不了消息循环，
    // 用户看到的就是「启动很慢才显示窗口」。现在交给低优先级后台线程，
    // 完成后用 kMsgStartupSourcesReady 触发依赖音源列表的启动收尾。
    musicfree::StartMusicFreeSourcesLoading(player.Handle(),
                                            kMsgStartupSourcesReady);
    TT_STARTUP_TRACE("bg_sources_scheduled");
    // 自检模式需要确定性的完整音源列表，且马上就要退出，这里直接等后台完成。
    if (command_line.smoke_test) musicfree::WaitForMusicFreeSources();

    // 发布 HWND 给第二实例之前先处理完本次启动请求，避免 WM_COPYDATA 交错。
    // 注意：这里**不要**调用 SetProcessWorkingSetSize(..., -1, -1)。原版用它
    // 主动清空工作集省内存，但刚装入的 ttpcomm/ttpres、皮肤位图和字体会被
    // 立刻换出，首帧绘制时又得逐页调回，实测拖慢窗口第一次响应几百毫秒。
    // 现代 Windows 的工作集管理已经足够，去掉它对内存占用的影响很小。
    bool dpi_ok = true;
    if (command_line.dpi_selftest) dpi_ok = player.RunDpiSelfTest();
    // 恢复上次播放可能要解析在线曲目（依赖音源插件），正常启动时放到音源
    // 就绪之后；自检模式保持同步，避免 --smoke-test 的 WM_CLOSE 抢在前面。
    bool playback_deferred = false;
    if (!command_line.mf_selftest.empty())
        player.RunMusicFreeSelfTest(command_line.mf_selftest);
    else if (!command_line.file.empty())
        player.OpenCommandLinePath(command_line.file, command_line.FileMode());
    else if (!command_line.smoke_test)
        playback_deferred = true;
    else
        player.RestoreStartupPlayback();
    TT_STARTUP_TRACE("playback_scheduled");
    single_instance.PublishWindow(player.Handle());
    SetForegroundWindow(player.Handle());
    SetActiveWindow(player.Handle());
    // 文件关联检查依赖音效库的格式表，已随 AddIn 一起移到延迟任务里。
    TT_STARTUP_TRACE("startup_sync_done");
    if (command_line.smoke_test) PostMessageW(player.Handle(), WM_CLOSE, 0, 0);

    struct PlayerFilter final : WTL::CMessageFilter {
        ui::PlayerWindow& player;
        ParsedCommandLine command_line;
        const plugins::PluginManager& sound_library;
        std::function<void()> ensure_sound_library;
        bool playback_deferred = false;
        bool deferred_done = false;
        bool sound_ready = false;
        PlayerFilter(ui::PlayerWindow& value, ParsedCommandLine parsed, bool defer,
                     const plugins::PluginManager& library,
                     std::function<void()> ensure, bool sound_already_ready)
            : player(value), command_line(std::move(parsed)),
              sound_library(library), ensure_sound_library(std::move(ensure)),
              playback_deferred(defer), sound_ready(sound_already_ready) {}

        // AddIn 加载完必须重新 SetSoundLibrary：它会在调用瞬间把 manager 的
        // ReaderFormats 复制进播放器；窗口创建前那次复制发生在懒加载之前，
        // 格式表是空的，不刷新会导致「打开音频」对话框无格式、媒体库不扫描。
        void EnsureSoundLibrary() {
            if (sound_ready) return;
            sound_ready = true;
            KillTimer(player.Handle(), kDeferredStartupTimer);
            if (ensure_sound_library) {
                ensure_sound_library();
                player.SetSoundLibrary(&sound_library);
                TT_STARTUP_TRACE("sound_library_ready");
            }
            // 文件关联检查依赖上面刷新的格式表，必须排在加载之后。
            if (!command_line.smoke_test) player.CheckStartupAssociations();
            TT_STARTUP_TRACE("associations_checked");
        }

        BOOL PreTranslateMessage(MSG* message) override {
            if (message->message == WM_TIMER &&
                message->wParam == kDeferredStartupTimer) {
                EnsureSoundLibrary();
                return TRUE;
            }
            if (message->message == kMsgStartupSourcesReady) {
                if (!deferred_done) {
                    deferred_done = true;
                    TT_STARTUP_TRACE("deferred_start");
                    // 恢复播放要用解码器，先确保 AddIn 已就位（正常情况下
                    // 350ms 的一次性定时器早就跑完了，这里是幂等兜底）。
                    EnsureSoundLibrary();
                    // 后台线程发消息前已置 done，这里不会阻塞。
                    musicfree::WaitForMusicFreeSources();
                    // 上次退出时音乐窗是开着的，按同样的状态恢复；必须等音源
                    // 挂载完，否则平台列表是空的。自检模式不弹窗。
                    if (!command_line.smoke_test)
                        player.RestoreStartupMusicBrowserWindow();
                    if (playback_deferred) player.RestoreStartupPlayback();
                    TT_STARTUP_TRACE("deferred_done");
                }
                return TRUE;
            }
            // 三个辅助顶层窗由 WM_CREATE 投递的消息在消息循环里补建；若用户
            // 输入更早到达，先补齐（幂等），保证命令处理时三个 HWND 都在。
            if (IsUserInputMessage(message->message))
                player.CreateStartupAuxWindows();
            // 用户比定时器更早操作：先把音效库补齐再放行，绝不让打开文件或
            // 播放走到空格式表上。
            if (!sound_ready && IsUserInputMessage(message->message))
                EnsureSoundLibrary();
            return player.PreTranslateMessage(*message);
        }
    } filter(player, command_line, playback_deferred, sound_library,
             ensure_sound_library, command_line.smoke_test);
    ui::PlayerMessageLoop loop;
    if (!_Module.AddMessageLoop(&loop)) return -1;
    loop.AddMessageFilter(&filter);
    TT_STARTUP_TRACE("message_loop_start");
    const int result = loop.Run();
    loop.RemoveMessageFilter(&filter);
    _Module.RemoveMessageLoop();
    // 回收后台音源加载线程，避免 CRT 静态析构时还有线程在跑。
    musicfree::FinishMusicFreeSourcesLoading();
    single_instance.PublishWindow(nullptr);
    // A geometry regression is only useful if CI can see it: fail the process
    // when any High-DPI invariant broke, even though the UI itself is fine.
    if (command_line.dpi_selftest && !dpi_ok) return 1;
    return result;
}
} // namespace ttplayer::app
