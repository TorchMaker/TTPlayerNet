// WebView2 宿主实现 —— 状态机 Idle → Creating → Ready / Failed
//
// 职责边界（见 musicfree_webview_host.h 与 spec「WebView2 宿主与页面加载」）：
//   · Create 立即返回，环境/控制器经 WebView2 异步回调在 UI 线程继续，绝不阻塞；
//   · 页面根目录（webui_assets 解包出的绝对目录）经
//     SetVirtualHostNameToFolderMapping 映射成 https://app.webui/ 再 Navigate；
//   · 与 MusicFreeBridge 的三个注入点：sender/poster（Create 时）、
//     NavigationCompleted 成功推 bridge.ready（+FlushPending）、
//     WebMessageReceived 收页面消息喂 OnWebMessage；
//   · 一切失败只落 <exe>\video\bridge.log + 置 Failed（宿主据此回退旧 Win32 控件），
//     绝不弹框、绝不崩溃（所有路径 catch）。
//
// 线程模型：
//   · WebView2 回调、TimerProc 均在父窗口所在线程（UI 线程）执行；
//   · poster 可从任意线程调：任务入互斥队列 + SetTimer 投递，
//     WM_TIMER（带 TIMERPROC）经 DispatchMessage 直接回调 TimerProc，
//     **不经过父窗口过程** —— 因此无需修改任何既有窗口过程。

#include "musicfree_webview_host.h"

#include <windows.h>
// objbase.h 必须先于 WebView2.h：interface 宏由 combaseapi.h 定义（经 objbase 带入），
// 而 windows.h 本身不包含它，缺了会让 WebView2.h 的 `typedef interface IXxx` 全线报 C2146。
#include <objbase.h>
#include <WebView2.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

#include "musicfree_bridge.h"
#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/webview2_dynamic.h"
#include "ttplayer/musicfree/webview2_env_options.h"
#include "ttplayer/musicfree/webview2_runtime.h"

namespace ttplayer::musicfree {
namespace {

// ───────────────────────── 日志 ─────────────────────────
// 与 protocol_web.cpp 的 AppendProtoFailLog 同一算法：GetModuleFileNameW(nullptr)
// 取 exe 所在目录拼 "\video\bridge.log"，失败级始终写；带时间戳块格式一致。

void AppendHostLog(const std::string& text) {
    try {
        wchar_t exe[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
        const std::wstring p(exe);
        const size_t s = p.find_last_of(L"\\/");
        if (s == std::wstring::npos) return;
        const std::wstring path = p.substr(0, s) + L"\\video\\bridge.log";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
        char stamp[64]{};
        SYSTEMTIME t{};
        GetLocalTime(&t);
        std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
                      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                      t.wSecond);
        std::fprintf(f, "\n==== %s ====\n[webview_host] %s\n", stamp,
                     text.c_str());
        std::fclose(f);
    } catch (...) {
    }
}

// ───────────────────────── 小工具 ─────────────────────────

// HRESULT 转可读后缀（拼进 Fail 的中文原因）。
std::string HrText(HRESULT hr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), " (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
    return std::string(buf);
}

// %LOCALAPPDATA%\TTPlayer\webview2（父目录已存在则忽略，webview2 照建）。
bool BuildUserDataFolder(std::wstring* out) {
    wchar_t base[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 1024);
    if (n == 0 || n >= 1024) return false;
    std::wstring dir(base);
    dir += L"\\TTPlayer";
    CreateDirectoryW(dir.c_str(), nullptr);  // 已存在返回 0，忽略
    dir += L"\\webview2";
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) return false;
    }
    *out = std::move(dir);
    return true;
}

// EdgeUpdate 注册表 pv 的读取已统一移到 webview2_runtime.cpp 的
// PlanWebView2Runtime()（按系统版本选 Runtime），此处不再重复实现。

// ───────────────── 单 Invoke 的 COM 回调壳 ─────────────────
// WebView2 的完成/事件回调都是「只有一个 Invoke」的 IUnknown 接口；
// 手写最小 COM 壳（不引 WRL），引用计数原子、归零自删。

