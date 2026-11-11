// 视频播放窗的「网页兜底」WebView2 宿主（精简实现）。
//
// 用途：线路是官方网页（v.qq.com / v.youku.com / bilibili.com 等）且所有解析
// 接口都没能解出直链时，由视频播放窗把该网页直接装进画面区播放，而不是只显示
// 「播放失败」。
//
// 与音乐窗宿主（musicfree_webview_host.cpp）的区别：
//   · 不依赖 MusicFreeBridge、不依赖 pageRoot、不做虚拟主机映射；
//   · 只做一件事：环境 → 控制器 → put_Bounds → Navigate(url)；
//   · 所有回调只做最小工作，异常一律吞掉，失败只落 <exe>\video\bridge.log。
//
// 线程模型：本文件所有 COM 回调都在父窗口所在线程（UI 线程）执行；
// Destroy 与回调同线程，故不需要额外加锁，只用 destroying 原子标志挡在途回调。

#include "video_web_host.h"

#include <windows.h>
// objbase.h 必须先于 WebView2.h：interface 宏由 combaseapi.h 定义（经 objbase
// 带入），缺了会让 WebView2.h 的 `typedef interface IXxx` 全线报 C2146。
#include <objbase.h>
#include <WebView2.h>

#include "ttplayer/musicfree/webview2_dynamic.h"
#include "ttplayer/musicfree/webview2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ttplayer::musicfree {
namespace {

// ───────────────────────── 日志 ─────────────────────────
// 与 musicfree_webview_host.cpp 的 AppendHostLog 同格式、同路径。
void AppendVideoHostLog(const std::string& text) {
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

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string HrText(HRESULT hr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), " (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
    return std::string(buf);
}

// %LOCALAPPDATA%\TTPlayer\webview2_video（父目录已存在则忽略）。
bool BuildUserDataFolder(std::wstring* out) {
    wchar_t base[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 1024);
    if (n == 0 || n >= 1024) return false;
    std::wstring dir(base);
    dir += L"\\TTPlayer";
    CreateDirectoryW(dir.c_str(), nullptr);  // 已存在返回 0，忽略
    dir += L"\\webview2_video";
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) return false;
    }
    *out = std::move(dir);
    return true;
}

