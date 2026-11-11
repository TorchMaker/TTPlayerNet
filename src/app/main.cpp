#include "ttplayer/app/application.h"
#include "ttplayer/app/startup_trace.h"
#include "ttplayer/app/file_info_worker.h"
#include "ttplayer/app/runtime.h"
#include "ttplayer/app/single_instance.h"
#include "ttplayer/app/worker_process.h"
#include "ttplayer/platform/optional_windows_api.h"
#include "ttplayer/i18n/i18n.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/video/video_web_host.h"   // 视频窗网页兜底（纯接口 + 工厂注入）
#include "ttplayer/video/video_module_api.h" // 视频模块插件入口（C ABI）
#include "../musicfree/video_web_host.h"     // WebView2 实现（只在 exe 里编入）
#include "../ui/file_info_probe_client.h"

#include <commctrl.h>
#include <delayimp.h>
#include <objbase.h>
#include <shellapi.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <windows.h>

namespace {

// 崩溃诊断：产品以 GUI 子系统运行，CRT 的 terminate 文本不会出现在 stderr，
// 只能自己抓。SIGABRT / std::terminate / SEH 三路都记录返回地址（RVA），
// 配合链接器生成的 .map 即可精确定位崩溃函数。日志追加到 %TEMP%。
void WriteCrashStack(const char* reason, DWORD code) {
    void* frames[48]{};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    const auto base =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    wchar_t path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"ttplayer_crash.log");
    FILE* f = nullptr;
    // 不要用 ccs=UTF-8：那会把流切到 Unicode 模式，窄字符 fprintf 会失败，
    // 甚至触发 invalid-parameter -> abort 递归。日志内容全是 ASCII。
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    fprintf(f, "CRASH reason=%s code=0x%08lX base=0x%p frames=%u\n",
            reason, static_cast<unsigned long>(code),
            reinterpret_cast<void*>(base), static_cast<unsigned>(count));
    for (USHORT i = 0; i < count; ++i) {
        const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
        fprintf(f, "  [%02u] rva=0x%08X abs=0x%p\n", static_cast<unsigned>(i),
                static_cast<unsigned>(addr - base),
                reinterpret_cast<void*>(addr));
    }
    fflush(f);
    fclose(f);
}

void OnSigAbort(int) { WriteCrashStack("SIGABRT", 0); }