template <typename Iface, typename Fn>
class ComCallback : public Iface {
public:
    explicit ComCallback(Fn fn) : fn_(std::move(fn)) {}
    ComCallback(const ComCallback&) = delete;
    ComCallback& operator=(const ComCallback&) = delete;
    virtual ~ComCallback() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override {
        if (!obj) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(Iface)) {
            *obj = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

protected:
    Fn fn_;

private:
    std::atomic<ULONG> ref_{1};
};

using EnvFn = std::function<void(HRESULT, ICoreWebView2Environment*)>;
using CtrlFn = std::function<void(HRESULT, ICoreWebView2Controller*)>;
using NavFn =
    std::function<void(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)>;
using MsgFn =
    std::function<void(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)>;

class ComEnvDone
    : public ComCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                         EnvFn> {
public:
    explicit ComEnvDone(EnvFn fn)
        : ComCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                      EnvFn>(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr,
                                     ICoreWebView2Environment* e) override {
        try {
            fn_(hr, e);
        } catch (...) {
        }
        return S_OK;
    }
};

class ComCtrlDone
    : public ComCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                         CtrlFn> {
public:
    explicit ComCtrlDone(CtrlFn fn)
        : ComCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                      CtrlFn>(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr,
                                     ICoreWebView2Controller* c) override {
        try {
            fn_(hr, c);
        } catch (...) {
        }
        return S_OK;
    }
};

class ComNavEvt : public ComCallback<ICoreWebView2NavigationCompletedEventHandler,
                                     NavFn> {
public:
    explicit ComNavEvt(NavFn fn)
        : ComCallback<ICoreWebView2NavigationCompletedEventHandler, NavFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) override {
        try {
            fn_(nullptr, a);
        } catch (...) {
        }
        return S_OK;
    }
};

class ComMsgEvt
    : public ComCallback<ICoreWebView2WebMessageReceivedEventHandler, MsgFn> {
public:
    explicit ComMsgEvt(MsgFn fn)
        : ComCallback<ICoreWebView2WebMessageReceivedEventHandler, MsgFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) override {
        try {
            fn_(nullptr, a);
        } catch (...) {
        }
        return S_OK;
    }
};

// ContextMenuRequested：拦截 WebView2 默认右键菜单（Edge 的刷新/后退/另存为/
// 检查等对壳内应用无意义，且菜单里的 Reload 会绕过宿主刷新链路）。put_Handled
// (TRUE) 直接吞掉；页面自身不提供右键功能。
using CtxFn =
    std::function<void(ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs*)>;
class ComCtxEvt
    : public ComCallback<ICoreWebView2ContextMenuRequestedEventHandler, CtxFn> {
public:
    explicit ComCtxEvt(CtxFn fn)
        : ComCallback<ICoreWebView2ContextMenuRequestedEventHandler, CtxFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* a) override {
        try {
            fn_(nullptr, a);
        } catch (...) {
        }
        return S_OK;
    }
};

}  // namespace

// ───────────────────────── 宿主状态（pimpl） ─────────────────────────

struct WebviewHost::Impl : std::enable_shared_from_this<WebviewHost::Impl> {
    enum class State { Idle, Creating, Ready, Failed };

    State state = State::Idle;
    HWND hwnd = nullptr;        // 父窗口（音乐窗 content 所在）
    RECT bounds{};              // 内容区（父窗口客户区坐标）
    std::wstring page_root;     // 虚拟主机映射的本地目录
    MusicFreeBridge* bridge = nullptr;  // 非拥有；Create 传入

    ICoreWebView2Environment* env = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* core = nullptr;
    EventRegistrationToken tok_nav{};
    EventRegistrationToken tok_msg{};
    EventRegistrationToken tok_ctx{};
    bool has_nav = false;
    bool has_msg = false;
    bool has_ctx = false;
    ComNavEvt* nav_evt = nullptr;  // 自持引用，Destroy 时撤事件并释放
    ComMsgEvt* msg_evt = nullptr;
    ComCtxEvt* ctx_evt = nullptr;

