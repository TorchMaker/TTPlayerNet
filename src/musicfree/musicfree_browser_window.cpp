// MusicFree 兼容层 —— 音乐窗实现
//
// 把「查看 → 音乐窗」(0x7d68) 变成 MusicFree 的入口：搜索 / 播放 / 导入音源。
// 搜索与播放地址解析都在工作线程执行，完成后用 PostMessage 把结果送回窗口。
//
// 外框沿用皮肤包的 <browser_window> 段（原版内嵌浏览器用的就是这一段）：背景位图 +
// 关闭 / 上一个音源 / 下一个音源 / 重新搜索 / 窗口置顶 + 底部提示行。皮肤没有这一段
// 时退化成自绘的浅色框体，搜索功能不受影响。
//
// 拖动吸附与位置记忆由宿主（播放器主窗口）提供，音乐窗因此和歌词窗、播放列表窗一样
// 参与窗口吸附，并随主窗口一起移动。

#include "ttplayer/musicfree/musicfree_browser_window.h"
#include "ttplayer/ui/dpi_scale.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_frame.h"
#include "ttplayer/musicfree/musicfree_import_dialog.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/musicfree/musicfree_theme.h"
#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_frame.h"

namespace ttplayer::musicfree {

namespace {

// 调色板与基本图元（填充/描边/圆角框/文字/字体）都在共用主题里，音乐窗与导入窗
// 共用同一套，免得两处各画一份风格漂移。
using namespace theme;

constexpr int kIdPlatform = 3001;
constexpr int kIdQuery = 3002;
constexpr int kIdSearch = 3003;
constexpr int kIdImport = 3004;
constexpr int kIdList = 3005;
constexpr int kIdPlay = 3006;
constexpr int kIdClose = 3007;
constexpr int kIdStatus = 3008;
constexpr int kIdEnqueue = 3009;
constexpr int kIdDownload = 3010;

// 结果列表右键菜单的命令号（TrackPopupMenu 返回值，不是窗口命令）。
constexpr int kMenuPlay = 4001;
constexpr int kMenuEnqueue = 4002;
constexpr int kMenuDownload = 4003;
constexpr int kMenuCopyTitle = 4004;

constexpr UINT kMsgSearchDone = WM_APP + 0x510;
constexpr UINT kMsgResolveDone = WM_APP + 0x511;
// 下载是两段式的：先解析地址（kMsgDownloadReady），用户确认落盘位置后再拉字节
// （kMsgDownloadDone）。扩展名要从解析出来的地址推断，所以不能颠倒。
constexpr UINT kMsgDownloadReady = WM_APP + 0x512;
constexpr UINT kMsgDownloadDone = WM_APP + 0x513;

// 皮肤包没有 <browser_window> 时使用的兜底窗口尺寸。
constexpr int kFallbackWidth = 720;
constexpr int kFallbackHeight = 462;

// 窗口缩放：可抓取的边框厚度，以及最小尺寸的上下限。
//
// 最小尺寸默认取皮肤背景位图的自然尺寸 —— 窗口小于位图时 skin::DrawFrameBackground
// 会退化成「整幅 1:1 缩放」（见该函数的守卫），边框与标题栏会被拉变形。把最小尺寸
// 钉在自然尺寸上，边框就永远保持原始像素厚度，窗口只往大变（拖内是无效的，和任何
// 已到最小尺寸的普通窗口一样）。皮肤自然尺寸特别大时用上限兜一下，免得小屏上窗口
// 一开就顶到边。
//
// ★ 抓取带厚度统一取 frame::kResizeBand（自检也用同一个值，不会各自漂移）。
// 必须小于皮肤左右/上下边框厚度，否则会把内容区（尤其是列表右侧的滚动条）的
// 点击抢走。实测内置皮肤的边框：左 8px、右 9px、上 31px、下 31px。
// kResizeBand 是 96-DPI 逻辑值，实际判定时按窗口 DPI 换算（§6.9 High-DPI 规格）。
constexpr int kResizeBorder = frame::kResizeBand;

// The grab band is a logical thickness: on a 200% display a 6px band is only
// 3 logical pixels and becomes impossible to hit.
int ScaledResizeBorder(HWND hwnd) {
    return std::max(kResizeBorder,
                    ui::ResolveScaleForWindow(hwnd).X(kResizeBorder));
}
constexpr int kMinTrackWidthCap = 640;   // 宽度不再随缩放变化，仅作超大皮肤的兜底上限
constexpr int kMinTrackHeightCap = 260;  // 高度允许缩小到的下限
constexpr int kMaxTrackHeightCap = 1600; // 高度允许放大的上限（拖动只改高度）

constexpr int kRowHeight = 22;
constexpr int kButtonHeight = 22;
constexpr int kColumnCount = 5;

// 框体上可命中的皮肤按钮。
enum FrameHit {
    kFrameHitNone = 0,
    kFrameHitClose,
    kFrameHitPrevSource,
    kFrameHitNextSource,
    kFrameHitRefresh,
    kFrameHitTopMost,
};

// 框体自绘部分的位置（当前客户区坐标）。皮肤有效时由 frame::Map 把
// <browser_window> 的位图坐标换算过来（窗口缩放后按钮右锚定、底部行下锚定），
// 否则按兜底尺寸推算。
struct FrameLayout {
    bool skinned{};
    RECT content{};
    RECT close{};
    RECT prev_source{};
    RECT next_source{};
    RECT refresh{};
    RECT check_box{};
    RECT check_label{};
    RECT link_text{};
    // 皮肤形态的换算结果（九宫格拉伸区、背景绘制、窗口区域都要用同一份）。
    frame::Metrics skin{};
    // 兜底外框的分区（标题栏 / 标题文字 / 页脚 …）。导航簇与标题文字的互不重叠
    // 由 theme::BuildFrameChrome 保证；皮肤形态下不使用。
    theme::FrameChrome chrome{};
    // 搜索输入框的外描边。EDIT 自带的是系统凹陷边（WM_CTLCOLOREDIT 只换得了
    // 底色，换不掉那圈 3D 边），所以把边框改成自己画在子控件下面的一像素圆角线。
    RECT query_field{};
};


// 子控件回调上下文。四个按钮和一个结果列表都需要 hover 高亮，而 Win32 不会把子控件
// 上的鼠标移动转给父窗口，因此统一挂一层子类化回调。
struct ChildProbe {
    void* state{};      // BrowserState*，声明成 void* 以便在这里前置定义
    WNDPROC base{};
    int id{};
    bool is_list{};
    bool tracking_leave{};
};

struct BrowserState {
    HWND hwnd{};
    HWND owner{};
    HWND platform{};
    HWND query{};
    HWND search_button{};
    HWND import_button{};
    HWND status{};
    HWND list{};
    HWND play_button{};
    HWND enqueue_button{};
    HWND download_button{};
    MusicFreeSourceManager* mgr{};
    BrowserWindowHost host;
    // 返回 true 表示播放器已接受该请求（已入列/已开始播放）。
    std::function<bool(OnlineTrackAction, const OnlineSearchHit&,
                       const std::wstring&)> on_track;
    std::vector<OnlineSearchHit> hits;
    // 搜索/解析同一时刻只允许一个后台任务，避免插件沙箱被并发重入。
    std::atomic<bool> busy{false};
    std::wstring query_pending;  // 供后台线程读取的搜索条件（UI 线程写、任务启动前定格）
    // 首次创建时要预填并立即搜索的关键词。来自「在线搜索当前曲目」：以前那个命令
    // 会 ShellExecute 打开 qianqian.com，现在改成在音乐窗里按歌名搜。
    std::wstring initial_query;
    FrameLayout layout;
    bool top_most{};
    bool dragging{};
    // 重入保护：ApplyFrameMetrics 里的 SetWindowPos 会同步投递 WM_SIZE，
    // 处理 WM_SIZE 又会回头调用 ApplyFrameMetrics。
    bool applying_frame{};
    // 用户是否手动调过窗口大小。调过之后换肤就不再回到皮肤位图尺寸。
    bool user_resized{};
    // 鼠标是否停在可缩放的边框上（WM_NCHITTEST 会返回 HTLEFT/HTBOTTOMRIGHT…）。
    bool resizing{};
    int frame_hover{};
    int frame_pressed{};
    int hot_button{};
    int hot_row{-1};
    POINT drag_anchor{};
    RECT drag_start_bounds{};
    std::wstring link_text;
    HFONT ui_font{};
    HFONT bold_font{};
    HFONT small_font{};
    HFONT link_font{};
    LOGFONTW link_logfont{};
    HBRUSH panel_brush{};
    std::vector<std::unique_ptr<ChildProbe>> probes;
};

// 搜索线程 → UI 线程的结果负载（含每音源的诊断信息）。
struct SearchPayload {
    std::vector<OnlineSearchHit> hits;
    OnlineSearchDiagnostics diagnostics;
};

// 解析线程 → UI 线程的负载：播放地址 + 失败原因 + 触发本次解析的在线结果。
// 带上元数据（而非只带 URL）是刻意的：入列时要用音源返回的歌曲名/歌手/专辑，
// 而不是从播放地址反推出来的文件名。
struct ResolvePayload {
    OnlineTrackAction action{OnlineTrackAction::play};
    OnlineSearchHit hit;
    std::wstring url;
    std::wstring error;
};

// 下载线程 → UI 线程的负载。「解析播放地址」也在这个线程里做：下载必须等地址
// 解析出来，分两步走只会让 busy 标志在两次切换之间出现空窗。
struct DownloadPayload {
    OnlineSearchHit hit;
    std::wstring path;    // 目标文件（用户在选择框里确认过的路径）
    std::wstring error;
    std::int64_t bytes{};
    bool ok{};
};

std::shared_ptr<BrowserState> g_browser;

BrowserState* StateOf(const ChildProbe* probe) {
    return probe ? static_cast<BrowserState*>(probe->state) : nullptr;
}

// ------------------------------------------------------------------ 基础

std::wstring WindowText(HWND control) {
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

std::wstring Utf8ToWideString(const std::string& value) {
    return core::Utf8ToWide(value);
}

void SetStatus(BrowserState* state, const std::wstring& text) {
    if (state && state->status) SetWindowTextW(state->status, text.c_str());
}

void InvalidateFrame(BrowserState* state) {
    if (state && state->hwnd) InvalidateRect(state->hwnd, nullptr, FALSE);
}

// 后台任务进行中时禁用会再触发任务的按钮，并把「重新搜索」画成禁用帧。
void SetBusyUi(BrowserState* state, bool busy) {
    if (!state) return;
    if (state->search_button) EnableWindow(state->search_button, !busy);
    if (state->play_button) EnableWindow(state->play_button, !busy);
    if (state->enqueue_button) EnableWindow(state->enqueue_button, !busy);
    if (state->download_button) EnableWindow(state->download_button, !busy);
    if (state->import_button) EnableWindow(state->import_button, !busy);
    InvalidateFrame(state);
}

std::wstring FormatDuration(std::int64_t duration_ms) {
    if (duration_ms <= 0) return L"-";
    const std::int64_t seconds = duration_ms / 1000;
    std::wstring text = std::to_wstring(seconds / 60) + L":";
    const std::int64_t remainder = seconds % 60;
    if (remainder < 10) text += L"0";
    text += std::to_wstring(remainder);
    return text;
}

const skin::LegacySkin* ActiveLegacySkin(const BrowserState* state) {
    if (!state || !state->host.skin) return nullptr;
    return state->host.skin();
}

const skin::BrowserSkin* ActiveSkin(const BrowserState* state) {
    const skin::LegacySkin* legacy = ActiveLegacySkin(state);
    if (!legacy || !legacy->Browser().valid) return nullptr;
    return &legacy->Browser();
}

COLORREF FrameTransparent(const BrowserState* state,
                          const skin::BrowserSkin& browser) {
    return frame::TransparentColor(ActiveLegacySkin(state), browser);
}

// ------------------------------------------------------------------ 字体

void EnsureFonts(BrowserState* state) {
    if (!state) return;
    if (!state->ui_font) state->ui_font = CreateUiFont(-12, FW_NORMAL);
    if (!state->bold_font) state->bold_font = CreateUiFont(-12, FW_SEMIBOLD);
    if (!state->small_font) state->small_font = CreateUiFont(-11, FW_NORMAL);
    const skin::BrowserSkin* browser = ActiveSkin(state);
    const int height = browser && browser->link.font_size > 0
        ? -browser->link.font_size : -12;
    const wchar_t* face = browser && !browser->link.font.empty()
        ? browser->link.font.c_str() : L"Tahoma";
    if (!state->link_font || state->link_logfont.lfHeight != height ||
        _wcsicmp(state->link_logfont.lfFaceName, face) != 0) {
        if (state->link_font) DeleteObject(state->link_font);
        state->link_font = MakeFont(height, FW_NORMAL, face);
        state->link_logfont = {};
        if (state->link_font)
            GetObjectW(state->link_font, sizeof(LOGFONTW), &state->link_logfont);
    }
}

void DiscardFonts(BrowserState* state) {
    if (!state) return;
    for (HFONT* font : {&state->ui_font, &state->bold_font, &state->small_font,
                        &state->link_font}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
}

// ------------------------------------------------------------------ 命中测试

bool HitTest(const RECT& rect, POINT point) {
    return theme::HitRect(rect, point);
}


// ------------------------------------------------------------------ 布局

// 内容区顶部那条工具行：音源下拉 + 关键词输入 + 搜索 / 导入。布局与绘制共用同一份
// 计算，免得描边和子控件位置漂移。
struct ToolbarLayout {
    RECT combo{};
    RECT query{};
    RECT search{};
    RECT import{};
};

ToolbarLayout BuildToolbarLayout(const RECT& content) {
    ToolbarLayout layout;
    const LONG inner_left = content.left + 6;
    const LONG inner_right = content.right - 6;
    const LONG top = content.top + 4;
    const LONG width = std::max<LONG>(160, inner_right - inner_left);
    const LONG combo_w = std::min<LONG>(104, width / 4);
    const LONG button_w = 58;
    layout.combo = {inner_left, top, inner_left + combo_w, top + kButtonHeight};
    layout.import = {inner_right - button_w, top, inner_right, top + kButtonHeight};
    layout.search = {layout.import.left - 6 - button_w, top,
                     layout.import.left - 6, top + kButtonHeight};
    const LONG query_left = layout.combo.right + 6;
    const LONG query_right = std::max<LONG>(query_left + 60, layout.search.left - 6);
    layout.query = {query_left, top, query_right, top + kButtonHeight};
    return layout;
}

// 皮肤有效时窗口的自然尺寸就是背景位图尺寸，框体元素按 frame::Map 由位图坐标换算
// 到当前客户区（窗口缩放后内容区变大、按钮右锚定、底部行下锚定）；否则按兜底尺寸
// 推算一条与皮肤风格相近的边框。
FrameLayout BuildFrameLayout(const RECT& client,
                             const skin::BrowserSkin* browser,
                             const ui::DpiScale& scale) {
    FrameLayout layout;
    if (browser) {
        layout.skinned = true;
        layout.skin = frame::Map(client, *browser, scale);
        layout.content = layout.skin.content;
        layout.close = layout.skin.close;
        layout.prev_source = layout.skin.backward;
        layout.next_source = layout.skin.forward;
        layout.refresh = layout.skin.refresh;
        layout.check_box = layout.skin.check_box;
        layout.check_label = layout.skin.check_label;
        layout.link_text = layout.skin.link;
        layout.query_field = BuildToolbarLayout(layout.content).query;
        return layout;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int pad = 10;
    // 标题栏 / 内容面板 / 页脚三段，以及右对齐的导航簇（音源 ◀ ▶ 重新搜索 ✕）。
    // 标题文字区域由 BuildFrameChrome 裁到导航簇左侧，不会和按钮重叠。
    layout.chrome = theme::BuildFrameChrome(client, pad, 30, 30);
    layout.content = layout.chrome.content;
    layout.close = layout.chrome.close;
    layout.prev_source = layout.chrome.prev_source;
    layout.next_source = layout.chrome.next_source;
    layout.refresh = layout.chrome.refresh;
    layout.check_box = {pad, height - 26, pad + 13, height - 13};
    layout.check_label = {pad + 18, height - 27, pad + 86, height - 12};
    layout.link_text = {width - pad - 230, height - 27, width - pad, height - 12};
    layout.query_field = BuildToolbarLayout(layout.content).query;
    return layout;
}

void SizeResultColumns(BrowserState* state) {
    if (!state || !state->list) return;
    RECT client{};
    GetClientRect(state->list, &client);
    const int available = (client.right - client.left) - GetSystemMetrics(SM_CXVSCROLL);
    if (available < 200) return;
    // 平台 / 歌曲名 / 歌手 / 专辑 / 时长
    const double ratio[kColumnCount] = {0.13, 0.35, 0.21, 0.20, 0.11};
    int used = 0;
    for (int index = 0; index < kColumnCount - 1; ++index) {
        const int column = std::max(40, static_cast<int>(available * ratio[index]));
        ListView_SetColumnWidth(state->list, index, column);
        used += column;
    }
    ListView_SetColumnWidth(state->list, kColumnCount - 1,
                            std::max(38, available - used));
}

void LayoutChildren(BrowserState* state) {
    if (!state || !state->hwnd) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    state->layout = BuildFrameLayout(client, ActiveSkin(state),
                            ui::ResolveScaleForWindow(state->hwnd));

    const RECT content = state->layout.content;
    const LONG inner_left = content.left + 6;
    const LONG inner_right = content.right - 6;
    const ToolbarLayout toolbar = BuildToolbarLayout(content);
    const LONG top = toolbar.combo.top;

    MoveWindow(state->platform, toolbar.combo.left, toolbar.combo.top,
               toolbar.combo.right - toolbar.combo.left, 240, TRUE);
    MoveWindow(state->import_button, toolbar.import.left, toolbar.import.top,
               toolbar.import.right - toolbar.import.left, kButtonHeight, TRUE);
    MoveWindow(state->search_button, toolbar.search.left, toolbar.search.top,
               toolbar.search.right - toolbar.search.left, kButtonHeight, TRUE);
    // 输入框缩进一像素，让框体自绘的那圈描边露出来（代替系统的凹陷边）。
    MoveWindow(state->query, toolbar.query.left + 1, toolbar.query.top + 1,
               std::max<LONG>(40, toolbar.query.right - toolbar.query.left - 2),
               kButtonHeight - 2, TRUE);

    const int status_y = top + kButtonHeight + 3;
    MoveWindow(state->status, inner_left, status_y, inner_right - inner_left, 16,
               TRUE);

    const int buttons_y = content.bottom - kButtonHeight;
    MoveWindow(state->play_button, inner_left, buttons_y, 92, kButtonHeight, TRUE);
    MoveWindow(state->enqueue_button, inner_left + 98, buttons_y, 108,
               kButtonHeight, TRUE);
    MoveWindow(state->download_button, inner_left + 212, buttons_y, 70,
               kButtonHeight, TRUE);

    const int list_top = status_y + 20;
    const int list_bottom = std::max(list_top + 40, buttons_y - 8);
    MoveWindow(state->list, inner_left, list_top, inner_right - inner_left,
               list_bottom - list_top, TRUE);

    SizeResultColumns(state);
}

void ApplyFrameRegion(BrowserState* state) {
    if (!state || !state->hwnd) return;
    const skin::BrowserSkin* browser = ActiveSkin(state);
    if (!browser) {
        // 兜底框是规则矩形，不需要区域。
        SetWindowRgn(state->hwnd, nullptr, TRUE);
        return;
    }
    RECT client{};
    GetClientRect(state->hwnd, &client);
    // 区域必须和背景用同一份九宫格映射，否则拉伸之后透明边角会对不上。
    frame::ApplyWindowRegion(state->hwnd, *browser, state->layout.skin, client,
                             FrameTransparent(state, *browser),
                             ui::ResolveScaleForWindow(state->hwnd));
}

// 外框的自然尺寸：皮肤背景位图尺寸；没有皮肤时用兜底尺寸。
SIZE NaturalFrameSize(const BrowserState* state) {
    SIZE size{kFallbackWidth, kFallbackHeight};
    if (const skin::BrowserSkin* browser = ActiveSkin(state)) {
        if (browser->background.size.cx > 0 && browser->background.size.cy > 0)
            size = browser->background.size;
    }
    return size;
}

// 允许的最小尺寸。宽度锁定在自然位图宽度，高度允许在 [下限, 上限] 内随拖拽变化。
//
// ★ 宽度钉死在自然位图宽度：低于它 skin::DrawFrameBackground 会退化成整幅 1:1 缩放、
// 标题栏变形；高于它标题栏被横向拉糊、按钮错位。两端都钉在同一宽度上，窗口宽度
// 永远不变。高度则放开上下限，内容区随之下移/拉伸（标题栏与底栏始终保持原始像素）。
SIZE MinFrameSize(const BrowserState* state) {
    const SIZE natural = NaturalFrameSize(state);
    SIZE size{};
    size.cx = natural.cx;  // 宽度锁定：始终等于皮肤自然宽度
    size.cy = std::clamp<int>(natural.cy, kMinTrackHeightCap, kMaxTrackHeightCap);
    return size;
}

// 换肤 / 缩放后的统一收尾：窗口区域 + 子控件位置 + 重画。
// reset_size 为 true 时把窗口拉回自然尺寸（只在皮肤变化需要重贴合时用）；
// 否则保留用户当前的尺寸，只做最小尺寸夹取 —— 音乐窗的宽高因此是「活的」，
// 不再每次唤出都被写死的位图尺寸覆盖。
void ApplyFrameMetrics(BrowserState* state, bool reset_size) {
    if (!state || !state->hwnd || !IsWindow(state->hwnd)) return;
    if (state->applying_frame) return;
    state->applying_frame = true;

    RECT current{};
    GetWindowRect(state->hwnd, &current);
    const SIZE natural = NaturalFrameSize(state);
    const SIZE minimum = MinFrameSize(state);
    int width = reset_size ? natural.cx : current.right - current.left;
    int height = reset_size ? natural.cy : current.bottom - current.top;
    width = std::max<int>(width, minimum.cx);
    height = std::max<int>(height, minimum.cy);
    if (width != current.right - current.left ||
        height != current.bottom - current.top) {
        // 尺寸变化会同步投递 WM_SIZE；下面重排一次即可，不必依赖那条消息。
        SetWindowPos(state->hwnd, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT client{};
    GetClientRect(state->hwnd, &client);
    state->layout = BuildFrameLayout(client, ActiveSkin(state),
                            ui::ResolveScaleForWindow(state->hwnd));
    ApplyFrameRegion(state);
    LayoutChildren(state);
    InvalidateRect(state->hwnd, nullptr, TRUE);

    state->applying_frame = false;
}

// --------------------------------------------------------------- 框体绘制

// 皮肤按钮的当前状态帧。禁用时恒为最后一帧（皮肤的四态图里通常是灰态）。
int FrameButtonState(const BrowserState* state, int hit, bool enabled = true) {
    if (!enabled) return 3;
    if (state->frame_pressed == hit && state->frame_hover == hit) return 2;
    return state->frame_hover == hit ? 1 : 0;
}

void PaintFallbackFrame(BrowserState* state, HDC dc, const RECT& client) {
    (void)client;
    const FrameLayout& layout = state->layout;
    // 分区与外观都交给共用主题；这里只负责把当前 hover/pressed 状态翻成按钮帧号。
    // FrameButtonState 的 0..3 与 theme::ButtonState 同序（常态/热/按下/禁用）。
    theme::ChromeStates states;
    states.close = FrameButtonState(state, kFrameHitClose);
    states.prev_source = FrameButtonState(state, kFrameHitPrevSource);
    states.next_source = FrameButtonState(state, kFrameHitNextSource);
    states.refresh = FrameButtonState(state, kFrameHitRefresh, !state->busy.load());
    theme::PaintFrameChrome(dc, layout.chrome, L"音乐窗 · MusicFree",
                            state->bold_font, state->ui_font, states);
}

void PaintCheckBox(BrowserState* state, HDC dc, const skin::BrowserSkin* browser) {
    const RECT box = state->layout.check_box;
    if (browser && browser->check.image) {
        // 皮肤的四态图：0 未选 / 1 热 / 2 选中 / 3 灰。
        skin::DrawFrameButton(dc, browser->check, box, state->top_most ? 2 : 0,
                              FrameTransparent(state, *browser));
    } else {
        const bool hot = state->frame_hover == kFrameHitTopMost;
        DrawRoundedBox(dc, box, state->top_most ? kAccent : kPanel,
                       hot ? kAccentHot : kOutline, 4);
        if (state->top_most)
            DrawTextIn(dc, box, L"✓", RGB(255, 255, 255), state->small_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    const bool hot = state->frame_hover == kFrameHitTopMost;
    DrawTextIn(dc, state->layout.check_label, L"窗口置顶",
               state->top_most ? kAccent : (hot ? kText : kSubtleText),
               state->small_font, kLabelFormat);
}

void PaintFrame(BrowserState* state, HDC dc) {
    if (!state || !state->hwnd) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    EnsureFonts(state);
    const skin::BrowserSkin* browser = ActiveSkin(state);
    state->layout = BuildFrameLayout(
        client, browser, ui::ResolveScaleForWindow(state->hwnd));

    if (!browser) {
        PaintFallbackFrame(state, dc, client);
    } else {
        // 九宫格铺满客户区：边框 / 标题栏 / 页脚保持原始像素，只有内容区随窗口
        // 变大，所以放大之后的框体和原尺寸下看起来是同一套像素。
        frame::PaintBackground(dc, *browser, state->layout.skin, client);
        const COLORREF transparent = FrameTransparent(state, *browser);
        const bool busy = state->busy.load();
        skin::DrawFrameButton(dc, browser->close, state->layout.close,
                              FrameButtonState(state, kFrameHitClose), transparent);
        skin::DrawFrameButton(dc, browser->backward, state->layout.prev_source,
                              FrameButtonState(state, kFrameHitPrevSource),
                              transparent);
        skin::DrawFrameButton(dc, browser->forward, state->layout.next_source,
                              FrameButtonState(state, kFrameHitNextSource),
                              transparent);
        skin::DrawFrameButton(dc, browser->refresh, state->layout.refresh,
                              FrameButtonState(state, kFrameHitRefresh, !busy),
                              transparent);
    }

    PaintCheckBox(state, dc, browser);

    // 关键词输入框的描边：画在 EDIT 下面，EDIT 缩进一像素嵌在里面。
    DrawRoundedBox(dc, state->layout.query_field, kPanel, kOutline, 4);

    const COLORREF link_color = browser ? browser->link.color : RGB(96, 122, 134);
    DrawTextIn(dc, state->layout.link_text, state->link_text.c_str(), link_color,
               state->link_font ? state->link_font : state->small_font,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// --------------------------------------------------- 子控件自绘（按钮）

struct ButtonVisual {
    const wchar_t* text{};
    bool primary{};
    bool enabled{true};
};

ButtonVisual ButtonVisualFor(const BrowserState* state, int id) {
    const bool busy = state->busy.load();
    switch (id) {
        case kIdSearch: return {L"搜索", true, !busy};
        case kIdImport: return {L"导入…", false, !busy};
        case kIdPlay: return {L"播放所选", true, !busy};
        case kIdEnqueue: return {L"添加到列表", false, !busy};
        case kIdDownload: return {L"下载", false, !busy};
        default: return {nullptr, false, true};
    }
}

void DrawOwnerButton(BrowserState* state, const DRAWITEMSTRUCT& item) {
    const ButtonVisual visual = ButtonVisualFor(state, static_cast<int>(item.CtlID));
    if (!visual.text) return;
    const bool hover = state->hot_button == static_cast<int>(item.CtlID);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool enabled = visual.enabled && (item.itemState & ODS_DISABLED) == 0;
    const int button_state = !enabled ? theme::kButtonDisabled
        : (pressed ? theme::kButtonPressed
                   : (hover ? theme::kButtonHot : theme::kButtonNormal));

    FillSolidRect(item.hDC, item.rcItem, kPanel);
    RECT rect = item.rcItem;
    InflateRect(&rect, -1, -1);
    // 走共用主题：导入窗的按钮用的是同一个 PanelButtonSkin，两处外观必然一致。
    theme::PaintButton(item.hDC, rect, visual.text,
                       visual.primary ? state->bold_font : state->ui_font,
                       theme::PanelButtonSkin(visual.primary, button_state));
}

// ------------------------------------------------- 结果列表自绘（行绘制）

std::wstring ListItemText(HWND list, int row, int subitem) {
    wchar_t buffer[512]{};
    LVITEMW item{};
    item.iSubItem = subitem;
    item.pszText = buffer;
    item.cchTextMax = static_cast<int>(std::size(buffer));
    SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
    return buffer;
}

void DrawResultRow(BrowserState* state, const DRAWITEMSTRUCT& item) {
    const int row = static_cast<int>(item.itemID);
    if (row < 0) return;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    HDC dc = item.hDC;

    COLORREF background = kPanel;
    if (selected) background = kRowSelected;
    else if (state->hot_row == row) background = kRowHot;
    else if ((row % 2) != 0) background = kRowAlternate;
    FillSolidRect(dc, item.rcItem, background);

    if (selected) {
        RECT accent = item.rcItem;
        accent.right = accent.left + 3;
        FillSolidRect(dc, accent, kAccent);
    }

    const struct { COLORREF color; HFONT font; UINT format; } style[kColumnCount] = {
        {kSubtleText, state->small_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kText, state->bold_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kSubtleText, state->ui_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kSubtleText, state->small_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kSubtleText, state->small_font,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX},
    };

    LONG x = item.rcItem.left;
    for (int column = 0; column < kColumnCount; ++column) {
        const int width = ListView_GetColumnWidth(state->list, column);
        const RECT cell{x + 6, item.rcItem.top, x + width - 6, item.rcItem.bottom};
        if (cell.right > cell.left) {
            const std::wstring text = ListItemText(state->list, row, column);
            DrawTextIn(dc, cell, text.c_str(), style[column].color,
                       style[column].font, style[column].format);
        }
        x += width;
    }

    const RECT separator{item.rcItem.left, item.rcItem.bottom - 1,
                         item.rcItem.right, item.rcItem.bottom};
    FillSolidRect(dc, separator, kRowSeparator);
}

// 表头用轻量自绘贴近内容面板，避免默认的灰色 3D 立体感。
LRESULT HandleHeaderCustomDraw(BrowserState* state, NMLVCUSTOMDRAW* draw) {
    (void)state;
    switch (draw->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            draw->clrText = kSubtleText;
            draw->clrTextBk = kHeaderFill;
            return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT: {
            const RECT bounds = draw->nmcd.rc;
            if (bounds.right - bounds.left < 8) return CDRF_DODEFAULT;
            const RECT separator{bounds.right - 1, bounds.top + 4, bounds.right,
                                 bounds.bottom - 4};
            FillSolidRect(draw->nmcd.hdc, separator, kHeaderSeparator);
            return CDRF_DODEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

// ------------------------------------------------------------------ 列表

void InitializeColumns(BrowserState* state) {
    static const wchar_t* const kTitles[kColumnCount] = {
        L"平台", L"歌曲名", L"歌手", L"专辑", L"时长"};
    for (int index = 0; index < kColumnCount; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.iSubItem = index;
        column.cx = 80;
        column.pszText = const_cast<wchar_t*>(kTitles[index]);
        ListView_InsertColumn(state->list, index, &column);
    }
}

void FillResultList(BrowserState* state) {
    if (!state || !state->list) return;
    SendMessageW(state->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state->list);
    for (size_t index = 0; index < state->hits.size(); ++index) {
        const OnlineSearchHit& hit = state->hits[index];
        const std::wstring platform = Utf8ToWideString(hit.platform);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(platform.c_str());
        const int row = ListView_InsertItem(state->list, &item);
        if (row < 0) continue;
        const std::wstring title = Utf8ToWideString(hit.title);
        const std::wstring artist = Utf8ToWideString(hit.artist);
        const std::wstring album = Utf8ToWideString(hit.album);
        const std::wstring duration = FormatDuration(hit.duration_ms);
        ListView_SetItemText(state->list, row, 1, const_cast<wchar_t*>(title.c_str()));
        ListView_SetItemText(state->list, row, 2, const_cast<wchar_t*>(artist.c_str()));
        ListView_SetItemText(state->list, row, 3, const_cast<wchar_t*>(album.c_str()));
        ListView_SetItemText(state->list, row, 4,
                             const_cast<wchar_t*>(duration.c_str()));
    }
    state->hot_row = -1;
    SendMessageW(state->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->list, nullptr, TRUE);
}

// ------------------------------------------------------------------ 音源

// 当前选中的音源 platform 名（UTF-8；"全部音源" 返回空串）。
std::string SelectedPlatform(BrowserState* state) {
    if (!state || !state->platform) return {};
    const int selection = static_cast<int>(
        SendMessageW(state->platform, CB_GETCURSEL, 0, 0));
    if (selection <= 0) return {};
    wchar_t buffer[256]{};
    if (SendMessageW(state->platform, CB_GETLBTEXT, selection,
                     reinterpret_cast<LPARAM>(buffer)) == CB_ERR)
        return {};
    return core::WideToUtf8(buffer);
}

void RefreshPlatformCombo(BrowserState* state) {
    if (!state || !state->platform) return;
    const std::wstring previous = WindowText(state->platform);
    SendMessageW(state->platform, CB_RESETCONTENT, 0, 0);
    SendMessageW(state->platform, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"全部音源"));
    if (state->mgr) {
        for (const auto& meta : state->mgr->ListPlugins()) {
            if (!meta.can_search) continue;
            const std::wstring name = Utf8ToWideString(meta.name);
            if (name.empty()) continue;
            SendMessageW(state->platform, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    int selection = 0;
    const int count = static_cast<int>(
        SendMessageW(state->platform, CB_GETCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
        wchar_t buffer[256]{};
        if (SendMessageW(state->platform, CB_GETLBTEXT, index,
                         reinterpret_cast<LPARAM>(buffer)) == CB_ERR)
            continue;
        if (previous == buffer) { selection = index; break; }
    }
    SendMessageW(state->platform, CB_SETCURSEL, selection, 0);
}

// --------------------------------------------------------------- 后台任务

void StartSearch(BrowserState* state) {
    if (!state || !state->mgr) {
        SetStatus(state, L"没有可用的音源管理器");
        return;
    }
    const std::wstring query = WindowText(state->query);
    if (query.empty()) {
        SetStatus(state, L"请输入搜索关键词");
        SetFocus(state->query);
        return;
    }
    if (state->busy.exchange(true)) {
        SetStatus(state, L"正在处理上一个请求…");
        return;
    }

    const std::string utf8_query = core::WideToUtf8(query);
    const std::string platform = SelectedPlatform(state);
    state->hits.clear();
    FillResultList(state);
    SetBusyUi(state, true);
    SetStatus(state, L"正在搜索 " + query + L" …（音源返回较慢，请稍候）");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, utf8_query, platform] {
        auto* payload = new SearchPayload();
        try {
            payload->hits = SearchAllSources(utf8_query, platform, 1,
                                             &payload->diagnostics);
        } catch (const std::exception& e) {
            // 单个音源异常不应让整个音乐窗失效，失败即空结果 + 诊断文本。
            payload->diagnostics.errors.push_back(std::string("搜索失败: ") + e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgSearchDone, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void StartResolve(BrowserState* state, OnlineTrackAction action) {
    if (!state) return;
    const int index = ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
    if (index < 0 || static_cast<size_t>(index) >= state->hits.size()) {
        SetStatus(state, L"请先在上方列表中选择一首歌曲");
        return;
    }
    if (state->busy.exchange(true)) {
        SetStatus(state, L"正在处理上一个请求…");
        return;
    }
    const OnlineSearchHit hit = state->hits[static_cast<size_t>(index)];
    const bool enqueue = action == OnlineTrackAction::enqueue;
    SetBusyUi(state, true);
    SetStatus(state, (enqueue ? L"正在解析后加入列表：" : L"正在解析播放地址：") +
                     Utf8ToWideString(hit.title) + L" …");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, hit, action] {
        auto* payload = new ResolvePayload();
        payload->action = action;
        payload->hit = hit;
        try {
            std::string error;
            const std::string url = ResolvePlayUrl(hit.platform, hit.id, &error);
            payload->url = core::Utf8ToWide(url);
            payload->error = core::Utf8ToWide(error);
        } catch (const std::exception& e) {
            payload->error = core::Utf8ToWide(e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgResolveDone, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// ------------------------------------------------------------ 下载 / 右键菜单
//
// 文件名的清洗与默认目录规则（SanitizeFileName / GuessExtension /
// DefaultDownloadName / DefaultDownloadDirectory）已经提到 musicfree_app 里，
// 播放列表的「下载歌曲」用同一套，两边不会再各写一份。

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (block) {
        if (void* target = GlobalLock(block)) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(block);
            ok = SetClipboardData(CF_UNICODETEXT, block) != nullptr;
        }
        if (!ok) GlobalFree(block);
    }
    CloseClipboard();
    return ok;
}

int SelectedRow(BrowserState* state) {
    if (!state || !state->list) return -1;
    const int row = ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
    if (row < 0 || static_cast<size_t>(row) >= state->hits.size()) return -1;
    return row;
}

// 下载的第一步：解析播放地址。地址解析出来后才弹「另存为」，因为文件扩展名
// 要从地址推断（音源返回的 url 通常是 .mp3/.flac/.m4a）。
void StartDownload(BrowserState* state) {
    if (!state) return;
    const int index = SelectedRow(state);
    if (index < 0) {
        SetStatus(state, L"请先在上方列表中选择一首歌曲");
        return;
    }
    if (state->busy.exchange(true)) {
        SetStatus(state, L"正在处理上一个请求…");
        return;
    }
    const OnlineSearchHit hit = state->hits[static_cast<size_t>(index)];
    SetBusyUi(state, true);
    SetStatus(state, L"正在解析下载地址：" + Utf8ToWideString(hit.title) + L" …");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, hit] {
        auto* payload = new ResolvePayload();
        payload->action = OnlineTrackAction::play;  // 此路径只做解析，不使用该字段
        payload->hit = hit;
        try {
            std::string error;
            const std::string url = ResolvePlayUrl(hit.platform, hit.id, &error);
            payload->url = core::Utf8ToWide(url);
            payload->error = core::Utf8ToWide(error);
        } catch (const std::exception& e) {
            payload->error = core::Utf8ToWide(e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgDownloadReady, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 下载的第二步：用户确认落盘位置后真正拉取字节。
void BeginDownloadToFile(BrowserState* state, const OnlineSearchHit& hit,
                         const std::wstring& url, const std::wstring& path) {
    SetStatus(state, L"正在下载：" + Utf8ToWideString(hit.title) + L" …");
    const std::wstring wide_url = url;
    const std::wstring target = path;
    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, hit, wide_url, target] {
        auto* payload = new DownloadPayload();
        payload->hit = hit;
        payload->path = target;
        try {
            std::string error;
            payload->ok = DownloadTrackFile(wide_url, std::filesystem::path(target),
                                            &error);
            if (!payload->ok) payload->error = core::Utf8ToWide(error);
            else {
                std::error_code size_error;
                const auto size = std::filesystem::file_size(target, size_error);
                if (!size_error) payload->bytes = static_cast<std::int64_t>(size);
            }
        } catch (const std::exception& e) {
            payload->error = core::Utf8ToWide(e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgDownloadDone, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 结果列表右键菜单。列表本身是自绘的，菜单用系统弹出菜单（文件对话框同理）——
// 这两处是系统级 UI，逐项自绘的收益不如保持一致。
void ShowResultContextMenu(BrowserState* state, POINT screen) {
    if (!state || !state->list) return;
    POINT client = screen;
    ScreenToClient(state->list, &client);
    LVHITTESTINFO info{};
    info.pt = client;
    const int row = ListView_HitTest(state->list, &info);
    if (row < 0) return;  // 空白处右键不弹菜单

    ListView_SetItemState(state->list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(state->list, row,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    SetFocus(state->list);
    if (static_cast<size_t>(row) >= state->hits.size()) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuPlay, L"播放");
    AppendMenuW(menu, MF_STRING, kMenuEnqueue, L"添加到列表");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuDownload, L"下载到…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuCopyTitle, L"复制歌曲名");
    if (state->busy.load()) {
        for (const UINT command : {static_cast<UINT>(kMenuPlay),
                                   static_cast<UINT>(kMenuEnqueue),
                                   static_cast<UINT>(kMenuDownload)})
            EnableMenuItem(menu, command, MF_BYCOMMAND | MF_GRAYED);
    }
    const int command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screen.x, screen.y, 0, state->hwnd, nullptr);
    DestroyMenu(menu);
    switch (command) {
        case kMenuPlay: StartResolve(state, OnlineTrackAction::play); break;
        case kMenuEnqueue: StartResolve(state, OnlineTrackAction::enqueue); break;
        case kMenuDownload: StartDownload(state); break;
        case kMenuCopyTitle: {
            const std::wstring text = ListItemText(state->list, row, 1);
            SetStatus(state, CopyTextToClipboard(state->hwnd, text)
                ? L"已复制歌曲名：" + text
                : L"复制到剪贴板失败");
            break;
        }
        default: break;
    }
}

// 切换音源（框体上的 ◀ / ▶）。只有关键词非空时才顺带重搜，避免空查询报错。
void StepPlatform(BrowserState* state, int delta) {
    if (!state || !state->platform) return;
    const int count = static_cast<int>(
        SendMessageW(state->platform, CB_GETCOUNT, 0, 0));
    if (count <= 1) {
        SetStatus(state, L"可用音源只有 0~1 个，先用「导入…」添加更多音源");
        return;
    }
    if (state->busy.load()) {
        SetStatus(state, L"正在搜索/解析，请稍候再切换音源");
        return;
    }
    int selection = static_cast<int>(
        SendMessageW(state->platform, CB_GETCURSEL, 0, 0));
    if (selection < 0) selection = 0;
    selection = ((selection + delta) % count + count) % count;
    SendMessageW(state->platform, CB_SETCURSEL, selection, 0);
    const std::wstring name = WindowText(state->platform);
    if (WindowText(state->query).empty()) {
        SetStatus(state, L"已切换到音源：" + name);
        return;
    }
    StartSearch(state);
}

// ------------------------------------------------------------- 框体交互

int FrameHitTest(const BrowserState* state, POINT point) {
    const FrameLayout& layout = state->layout;
    if (HitTest(layout.close, point)) return kFrameHitClose;
    if (HitTest(layout.prev_source, point)) return kFrameHitPrevSource;
    if (HitTest(layout.next_source, point)) return kFrameHitNextSource;
    if (HitTest(layout.refresh, point)) return kFrameHitRefresh;
    if (HitTest(layout.check_box, point) || HitTest(layout.check_label, point))
        return kFrameHitTopMost;
    return kFrameHitNone;
}

// 鼠标落在窗口边框上时返回对应的 HT*，落在别处返回 HTCLIENT。
//
// 窗口是 WS_POPUP，没有系统边框可以拖，缩放完全靠 WM_NCHITTEST 自己报。注意
// **框体按钮必须先判**：默认皮肤里关闭按钮右边缘离窗口右边只有 6 像素，缩放带
// 会把它抢走，点关闭就变成拉伸了。
//
// 真正的几何判定在 frame::ResizeHitCode（纯算术、可被自检直接调用），这里只负责
// 取当前客户区尺寸。见该函数上方关于「不要按最小尺寸屏蔽某条边」的说明。
int ResizeHitTest(BrowserState* state, POINT point) {
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const SIZE size{client.right - client.left, client.bottom - client.top};
    return frame::ResizeHitCode(size, ScaledResizeBorder(state->hwnd), point);
}

// ------------------------------------------------------- 缩放带自检（诊断）
//
// 缩放带失效时**没有任何视觉提示** —— 只是鼠标到了边上不变形，靠肉眼很难定位，
// 以前只能反复手动去拖。这里把「鼠标落在四个边 / 四个角 / 内容区中心时分别拿到
// 哪个 HT*」直接写进 trace。
//
// 判定顺序和 WM_NCHITTEST 保持一致：**框体按钮优先于缩放带**（关闭按钮右边缘
// 距窗口右边只有 6px，两者会争抢同一条带）。被按钮抢走的位置标 (button)。
void TraceResizeBands(BrowserState* state) {
    if (!state || !state->hwnd || !IsWindow(state->hwnd)) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    const LONG inset = ScaledResizeBorder(state->hwnd) / 2;  // 探针落在带**内**，不是压在边界线上
    struct Probe {
        const char* name;
        POINT point;
    };
    const Probe probes[] = {
        {"left", {inset, height / 2}},
        {"right", {width - 1 - inset, height / 2}},
        {"top", {width / 2, inset}},
        {"bottom", {width / 2, height - 1 - inset}},
        {"top_left", {inset, inset}},
        {"top_right", {width - 1 - inset, inset}},
        {"bottom_left", {inset, height - 1 - inset}},
        {"bottom_right", {width - 1 - inset, height - 1 - inset}},
        {"content", {width / 2, height / 2}},
    };
    std::string detail = "client=" + std::to_string(width) + "x" +
                         std::to_string(height) +
                         " band=" + std::to_string(ScaledResizeBorder(state->hwnd));
    for (const Probe& probe : probes) {
        const int frame_hit = FrameHitTest(state, probe.point);
        const int code = frame_hit != kFrameHitNone
                             ? HTCLIENT
                             : ResizeHitTest(state, probe.point);
        detail += std::string("\n") + probe.name + "=" + frame::HitCodeName(code) +
                  (frame_hit != kFrameHitNone ? " (button)" : "");
    }
    TraceDiagnostic("BROWSER_RESIZE_HIT", detail);
}

void SetTopMost(BrowserState* state, bool top_most) {
    if (!state || !state->hwnd) return;
    state->top_most = top_most;
    SetWindowPos(state->hwnd, top_most ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (state->host.store_top_most) state->host.store_top_most(top_most);
    InvalidateFrame(state);
}

void InvokeFrameHit(BrowserState* state, int hit) {
    switch (hit) {
        case kFrameHitClose: PostMessageW(state->hwnd, WM_CLOSE, 0, 0); break;
        case kFrameHitPrevSource: StepPlatform(state, -1); break;
        case kFrameHitNextSource: StepPlatform(state, 1); break;
        case kFrameHitRefresh:
            if (state->busy.load()) SetStatus(state, L"正在搜索/解析，请稍候");
            else StartSearch(state);
            break;
        case kFrameHitTopMost: SetTopMost(state, !state->top_most); break;
        default: break;
    }
}

// 拖动。宿主在时由主窗口统一处理（带十像素吸附与工作区约束）；宿主缺席时退化成
// 自带的整窗拖动 + 工作区夹取，至少保证窗口还能移动。
void BeginDrag(BrowserState* state, POINT point) {
    state->dragging = true;
    if (state->host.begin_drag) {
        state->host.begin_drag(state->hwnd, point);
        return;
    }
    POINT screen = point;
    ClientToScreen(state->hwnd, &screen);
    state->drag_anchor = screen;
    GetWindowRect(state->hwnd, &state->drag_start_bounds);
    SetCapture(state->hwnd);
}

void ContinueDrag(BrowserState* state, POINT point) {
    if (state->host.continue_drag) {
        state->host.continue_drag(state->hwnd, point);
        return;
    }
    if (!state->dragging || GetCapture() != state->hwnd) return;
    POINT screen = point;
    ClientToScreen(state->hwnd, &screen);
    RECT bounds = state->drag_start_bounds;
    OffsetRect(&bounds, screen.x - state->drag_anchor.x,
               screen.y - state->drag_anchor.y);
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &info)) {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        bounds.left = std::clamp(bounds.left, info.rcWork.left,
                                 std::max(info.rcWork.left,
                                          info.rcWork.right - width));
        bounds.top = std::clamp(bounds.top, info.rcWork.top,
                                std::max(info.rcWork.top,
                                         info.rcWork.bottom - height));
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }
    SetWindowPos(state->hwnd, nullptr, bounds.left, bounds.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EndDrag(BrowserState* state) {
    if (!state || !state->hwnd) return;
    // 先落 dragging 再交给宿主：host.end_drag 里会 ReleaseCapture，而释放捕获
    // 会同步送回 WM_CAPTURECHANGED；那时若 dragging 还是 true，就会二次进入
    // 这里、把同一份窗口位置存两遍。
    const bool was_dragging = state->dragging;
    state->dragging = false;
    if (state->host.end_drag) {
        state->host.end_drag(state->hwnd);
    } else if (was_dragging) {
        if (GetCapture() == state->hwnd) ReleaseCapture();
    }
    if (state->host.store_bounds) {
        RECT bounds{};
        GetWindowRect(state->hwnd, &bounds);
        if (bounds.right > bounds.left && bounds.bottom > bounds.top)
            state->host.store_bounds(bounds);
    }
}

// ------------------------------------------------------- 子控件子类化

LRESULT CALLBACK ChildProc(HWND control, UINT message, WPARAM wparam,
                           LPARAM lparam) {
    auto* probe = reinterpret_cast<ChildProbe*>(
        GetWindowLongPtrW(control, GWLP_USERDATA));
    if (!probe || !probe->base)
        return DefWindowProcW(control, message, wparam, lparam);
    BrowserState* state = StateOf(probe);

    switch (message) {
        case WM_MOUSEMOVE: {
            if (state) {
                if (probe->is_list) {
                    LVHITTESTINFO info{};
                    info.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    const int row = ListView_HitTest(control, &info);
                    const int hot = row >= 0 ? row : -1;
                    if (hot != state->hot_row) {
                        state->hot_row = hot;
                        InvalidateRect(control, nullptr, FALSE);
                    }
                } else if (state->hot_button != probe->id) {
                    state->hot_button = probe->id;
                    InvalidateRect(control, nullptr, FALSE);
                }
            }
            if (!probe->tracking_leave) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
                if (TrackMouseEvent(&track)) probe->tracking_leave = true;
            }
            break;
        }
        case WM_MOUSELEAVE:
            probe->tracking_leave = false;
            if (state) {
                if (probe->is_list) {
                    if (state->hot_row != -1) {
                        state->hot_row = -1;
                        InvalidateRect(control, nullptr, FALSE);
                    }
                } else if (state->hot_button == probe->id) {
                    state->hot_button = 0;
                    InvalidateRect(control, nullptr, FALSE);
                }
            }
            break;
        case WM_SETCURSOR:
            if (!probe->is_list && LOWORD(lparam) == HTCLIENT &&
                IsWindowEnabled(control)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            // 搜索框回车即搜索，省得每次都去点按钮。
            if (probe->id == kIdQuery && wparam == VK_RETURN && state &&
                state->hwnd) {
                PostMessageW(state->hwnd, WM_COMMAND,
                             MAKEWPARAM(kIdSearch, BN_CLICKED), 0);
                return 0;
            }
            break;
        case WM_CONTEXTMENU:
            // ListView 不会把右键上下文菜单消息转发给父窗口，
            // 这里显式转给 BrowserProc，由它统一弹「播放/下载」菜单。
            if (state && state->hwnd) {
                LRESULT handled = SendMessageW(state->hwnd, WM_CONTEXTMENU,
                                               wparam, lparam);
                if (handled) return handled;
            }
            break;
        case WM_NCDESTROY:
            // 上下文由 BrowserState::probes 持有；这里只是摘掉反向引用。
            SetWindowLongPtrW(control, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return CallWindowProcW(probe->base, control, message, wparam, lparam);
}

void TrackControl(BrowserState* state, HWND control, int id, bool is_list) {
    if (!control) return;
    auto probe = std::make_unique<ChildProbe>();
    probe->state = state;
    probe->id = id;
    probe->is_list = is_list;
    probe->base = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ChildProc)));
    SetWindowLongPtrW(control, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(probe.get()));
    state->probes.push_back(std::move(probe));
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK BrowserProc(HWND hwnd, UINT message, WPARAM wparam,
                            LPARAM lparam) {
    BrowserState* state = reinterpret_cast<BrowserState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            state = static_cast<BrowserState*>(create->lpCreateParams);
            if (state) {
                state->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(state));
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        case WM_CREATE: {
            if (!state) return -1;
            state->panel_brush = CreateSolidBrush(kPanel);
            // 底部提示行：告诉用户在线结果可以直接下载（右键菜单与「下载」按钮
            // 走的是同一条路径）。提示文字不用符号字符，界面字体没有那些字形。
            state->link_text = L"右键结果可下载";

            state->platform = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdPlatform), nullptr,
                nullptr);
            // 不要 WS_EX_CLIENTEDGE：系统凹陷边盖不住也换不掉底色，这里改用
            // PaintFrame 自绘的一像素描边。
            state->query = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdQuery), nullptr,
                nullptr);
            state->search_button = CreateWindowW(L"BUTTON", L"搜索",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdSearch), nullptr,
                nullptr);
            state->import_button = CreateWindowW(L"BUTTON", L"导入…",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdImport), nullptr,
                nullptr);
            state->play_button = CreateWindowW(L"BUTTON", L"播放所选",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdPlay), nullptr,
                nullptr);
            state->enqueue_button = CreateWindowW(L"BUTTON", L"添加到列表",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdEnqueue), nullptr,
                nullptr);
            state->download_button = CreateWindowW(L"BUTTON", L"下载",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdDownload), nullptr,
                nullptr);
            state->status = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdStatus), nullptr,
                nullptr);
            state->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                    LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_OWNERDRAWFIXED,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdList), nullptr,
                nullptr);
            if (!state->list || !state->platform || !state->query) return -1;

            InitializeColumns(state);
            ListView_SetExtendedListViewStyle(state->list,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetBkColor(state->list, kPanel);
            ListView_SetTextBkColor(state->list, kPanel);
            ListView_SetTextColor(state->list, kText);
            EnsureFonts(state);
            for (HWND control : {state->platform, state->query, state->status,
                                 state->list}) {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state->ui_font), TRUE);
            }
            if (HWND header = ListView_GetHeader(state->list)) {
                SendMessageW(header, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state->small_font), TRUE);
            }

            TrackControl(state, state->search_button, kIdSearch, false);
            TrackControl(state, state->import_button, kIdImport, false);
            TrackControl(state, state->play_button, kIdPlay, false);
            TrackControl(state, state->enqueue_button, kIdEnqueue, false);
            TrackControl(state, state->download_button, kIdDownload, false);
            TrackControl(state, state->query, kIdQuery, false);
            TrackControl(state, state->list, kIdList, true);

            RefreshPlatformCombo(state);
            // 保留创建时给定的尺寸（可能是用户上次调好的大小），只做最小尺寸夹取。
            ApplyFrameMetrics(state, false);
            SetStatus(state,
                L"输入关键词后回车或点「搜索」。音源需要先通过「导入…」添加。");
            SetFocus(state->query);
            // 「在线搜索当前曲目」这类入口会带一个初始关键词进来：填进输入框并
            // 立即搜索，省得用户再手敲一遍歌名。
            if (!state->initial_query.empty()) {
                SetWindowTextW(state->query, state->initial_query.c_str());
                StartSearch(state);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure && measure->CtlType == ODT_LISTVIEW) {
                measure->itemHeight = kRowHeight;
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (!item || !state) break;
            if (item->CtlType == ODT_LISTVIEW) {
                DrawResultRow(state, *item);
                return TRUE;
            }
            if (item->CtlType == ODT_BUTTON) {
                DrawOwnerButton(state, *item);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            if (!state || !state->panel_brush) break;
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, kPanel);
            SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(state->panel_brush);
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (dc && state) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const int width = std::max(1L, client.right - client.left);
                const int height = std::max(1L, client.bottom - client.top);
                // 框体是一整幅位图 + 多个按钮帧，逐次直接画会闪；先画进内存位图再
                // 一次性 BitBlt。
                const HDC buffer = CreateCompatibleDC(dc);
                const HBITMAP canvas = buffer
                    ? CreateCompatibleBitmap(dc, width, height) : nullptr;
                if (buffer && canvas) {
                    const HGDIOBJ old_bitmap = SelectObject(buffer, canvas);
                    PaintFrame(state, buffer);
                    BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
                    SelectObject(buffer, old_bitmap);
                }
                if (canvas) DeleteObject(canvas);
                if (buffer) DeleteDC(buffer);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {
            if (!state) return HTCLIENT;
            // WM_NCHITTEST 的坐标是屏幕坐标，必须先转成客户区。
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            // 框体按钮优先于缩放带（关闭按钮离右边缘只有几个像素）。
            if (FrameHitTest(state, point) != kFrameHitNone) return HTCLIENT;
            return ResizeHitTest(state, point);
        }
        case WM_GETMINMAXINFO: {
            if (!state) break;
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const SIZE minimum = MinFrameSize(state);
            info->ptMinTrackSize.x = minimum.cx;
            info->ptMinTrackSize.y = minimum.cy;
            // 宽度钉死：最小 == 最大 == 自然宽度，横向彻底不可缩放（避免标题栏被拉糊）。
            // 高度不约束上限，保留默认的「随拖拽长高」能力。
            info->ptMaxTrackSize.x = minimum.cx;
            return 0;
        }
        case WM_SIZE: {
            // 缩放时跟着重排：内容区、右侧锚定的框体按钮、底部行都要按新客户区
            // 重新算一遍，窗口区域也要同步（否则拉伸后的边角会被旧掩码裁掉）。
            if (wparam != SIZE_MINIMIZED) {
                ApplyFrameMetrics(state, false);
                // 记住「用户调过大小」——换肤时就不再强行拉回新皮肤的自然尺寸。
                if (state && !state->applying_frame) {
                    const SIZE natural = NaturalFrameSize(state);
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    if (client.right - client.left != natural.cx ||
                        client.bottom - client.top != natural.cy)
                        state->user_resized = true;
                }
            }
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            // 拖动缩放结束：再对齐一次并把新的宽高一并记进设置，下次打开就是
            // 用户选的大小（不再被位图尺寸写死）。
            ApplyFrameMetrics(state, false);
            if (state && state->host.store_bounds) {
                RECT bounds{};
                GetWindowRect(hwnd, &bounds);
                if (bounds.right > bounds.left && bounds.bottom > bounds.top)
                    state->host.store_bounds(bounds);
            }
            // 缩放带跟着新客户区走，顺便把四条边的命中结果留一份证据。
            TraceResizeBands(state);
            return 0;
        }
        case WM_SETCURSOR: {
            if (reinterpret_cast<HWND>(wparam) != hwnd || LOWORD(lparam) != HTCLIENT)
                break;
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            const bool hand = state && FrameHitTest(state, point) != kFrameHitNone;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_MOUSEMOVE: {
            if (!state) break;
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (state->dragging) {
                ContinueDrag(state, point);
                return 0;
            }
            const int hit = FrameHitTest(state, point);
            if (hit != state->frame_hover) {
                state->frame_hover = hit;
                InvalidateFrame(state);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (state && state->frame_hover != kFrameHitNone) {
                state->frame_hover = kFrameHitNone;
                InvalidateFrame(state);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!state) break;
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int hit = FrameHitTest(state, point);
            if (hit != kFrameHitNone) {
                state->frame_pressed = hit;
                InvalidateFrame(state);
                return 0;
            }
            BeginDrag(state, point);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!state) break;
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (state->frame_pressed != kFrameHitNone) {
                const int pressed = state->frame_pressed;
                state->frame_pressed = kFrameHitNone;
                InvalidateFrame(state);
                if (pressed == FrameHitTest(state, point))
                    InvokeFrameHit(state, pressed);
                return 0;
            }
            EndDrag(state);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (state && state->dragging) EndDrag(state);
            return 0;
        case WM_KEYDOWN:
            if (state && wparam == VK_ESCAPE) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_COMMAND: {
            if (!state) break;
            switch (LOWORD(wparam)) {
                case kIdSearch: StartSearch(state); return 0;
                case kIdPlay:
                    StartResolve(state, OnlineTrackAction::play);
                    return 0;
                case kIdEnqueue:
                    StartResolve(state, OnlineTrackAction::enqueue);
                    return 0;
                case kIdDownload:
                    StartDownload(state);
                    return 0;
                case kIdImport: {
                    // 音源集合在导入时会被改写，而搜索线程正在遍历插件表；
                    // 两者不能并发，否则可能出现悬垂指针。
                    if (state->busy.load()) {
                        SetStatus(state, L"正在搜索/解析，请稍候再导入音源");
                        return 0;
                    }
                    // 导入对话框自带模态消息循环；导入完成后音源集合可能变化，
                    // 因此回到这里要重建音源下拉框。
                    // 皮肤来源直接转交音乐窗自己的 host.skin：导入窗因此和音乐窗
                    // 用同一个框体（同尺寸、同背景位图、同关闭按钮）。
                    ShowMusicFreeImportDialog(hwnd, state->mgr, state->host.skin);
                    RefreshPlatformCombo(state);
                    if (state->mgr) {
                        const size_t count = state->mgr->ListPlugins().size();
                        SetStatus(state, count
                            ? L"已导入音源：" + std::to_wstring(count) +
                                  L" 个，可以开始搜索"
                            : L"尚未导入任何音源，请再次点击「导入…」");
                    }
                    return 0;
                }
                default: break;
            }
            break;
        }
        case WM_NOTIFY: {
            if (!state) break;
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (!header) break;
            if (header->hwndFrom == state->list && header->code == NM_DBLCLK) {
                StartResolve(state, OnlineTrackAction::play);
                return 0;
            }
            const HWND list_header = state->list ? ListView_GetHeader(state->list)
                                                 : nullptr;
            if (list_header && header->hwndFrom == list_header &&
                header->code == NM_CUSTOMDRAW) {
                return HandleHeaderCustomDraw(
                    state, reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
            }
            break;
        }
        case kMsgSearchDone: {
            auto* payload = reinterpret_cast<SearchPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            state->hits = std::move(payload->hits);
            const OnlineSearchDiagnostics diagnostics = std::move(payload->diagnostics);
            delete payload;
            state->busy.store(false);
            SetBusyUi(state, false);
            FillResultList(state);

            const std::wstring failed = diagnostics.errors.empty()
                ? std::wstring()
                : Utf8ToWideString(diagnostics.errors.front());
            if (state->hits.empty()) {
                std::wstring text = L"没有结果（已查询 " +
                    std::to_wstring(diagnostics.sources) + L" 个音源";
                if (!diagnostics.errors.empty())
                    text += L"，其中 " + std::to_wstring(diagnostics.errors.size()) +
                            L" 个报错";
                text += L"）";
                if (!failed.empty()) text += L"：" + failed;
                else if (diagnostics.sources == 0)
                    text += L"，请先用「导入…」添加音源";
                SetStatus(state, text);
            } else {
                std::wstring text = L"找到 " + std::to_wstring(state->hits.size()) +
                    L" 条结果（" + std::to_wstring(diagnostics.sources_with_hits) +
                    L"/" + std::to_wstring(diagnostics.sources) +
                    L" 个音源有结果），双击任意一行即可播放";
                if (!failed.empty()) text += L"；部分音源失败：" + failed;
                SetStatus(state, text);
            }
            return 0;
        }
        case kMsgResolveDone: {
            auto* payload = reinterpret_cast<ResolvePayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const OnlineTrackAction action = payload->action;
            const OnlineSearchHit hit = std::move(payload->hit);
            const std::wstring resolved = std::move(payload->url);
            const std::wstring reason = std::move(payload->error);
            delete payload;
            state->busy.store(false);
            SetBusyUi(state, false);
            if (resolved.empty()) {
                // 把音源自己给出的失败原因显示出来，用户才知道该换音源、
                // 换歌曲，还是该去关心会员/地区限制。
                std::wstring text = L"解析失败：该音源没有返回可播放地址";
                if (!reason.empty()) text += L"（" + reason + L"）";
                SetStatus(state, text);
                std::wstring detail =
                    L"无法解析播放地址。\n\n常见原因：音源已失效、需要会员、或该曲目被限制。\n"
                    L"可尝试换一个音源或换一首歌。";
                if (!reason.empty()) detail += L"\n\n音源返回：\n" + reason;
                MessageBoxW(hwnd, detail.c_str(), L"MusicFree 音乐窗",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            // 元数据本身已是 UTF-8；只有解析出的地址需要转码，且转码可能抛，
            // 不能让它逃出窗口过程。
            try {
                TraceDiagnostic("RESOLVED",
                    "action=" + std::string(action == OnlineTrackAction::enqueue
                                   ? "enqueue" : "play") +
                    "\nplatform=" + hit.platform +
                    "\nid=" + hit.id +
                    "\ntitle=" + hit.title +
                    "\nartist=" + hit.artist +
                    "\nalbum=" + hit.album +
                    "\nduration_ms=" + std::to_string(hit.duration_ms) +
                    "\nurl=" + core::WideToUtf8(resolved));
            } catch (const std::exception&) {
            }
            if (!state->on_track) {
                SetStatus(state, L"播放地址已解析，但播放器未接入");
                return 0;
            }
            // 元数据与地址一起交给播放器：列表/标题栏用 hit 里的歌曲名与歌手，
            // 不要用解析出来的 CDN 地址反推文件名。
            const std::wstring label = Utf8ToWideString(hit.title);
            const bool accepted = state->on_track(action, hit, resolved);
            if (!accepted) {
                SetStatus(state, action == OnlineTrackAction::enqueue
                    ? L"加入播放列表失败：" + label
                    : L"播放请求未被接受：" + label);
                return 0;
            }
            SetStatus(state, action == OnlineTrackAction::enqueue
                ? L"已加入播放列表：" + label : L"正在播放…");
            return 0;
        }
        case kMsgDownloadReady: {
            auto* payload = reinterpret_cast<ResolvePayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const OnlineSearchHit hit = std::move(payload->hit);
            const std::wstring url = std::move(payload->url);
            const std::wstring reason = std::move(payload->error);
            delete payload;
            if (url.empty()) {
                state->busy.store(false);
                SetBusyUi(state, false);
                std::wstring text = L"下载失败：该音源没有返回可播放地址";
                if (!reason.empty()) text += L"（" + reason + L"）";
                SetStatus(state, text);
                return 0;
            }
            // 解析出来的地址决定文件扩展名，所以「另存为」必须放在这一步之后。
            const std::wstring name = DefaultDownloadName(hit.title, hit.artist, url);
            const auto directory = DefaultDownloadDirectory();
            std::wstring initial = directory.empty()
                ? name : (directory / name).wstring();
            std::wstring buffer(initial.size() + 64, L'\0');
            std::copy(initial.begin(), initial.end(), buffer.begin());
            const std::wstring directory_text = directory.wstring();

            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = state->hwnd;
            ofn.lpstrFilter =
                L"音频文件\0*.mp3;*.flac;*.m4a;*.aac;*.wav;*.ogg;*.wma\0"
                L"所有文件\0*.*\0";
            ofn.lpstrFile = buffer.data();
            ofn.nMaxFile = static_cast<DWORD>(buffer.size());
            ofn.lpstrInitialDir = directory_text.empty()
                ? nullptr : directory_text.c_str();
            // OFN_NOCHANGEDIR 很关键：不设的话公共对话框会把进程当前目录改掉，
            // 而播放器是按进程当前目录找资源的。
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&ofn)) {
                state->busy.store(false);
                SetBusyUi(state, false);
                SetStatus(state, L"已取消下载");
                return 0;
            }
            BeginDownloadToFile(state, hit, url, buffer.c_str());
            return 0;
        }
        case kMsgDownloadDone: {
            auto* payload = reinterpret_cast<DownloadPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const DownloadPayload result = std::move(*payload);
            delete payload;
            state->busy.store(false);
            SetBusyUi(state, false);
            const std::wstring title = Utf8ToWideString(result.hit.title);
            if (!result.ok) {
                std::wstring text = L"下载失败：" + title;
                if (!result.error.empty()) text += L"（" + result.error + L"）";
                SetStatus(state, text);
                return 0;
            }
            // 保留一位小数的 MB，不引 printf：字节 / 1MiB 的十分之一。
            const std::int64_t tenths = (result.bytes * 10 + 524288) / 1048576;
            const std::wstring size = std::to_wstring(tenths / 10) + L"." +
                std::to_wstring(tenths % 10) + L" MB";
            SetStatus(state, L"已下载 " + title + L"（" + size + L"）→ " +
                             result.path);
            try {
                TraceDiagnostic("DOWNLOADED",
                    "platform=" + result.hit.platform +
                    "\ntitle=" + result.hit.title +
                    "\nartist=" + result.hit.artist +
                    "\nbytes=" + std::to_string(result.bytes) +
                    "\npath=" + core::WideToUtf8(result.path));
            } catch (const std::exception&) {
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (!state) break;
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // 键盘唤出（VK_APPS / Shift+F10）时坐标为 (-1,-1)，用列表左上角附近
            // 作为默认落点；命中测试拿不到行时不会弹菜单。
            if (point.x == -1 && point.y == -1) {
                if (!state->list) break;
                RECT bounds{};
                GetWindowRect(state->list, &bounds);
                point = {bounds.left + 40, bounds.top + 40};
            }
            ShowResultContextMenu(state, point);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            // 子控件在这里之后才会被销毁，它们的子类化上下文必须留到那之后，
            // 因此只释放本窗口自己的 GDI 资源。
            if (state) {
                if (state->host.store_bounds) {
                    RECT bounds{};
                    GetWindowRect(hwnd, &bounds);
                    if (bounds.right > bounds.left && bounds.bottom > bounds.top)
                        state->host.store_bounds(bounds);
                }
                if (state->host.on_closed) state->host.on_closed();
                DiscardFonts(state);
                if (state->panel_brush) {
                    DeleteObject(state->panel_brush);
                    state->panel_brush = nullptr;
                }
                state->on_track = nullptr;
            }
            // 这里不能 PostQuitMessage：本窗口不是进程的主窗口。
            return 0;
        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            const bool owns = g_browser && g_browser->hwnd == hwnd;
            const LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
            if (owns) g_browser.reset();
            return result;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

constexpr wchar_t kBrowserClass[] = L"TTPlayerMusicFreeBrowser";

bool EnsureWindowClass() {
    static int state = 0;  // 0 未注册 / 1 成功 / -1 失败
    if (state != 0) return state > 0;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = BrowserProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kBrowserClass;
    // 框体（含圆角透明区）全部自绘，所以不能挂系统背景刷，否则透明角会被刷成
    // 系统色。区域由 ApplyFrameRegion 设置。
    window_class.hbrBackground = nullptr;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

// 初始位置：优先用保存过的位置，否则贴在主窗口右侧，最后夹到工作区内。
RECT InitialBrowserBounds(HWND owner, int width, int height) {
    RECT bounds{0, 0, width, height};
    RECT owner_rect{};
    if (owner && GetWindowRect(owner, &owner_rect)) {
        bounds.left = owner_rect.right + 8;
        bounds.top = owner_rect.top;
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &info)) {
        const int work_width = info.rcWork.right - info.rcWork.left;
        const int work_height = info.rcWork.bottom - info.rcWork.top;
        bounds.left = std::clamp(bounds.left, info.rcWork.left,
                                 std::max(info.rcWork.left,
                                          info.rcWork.right - width));
        bounds.top = std::clamp(bounds.top, info.rcWork.top,
                                std::max(info.rcWork.top,
                                         info.rcWork.bottom - height));
        bounds.right = bounds.left + std::min(width, work_width);
        bounds.bottom = bounds.top + std::min(height, work_height);
    }
    return bounds;
}

} // namespace

bool IsMusicFreeBrowserVisible() {
    return g_browser && g_browser->hwnd && IsWindowVisible(g_browser->hwnd);
}

HWND MusicFreeBrowserWindow() {
    return g_browser ? g_browser->hwnd : nullptr;
}

void CloseMusicFreeBrowser() {
    if (g_browser && g_browser->hwnd && IsWindow(g_browser->hwnd))
        PostMessageW(g_browser->hwnd, WM_CLOSE, 0, 0);
}

void RefreshMusicFreeBrowserFrame() {
    if (!g_browser || !g_browser->hwnd) return;
    // 换肤时只有「用户从没调过大小」才回到新皮肤的自然尺寸；用户手动拉大过就
    // 保留他的尺寸，否则每换一次皮肤窗口尺寸都会被重置。
    ApplyFrameMetrics(g_browser.get(), !g_browser->user_resized);
}

void ShowMusicFreeBrowser(HWND owner, MusicFreeSourceManager* mgr,
                          BrowserWindowHost host,
                          std::function<bool(OnlineTrackAction,
                                             const OnlineSearchHit&,
                                             const std::wstring&)> on_track,
                          const std::wstring& initial_query) {
    // 已存在则只做前置、刷新音源与宿主，保持单一音乐窗。
    if (g_browser && g_browser->hwnd && IsWindow(g_browser->hwnd)) {
        if (on_track) g_browser->on_track = std::move(on_track);
        g_browser->host = std::move(host);
        g_browser->mgr = mgr ? mgr : AppMusicFreeManager();
        // 宿主的置顶状态可能在上次关闭后被「选项/主窗口」改过，这里同步一次。
        if (g_browser->top_most != g_browser->host.top_most)
            SetTopMost(g_browser.get(), g_browser->host.top_most);
        RefreshPlatformCombo(g_browser.get());
        // 唤出已存在的窗口时**不再**把尺寸拉回皮肤位图尺寸：窗口现在可以缩放，
        // 用户上次调好的宽高要留着，这里只重排一次。
        ApplyFrameMetrics(g_browser.get(), false);
        ShowWindow(g_browser->hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_browser->hwnd);
        // 带着关键词再次唤出（例如在播放列表里对另一首歌再点一次「在线搜索」）：
        // 覆盖输入框并重新搜索，而不是让用户对着旧结果发呆。
        if (!initial_query.empty()) {
            SetWindowTextW(g_browser->query, initial_query.c_str());
            StartSearch(g_browser.get());
        }
        return;
    }

    if (!EnsureWindowClass()) return;

    auto state = std::make_shared<BrowserState>();
    state->owner = owner;
    state->mgr = mgr ? mgr : AppMusicFreeManager();
    state->host = std::move(host);
    state->on_track = std::move(on_track);
    state->top_most = state->host.top_most;
    state->initial_query = initial_query;
    g_browser = state;

    const SIZE natural = NaturalFrameSize(state.get());
    const SIZE minimum = MinFrameSize(state.get());
    int width = natural.cx;
    int height = natural.cy;
    RECT bounds = InitialBrowserBounds(owner, width, height);
    if (state->host.saved_bounds) {
        const RECT saved = state->host.saved_bounds();
        if (saved.right > saved.left && saved.bottom > saved.top) {
            bounds.left = saved.left;
            bounds.top = saved.top;
            // 上次的**尺寸**也一起恢复（只夹到最小尺寸）：窗口能缩放之后，
            // 每次唤出都回到皮肤位图尺寸就等于把用户调过的宽高丢了。
            width = std::max<int>(saved.right - saved.left, minimum.cx);
            height = std::max<int>(saved.bottom - saved.top, minimum.cy);
            bounds.right = saved.left + width;
            bounds.bottom = saved.top + height;
        }
    }

    // 皮肤位图尺寸 = 想要的**客户区**尺寸。用 AdjustWindowRectEx 反算外框尺寸，
    // 而不是直接把位图尺寸当外框尺寸传：只要窗口样式里混进了任何非客户区边框
    // （例如 WS_EX_DLGMODALFRAME 会给 WS_POPUP 加 3px 边框），客户区就会比位图
    // 小，从而触发 DrawFrameBackground 的「整幅缩放」退化分支 —— 外观上正是
    // 「背景偏 / 被压扁」。当前用的是 WS_POPUP + WS_EX_TOOLWINDOW（delta=0），
    // 这层换算只是防止以后有人改样式时静默退化的保险。
    const DWORD frame_style = WS_POPUP;
    const DWORD frame_ex_style = WS_EX_TOOLWINDOW;
    RECT frame_rect{0, 0, width, height};
    AdjustWindowRectEx(&frame_rect, frame_style, FALSE, frame_ex_style);
    const HWND hwnd = CreateWindowExW(
        frame_ex_style, kBrowserClass, L"音乐窗 · MusicFree", frame_style,
        bounds.left + frame_rect.left, bounds.top + frame_rect.top,
        frame_rect.right - frame_rect.left, frame_rect.bottom - frame_rect.top,
        owner, nullptr, GetModuleHandleW(nullptr), state.get());
    if (!hwnd) { g_browser.reset(); return; }

    // 诊断：客户区必须正好等于皮肤背景位图尺寸，否则皮肤会被整幅压扁。
    // 这是「音乐窗和导入窗外框是否真的像素一致」的可核对证据。
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        TraceDiagnostic("BROWSER_FRAME_CLIENT",
                        "client=" + std::to_string(client.right - client.left) + "x" +
                            std::to_string(client.bottom - client.top) +
                            " natural=" + std::to_string(natural.cx) + "x" +
                            std::to_string(natural.cy) + " identical=" +
                            ((client.right - client.left == natural.cx &&
                              client.bottom - client.top == natural.cy)
                                 ? "1"
                                 : "0"));
    }
    TraceResizeBands(g_browser.get());

    if (g_browser->top_most)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
}

} // namespace ttplayer::musicfree
