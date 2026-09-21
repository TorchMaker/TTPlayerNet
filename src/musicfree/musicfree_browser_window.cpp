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

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <commctrl.h>
#include <windowsx.h>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_import_dialog.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_frame.h"

namespace ttplayer::musicfree {

namespace {

constexpr int kIdPlatform = 3001;
constexpr int kIdQuery = 3002;
constexpr int kIdSearch = 3003;
constexpr int kIdImport = 3004;
constexpr int kIdList = 3005;
constexpr int kIdPlay = 3006;
constexpr int kIdClose = 3007;
constexpr int kIdStatus = 3008;
constexpr int kIdEnqueue = 3009;

constexpr UINT kMsgSearchDone = WM_APP + 0x510;
constexpr UINT kMsgResolveDone = WM_APP + 0x511;

// 皮肤包没有 <browser_window> 时使用的兜底窗口尺寸。
constexpr int kFallbackWidth = 720;
constexpr int kFallbackHeight = 462;

constexpr int kRowHeight = 22;
constexpr int kButtonHeight = 22;
constexpr int kColumnCount = 5;

// 内容区是皮肤里的白色面板（原版在此嵌了一个 IE 视图），所以控件走浅色配色，
// 与深色框体形成原版那样的对比。
constexpr COLORREF kPanel = RGB(255, 255, 255);
constexpr COLORREF kAccent = RGB(38, 134, 163);
constexpr COLORREF kAccentHot = RGB(56, 162, 194);
constexpr COLORREF kAccentDown = RGB(26, 104, 128);
constexpr COLORREF kOutline = RGB(198, 212, 220);
constexpr COLORREF kOutlineHot = RGB(142, 172, 188);
constexpr COLORREF kText = RGB(38, 54, 62);
constexpr COLORREF kSubtleText = RGB(118, 134, 142);
constexpr COLORREF kDisabledText = RGB(176, 188, 194);
constexpr COLORREF kRowAlternate = RGB(247, 251, 253);
constexpr COLORREF kRowHot = RGB(232, 244, 250);
constexpr COLORREF kRowSelected = RGB(206, 231, 244);
constexpr COLORREF kRowSeparator = RGB(238, 243, 246);
constexpr COLORREF kHeaderFill = RGB(238, 245, 248);
constexpr COLORREF kHeaderSeparator = RGB(214, 228, 236);

// 框体上可命中的皮肤按钮。
enum FrameHit {
    kFrameHitNone = 0,
    kFrameHitClose,
    kFrameHitPrevSource,
    kFrameHitNextSource,
    kFrameHitRefresh,
    kFrameHitTopMost,
};

// 框体自绘部分的位置（客户区坐标）。皮肤有效时取自 <browser_window>，
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
    MusicFreeSourceManager* mgr{};
    BrowserWindowHost host;
    // 返回 true 表示播放器已接受该请求（已入列/已开始播放）。
    std::function<bool(OnlineTrackAction, const OnlineSearchHit&,
                       const std::wstring&)> on_track;
    std::vector<OnlineSearchHit> hits;
    // 搜索/解析同一时刻只允许一个后台任务，避免插件沙箱被并发重入。
    std::atomic<bool> busy{false};
    std::wstring query_pending;  // 供后台线程读取的搜索条件（UI 线程写、任务启动前定格）
    FrameLayout layout;
    bool top_most{};
    bool dragging{};
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
    if (browser.transparent_color != CLR_INVALID) return browser.transparent_color;
    if (const skin::LegacySkin* legacy = ActiveLegacySkin(state))
        return legacy->TransparentColor();
    return RGB(255, 0, 255);
}

// ------------------------------------------------------------------ 字体

HFONT MakeFont(int height, int weight, const wchar_t* face) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

HFONT CreateUiFont(int height, int weight) {
    // 中文界面优先用雅黑；CreateFontW 在字体缺失时会自动替换，无需探测。
    HFONT font = MakeFont(height, weight, L"Microsoft YaHei UI");
    if (!font) font = MakeFont(height, weight, L"Tahoma");
    return font;
}

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

// ------------------------------------------------------------- 绘图工具

void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void StrokeSolidRect(HDC dc, const RECT& rect, COLORREF color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawRoundedBox(HDC dc, const RECT& rect, COLORREF fill, COLORREF border,
                    int radius) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const HBRUSH brush = CreateSolidBrush(fill);
    const bool outlined = border != CLR_INVALID;
    const HPEN pen = outlined ? CreatePen(PS_SOLID, 1, border)
                              : static_cast<HPEN>(GetStockObject(NULL_PEN));
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    if (outlined) DeleteObject(pen);
}

void DrawTextIn(HDC dc, const RECT& rect, const wchar_t* text, COLORREF color,
                HFONT font, UINT format) {
    if (!text || !*text) return;
    const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    const COLORREF old_color = SetTextColor(dc, color);
    const int old_mode = SetBkMode(dc, TRANSPARENT);
    RECT bounds = rect;
    DrawTextW(dc, text, -1, &bounds, format);
    SetBkMode(dc, old_mode);
    SetTextColor(dc, old_color);
    if (old_font) SelectObject(dc, old_font);
}

constexpr UINT kLabelFormat = DT_LEFT | DT_VCENTER | DT_SINGLELINE;

bool HitTest(const RECT& rect, POINT point) {
    return rect.right > rect.left && rect.bottom > rect.top &&
           PtInRect(&rect, point) != FALSE;
}

// ------------------------------------------------------------------ 布局

RECT InsetCopy(RECT rect, int dx, int dy) {
    rect.left += dx;
    rect.right -= dx;
    rect.top += dy;
    rect.bottom -= dy;
    return rect;
}

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

// 皮肤有效时窗口尺寸就是背景位图尺寸，框体元素直接使用皮肤坐标；否则按兜底尺寸
// 推算一条与皮肤风格相近的边框。
FrameLayout BuildFrameLayout(const RECT& client,
                             const skin::BrowserSkin* browser) {
    FrameLayout layout;
    if (browser) {
        layout.skinned = true;
        layout.content = browser->content_bounds;
        if (layout.content.right <= layout.content.left ||
            layout.content.bottom <= layout.content.top)
            layout.content = InsetCopy(client, 8, 31);
        layout.close = browser->close.bounds;
        layout.prev_source = browser->backward.bounds;
        layout.next_source = browser->forward.bounds;
        layout.refresh = browser->refresh.bounds;
        layout.check_box = browser->check.bounds;
        layout.check_label = layout.check_box;
        layout.check_label.left = layout.check_box.right + 5;
        layout.check_label.right = layout.check_label.left + 56;
        layout.link_text = browser->link.bounds;
        layout.query_field = BuildToolbarLayout(layout.content).query;
        return layout;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int pad = 10;
    layout.content = {pad, 30, width - pad, height - 30};
    layout.close = {width - pad - 24, 6, width - pad, 24};
    layout.prev_source = {pad, 7, pad + 22, 23};
    layout.next_source = {pad + 24, 7, pad + 46, 23};
    layout.refresh = {pad + 48, 7, pad + 70, 23};
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
    state->layout = BuildFrameLayout(client, ActiveSkin(state));

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
        SetWindowRgn(state->hwnd, nullptr, TRUE);
        return;
    }
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HRGN region = skin::CreateFrameRegion(browser->background, {}, width, height,
                                          false,
                                          FrameTransparent(state, *browser));
    RECT bounds{};
    if (!region || GetRgnBox(region, &bounds) == NULLREGION ||
        !SetWindowRgn(state->hwnd, region, TRUE)) {
        if (region) DeleteObject(region);
    }
}

// 运行期更换皮肤后重新贴合外框尺寸、区域与控件位置。
void ApplyFrameSize(BrowserState* state) {
    if (!state || !state->hwnd || !IsWindow(state->hwnd)) return;
    const skin::BrowserSkin* browser = ActiveSkin(state);
    int width = kFallbackWidth;
    int height = kFallbackHeight;
    if (browser && browser->background.size.cx > 0 &&
        browser->background.size.cy > 0) {
        width = browser->background.size.cx;
        height = browser->background.size.cy;
    }
    RECT current{};
    GetWindowRect(state->hwnd, &current);
    SetWindowPos(state->hwnd, nullptr, current.left, current.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyFrameRegion(state);
    LayoutChildren(state);
    InvalidateRect(state->hwnd, nullptr, TRUE);
}

// --------------------------------------------------------------- 框体绘制

// 皮肤按钮的当前状态帧。禁用时恒为最后一帧（皮肤的四态图里通常是灰态）。
int FrameButtonState(const BrowserState* state, int hit, bool enabled = true) {
    if (!enabled) return 3;
    if (state->frame_pressed == hit && state->frame_hover == hit) return 2;
    return state->frame_hover == hit ? 1 : 0;
}

void PaintFallbackFrame(BrowserState* state, HDC dc, const RECT& client) {
    const FrameLayout& layout = state->layout;
    FillSolidRect(dc, client, RGB(248, 251, 252));

    const RECT title{client.left + 1, client.top + 1, client.right - 1,
                     layout.content.top};
    FillSolidRect(dc, title, kAccent);
    DrawTextIn(dc, InsetCopy(title, 10, 0), L"音乐窗 · MusicFree",
               RGB(255, 255, 255), state->bold_font, kLabelFormat);

    const bool close_hot = state->frame_hover == kFrameHitClose;
    if (close_hot) FillSolidRect(dc, layout.close, kAccentDown);
    DrawTextIn(dc, layout.close, L"✕",
               close_hot ? RGB(255, 255, 255) : RGB(206, 232, 240),
               state->ui_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const struct { RECT rect; const wchar_t* glyph; int hit; } navigation[] = {
        {layout.prev_source, L"◀", kFrameHitPrevSource},
        {layout.next_source, L"▶", kFrameHitNextSource},
        {layout.refresh, L"⟳", kFrameHitRefresh},
    };
    for (const auto& item : navigation) {
        const bool hot = state->frame_hover == item.hit;
        if (hot) FillSolidRect(dc, item.rect, kAccentDown);
        DrawTextIn(dc, item.rect, item.glyph,
                   hot ? RGB(255, 255, 255) : RGB(206, 232, 240),
                   state->ui_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT panel = layout.content;
    panel.top -= 4;
    panel.left -= 2;
    panel.right += 2;
    StrokeSolidRect(dc, panel, kOutline);

    const RECT footer{client.left + 1, panel.bottom, client.right - 1,
                      client.bottom - 1};
    FillSolidRect(dc, footer, RGB(240, 246, 248));
    StrokeSolidRect(dc, client, kOutline);
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
    state->layout = BuildFrameLayout(client, browser);

    if (!browser) {
        PaintFallbackFrame(state, dc, client);
    } else {
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        skin::DrawFrameBackground(dc, browser->background, {}, width, height, false);
        const COLORREF transparent = FrameTransparent(state, *browser);
        const bool busy = state->busy.load();
        skin::DrawFrameButton(dc, browser->close, browser->close.bounds,
                              FrameButtonState(state, kFrameHitClose), transparent);
        skin::DrawFrameButton(dc, browser->backward, browser->backward.bounds,
                              FrameButtonState(state, kFrameHitPrevSource),
                              transparent);
        skin::DrawFrameButton(dc, browser->forward, browser->forward.bounds,
                              FrameButtonState(state, kFrameHitNextSource),
                              transparent);
        skin::DrawFrameButton(dc, browser->refresh, browser->refresh.bounds,
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
        default: return {nullptr, false, true};
    }
}

void DrawOwnerButton(BrowserState* state, const DRAWITEMSTRUCT& item) {
    const ButtonVisual visual = ButtonVisualFor(state, static_cast<int>(item.CtlID));
    if (!visual.text) return;
    RECT rect = item.rcItem;
    const bool hover = state->hot_button == static_cast<int>(item.CtlID);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool enabled = visual.enabled && (item.itemState & ODS_DISABLED) == 0;

    COLORREF fill{};
    COLORREF border = CLR_INVALID;
    COLORREF label{};
    if (!enabled) {
        fill = RGB(238, 243, 245);
        border = kOutline;
        label = kDisabledText;
    } else if (visual.primary) {
        fill = pressed ? kAccentDown : (hover ? kAccentHot : kAccent);
        label = RGB(255, 255, 255);
    } else {
        fill = pressed ? RGB(226, 238, 244) : (hover ? RGB(242, 249, 252) : kPanel);
        border = hover ? kOutlineHot : kOutline;
        label = hover ? kAccent : kText;
    }

    FillSolidRect(item.hDC, item.rcItem, kPanel);
    InflateRect(&rect, -1, -1);
    DrawRoundedBox(item.hDC, rect, fill, border, 6);
    DrawTextIn(item.hDC, rect, visual.text, label,
               visual.primary ? state->bold_font : state->ui_font,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
            state->link_text = L"◀ ▶ 切换音源　⟳ 重新搜索";

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
            TrackControl(state, state->query, kIdQuery, false);
            TrackControl(state, state->list, kIdList, true);

            RefreshPlatformCombo(state);
            ApplyFrameSize(state);
            SetStatus(state,
                L"输入关键词后回车或点「搜索」。音源需要先通过「导入…」添加。");
            SetFocus(state->query);
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
        case WM_NCHITTEST:
            return HTCLIENT;
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
                case kIdImport: {
                    // 音源集合在导入时会被改写，而搜索线程正在遍历插件表；
                    // 两者不能并发，否则可能出现悬垂指针。
                    if (state->busy.load()) {
                        SetStatus(state, L"正在搜索/解析，请稍候再导入音源");
                        return 0;
                    }
                    // 导入对话框自带模态消息循环；导入完成后音源集合可能变化，
                    // 因此回到这里要重建音源下拉框。
                    ShowMusicFreeImportDialog(hwnd, state->mgr);
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
    if (g_browser && g_browser->hwnd) ApplyFrameSize(g_browser.get());
}

void ShowMusicFreeBrowser(HWND owner, MusicFreeSourceManager* mgr,
                          BrowserWindowHost host,
                          std::function<bool(OnlineTrackAction,
                                             const OnlineSearchHit&,
                                             const std::wstring&)> on_track) {
    // 已存在则只做前置、刷新音源与宿主，保持单一音乐窗。
    if (g_browser && g_browser->hwnd && IsWindow(g_browser->hwnd)) {
        if (on_track) g_browser->on_track = std::move(on_track);
        g_browser->host = std::move(host);
        g_browser->mgr = mgr ? mgr : AppMusicFreeManager();
        // 宿主的置顶状态可能在上次关闭后被「选项/主窗口」改过，这里同步一次。
        if (g_browser->top_most != g_browser->host.top_most)
            SetTopMost(g_browser.get(), g_browser->host.top_most);
        RefreshPlatformCombo(g_browser.get());
        ApplyFrameSize(g_browser.get());
        ShowWindow(g_browser->hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_browser->hwnd);
        return;
    }

    if (!EnsureWindowClass()) return;

    auto state = std::make_shared<BrowserState>();
    state->owner = owner;
    state->mgr = mgr ? mgr : AppMusicFreeManager();
    state->host = std::move(host);
    state->on_track = std::move(on_track);
    state->top_most = state->host.top_most;
    g_browser = state;

    const skin::BrowserSkin* browser = ActiveSkin(state.get());
    int width = kFallbackWidth;
    int height = kFallbackHeight;
    if (browser && browser->background.size.cx > 0 &&
        browser->background.size.cy > 0) {
        width = browser->background.size.cx;
        height = browser->background.size.cy;
    }
    RECT bounds = InitialBrowserBounds(owner, width, height);
    if (state->host.saved_bounds) {
        const RECT saved = state->host.saved_bounds();
        if (saved.right > saved.left && saved.bottom > saved.top) {
            bounds.left = saved.left;
            bounds.top = saved.top;
            bounds.right = saved.left + width;
            bounds.bottom = saved.top + height;
        }
    }

    const HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, kBrowserClass, L"音乐窗 · MusicFree", WS_POPUP,
        bounds.left, bounds.top, width, height, owner, nullptr,
        GetModuleHandleW(nullptr), state.get());
    if (!hwnd) { g_browser.reset(); return; }

    if (g_browser->top_most)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
}

} // namespace ttplayer::musicfree