// 最小 ICoreWebView2EnvironmentOptions：只为传 AdditionalBrowserArguments
//（--autoplay-policy=no-user-gesture-required，允许网页自动播放）。
// 本机 Windows SDK 没有 WRL 头，故手写 COM 壳，不用 CoreWebView2EnvironmentOptions。
class EnvironmentOptions : public ICoreWebView2EnvironmentOptions {
public:
    // compatible_version 必须不高于实际 Runtime 版本：Win7 上只有 109，
    // 若仍写 SDK 的 128，loader 会以「版本不兼容」拒绝创建环境。
    EnvironmentOptions(std::wstring args, std::wstring compatible_version)
        : args_(std::move(args)),
          compatible_version_(std::move(compatible_version)) {}
    EnvironmentOptions(const EnvironmentOptions&) = delete;
    EnvironmentOptions& operator=(const EnvironmentOptions&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override {
        if (!obj) return E_POINTER;
        if (riid == IID_IUnknown ||
            riid == __uuidof(ICoreWebView2EnvironmentOptions)) {
            *obj = static_cast<ICoreWebView2EnvironmentOptions*>(this);
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

    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(
        LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = CopyString(args_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(
        LPCWSTR value) override {
        args_ = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = CopyString(language_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
        language_ = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(
        LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = CopyString(compatible_version_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(
        LPCWSTR value) override {
        compatible_version_ = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(
        BOOL* allow) override {
        if (!allow) return E_POINTER;
        *allow = sso_ ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(
        BOOL allow) override {
        sso_ = allow == TRUE;
        return S_OK;
    }

private:
    static LPWSTR CopyString(const std::wstring& s) {
        const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
        auto* p = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!p) return nullptr;
        std::memcpy(p, s.c_str(), bytes);
        return p;
    }

    std::atomic<ULONG> ref_{1};
    std::wstring args_;
    std::wstring language_;
    std::wstring compatible_version_;
    bool sso_ = false;
};

// ───────────────── 单 Invoke 的 COM 回调壳 ─────────────────
// WebView2 的完成/事件回调都只有 Invoke；手写最小 COM 壳（本机 SDK 无 WRL），
// 引用计数原子、归零自删。

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
using NavFn = std::function<void(ICoreWebView2NavigationCompletedEventArgs*)>;

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

class ComNavEvt
    : public ComCallback<ICoreWebView2NavigationCompletedEventHandler, NavFn> {
public:
    explicit ComNavEvt(NavFn fn)
        : ComCallback<ICoreWebView2NavigationCompletedEventHandler, NavFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) override {
        try {
            fn_(a);
        } catch (...) {
        }
        return S_OK;
    }
};

// ContextMenuRequested：拦截 WebView2 默认右键菜单（Edge 的刷新/后退/检查等
// 对播放窗内的网页兜底同样无意义）。put_Handled(TRUE) 吞掉；SDK/Runtime 过旧
// （无 ICoreWebView2_11）时静默跳过。
using CtxFn = std::function<void(ICoreWebView2ContextMenuRequestedEventArgs*)>;
class ComCtxEvt
    : public ComCallback<ICoreWebView2ContextMenuRequestedEventHandler, CtxFn> {
public:
    explicit ComCtxEvt(CtxFn fn)
        : ComCallback<ICoreWebView2ContextMenuRequestedEventHandler, CtxFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* a) override {
        try {
            fn_(a);
        } catch (...) {
        }
        return S_OK;
    }
};

// 页面请求全屏 / 退出全屏：WebView2 的 ContainsFullScreenElementChanged。
// 宿主播放窗收到后切换真全屏（无边框覆盖显示器），否则网页播放器的全屏按钮
// 只会把内容放进原窗口，用户看到的就是「不是真全屏」。
using FsFn = std::function<void(bool)>;
class ComContainsFs
    : public ComCallback<
          ICoreWebView2ContainsFullScreenElementChangedEventHandler, FsFn> {
public:
    explicit ComContainsFs(FsFn fn)
        : ComCallback<
              ICoreWebView2ContainsFullScreenElementChangedEventHandler, FsFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     IUnknown*) override {
        BOOL on = FALSE;
        try {
            if (sender) sender->get_ContainsFullScreenElement(&on);
            fn_(on == TRUE);
        } catch (...) {
        }
        return S_OK;
    }
};

// 自定义控制条 → 网页播放器的控制脚本 / 状态回推。
// ★ 需求变更（2026-11-10）：过去 web 兜底「只保留站点自带控制条，收掉原生
//   控制栏」（见 .trae/specs/diagnose-video-player-and-music-play/spec.md），
//   用户明确否决：不要 web 视频的进度条和播放框，一律用 Win32 自绘的。
//   WebView2 现在只当渲染面：注入脚本隐藏页面所有播放器控件、把 <video>
//   的状态回推宿主，宿主用 ExecuteScript 反向驱动 currentTime/play/pause。
using ScriptFn = std::function<void(HRESULT, LPCWSTR)>;
class ComScriptDone
    : public ComCallback<ICoreWebView2ExecuteScriptCompletedHandler, ScriptFn> {
public:
    explicit ComScriptDone(ScriptFn fn)
        : ComCallback<ICoreWebView2ExecuteScriptCompletedHandler, ScriptFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, LPCWSTR result) override {
        try {
            fn_(hr, result);
        } catch (...) {
        }
        return S_OK;
    }
};

class ComAddScriptDone
    : public ComCallback<
          ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
          ScriptFn> {
public:
    explicit ComAddScriptDone(ScriptFn fn)
        : ComCallback<
              ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
              ScriptFn>(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, LPCWSTR id) override {
        try {
            fn_(hr, id);
        } catch (...) {
        }
        return S_OK;
    }
};

using MsgFn =
    std::function<void(ICoreWebView2WebMessageReceivedEventArgs*)>;
class ComWebMsg
    : public ComCallback<ICoreWebView2WebMessageReceivedEventHandler, MsgFn> {
public:
    explicit ComWebMsg(MsgFn fn)
        : ComCallback<ICoreWebView2WebMessageReceivedEventHandler, MsgFn>(
              std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) override {
        try {
            fn_(a);
        } catch (...) {
        }
        return S_OK;
    }
};

// 每个文档创建时注入。站点播放器（ArtPlayer / DPlayer / xgplayer / Plyr /
// bilibili / 腾讯 / 优酷…）的进度条、播放框、全屏按钮一律隐藏；只留 <video>
// 画面。宿主的自绘控制条通过 window.__ttpVideo() 拿元素并操控。
const wchar_t* kPlayerControlScript = LR"TTPJS(
(function () {
  'use strict';
  var CSS = [
    // 网页形态也要保持比例：站点常把 <video> 设成容器宽高，播放窗全屏后
    // 容器比例与片源不一致就被拉伸。contain 让浏览器自己留黑边。
    'video{object-fit:contain !important;background:#000 !important;}',
    'video::-webkit-media-controls,video::-webkit-media-controls-enclosure,',
    'video::-webkit-media-controls-panel,video::-webkit-media-controls-play-button,',
    'video::-webkit-media-controls-timeline,video::-webkit-media-controls-fullscreen-button,',
    '.art-controls,.art-bottom,.art-control,.art-progress,.art-fullscreen,',
    '.art-layer[data-name="controls"],.art-layer[data-name="mask"],',
    '.art-state,.art-mask,.art-notice,.art-settings,.art-subtitle,',
    '.dplayer-controller,.dplayer-controller-mask,.dplayer-behind,.dplayer-mask,',
    '.plyr__controls,.plyr__control,.plyr__progress__container,',
    '.xgplayer-controls,.xgplayer-progress,.xgplayer-fullscreen,.xgplayer-cssfullscreen,',
    '.jw-controls,.vjs-control-bar,.vjs-big-play-button,.vjs-loading-spinner,',
    '.bpx-player-control-wrap,.bpx-player-control-bottom,.bpx-player-control-top,',
    '.tcplayer-control,.mplayer-control,.prism-controlbar,.shaka-controls,',
    '.qtp-controlbar,.youku-player-controls,.player-controls,.control-bar',
    '{display:none !important;visibility:hidden !important;pointer-events:none !important;}'
  ].join('');
  function findVideo(doc, depth) {
    if (!doc || depth > 3) return null;
    try {
      var v = doc.querySelector('video');
      if (v) return v;
    } catch (e) {}
    try {
      var frames = doc.querySelectorAll('iframe');
      for (var i = 0; i < frames.length; ++i) {
        try {
          var r = findVideo(frames[i].contentDocument, depth + 1);
          if (r) return r;
        } catch (e) {}
      }
    } catch (e) {}
    return null;
  }
  function hideChrome(doc) {
    if (!doc) return;
    try {
      if (!doc.getElementById('__ttp_hide_controls__')) {
        var s = doc.createElement('style');
        s.id = '__ttp_hide_controls__';
        s.textContent = CSS;
        (doc.head || doc.documentElement).appendChild(s);
      }
    } catch (e) {}
    try {
      var v = findVideo(doc, 0);
      if (v) v.controls = false;
    } catch (e) {}
  }
  function report() {
    try {
      if (!window.chrome || !window.chrome.webview) return;
      var v = findVideo(document, 0);
      if (!v) { window.chrome.webview.postMessage('TTPVS:0:0:0:0'); return; }
      var d = (isFinite(v.duration) && v.duration > 0) ? v.duration : 0;
      var p = (isFinite(v.currentTime) && v.currentTime > 0) ? v.currentTime : 0;
      window.chrome.webview.postMessage('TTPVS:' + p.toFixed(2) + ':' +
          d.toFixed(2) + ':' + (v.paused ? '0' : '1') + ':1');
    } catch (e) {}
  }
  window.__ttpVideo = function () { return findVideo(document, 0); };
  // 宿主 → 页面：用 WebMessage 广播而不是 ExecuteScript，这样脚本也注入了
  // 的跨域 iframe 能收到命令，视频放在子框架里时自绘进度条照样能拖。
  function apply(cmd) {
    if (!cmd || cmd.tt !== 'TTPCMD') return;
    var v = findVideo(document, 0);
    if (v) {
      try {
        if (cmd.a === 'seek') {
          var t = Number(cmd.v);
          if (!(t >= 0) && isFinite(Number(cmd.r))) {
            var d = (isFinite(v.duration) && v.duration > 0) ? v.duration :
                (v.seekable && v.seekable.length ?
                    v.seekable.end(v.seekable.length - 1) : 0);
            if (d > 0) t = d * Number(cmd.r);
          }
          if (t >= 0) v.currentTime = t;
        }
        else if (cmd.a === 'play') {
          var p = v.play(); if (p && p.catch) p.catch(function () {});
        }
        else if (cmd.a === 'pause') v.pause();
        else if (cmd.a === 'volume') v.volume = Number(cmd.v);
        else if (cmd.a === 'muted') v.muted = !!cmd.v;
      } catch (e) {}
    }
    // 只向下转发，避免父子框架来回弹消息。
    try {
      var frames = document.querySelectorAll('iframe');
      for (var i = 0; i < frames.length; ++i) {
        try { frames[i].contentWindow.postMessage(cmd, '*'); } catch (e) {}
      }
    } catch (e) {}
  }
  window.__ttpApply = apply;
  if (window.chrome && window.chrome.webview &&
      window.chrome.webview.addEventListener) {
    window.chrome.webview.addEventListener('message', function (e) {
      apply(e.data);
    });
  }
  window.addEventListener('message', function (e) { apply(e.data); }, false);
  function tick() { hideChrome(document); report(); }
  function boot() { tick(); setInterval(tick, 250); }
  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', boot, {once: true});
  else boot();
})();
)TTPJS";

// 状态：0=Creating，1=Ready，2=Failed
constexpr int kCreating = 0;
constexpr int kReady = 1;
constexpr int kFailed = 2;

struct Impl : std::enable_shared_from_this<Impl> {
    std::atomic<int> state{kCreating};
    std::atomic<bool> destroying{false};
    HWND hwnd = nullptr;
    RECT bounds{};
    std::wstring url;
    ICoreWebView2Environment* env = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* core = nullptr;
    ComNavEvt* nav_evt = nullptr;
    EventRegistrationToken tok_nav{};
    bool has_nav = false;
    ComContainsFs* fs_evt = nullptr;
    EventRegistrationToken tok_fs{};
    bool has_fs = false;
    ComWebMsg* msg_evt = nullptr;
    EventRegistrationToken tok_msg{};
    bool has_msg = false;
    ComCtxEvt* ctx_evt = nullptr;
    EventRegistrationToken tok_ctx{};
    bool has_ctx = false;
    std::function<void(bool)> fullscreen_cb;
    std::function<void(const ttplayer::video::VideoWebState&)> state_cb;
    // AddScriptToExecuteOnDocumentCreated 也会注入同源/跨域子 iframe。没有
    // <video> 的框架同样每 250ms 回推 has_video=0，若不屏蔽会把主框架报的
    // 真实进度/时长刷成 0（表现为进度条一直在跳动、拖不动）。
    DWORD last_video_tick = 0;
    std::string last_error;

    ~Impl() { Close(); }

    void Fail(std::string reason);
    void Close();
    void OnEnvironment(HRESULT hr, ICoreWebView2Environment* e);
    void OnController(HRESULT hr, ICoreWebView2Controller* c);
    void OnNavigationDone(bool success);
    void OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args);
    void PostCommand(const std::wstring& json);
    void RunScript(const std::wstring& js);
};

void Impl::Fail(std::string reason) {
    if (state.load(std::memory_order_acquire) == kFailed) return;  // 保留首个原因
    if (reason.empty()) reason = "unknown";
    state.store(kFailed, std::memory_order_release);
    last_error = std::move(reason);
    AppendVideoHostLog("视频网页兜底失败：" + last_error);
}

void Impl::Close() {
    try {
        destroying.store(true, std::memory_order_release);
        if (core && has_nav) {
            core->remove_NavigationCompleted(tok_nav);
            has_nav = false;
        }
        if (core && has_ctx) {
            ICoreWebView2_11* core11 = nullptr;
            if (SUCCEEDED(core->QueryInterface(IID_PPV_ARGS(&core11))) && core11) {
                core11->remove_ContextMenuRequested(tok_ctx);
                core11->Release();
            }
            has_ctx = false;
        }
        if (nav_evt) {
            nav_evt->Release();
            nav_evt = nullptr;
        }
        if (ctx_evt) {
            ctx_evt->Release();
            ctx_evt = nullptr;
        }
        if (core && has_fs) {
            core->remove_ContainsFullScreenElementChanged(tok_fs);
            has_fs = false;
        }
        if (fs_evt) {
            fs_evt->Release();
            fs_evt = nullptr;
        }
        if (core && has_msg) {
            core->remove_WebMessageReceived(tok_msg);
            has_msg = false;
        }
        if (msg_evt) {
            msg_evt->Release();
            msg_evt = nullptr;
        }
        state_cb = nullptr;
        fullscreen_cb = nullptr;
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

void Impl::OnEnvironment(HRESULT hr, ICoreWebView2Environment* e) {
    if (destroying.load(std::memory_order_acquire)) return;
    if (state.load(std::memory_order_acquire) != kCreating) return;
    if (FAILED(hr) || !e) {
        Fail("创建 WebView2 环境失败" + HrText(hr));
        return;
    }
    if (FAILED(e->QueryInterface(IID_PPV_ARGS(&env))) || !env) {
        Fail("查询 WebView2 环境接口失败");
        return;
    }
    auto self = shared_from_this();
    ComCtrlDone* cb = new ComCtrlDone(
        [self](HRESULT hr2, ICoreWebView2Controller* c) {
            self->OnController(hr2, c);
        });
    const HRESULT chr = env->CreateCoreWebView2Controller(hwnd, cb);
    cb->Release();  // 交出创建者那份引用；运行时在回调期间自持
    if (FAILED(chr)) Fail("发起创建 WebView2 控制器失败" + HrText(chr));
}

void Impl::OnController(HRESULT hr, ICoreWebView2Controller* c) {
    if (destroying.load(std::memory_order_acquire)) return;
    if (state.load(std::memory_order_acquire) != kCreating) return;
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
        Fail("设置网页内容区尺寸失败" + HrText(bhr));
        return;
    }
    if (FAILED(controller->get_CoreWebView2(&core)) || !core) {
        Fail("获取 WebView2 核心接口失败");
        return;
    }

    // 自定义控制条需要页面回推状态：显式打开 WebMessage（默认开，避免运行时
    // 策略变更导致进度/时长永远为 0）。
    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(core->get_Settings(&settings)) && settings) {
        settings->put_IsWebMessageEnabled(TRUE);
        settings->Release();
    }

    auto self = shared_from_this();

    // 每个文档创建时隐藏站点自带进度条/播放框，并定时把 <video> 状态回推。
    ComAddScriptDone* inject_cb = new ComAddScriptDone(
        [](HRESULT hr, LPCWSTR) {
            if (FAILED(hr))
                AppendVideoHostLog("注入自定义控制条脚本失败" + HrText(hr));
        });
    if (FAILED(core->AddScriptToExecuteOnDocumentCreated(
            kPlayerControlScript, inject_cb))) {
        AppendVideoHostLog("注册自定义控制条脚本失败（站点控件可能仍可见）");
    }
    inject_cb->Release();

    msg_evt = new ComWebMsg(
        [self](ICoreWebView2WebMessageReceivedEventArgs* a) {
            self->OnWebMessage(a);
        });
    if (FAILED(core->add_WebMessageReceived(msg_evt, &tok_msg))) {
        msg_evt->Release();
        msg_evt = nullptr;
        AppendVideoHostLog("挂接网页状态回推失败（自定义进度条无时长）");
    } else {
        has_msg = true;
    }

    nav_evt = new ComNavEvt(
        [self](ICoreWebView2NavigationCompletedEventArgs* a) {
            BOOL ok = FALSE;
            if (a) a->get_IsSuccess(&ok);
            self->OnNavigationDone(ok == TRUE);
        });
    if (FAILED(core->add_NavigationCompleted(nav_evt, &tok_nav))) {
        nav_evt->Release();
        nav_evt = nullptr;
        Fail("挂接 NavigationCompleted 回调失败");
        return;
    }
    has_nav = true;

    // 拦截默认右键菜单（与音乐窗宿主同策略；SDK 过旧时静默跳过）。
    ICoreWebView2_11* core11 = nullptr;
    if (SUCCEEDED(core->QueryInterface(IID_PPV_ARGS(&core11))) && core11) {
        ctx_evt = new ComCtxEvt(
            [self](ICoreWebView2ContextMenuRequestedEventArgs* args) {
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

    // 页面全屏联动（失败不致命：只是网页播放器的全屏按钮不生效）。
    fs_evt = new ComContainsFs([self](bool on) {
        if (self->destroying.load(std::memory_order_acquire)) return;
        if (self->fullscreen_cb) {
            try {
                self->fullscreen_cb(on);
            } catch (...) {
            }
        }
    });
    if (FAILED(core->add_ContainsFullScreenElementChanged(fs_evt, &tok_fs))) {
        fs_evt->Release();
        fs_evt = nullptr;
        AppendVideoHostLog("挂接网页全屏事件失败（全屏按钮可能不生效）");
    } else {
        has_fs = true;
    }

    const HRESULT nhr = core->Navigate(url.c_str());
    if (FAILED(nhr)) {
        Fail("导航到网页地址失败" + HrText(nhr));
        return;
    }
    AppendVideoHostLog("开始加载网页线路：" + WideToUtf8(url));
}

void Impl::OnNavigationDone(bool success) {
    if (destroying.load(std::memory_order_acquire)) return;
    if (state.load(std::memory_order_acquire) != kCreating) return;
    if (!success) {
        Fail("网页导航未成功（网络或站点拒绝加载）");
        return;
    }
    state.store(kReady, std::memory_order_release);
    AppendVideoHostLog("网页线路已就绪");
}

void Impl::OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (!args || destroying.load(std::memory_order_acquire)) return;
    LPWSTR raw = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return;
    const std::wstring text(raw);
    CoTaskMemFree(raw);
    const std::wstring prefix = L"TTPVS:";
    const std::size_t at = text.find(prefix);
    if (at == std::wstring::npos) return;
    std::vector<std::wstring> parts;
    std::size_t start = at + prefix.size();
    for (int k = 0; k < 4 && start <= text.size(); ++k) {
        const std::size_t end = text.find(L':', start);
        const std::size_t stop = end == std::wstring::npos ? text.size() : end;
        parts.push_back(text.substr(start, stop - start));
        start = stop + 1;
        if (end == std::wstring::npos) break;
    }
    if (parts.size() < 4) return;
    try {
        ttplayer::video::VideoWebState s;
        s.position = std::stod(parts[0]);
        s.duration = std::stod(parts[1]);
        s.playing = std::stoi(parts[2]) != 0;
        s.has_video = std::stoi(parts[3]) != 0;
        if (!std::isfinite(s.position) || s.position < 0.0) s.position = 0.0;
        if (!std::isfinite(s.duration) || s.duration < 0.0) s.duration = 0.0;
        const DWORD now = GetTickCount();
        if (s.has_video) {
            last_video_tick = now;
        } else if (last_video_tick != 0 &&
                   now - last_video_tick < 3000) {
            return;  // 忽略子 iframe 的空状态，保留主框架的真实进度
        }
        if (state_cb) state_cb(s);
    } catch (...) {
        // 页面回推格式异常：忽略，不影响播放。
    }
}

// PostWebMessageAsJson 会把命令送到顶层文档，注入脚本再向所有 iframe 转发，
// 因此跨域子框架里的 <video> 也能被自绘控制条操控。老 runtime 不支持时
// 退回 ExecuteScript（只覆盖顶层 + 同源 iframe）。
void Impl::PostCommand(const std::wstring& json) {
    if (!core || destroying.load(std::memory_order_acquire)) return;
    if (SUCCEEDED(core->PostWebMessageAsJson(json.c_str()))) return;
    RunScript(L"window.__ttpApply&&window.__ttpApply(" + json + L");");
}

void Impl::RunScript(const std::wstring& js) {
    if (!core || destroying.load(std::memory_order_acquire)) return;
    ComScriptDone* cb = new ComScriptDone([](HRESULT hr, LPCWSTR) {
        if (FAILED(hr)) AppendVideoHostLog("执行控制脚本失败" + HrText(hr));
    });
    const HRESULT hr = core->ExecuteScript(js.c_str(), cb);
    cb->Release();
    if (FAILED(hr)) AppendVideoHostLog("发起控制脚本失败" + HrText(hr));
}

// ───────────────────────── 对外宿主对象 ─────────────────────────

class VideoHost final : public ttplayer::video::VideoWebHost {
public:
    explicit VideoHost(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    ~VideoHost() override {
        try {
            if (impl_) impl_->Close();
        } catch (...) {
        }
    }
    VideoHost(const VideoHost&) = delete;
    VideoHost& operator=(const VideoHost&) = delete;

    void Resize(RECT content) override {
        try {
            if (!impl_) return;
            impl_->bounds = content;
            if (impl_->controller &&
                !impl_->destroying.load(std::memory_order_acquire))
                impl_->controller->put_Bounds(content);
        } catch (...) {
        }
    }

    bool Ready() const override {
        return impl_ && impl_->state.load(std::memory_order_acquire) == kReady;
    }
    bool Failed() const override {
        return impl_ && impl_->state.load(std::memory_order_acquire) == kFailed;
    }
    const char* LastError() const override {
        if (!impl_) return "";
        return impl_->last_error.c_str();
    }
    void SetFullscreenCallback(std::function<void(bool)> fn) override {
        if (impl_) impl_->fullscreen_cb = std::move(fn);
    }

    void Seek(double seconds) override {
        if (!impl_ || !std::isfinite(seconds) || seconds < 0.0) return;
        wchar_t buf[160]{};
        swprintf_s(buf, L"{\"tt\":\"TTPCMD\",\"a\":\"seek\",\"v\":%.3f}",
                   seconds);
        impl_->PostCommand(buf);
    }

    void SeekRatio(double ratio) override {
        if (!impl_) return;
        const double r = std::max(0.0, std::min(1.0, ratio));
        wchar_t buf[160]{};
        swprintf_s(buf,
                   L"{\"tt\":\"TTPCMD\",\"a\":\"seek\",\"v\":-1,\"r\":%.6f}",
                   r);
        impl_->PostCommand(buf);
    }

    void SetPaused(bool paused) override {
        if (!impl_) return;
        impl_->PostCommand(paused
            ? L"{\"tt\":\"TTPCMD\",\"a\":\"pause\"}"
            : L"{\"tt\":\"TTPCMD\",\"a\":\"play\"}");
    }

    void SetVolume(double volume) override {
        if (!impl_) return;
        const double v = std::max(0.0, std::min(1.0, volume));
        wchar_t buf[160]{};
        swprintf_s(buf, L"{\"tt\":\"TTPCMD\",\"a\":\"volume\",\"v\":%.3f}", v);
        impl_->PostCommand(buf);
    }

    void SetMuted(bool muted) override {
        if (!impl_) return;
        impl_->PostCommand(muted
            ? L"{\"tt\":\"TTPCMD\",\"a\":\"muted\",\"v\":true}"
            : L"{\"tt\":\"TTPCMD\",\"a\":\"muted\",\"v\":false}");
    }

    void SetStateCallback(
        std::function<void(const ttplayer::video::VideoWebState&)> fn) override {
        if (impl_) impl_->state_cb = std::move(fn);
    }

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace

std::unique_ptr<ttplayer::video::VideoWebHost> CreateVideoWebHost(
    HWND parent, RECT content, const std::wstring& url) {
    try {
        if (!parent || url.empty()) return nullptr;
        if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0)
            return nullptr;

        std::wstring user_data;
        if (!BuildUserDataFolder(&user_data)) {
            AppendVideoHostLog("无法准备 WebView2 用户数据目录（%LOCALAPPDATA%）");
            return nullptr;
        }

        auto impl = std::make_shared<Impl>();
        impl->hwnd = parent;
        impl->bounds = content;
        impl->url = url;
        auto host = std::make_unique<VideoHost>(impl);

        // 按系统版本选 Runtime（Win10+ 系统 Evergreen / Win7 随包或已装 109），
        // 并把 TargetCompatibleBrowserVersion 同步降到 Runtime 能接受的值。
        const auto plan = ttplayer::musicfree::PlanWebView2Runtime();
        PCWSTR folder = plan.browser_folder.empty()
                            ? nullptr
                            : plan.browser_folder.c_str();
        const std::wstring compat = plan.compatible_version.empty()
                                        ? std::wstring(L"128.0.2739.15")
                                        : plan.compatible_version;
        auto* options = new EnvironmentOptions(
            L"--autoplay-policy=no-user-gesture-required", compat);
        ComEnvDone* cb = new ComEnvDone(
            [impl](HRESULT hr, ICoreWebView2Environment* e) {
                impl->OnEnvironment(hr, e);
            });
#if defined(TTPLAYER_LEGACY_WINDOWS)
        const HRESULT hr =
            ttplayer::musicfree::CreateWebView2EnvironmentDynamic(
                folder, user_data.c_str(), options, cb);
#else
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            folder, user_data.c_str(), options, cb);
#endif
        cb->Release();       // 运行时在回调期间自持
        options->Release();  // 环境创建会同步读走参数，随后即可释放
        if (FAILED(hr)) {
            impl->Fail("发起创建 WebView2 环境失败" + HrText(hr));
            return nullptr;  // 同步失败直接交给调用方回退「播放失败」浮层
        }
        return host;
    } catch (const std::exception& e) {
        AppendVideoHostLog(std::string("创建网页兜底异常：") + e.what());
        return nullptr;
    } catch (...) {
        AppendVideoHostLog("创建网页兜底发生未知异常");
        return nullptr;
    }
}

}  // namespace ttplayer::musicfree