[[noreturn]] void OnTerminate() {
    WriteCrashStack("std::terminate", 0);
    std::abort();
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    WriteCrashStack("SEH", info && info->ExceptionRecord
                               ? info->ExceptionRecord->ExceptionCode
                               : 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashDiag() {
    WriteCrashStack("hooks_installed", 0);
    std::signal(SIGABRT, OnSigAbort);
    std::set_terminate(OnTerminate);
    SetUnhandledExceptionFilter(OnUnhandledException);
}

constexpr wchar_t kSingleInstanceName[] = L"TTPlayer";
constexpr DWORD kSingleInstanceMappingSize = 4096;

// ── 视频模块（ttplayer_video.dll）的插件化加载 ──────────────────────────────
// 与 OptionalI18nRuntime 同策略：可选模块，缺失不阻断启动。
//
// 模块按 C ABI 插件约定（video_module_api.h）导出唯一工厂 ttpGetVideoModule，
// 提供 name/version 与 query() 能力协商 —— 未来新增内容类型（短剧/漫画/…）只需
// 在模块内登记新的 api 名字，宿主与 ABI 均零改动。发现逻辑与
// video_protocol_registry 加载 AddIn 插件的方式同构（LoadLibrary + 双名查找）。
//
// 配合链接器的 /DELAYLOAD:ttplayer_video.dll：进程启动不强制加载该 DLL，只有
// 第一次真正调用视频函数时才解析 —— DLL 缺失时主程序照常运行（听歌/其它功能
// 全部可用），由下方失败钩子兜底提示。
class OptionalVideoModuleRuntime {
public:
    OptionalVideoModuleRuntime() {
        const auto runtime = ttplayer::app::RuntimePath({});
        if (!runtime.empty())
            module_ = LoadLibraryW((runtime / L"ttplayer_video.dll").c_str());
        if (!module_) return;  // 缺失：视频功能不可用，主程序照常。
        // Win32/x86 的 WINAPI(stdcall) C 导出会被链接器修饰成 _name@N；
        // 与 video_protocol_registry 一样同时兼容朴素名与修饰名。
        FARPROC proc = GetProcAddress(module_, "ttpGetVideoModule");
        if (!proc) proc = GetProcAddress(module_, "_ttpGetVideoModule@0");
        const auto factory = reinterpret_cast<decltype(&ttpGetVideoModule)>(proc);
        if (!factory) return;
        const TtpVideoModule* mod = factory();
        if (!mod || mod->abi_version != TTPLAYER_VIDEO_MODULE_ABI_VERSION ||
            mod->size < sizeof(TtpVideoModule))
            return;  // ABI 不匹配：按能力协商原则视为不可用。
        // 能力协商：按名字探测已登记的扩展点（统计只为诊断，不改变行为）。
        static const char* const kKnownApis[] = {
            "video.cms", "video.resolve", "video.source_dialog",
            "video.player_window", "video.source_manager", "video.web_host",
        };
        int caps = 0;
        if (mod->query)
            for (const char* api : kKnownApis)
                if (mod->query(api)) ++caps;
        (void)caps;
        ready_ = mod;
    }
    ~OptionalVideoModuleRuntime() {
        if (module_) FreeLibrary(module_);
    }
    OptionalVideoModuleRuntime(const OptionalVideoModuleRuntime&) = delete;
    OptionalVideoModuleRuntime& operator=(const OptionalVideoModuleRuntime&) = delete;
    bool ready() const { return ready_ != nullptr; }

private:
    HMODULE module_{};
    const TtpVideoModule* ready_{};
};

// /DELAYLOAD 的兜底：ttplayer_video.dll 真缺失且代码走到了视频调用点时，
// 记崩溃诊断并明确提示，而不是无声地空指针崩溃。
FARPROC WINAPI VideoDelayLoadFailure(unsigned, PDelayLoadInfo) {
    WriteCrashStack("video_module_missing", 0);
    MessageBoxW(nullptr,
                ttplayer::i18n::Literal(
                    L"Video module ttplayer_video.dll is missing, video features are disabled."),
                ttplayer::i18n::Literal(L"TTPlayer"), MB_OK | MB_ICONWARNING);
    return nullptr;
}

// delayimp 的全局失败钩子（本工程仅 ttplayer_video 一个延迟加载 DLL）。
extern "C" const PfnDliHook __pfnDliFailureHook2 = VideoDelayLoadFailure;

// Read only the language and initialize translations before any startup error
// can open a message box. Full settings and skin loading still happen later.
class OptionalI18nRuntime {
public:
    OptionalI18nRuntime() {
        const auto runtime = ttplayer::app::RuntimePath({});
        if (!runtime.empty()) module_ = LoadLibraryW((runtime / L"AddIn" / L"ttp_i18n.dll").c_str());
        if (module_) {
            ttplayer::i18n::Initialize(runtime, ttplayer::settings::LoadRuntimeLanguage(runtime));
        }
    }
    ~OptionalI18nRuntime() {
        ttplayer::i18n::Shutdown();
        if (module_) FreeLibrary(module_);
    }
    OptionalI18nRuntime(const OptionalI18nRuntime&) = delete;
    OptionalI18nRuntime& operator=(const OptionalI18nRuntime&) = delete;
private:
    HMODULE module_{};
};

int TTPlayer_wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    namespace i18n = ttplayer::i18n;
    // Load the optional i18n provider before actively loading ttpcomm. Missing
    // translations never prevent startup. The XP edition retains a mandatory
    // ttpcomm startup import for static TLS, so Windows loads that dependency
    // before this entry point; do not replace it with LoadLibrary-only loading.
    const OptionalI18nRuntime i18n_runtime;
    TT_STARTUP_TRACE("i18n_ready");

    // Retain EXE-local dependency paths and omit the original exact-version
    // gate. ABI-compatible revisions are accepted; consumers check exports.
    ttplayer::app::TtpCommRuntime ttpcomm;
    if (!ttpcomm.Initialize()) {
        MessageBoxW(nullptr, i18n::Literal(L"Error in ttpcomm.dll, please resetup this program!"),
                    i18n::Literal(L"TTPlayer"), MB_OK | MB_ICONERROR);
        return -1;
    }
    TT_STARTUP_TRACE("ttpcomm_ready");

    // CSingleInstanceIpc_Initialize is a static-runtime predecessor of the
    // recovered wWinMain. Its observable branch is reproduced here.
    const auto command_line = ttplayer::app::ParseCommandLine();
    std::wstring instance_name = kSingleInstanceName;
    if (command_line.smoke_test)
        instance_name += L".Smoke." + std::to_wstring(GetCurrentProcessId());
    ttplayer::app::SingleInstanceIpc single_instance(
        instance_name, kSingleInstanceMappingSize);
    if (!single_instance.Valid()) return 1;
    if (!single_instance.IsPrimary()) {
        single_instance.ForwardCommandLine(command_line.file.native(),
                                           command_line.FileMode());
        single_instance.Shutdown();
        return 1;
    }
    single_instance.PublishProcess();
    TT_STARTUP_TRACE("single_instance_ready");

    // Main-instance initialization order from 004C0E8F.
    const DWORD tls_index = TlsAlloc();
    static constexpr char kMainThreadName[] = "wWinMain";
    if (tls_index != TLS_OUT_OF_INDEXES)
        TlsSetValue(tls_index, const_cast<char*>(kMainThreadName));
    const HRESULT ole_result = OleInitialize(nullptr);
    // 视频模块发现（插件化）：LoadLibrary + ttpGetVideoModule 校验 ABI 与能力。
    // 必须在首次调用任何视频导入函数之前完成，/DELAYLOAD 的解析随即命中已加载
    // 的同一实例；缺失时仅记录，视频功能不可用但主程序照常启动。
    const OptionalVideoModuleRuntime video_module;
    TT_STARTUP_TRACE(video_module.ready() ? "video_module_ready"
                                          : "video_module_missing");
    // 视频窗网页兜底：官方网页线路解析全部失败时用 WebView2 在播放窗内加载网页。
    // WebView2 只链接在本 exe 上，核心库（ttplayer_core）只持有函数指针，
    // 故在 OLE 初始化成功后注入实现。
    if (SUCCEEDED(ole_result))
        ttplayer::video::SetVideoWebHostFactory(
            &ttplayer::musicfree::CreateVideoWebHost);
    // Dialogs 250 and 260 in ttpres.dll use SysDateTimePick32 and
    // ComboBoxEx32 respectively.  Register them explicitly rather than
    // depending on another component having initialized those classes.
    INITCOMMONCONTROLSEX controls{
        sizeof(controls), ICC_WIN95_CLASSES | ICC_BAR_CLASSES |
                              ICC_DATE_CLASSES | ICC_USEREX_CLASSES};
    InitCommonControlsEx(&controls);

    ttplayer::app::ResourceRuntime resources;
    if (!resources.Initialize()) {
        if (SUCCEEDED(ole_result)) OleUninitialize();
        if (tls_index != TLS_OUT_OF_INDEXES) TlsFree(tls_index);
        MessageBoxW(nullptr, i18n::Literal(L"Load resource (ttpres.dll) failed, please resetup this program!"),
                    i18n::Literal(L"TTPlayer"), MB_OK | MB_ICONERROR);
        return -1;
    }

    TT_STARTUP_TRACE("ttpres_ready");
    if (ttpcomm.Api().srand48) ttpcomm.Api().srand48(GetTickCount());
    ttplayer::app::SoundLibraryRuntime sound_library;
    const auto addin_path = ttplayer::app::FindRuntimePath(L"AddIn");
    // AddIn 音效库要逐个 LoadLibrary + 枚举接口，DLL 越多越慢（实测 40~457ms），
    // 而主窗口创建、皮肤解析与首帧绘制完全不依赖它，多数启动也不会立刻播放。
    // 这里只登记目录，真正的加载由会话在窗口显示后的空闲时段触发；用户第一次
    // 操作前会兜底同步加载（见 application.cpp 的 PlayerFilter）。
    sound_library.SetDirectory(addin_path);
    // 无窗口的自检与文件关联注册分支需要确定性结果，保持启动即完整加载。
    const bool sound_needed_now = command_line.smoke_test ||
        command_line.HasSwitch(L"reg") || command_line.HasSwitch(L"unreg");
    const HRESULT sound_result = sound_needed_now ? sound_library.EnsureLoaded() : S_OK;
    if (FAILED(sound_result)) {
        // DAT_005474FC remains a live application-title string after
        // CPlayerApp_ShutdownResources in 004C0E8F.  Preserve its resource
        // value before unloading ttpres.dll.
        const auto title = i18n::ResourceText(resources.Module(), 0x80);
        resources.Shutdown();
        if (SUCCEEDED(ole_result)) OleUninitialize();
        if (tls_index != TLS_OUT_OF_INDEXES) TlsFree(tls_index);
        MessageBoxW(nullptr, i18n::Literal(L"Sound Library init failed, the player can not run!"),
                    title.c_str(), MB_OK | MB_ICONERROR);
        return static_cast<int>(sound_result);
    }

    // 004B58AD (WH_CALLWNDPROC + WH_CBT) precedes CoolSB ordinal 200.
    ttplayer::app::ThreadMessageHooksRuntime message_hooks;
    message_hooks.Install();
    TT_STARTUP_TRACE(sound_needed_now ? "sound_library_ready"
                                     : "sound_library_deferred");
    if (ttpcomm.Api().coolsb_init_app) ttpcomm.Api().coolsb_init_app();
    TT_STARTUP_TRACE("session_start");
    const int result = ttplayer::app::TTPlayer_RunApplicationSession(
        instance, show_command, single_instance, resources.Module(),
        ttpcomm.Api().module,
        sound_library.Manager(),
        [&sound_library] { static_cast<void>(sound_library.EnsureLoaded()); });

    // Exact reverse-order teardown from 004C0E8F.
    TT_STARTUP_TRACE("teardown_session_returned");
    single_instance.Shutdown();
    TT_STARTUP_TRACE("teardown_single_instance");
    if (ttpcomm.Api().coolsb_uninit_app) ttpcomm.Api().coolsb_uninit_app();
    message_hooks.Shutdown();
    TT_STARTUP_TRACE("teardown_hooks");
    sound_library.Shutdown();
    TT_STARTUP_TRACE("teardown_sound_library");
    resources.Shutdown();
    TT_STARTUP_TRACE("teardown_resources");
    if (SUCCEEDED(ole_result)) OleUninitialize();
    if (tls_index != TLS_OUT_OF_INDEXES) { TlsSetValue(tls_index, nullptr); TlsFree(tls_index); }
    ttpcomm.Shutdown();
    TT_STARTUP_TRACE("teardown_ttpcomm_done_wWinMain_returning");
    return result;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show_command) {
    TT_STARTUP_TRACE_RESET();
    InstallCrashDiag();
    TT_STARTUP_TRACE("wWinMain_enter");
    // Original AddIns use pre-ATL-8 heap window thunks. Keep DEP enabled,
    // but let Windows emulate those thunks, rather than declaring the whole
    // legacy plugin graph NX-compatible. Paired with /NXCOMPAT:NO in CMake;
    // applies before both normal startup and embedded plugin workers.
    // AlwaysOn/AlwaysOff or externally imposed process policy takes precedence.
    SetProcessDEPPolicy(PROCESS_DEP_ENABLE);
    // Enable per-monitor (V2) DPI awareness as early as possible, before any
    // window is created. V2 gives us WM_DPICHANGED per window and proper
    // dialog/Menu/Tooltip scaling; the manifest declares the same so a
    // pre-wWinMain path also takes effect. Layout/scaling is implemented in
    // the skin layer (docs/HIGH_DPI_SKIN_SPEC.md), so the OS no longer has to
    // bitmap-stretch a DPI-unaware window.
    {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4. Declared as
        // void* to avoid depending on a Win10 SDK DPI_AWARENESS_CONTEXT typedef.
        // reinterpret_cast is not a constant expression under /W4; a plain
        // local (not constexpr) is enough here.
        void* kPerMonitorV2 = reinterpret_cast<void*>(-4);
        if (!ttplayer::platform::SetProcessDpiAwarenessContext(kPerMonitorV2)) {
            // PROCESS_PER_MONITOR_DPI_AWARE == 2 (Win8.1 fallback).
            if (FAILED(ttplayer::platform::SetProcessDpiAwareness(2)))
                SetProcessDPIAware(); // XP/Win7 final fallback keeps system DPI.
        }
    }
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    // Private workers bypass DLL startup, single-instance IPC, settings and
    // all player windows. Dispatch even malformed requests here: a failed
    // probe must never fall through to launch another player instance.
    if (arguments && count >= 2) {
        int result{};
        bool worker = true;
        if (wcscmp(arguments[1], ttplayer::app::kFileInfoWorkerSwitch) == 0)
            result = ttplayer::app::RunFileInfoWorker(count - 1, arguments + 1);
        else if (wcscmp(arguments[1], ttplayer::app::kDspWorkerSwitch) == 0)
            result = ttplayer::app::RunDspWorker(count - 1, arguments + 1, true);
        else if (wcscmp(arguments[1], ttplayer::app::kOutputDeviceWorkerSwitch) == 0)
            result = ttplayer::app::RunOutputDeviceWorker(count - 1, arguments + 1);
        else
            worker = false;
        if (worker) {
            LocalFree(arguments);
            return result;
        }
    }
    if (arguments) LocalFree(arguments);
    ttplayer::ui::detail::EnableEmbeddedFileInfoProbe();
    return TTPlayer_wWinMain(instance, previous, command_line, show_command);
}
