#pragma once

// WebView2 运行库按系统版本选择 + 缺失时自动安装。
//
// 事实依据：
//   · WebView2 Runtime 109.0.1518.78 是最后一个支持 Windows 7/8/8.1 的版本；
//     微软在 109 之后把内核基线抬到 Win8+ API（例如 KERNEL32!GetSystemTimePreciseAsFileTime），
//     高版本 Runtime 在 Win7 上装不上 / 起不来。
//   · 官方 Evergreen Bootstrapper（go.microsoft.com/fwlink/p/?LinkId=2124703）
//     会根据当前系统分发：Win10+ 装最新 Evergreen，Win7/8/8.1 装 109.0.1518.78。
//     所以两种系统用同一个安装入口即可，不需要维护两套下载地址。
//   · WebView2Loader.dll 自身只导入 KERNEL32/ADVAPI32/ole32 的基础接口，
//     在 Win7 上可以加载；XP 完全没有 WebView2，必须直接回退原生界面。
//
// 策略：
//   Win10+   → 用系统已装的 Evergreen（browserExecutableFolder 传 nullptr）；
//   Win7/8/8.1 → 优先用随包 Fixed Version 109 目录（无需安装、无需管理员），
//              否则用系统已装的 Evergreen 109；
//   XP      → 不创建、不安装，回退旧 Win32 控件；
//   以上任何系统没装 → 后台下载并静默安装官方 Bootstrapper，装完后
//   PostMessage(notify_hwnd, notify_msg) 让音乐窗重试创建。
//
// 关键坑：ICoreWebView2EnvironmentOptions::TargetCompatibleBrowserVersion
// 默认取 SDK 编译版本（本项目 128.0.2739.15）。109 Runtime 低于它时 loader
// 会直接以「版本不兼容」拒绝创建，所以旧系统上必须显式传低版本。

#include <windows.h>

#include <string>

namespace ttplayer::musicfree {

enum class WebView2OsTier {
    kWin10Plus,    // Win10 / Win11：用系统 Evergreen
    kLegacy,       // Win7 / 8 / 8.1：只能用 109.x
    kUnsupported,  // XP 或更早：WebView2 完全不可用
};

struct WebView2RuntimePlan {
    // CreateCoreWebView2EnvironmentWithOptions 的第一个参数。
    // 空 = 用系统 Evergreen；非空 = 随包 Fixed Version 目录（含 msedgewebview2.exe）。
    std::wstring browser_folder;
    // ICoreWebView2EnvironmentOptions::TargetCompatibleBrowserVersion。
    // 必须不高于实际 Runtime 大版本，否则 loader 拒绝创建。
    std::wstring compatible_version;
    bool available = false;
    WebView2OsTier tier = WebView2OsTier::kUnsupported;
    std::wstring detail;  // 诊断文本（写日志 / 排查用）
};

// 同步、只读、很快：读系统版本 + EdgeUpdate 注册表 pv + 扫描随包 Runtime 目录。
WebView2RuntimePlan PlanWebView2Runtime();

// 后台下载并静默安装官方 Evergreen Bootstrapper。进程内只启动一次；
// 完成时（且重新检测到 Runtime 可用）向 notify_hwnd 投递 notify_msg。
// 返回是否真的启动了安装任务。TTPLAYER_NO_WEBVIEW_INSTALL 可关闭。
bool EnsureWebView2RuntimeAsync(HWND notify_hwnd, UINT notify_msg);

bool WebView2RuntimeInstallInFlight();

}  // namespace ttplayer::musicfree