    DWORD create_tick = 0;      // 进入 Creating 的 GetTickCount（超时判定）
    unsigned long long serial = 0;  // 创建代数：超时重试后丢弃上一代在途回调
    std::string last_error;     // 最后一次失败的中文原因
    std::atomic<bool> destroying{false};  // 置位后 sender/poster 全部 no-op

    // poster 任务队列：任意线程入队；UI 线程 TimerProc 出队执行
    std::mutex task_mtx;
    std::deque<std::function<void()>> tasks;

    ~Impl();

    // 置 Failed + 记原因 + 落日志（保留首个原因）
    void Fail(std::string reason);
    // 惰性超时检查：Creating 超 10 秒未就绪 → Failed
    void CheckTimeout();
    // 定时器回调：出队执行；队列空时自停定时器
    void RunPostedTasks(UINT_PTR timer_id);
    static void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD);

    // WebView2 异步回调（均在 UI 线程）
    void OnEnvironment(HRESULT hr, ICoreWebView2Environment* e);
    void OnController(HRESULT hr, ICoreWebView2Controller* c);
    void OnNavigationCompleted(bool success);
    void OnWebMessageJson(const std::wstring& text);
};

// ───────────────────────── Impl 方法 ─────────────────────────

WebviewHost::Impl::~Impl() {
    // 正常路径 Destroy 已清干净；这里是兜底（异常退出 / 忘调 Destroy）。
    try {
        if (hwnd) ::KillTimer(hwnd, reinterpret_cast<UINT_PTR>(this));
        if (nav_evt) {
            nav_evt->Release();
            nav_evt = nullptr;
        }
        if (msg_evt) {
            msg_evt->Release();
            msg_evt = nullptr;
        }
        if (core) {
            core->Release();
            core = nullptr;
        }
        if (controller) {
            controller->Close();
            controller->Release();
            controller = nullptr;
        }
        if (env) {
            env->Release();
            env = nullptr;
        }
    } catch (...) {
    }
}

void WebviewHost::Impl::Fail(std::string reason) {
    if (state == State::Failed) return;  // 保留首个原因
    if (reason.empty()) reason = "unknown";
    state = State::Failed;
    last_error = reason;
    AppendHostLog("创建失败：" + last_error);
}

void WebviewHost::Impl::CheckTimeout() {
    // GetTickCount 差值运算天然免疫回绕；>10 秒未到 Ready 判失败。
    if (state == State::Creating && create_tick != 0 &&
        (GetTickCount() - create_tick) > 10000u) {
        Fail("创建超时：10 秒内未就绪");
    }
}

void WebviewHost::Impl::RunPostedTasks(UINT_PTR timer_id) {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lk(task_mtx);
            if (tasks.empty()) {
                // 持锁停表：poster 先入队再 SetTimer，两边都过同一把锁，
                // 不会漏投（若与本循环交错，最多多醒一次、醒后队列空即停）。
                if (hwnd) ::KillTimer(hwnd, timer_id);
                return;
            }
            task = std::move(tasks.front());
            tasks.pop_front();
        }
        if (destroying.load(std::memory_order_acquire)) continue;  // 销毁中丢弃
        try {
            task();
        } catch (...) {
        }
    }
}

void CALLBACK WebviewHost::Impl::TimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    // id 即 Impl 指针（SetTimer 时以 this 命名）：定时器只在 poster 任务
    // 在途/宿主存活期间被 arm，Destroy 与 ~Impl 双重 KillTimer 兜底。
    try {
        reinterpret_cast<Impl*>(id)->RunPostedTasks(id);
    } catch (...) {
    }
}

