#pragma once
#include <windows.h>
#include <memory>
namespace ttplayer::musicfree {
class MusicFreeBridge;   // 见 musicfree_bridge.h（已存在，勿改）

// WebView2 宿主：把页面装进音乐窗 content 区。实现细节藏在 .cpp，
// 本头文件**不得**包含 WebView2.h，避免污染其它编译单元。
class WebviewHost {
public:
    WebviewHost();
    ~WebviewHost();
    WebviewHost(const WebviewHost&) = delete;
    WebviewHost& operator=(const WebviewHost&) = delete;

    // 探测 WebView2 Runtime 是否可用（本机已装 157.x 即 true）
    static bool RuntimeAvailable();

    // 创建控制器与 WebView，挂到 parent，内容填充 content（父窗口客户区坐标）。
    // pageRoot 来自 webui_assets.h 的 PrepareWebuiRoot()；为空直接失败。
    // bridge 非空时注入 sender/poster 并在导航完成后推 bridge.ready。
    // 任何失败都返回 false，且**不抛异常、不崩溃**。
    bool Create(HWND parent, RECT content, const wchar_t* pageRoot,
                MusicFreeBridge* bridge);

    // 内容区变化（WM_SIZE / 皮肤重布局）时同步 WebView 尺寸
    void Resize(RECT content);

    // 重新加载页面（标题栏「刷新」）。reload 完成后 NavigationCompleted 会
    // 重发 bridge.ready，页面握手重新跑 —— 既能拉新数据，也能把卡在
    // 「正在连接宿主…」的页面拉回来。Idle/Failed 态为 no-op。
    void Reload();

    // 关闭控制器；析构前须确保没有在途的 sender/poster 回调
    void Destroy();

    bool Ready() const;      // 是否处于 Ready 态
    bool Failed() const;     // 是否已判定失败（宿主据此回退到旧 Win32 控件）
    const char* LastError() const;  // 简短中文原因，只读，不抛

private:
    // 状态机与 WebView2 COM 对象全部藏在 .cpp（pimpl）：
    // 头文件保持零 WebView2 依赖，任何编译单元包含本头都无需 SDK 路径。
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}  // namespace ttplayer::musicfree