void WebviewHost::Impl::OnEnvironment(HRESULT hr, ICoreWebView2Environment* e) {
    if (destroying.load(std::memory_order_acquire)) return;
    if (state != State::Creating) return;  // 已判失败/已就绪，丢弃
    if (FAILED(hr) || !e) {
        Fail("创建 WebView2 环境失败" + HrText(hr));
        return;
    }
    // QI 拿属于自己的引用（回调入参按借用处理，不额外 Release 原指针）
    if (FAILED(e->QueryInterface(IID_PPV_ARGS(&env))) || !env) {
        Fail("查询 WebView2 环境接口失败");
        return;
    }
    auto self = shared_from_this();
    const unsigned long long my_serial = serial;
    ComCtrlDone* cb = new ComCtrlDone(
        [self, my_serial](HRESULT hr2, ICoreWebView2Controller* c) {
            if (self->serial != my_serial) return;  // 上一代回调，作废
            self->OnController(hr2, c);
        });
    const HRESULT chr = env->CreateCoreWebView2Controller(hwnd, cb);
    cb->Release();  // 交出创建者那份引用；运行时在回调期间自持
    if (FAILED(chr)) Fail("发起创建 WebView2 控制器失败" + HrText(chr));
}

void WebviewHost::Impl::OnController(HRESULT hr, ICoreWebView2Controller* c) {
    if (destroying.load(std::memory_order_acquire)) return;
    if (state != State::Creating) return;
    if (FAILED(hr) || !c) {
        Fail("创建 WebView2 控制器失败" + HrText(hr));
        return;
    }
    if (FAILED(c->QueryInterface(IID_PPV_ARGS(&controller))) || !controller) {
        Fail("查询 WebView2 控制器接口失败");
        return;
    }
    controller->put_IsVisible(TRUE);
    const HRESULT bhr = controller->put_Bounds(bounds);
    if (FAILED(bhr)) {
        Fail("设置内容区尺寸失败" + HrText(bhr));
        return;
    }
    if (FAILED(controller->get_CoreWebView2(&core)) || !core) {
        Fail("获取 WebView2 核心接口失败");
        return;
    }

    // 虚拟主机映射：https://app.webui → 页面根目录（须在 Navigate 之前）
    ICoreWebView2_3* core3 = nullptr;
    if (FAILED(core->QueryInterface(IID_PPV_ARGS(&core3))) || !core3) {
        Fail("查询 ICoreWebView2_3 失败（WebView2 SDK / Runtime 过旧）");
        return;
    }
    const HRESULT mhr = core3->SetVirtualHostNameToFolderMapping(
        L"app.webui", page_root.c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    core3->Release();
    if (FAILED(mhr)) {
        Fail("映射虚拟主机 app.webui 失败" + HrText(mhr));
        return;
    }

    // 收页面消息：get_WebMessageAsJson 得到的是 JSON 字符串
    //（页面发对象也一样，bridge.cpp 已做双层剥壳容错）。
    auto self = shared_from_this();
    msg_evt = new ComMsgEvt(
        [self](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
            if (!args) return;
            LPWSTR raw = nullptr;
            if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return;
            const std::wstring text(raw);  // 先拷贝再释放，转码异常也安全
            CoTaskMemFree(raw);
            self->OnWebMessageJson(text);
        });
    if (FAILED(core->add_WebMessageReceived(msg_evt, &tok_msg))) {
        msg_evt->Release();
        msg_evt = nullptr;
        Fail("挂接 WebMessageReceived 回调失败");
        return;
    }
    has_msg = true;

    // 导航完成：成功才推 bridge.ready（与页面 app.init 握手）
    nav_evt = new ComNavEvt(
        [self](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) {
            BOOL ok = FALSE;
            if (args) args->get_IsSuccess(&ok);
            self->OnNavigationCompleted(ok == TRUE);
        });
    if (FAILED(core->add_NavigationCompleted(nav_evt, &tok_nav))) {
        nav_evt->Release();
        nav_evt = nullptr;
        Fail("挂接 NavigationCompleted 回调失败");
        return;
    }
    has_nav = true;

    // 拦截 WebView2 默认右键菜单：壳内应用不需要 Edge 菜单（刷新/后退/另存为/
    // 检查），其中菜单里的 Reload 还会绕开宿主的刷新链路、让页面重新握手。
    // SDK/Runtime 过旧（无 ICoreWebView2_11）时静默跳过 —— 右键仍弹系统菜单，无碍。
    ICoreWebView2_11* core11 = nullptr;
    if (SUCCEEDED(core->QueryInterface(IID_PPV_ARGS(&core11))) && core11) {
        ctx_evt = new ComCtxEvt(
            [self](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) {
                if (args) args->put_Handled(TRUE);
            });
        if (SUCCEEDED(core11->add_ContextMenuRequested(ctx_evt, &tok_ctx))) {
            has_ctx = true;
        } else {
            ctx_evt->Release();
            ctx_evt = nullptr;
        }
        core11->Release();
    }

    // 白屏消除：WebView2 控件在页面首帧前默认显示**白色**背景（SDK 没有
    // DefaultBackgroundColor 接口可用）。创建后先隐藏控件，让父窗的深色
    // 「正在加载页面…」底露出来；导航完成（DOM/CSS 就绪，页面本身是深色）
    // 后再显示 —— 用户从头到尾看不到白块。
    if (controller) controller->put_IsVisible(FALSE);

    const HRESULT nhr = core->Navigate(L"https://app.webui/index.html");
    if (FAILED(nhr)) {
        Fail("导航到 https://app.webui/index.html 失败" + HrText(nhr));
        return;
    }
    state = State::Ready;  // 异步余下只有页面加载，宿主已可用
}

void WebviewHost::Reload() {
    if (!impl_) return;
    Impl* st = impl_.get();
    if (st->destroying.load(std::memory_order_acquire)) return;
    if (st->state != Impl::State::Ready || !st->core) return;
    st->core->Reload();  // 完成后 OnNavigationCompleted 重发 bridge.ready
}

void WebviewHost::Impl::OnNavigationCompleted(bool success) {
    if (destroying.load(std::memory_order_acquire) || !bridge) return;
    if (!success) {
        AppendHostLog("页面导航未成功（回退/错误页），等待上层回退");
        return;
    }
    // 页面 DOM/CSS 已就绪（深色）：现在才显示控件，消除创建→首帧之间的白屏。
    if (controller) controller->put_IsVisible(TRUE);
    try {
        bridge->PushEvent("bridge.ready", nlohmann::json::object());
        bridge->FlushPending();
    } catch (...) {
        AppendHostLog("推送 bridge.ready 异常（已忽略）");
    }
}

void WebviewHost::Impl::OnWebMessageJson(const std::wstring& text) {
    if (destroying.load(std::memory_order_acquire) || !bridge) return;
    try {
        // 注意：限定到 ::ttplayer::core —— 本类有同名成员 core（WebView2 指针），
        // 非限定写法会被成员遮蔽而编译失败。
        bridge->OnWebMessage(::ttplayer::core::WideToUtf8(text));
    } catch (...) {
        AppendHostLog("WebMessage 转码/处理异常（已忽略）");
    }
}

// ───────────────────────── WebviewHost 对外接口 ─────────────────────────

WebviewHost::WebviewHost() : impl_(std::make_shared<Impl>()) {}

WebviewHost::~WebviewHost() {
    try {
        Destroy();
    } catch (...) {
    }
    // impl_ 释放；若仍有在途回调经其捕获的 shared_ptr 自持，不会野指针。
}

bool WebviewHost::RuntimeAvailable() {
    // 统一交给运行时选择层：按系统版本决定用系统 Evergreen 还是随包 109，
    // 并区分「已装 / 未装 / XP 不支持」。未装时窗口层会启动后台自动安装。
    try {
        return ttplayer::musicfree::PlanWebView2Runtime().available;
    } catch (...) {
        return false;  // 探测异常就回退原生界面，绝不误判成可用
    }
}

bool WebviewHost::Create(HWND parent, RECT content, const wchar_t* pageRoot,
                         MusicFreeBridge* bridge) {
    try {
        if (!impl_) impl_ = std::make_shared<Impl>();
        const std::shared_ptr<Impl> st = impl_;
        if (st->state == Impl::State::Creating ||
            st->state == Impl::State::Ready) {
            st->last_error = "宿主已创建或正在创建";
            return false;
        }
        if (st->state == Impl::State::Failed) Destroy();  // 清上次半成品再重试
        if (!parent) {
            st->Fail("父窗口句柄为空");
            return false;
        }
        if (!pageRoot || !*pageRoot) {
            st->Fail("页面根目录为空");
            return false;
        }
        std::wstring user_data;
        if (!BuildUserDataFolder(&user_data)) {
            st->Fail("无法准备 WebView2 用户数据目录（%LOCALAPPDATA%）");
            return false;
        }

        st->destroying.store(false, std::memory_order_release);
        st->hwnd = parent;
        st->bounds = content;
        st->page_root = pageRoot;
        st->bridge = bridge;
        st->last_error.clear();
        st->create_tick = GetTickCount();
        ++st->serial;  // 代数+1：上一次（超时失败的）在途回调一律作废
        const unsigned long long my_serial = st->serial;
        st->state = Impl::State::Creating;

        // —— 桥接注入①：sender / poster（导航前装好，页面 app.init 的
        //    回执才发得出去）。sender 保证在 UI 线程执行（经 poster 投递）。——
        if (bridge) {
            bridge->sender = [st](const std::string& s) {
                if (st->destroying.load(std::memory_order_acquire)) return;
                if (!st->core) return;  // 尚未就绪：页面还没加载，丢弃无害
                try {
                    st->core->PostWebMessageAsJson(
                        ::ttplayer::core::Utf8ToWide(s).c_str());
                } catch (...) {
                    AppendHostLog("sender：JSON 转宽字符异常（已丢弃）");
                }
            };
            bridge->poster = [st](std::function<void()> task) {
                try {
                    {
                        std::lock_guard<std::mutex> lk(st->task_mtx);
                        if (st->destroying.load(std::memory_order_acquire))
                            return;
                        st->tasks.push_back(std::move(task));
                    }
                    // 以 Impl 指针作定时器 id：TimerProc 据此找回队列。
                    // 带 TIMERPROC 的 WM_TIMER 由 DispatchMessage 直接分发到
                    // TimerProc，不进父窗口过程 —— 父窗口零改动。
                    if (st->hwnd &&
                        !::SetTimer(st->hwnd,
                                    reinterpret_cast<UINT_PTR>(st.get()), 0,
                                    &Impl::TimerProc)) {
                        AppendHostLog("poster：SetTimer 投递失败（任务挂起）");
                    }
                } catch (...) {
                }
            };
        }

        // 环境创建：异步，立即返回；回调在 UI 线程继续。
        ComEnvDone* cb =
            new ComEnvDone([st, my_serial](HRESULT hr,
                                           ICoreWebView2Environment* e) {
                if (st->serial != my_serial) return;  // 上一代回调，作废
                st->OnEnvironment(hr, e);
            });
        // 按系统版本选 Runtime：Win10+ 用系统 Evergreen；Win7/8/8.1 用随包
        // Fixed Version 109 或已装的 109 Evergreen。TargetCompatibleBrowserVersion
        // 必须同步降下来，否则 109 会被 loader 判成「版本不兼容」直接失败。
        const auto plan = ttplayer::musicfree::PlanWebView2Runtime();
        OutputDebugStringW((L"[TTPlayer WebView2] " + plan.detail + L"\n").c_str());
        PCWSTR folder = plan.browser_folder.empty()
                            ? nullptr
                            : plan.browser_folder.c_str();
        const std::wstring compat = plan.compatible_version.empty()
                                        ? std::wstring(L"128.0.2739.15")
                                        : plan.compatible_version;
        auto* options = new ttplayer::musicfree::WebView2EnvOptions(
            compat,
            // 影视源的封面大量是 http:// 图片，而页面跑在 https://app.webui
            // 虚拟主机上，Chromium 默认按混合内容直接拦截 → 表现为「有的封面
            // 空白」。放开混合内容，配合前端 http→https 兜底（app.js
            // ttCoverFail）即可覆盖绝大多数图床。
            L"--allow-running-insecure-content");
#if defined(TTPLAYER_LEGACY_WINDOWS)
        // XP / Win7 版不链接 WebView2Loader.dll.lib（启动导入表里不能有它），
        // 改为运行时解析；旧系统没有 WebView2 Runtime 时直接失败回退原生控件。
        const HRESULT hr =
            ttplayer::musicfree::CreateWebView2EnvironmentDynamic(
                folder, user_data.c_str(), options, cb);
#else
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            folder, user_data.c_str(), options, cb);
#endif
        cb->Release();       // 交出创建者那份引用；运行时在回调期间自持
        options->Release();  // 环境创建会同步读走参数，随后即可释放
        if (FAILED(hr)) {
            st->Fail("发起创建 WebView2 环境失败" + HrText(hr));
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        try {
            if (impl_) impl_->Fail(std::string("Create 异常：") + e.what());
        } catch (...) {
        }
        return false;
    } catch (...) {
        try {
            if (impl_) impl_->Fail("Create 发生未知异常");
        } catch (...) {
        }
        return false;
    }
}

void WebviewHost::Resize(RECT content) {
    try {
        if (!impl_) return;
        impl_->bounds = content;
        impl_->CheckTimeout();  // 惰性超时检查：外界布局时顺带判
        if (impl_->controller) impl_->controller->put_Bounds(content);
    } catch (...) {
    }
}

void WebviewHost::Destroy() {
    try {
        if (!impl_) return;
        const std::shared_ptr<Impl> st = impl_;
        // 1) 先立 destroying 标志：在途 sender/poster 立即 no-op
        st->destroying.store(true, std::memory_order_release);
        // 2) 清队 + 拆表：不再有新任务被取走执行
        {
            std::lock_guard<std::mutex> lk(st->task_mtx);
            st->tasks.clear();
        }
        if (st->hwnd) ::KillTimer(st->hwnd, reinterpret_cast<UINT_PTR>(st.get()));
        // 3) 撤事件 → 关控制器 → 释放 COM 对象
        if (st->core) {
            if (st->has_nav) {
                st->core->remove_NavigationCompleted(st->tok_nav);
                st->has_nav = false;
            }
            if (st->has_msg) {
                st->core->remove_WebMessageReceived(st->tok_msg);
                st->has_msg = false;
            }
            if (st->has_ctx) {
                ICoreWebView2_11* core11 = nullptr;
                if (SUCCEEDED(st->core->QueryInterface(IID_PPV_ARGS(&core11))) &&
                    core11) {
                    core11->remove_ContextMenuRequested(st->tok_ctx);
                    core11->Release();
                }
                st->has_ctx = false;
            }
        }
        if (st->nav_evt) {
            st->nav_evt->Release();  // 事件已撤 + 自持引用归零 → 释放
            st->nav_evt = nullptr;
        }
        if (st->ctx_evt) {
            st->ctx_evt->Release();
            st->ctx_evt = nullptr;
        }
        if (st->msg_evt) {
            st->msg_evt->Release();
            st->msg_evt = nullptr;
        }
        if (st->core) {
            st->core->Release();
            st->core = nullptr;
        }
        if (st->controller) {
            st->controller->Close();
            st->controller->Release();
            st->controller = nullptr;
        }
        if (st->env) {
            st->env->Release();
            st->env = nullptr;
        }
        st->bridge = nullptr;
        st->create_tick = 0;
        st->state = Impl::State::Idle;  // 允许下次重新 Create
    } catch (...) {
    }
}

bool WebviewHost::Ready() const {
    try {
        if (!impl_) return false;
        impl_->CheckTimeout();  // 超时在查询时惰性转 Failed
        return impl_->state == Impl::State::Ready;
    } catch (...) {
        return false;
    }
}

bool WebviewHost::Failed() const {
    try {
        if (!impl_) return false;
        impl_->CheckTimeout();
        return impl_->state == Impl::State::Failed;
    } catch (...) {
        return true;  // 查询都异常了：按失败处理，回退旧控件
    }
}

const char* WebviewHost::LastError() const {
    try {
        if (!impl_) return "";
        return impl_->last_error.c_str();
    } catch (...) {
        return "";
    }
}

}  // namespace ttplayer::musicfree
