// MusicFree 兼容层 —— 手动导入对话框实现（纯 Win32，自建窗口，无需 RC）
//
// 控件全部用 CreateWindow 创建，消息循环在 ShowMusicFreeImportDialog 内自包含，
// 因此接入方无需改动任何 .rc 资源，也无需 ATL/WTL 依赖。
//
// 外观与音乐窗完全一致：皮肤包有 <browser_window> 段时用**同一幅背景位图 + 同一套
// 坐标映射**（musicfree_frame.h）画外框，关闭按钮也用皮肤里的那一枚；没有该段时
// 才退回主题自绘的强调色标题栏（musicfree_theme.h）。按钮、列表行、输入框描边一直
// 走共用主题，两处不会各自漂移。
//
// 这里刻意不用系统灰底按钮和 MessageBox —— 那两者无法被主题着色，会在浅色窗体里
// 显得格格不入。导入结果写进窗口底部的只读结果框，而不是弹一个模态消息框。

#include "ttplayer/musicfree/musicfree_import_dialog.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <windowsx.h>

#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_frame.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/musicfree/musicfree_theme.h"

namespace ttplayer::musicfree {

namespace {

using namespace theme;

const int kIdUrlEdit = 1001;
const int kIdImportUrl = 1002;
const int kIdImportFile = 1003;
const int kIdList = 1004;
const int kIdRemove = 1005;
const int kIdClose = 1006;
const int kIdReport = 1007;

// 皮肤包没有 <browser_window> 段时的兜底版式（这个窗口本身不缩放，尺寸写死比
// 跑一遍 WM_SIZE 更省心）。有皮肤时窗口尺寸取背景位图尺寸，内容区取
// <browser position>。
constexpr int kWindowWidth = 468;
constexpr int kWindowHeight = 474;
constexpr int kTitleHeight = 30;
constexpr int kPad = 12;

struct DlgState;

// 子控件子类化上下文：只为按钮做悬停高亮，其余控件不需要。
struct ChildProbe {
    DlgState* state = nullptr;
    bool tracking_leave = false;
    WNDPROC base = nullptr;
};

struct DlgState {
    MusicFreeSourceManager* mgr = nullptr;
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    // 取当前皮肤（可为空）。音乐窗把自己的 host.skin 原样传进来，于是两个窗口
    // 永远用同一份皮肤对象。
    std::function<const skin::LegacySkin*()> skin;
    HWND url_edit = nullptr;
    HWND list = nullptr;
    HWND report = nullptr;
    HWND label_url = nullptr;
    HWND label_list = nullptr;
    HWND button_import_url = nullptr;
    HWND button_import_file = nullptr;
    HWND button_remove = nullptr;
    HFONT ui_font = nullptr;
    HFONT title_font = nullptr;
    HBRUSH panel_brush = nullptr;
    std::vector<std::unique_ptr<ChildProbe>> probes;
    int hot_button = 0;
    bool close_hot = false;
    bool close_pressed = false;
    bool dragging = false;
    POINT drag_origin{};
    RECT drag_window{};
    bool closed = false;
};

// 外框分区（客户区坐标）。皮肤有效时由 frame::Map 从 <browser_window> 的位图坐标
// 换算，否则按兜底版式推算。
struct Layout {
    bool skinned = false;
    RECT client{};
    RECT title{};
    RECT title_text{};
    RECT content{};
    RECT close{};
    frame::Metrics skin{};
};

const skin::BrowserSkin* ActiveSkin(const DlgState* s) {
    if (!s || !s->skin) return nullptr;
    const skin::LegacySkin* legacy = s->skin();
    if (!legacy || !legacy->Browser().valid) return nullptr;
    return &legacy->Browser();
}

Layout ComputeLayout(const DlgState* s, const RECT& client) {
    Layout layout;
    layout.client = client;
    if (const skin::BrowserSkin* browser = ActiveSkin(s)) {
        layout.skinned = true;
        layout.skin = frame::Map(client, *browser,
                                 ui::ResolveScaleForWindow(s->hwnd));
        layout.content = layout.skin.content;
        layout.close = layout.skin.close;
        layout.title = {client.left, client.top, client.right, layout.content.top};
        layout.title_text = {client.left + 14, client.top, layout.close.left - 8,
                             layout.content.top};
        return layout;
    }
    layout.title = {client.left, client.top, client.right, client.top + kTitleHeight};
    layout.close = {client.right - kPad - 24, client.top + 5,
                    client.right - kPad, client.top + 25};
    layout.title_text = {client.left + 14, client.top, layout.close.left - 10,
                         layout.title.bottom};
    layout.content = {client.left + kPad, layout.title.bottom + 8,
                      client.right - kPad, client.bottom - kPad};
    return layout;
}

// 当前布局（每次都按最新客户区算；Map 只是几个矩形运算，不值得缓存）。
Layout CurrentLayout(const DlgState* s) {
    RECT client{};
    if (s && s->hwnd) GetClientRect(s->hwnd, &client);
    return ComputeLayout(s, client);
}

void SetReport(DlgState* s, const std::wstring& text) {
    if (s->report) SetWindowTextW(s->report, text.c_str());
}

void RefreshList(DlgState* s) {
    if (!s->list || !s->mgr) return;
    SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
    auto plugins = s->mgr->ListPlugins();
    for (const auto& p : plugins) {
        std::wstring line = L"[插件] " + std::wstring(p.name.begin(), p.name.end());
        if (!p.version.empty())
            line += L"  v" + std::wstring(p.version.begin(), p.version.end());
        line += p.can_search ? L"  搜" : L"";
        line += p.can_media_source ? L"  播" : L"";
        line += p.can_lyric ? L"  词" : L"";
        SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
}

std::wstring ImportSummary(const MusicFreeSourceManager::ImportReport& report) {
    std::wstring info = L"音源：" +
        std::wstring(report.source_name.begin(), report.source_name.end()) +
        L"　成功挂载插件：" + std::to_wstring(report.plugins_ok) + L" / " +
        std::to_wstring(report.plugins_total);
    for (const auto& e : report.errors)
        info += L"\r\n- " + std::wstring(e.begin(), e.end());
    return info;
}

void DoImportUrl(DlgState* s) {
    wchar_t buf[4096] = {0};
    GetWindowTextW(s->url_edit, buf, 4096);
    std::wstring wurl(buf);
    if (wurl.empty()) {
        SetReport(s, L"请先粘贴音源 URL。");
        return;
    }
    std::string url(wurl.begin(), wurl.end());
    SetReport(s, L"正在导入…");
    auto report = s->mgr->ImportFromUrl(url);
    SetReport(s, ImportSummary(report));
    RefreshList(s);
}

void DoImportFile(DlgState* s) {
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"JSON/JS 文件\0*.json;*.js\0所有文件\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    // path 是 wchar_t 数组，需走项目的宽路径重载，不能用 std::string 构造。
    std::wstring wp(path);
    SetReport(s, L"正在导入…");
    auto report = s->mgr->ImportFromFile(std::filesystem::path(wp));
    SetReport(s, ImportSummary(report));
    RefreshList(s);
}

void DoRemove(DlgState* s) {
    int idx = (int)SendMessageW(s->list, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) {
        SetReport(s, L"请先在列表中选择要移除的音源。");
        return;
    }
    auto plugins = s->mgr->ListPlugins();
    if (idx < 0 || idx >= (int)plugins.size()) return;
    const std::wstring name(plugins[idx].name.begin(), plugins[idx].name.end());
    SetReport(s, s->mgr->RemoveSource(plugins[idx].name)
                     ? L"已移除音源：" + name
                     : L"移除失败：" + name);
    RefreshList(s);
}

// 所有子控件都排在**内容区**里：皮肤有效时内容区来自 <browser position>，
// 所以换皮肤（框体尺寸、边框厚度都变）时控件跟着走，不会压到边框上。
void LayoutChildren(DlgState* s) {
    if (!s->hwnd) return;
    const Layout layout = CurrentLayout(s);
    const RECT area = layout.content;
    const int left = area.left;
    const int right = area.right;
    const int width = std::max(120, right - left);

    // 行 1：URL 说明 + 输入框 + 「导入 URL」
    const int url_label_y = area.top;
    const int url_edit_y = url_label_y + 20;
    const int button_w = 92;
    const int url_button_x = right - button_w;
    if (s->label_url)
        MoveWindow(s->label_url, left, url_label_y, width, 16, TRUE);
    if (s->url_edit)
        MoveWindow(s->url_edit, left, url_edit_y,
                   std::max(60, url_button_x - left - 8), 24, TRUE);
    if (s->button_import_url)
        MoveWindow(s->button_import_url, url_button_x, url_edit_y - 1, button_w, 26,
                   TRUE);

    // 行 2：「选择本地文件…」「移除所选」
    const int tools_y = url_edit_y + 34;
    if (s->button_import_file)
        MoveWindow(s->button_import_file, left, tools_y, 140, 26, TRUE);
    if (s->button_remove)
        MoveWindow(s->button_remove, left + 148, tools_y, 110, 26, TRUE);

    // 行 3：列表（占满剩余高度）+ 底部结果框
    const int list_label_y = tools_y + 38;
    const int list_y = list_label_y + 20;
    const int report_height = 64;
    const int report_y = area.bottom - report_height;
    const int list_height = report_y - 10 - list_y;
    if (s->label_list)
        MoveWindow(s->label_list, left, list_label_y, width, 16, TRUE);
    if (s->list)
        MoveWindow(s->list, left, list_y, width,
                   list_height > 48 ? list_height : 48, TRUE);
    if (s->report)
        MoveWindow(s->report, left, report_y, width, report_height, TRUE);
}

// 关闭按钮的皮肤帧号：0 常态 / 1 热 / 2 按下（与皮肤四态图的顺序一致）。
int CloseButtonState(const DlgState* s) {
    return s->close_pressed ? 2 : (s->close_hot ? 1 : 0);
}

// ------------------------------------------------------- 子控件子类化（按钮悬停）

LRESULT CALLBACK ChildProc(HWND control, UINT message, WPARAM wparam,
                           LPARAM lparam) {
    auto* probe = reinterpret_cast<ChildProbe*>(
        GetWindowLongPtrW(control, GWLP_USERDATA));
    if (!probe || !probe->base)
        return DefWindowProcW(control, message, wparam, lparam);
    DlgState* state = probe->state;

    switch (message) {
        case WM_MOUSEMOVE:
            if (state) {
                const int id = GetDlgCtrlID(control);
                if (state->hot_button != id) {
                    state->hot_button = id;
                    InvalidateRect(control, nullptr, FALSE);
                }
            }
            if (!probe->tracking_leave) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
                if (TrackMouseEvent(&track)) probe->tracking_leave = true;
            }
            break;
        case WM_MOUSELEAVE:
            probe->tracking_leave = false;
            if (state && state->hot_button == GetDlgCtrlID(control)) {
                state->hot_button = 0;
                InvalidateRect(control, nullptr, FALSE);
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && IsWindowEnabled(control)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(control, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return CallWindowProcW(probe->base, control, message, wparam, lparam);
}

void TrackButton(DlgState* s, HWND control) {
    if (!control) return;
    auto probe = std::make_unique<ChildProbe>();
    probe->state = s;
    probe->base = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ChildProc)));
    SetWindowLongPtrW(control, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(probe.get()));
    s->probes.push_back(std::move(probe));
}

// 窗口过程必须返回 LRESULT（INT_PTR 只在 DialogProc 中合法）。
//
// 注意：本窗口是用 CreateWindowExW 建的**普通窗口**（不是 DialogBox 模板），
// 因此系统永远不会发送 WM_INITDIALOG。状态只能从 WM_NCCREATE 的
// CREATESTRUCT::lpCreateParams 取，并立刻写入 GWLP_USERDATA；否则后续所有
// WM_COMMAND 都会因为拿不到 DlgState 而被丢弃（按钮点了没反应）。
LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DlgState* s = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = static_cast<DlgState*>(create->lpCreateParams);
        if (s) {
            s->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    s = reinterpret_cast<DlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            s->ui_font = CreateUiFont(-12, FW_NORMAL);
            s->title_font = CreateUiFont(-14, FW_SEMIBOLD);
            s->panel_brush = CreateSolidBrush(kPanel);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            const Layout layout = ComputeLayout(s, client);
            const skin::BrowserSkin* browser = ActiveSkin(s);
            if (browser) {
                // 与音乐窗同一幅背景位图、同一套九宫格映射；关闭按钮也用皮肤
                // 里那一枚，所以两个窗口的框体是一模一样的像素。
                const skin::LegacySkin* legacy = s->skin ? s->skin() : nullptr;
                const COLORREF transparent = frame::TransparentColor(legacy, *browser);
                // 先把客户区铺成面板白：内置皮肤的内容区本身就是白色，铺一层
                // 既能盖住位图里的透明色键，也保证子控件的底色无缝。
                FillSolidRect(dc, client, kPanel);
                frame::PaintBackground(dc, *browser, layout.skin, client);
                skin::DrawFrameButton(dc, browser->close, layout.close,
                                      CloseButtonState(s), transparent);
            } else {
                FillSolidRect(dc, client, kPanel);
                FillSolidRect(dc, layout.title, kAccent);
                DrawTextIn(dc, layout.title_text, L"导入 MusicFree 音源",
                           kOnAccentText, s->title_font,
                           kLabelFormat | DT_END_ELLIPSIS);
                PaintButton(dc, layout.close, L"✕", s->ui_font,
                            TitleButtonSkin(CloseButtonState(s)));
                // 客户区外框：把强调色标题栏与白底内容区圈在一起。
                StrokeSolidRect(dc, client, kOutline);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // 全部在 WM_PAINT 里画，避免先刷一遍系统色再覆盖。
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kText);
            SetBkColor(dc, kPanel);
            return reinterpret_cast<LRESULT>(s->panel_brush);
        }
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_LISTBOX) {
                item->itemHeight = 22;
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                const bool primary = item->CtlID == kIdImportUrl;
                const bool disabled = (item->itemState & ODS_DISABLED) != 0;
                int state = kButtonNormal;
                if (disabled) state = kButtonDisabled;
                else if (item->itemState & ODS_SELECTED) state = kButtonPressed;
                else if (s->hot_button == static_cast<int>(item->CtlID))
                    state = kButtonHot;
                wchar_t text[128] = {0};
                GetWindowTextW(item->hwndItem, text, 128);
                PaintButton(item->hDC, item->rcItem, text, s->ui_font,
                            PanelButtonSkin(primary, state));
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX) {
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                const COLORREF fill = selected ? kRowSelected : kPanel;
                FillSolidRect(item->hDC, item->rcItem, fill);
                wchar_t text[512] = {0};
                if (SendMessageW(item->hwndItem, LB_GETTEXT,
                                 static_cast<WPARAM>(item->itemID),
                                 reinterpret_cast<LPARAM>(text)) != LB_ERR) {
                    RECT row = item->rcItem;
                    row.left += 8;
                    DrawTextIn(item->hDC, row, text,
                               selected ? kAccent : kText, s->ui_font,
                               kLabelFormat | DT_END_ELLIPSIS);
                }
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kIdImportUrl) DoImportUrl(s);
            else if (id == kIdImportFile) DoImportFile(s);
            else if (id == kIdRemove) DoRemove(s);
            else if (id == kIdClose) { s->closed = true; DestroyWindow(hwnd); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Layout layout = CurrentLayout(s);
            if (HitRect(layout.close, point)) {
                s->close_pressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &layout.close, FALSE);
            } else if (!HitRect(layout.content, point)) {
                // 内容区之外的整圈（标题栏 / 边框 / 页脚）都可以拖动窗口，
                // 皮肤形态下正好对应原版那种「抓着框体走」的手感。
                s->dragging = true;
                s->drag_origin = point;
                ClientToScreen(hwnd, &s->drag_origin);
                GetWindowRect(hwnd, &s->drag_window);
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Layout layout = CurrentLayout(s);
            const bool hot = HitRect(layout.close, point);
            if (hot != s->close_hot) {
                s->close_hot = hot;
                InvalidateRect(hwnd, &layout.close, FALSE);
            }
            if (s->dragging) {
                POINT now = point;
                ClientToScreen(hwnd, &now);
                SetWindowPos(hwnd, nullptr,
                             s->drag_window.left + (now.x - s->drag_origin.x),
                             s->drag_window.top + (now.y - s->drag_origin.y),
                             0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Layout layout = CurrentLayout(s);
            if (s->close_pressed) {
                s->close_pressed = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, &layout.close, FALSE);
                if (HitRect(layout.close, point)) { s->closed = true; DestroyWindow(hwnd); }
            }
            if (s->dragging) {
                s->dragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            s->dragging = false;
            s->close_pressed = false;
            return 0;
        case WM_SETCURSOR:
            // 关闭按钮用普通箭头，其余非内容区用移动手型，暗示可以拖框体。
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                const Layout layout = CurrentLayout(s);
                if (!HitRect(layout.content, point) && !HitRect(layout.close, point)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
            }
            break;
        case WM_CLOSE:
            s->closed = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (s->owner) EnableWindow(s->owner, TRUE);
            if (s->ui_font) { DeleteObject(s->ui_font); s->ui_font = nullptr; }
            if (s->title_font) { DeleteObject(s->title_font); s->title_font = nullptr; }
            if (s->panel_brush) { DeleteObject(s->panel_brush); s->panel_brush = nullptr; }
            // 不要 PostQuitMessage：本对话框是播放器主消息循环里的嵌套模态循环，
            // 投递 WM_QUIT 会连带把整个应用的消息循环一起结束。
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowMusicFreeImportDialog(HWND owner, MusicFreeSourceManager* mgr,
                               std::function<const skin::LegacySkin*()> skin) {
    if (!mgr) return;

    DlgState state;
    state.mgr = mgr;
    state.owner = owner;
    state.skin = std::move(skin);

    // 皮肤包有 <browser_window> 段时，窗口尺寸就取背景位图尺寸 —— 和音乐窗同一个
    // 框体、同一尺寸，两个窗口叠在一起边框厚度完全一致。没有该段才用兜底版式。
    const skin::LegacySkin* legacy = state.skin ? state.skin() : nullptr;
    const skin::BrowserSkin* browser =
        legacy && legacy->Browser().valid ? &legacy->Browser() : nullptr;
    int width = kWindowWidth;
    int height = kWindowHeight;
    if (browser && browser->background.size.cx > 0 &&
        browser->background.size.cy > 0) {
        width = browser->background.size.cx;
        height = browser->background.size.cy;
    }

    // 窗口类名（一次性）
    static const wchar_t* kCls = L"TTPlayerMusicFreeImport";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kCls;
        // 外框全部自绘，不能挂系统背景刷，否则 WM_ERASEBKGND 之外还会先刷一遍灰底。
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            registered = true;
        else
            return;
    }

    if (owner) EnableWindow(owner, FALSE);

    // ★ 外框样式必须和音乐窗**逐字一致**（音乐窗用的是 WS_POPUP + WS_EX_TOOLWINDOW）。
    // 这里原来挂的是 WS_EX_DLGMODALFRAME —— 它会给 WS_POPUP 窗口加上 3px 的非客户区
    // 边框（SM_CXDLGFRAME=3）。于是外框传 474x486、客户区却只有 468x480：
    //   · 皮肤位图被塞进更小的客户区 ⇒ 命中 DrawFrameBackground 里
    //     `width < bitmap.size.cx` 的退化分支，整幅被压扁；外面还多一圈系统边框。
    //     两者叠加就是用户看到的「导入窗背景有点偏」。
    // 改成和音乐窗同款样式后客户区 delta 归零。另外仍用 AdjustWindowRectEx 反算外框
    // 尺寸：这样即便以后有人再加回带边框的样式，客户区也依旧正好等于皮肤位图尺寸，
    // 不会静默退化成整幅缩放（这是最难查的一类回归）。
    const DWORD frame_style = WS_POPUP | WS_CLIPCHILDREN;
    const DWORD frame_ex_style = WS_EX_TOOLWINDOW;
    RECT frame_rect{0, 0, width, height};
    AdjustWindowRectEx(&frame_rect, frame_style, FALSE, frame_ex_style);
    const int outer_width = frame_rect.right - frame_rect.left;
    const int outer_height = frame_rect.bottom - frame_rect.top;

    // 位置：**不能**给 WS_POPUP 窗口传 CW_USEDEFAULT —— 它只在 overlapped 窗口里
    // 有定义，对 popup 等于塞了一个 0x80000000 的坐标，窗口会被系统夹到屏幕角上。
    // 显式居中到 owner（没有 owner 就用工作区），再夹进工作区免得压住任务栏。
    RECT anchor{};
    if (!owner || !GetWindowRect(owner, &anchor) ||
        anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0))
            anchor = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                          GetSystemMetrics(SM_CYSCREEN)};
    }
    int x = anchor.left + ((anchor.right - anchor.left) - outer_width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - outer_height) / 2;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) &&
        work.right > work.left && work.bottom > work.top) {
        x = std::clamp<int>(x, work.left,
                            std::max<int>(work.left, work.right - outer_width));
        y = std::clamp<int>(y, work.top,
                            std::max<int>(work.top, work.bottom - outer_height));
    }

    HWND hwnd = CreateWindowExW(
        frame_ex_style, kCls, L"导入 MusicFree 音源", frame_style,
        x, y, outer_width, outer_height,
        owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!hwnd) { if (owner) EnableWindow(owner, TRUE); return; }

    // 诊断：导入窗的**客户区**必须正好等于皮肤背景位图尺寸，和音乐窗逐像素对齐。
    // 客户区偏小 ⇒ DrawFrameBackground 走「整幅缩放」退化分支 ⇒ 就是「背景偏/被压扁」。
    if (browser) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const LONG cw = client.right - client.left;
        const LONG ch = client.bottom - client.top;
        TraceDiagnostic("IMPORT_FRAME",
                        "client=" + std::to_string(cw) + "x" +
                            std::to_string(ch) + " bitmap=" +
                            std::to_string(width) + "x" + std::to_string(height) +
                            " identical=" +
                            ((cw == width && ch == height) ? "1" : "0"));
    }

    // 皮肤外框的圆角/斜边靠窗口区域裁出来，和音乐窗走同一份九宫格映射。
    if (browser) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const Layout layout = ComputeLayout(&state, client);
        frame::ApplyWindowRegion(hwnd, *browser, layout.skin, client,
                                 frame::TransparentColor(legacy, *browser),
                                 ui::ResolveScaleForWindow(hwnd));
    }

    HFONT ui_font = state.ui_font;

    // 控件：坐标随后由 LayoutChildren 统一给出，这里先给个占位尺寸。
    state.label_url = CreateWindowW(L"STATIC",
                  L"音源 URL（单个插件 JS，或注册表 JSON 如 yuanli.json）：",
                  WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr,
                  nullptr, nullptr);
    state.url_edit = CreateWindowW(L"EDIT", L"",
                  WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                  0, 0, 10, 10, hwnd, (HMENU)kIdUrlEdit, nullptr, nullptr);
    state.button_import_url = CreateWindowW(L"BUTTON", L"导入 URL",
                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  0, 0, 10, 10, hwnd, (HMENU)kIdImportUrl, nullptr, nullptr);
    state.button_import_file = CreateWindowW(L"BUTTON", L"选择本地文件…",
                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  0, 0, 10, 10, hwnd, (HMENU)kIdImportFile, nullptr, nullptr);
    state.button_remove = CreateWindowW(L"BUTTON", L"移除所选",
                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  0, 0, 10, 10, hwnd, (HMENU)kIdRemove, nullptr, nullptr);
    state.label_list = CreateWindowW(L"STATIC", L"已导入音源 / 插件：",
                  WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr,
                  nullptr, nullptr);
    state.list = CreateWindowW(L"LISTBOX", L"",
                  WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY |
                      LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                  0, 0, 10, 10, hwnd, (HMENU)kIdList, nullptr, nullptr);
    state.report = CreateWindowW(L"EDIT", L"",
                  WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
                      ES_READONLY | ES_AUTOVSCROLL,
                  0, 0, 10, 10, hwnd, (HMENU)kIdReport, nullptr, nullptr);

    // 统一字体（子控件不继承父窗口字体，必须显式设一遍）。两个 STATIC 标签以前
    // 漏在循环外，而且从没被 LayoutChildren 摆过位置 —— 它们其实一直是不可见的。
    for (HWND control : {state.label_url, state.label_list, state.url_edit,
                         state.button_import_url, state.button_import_file,
                         state.button_remove, state.list, state.report}) {
        if (control) SendMessageW(control, WM_SETFONT,
                                  reinterpret_cast<WPARAM>(ui_font), TRUE);
    }
    TrackButton(&state, state.button_import_url);
    TrackButton(&state, state.button_import_file);
    TrackButton(&state, state.button_remove);

    LayoutChildren(&state);

    // 控件建好之后才刷新（WM_INITDIALOG 不会到达这里的普通窗口）。
    RefreshList(&state);
    SetReport(&state, L"粘贴音源 URL 或选择本地文件即可导入；已导入的插件会持久化，"
                      L"下次启动自动加载。");

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    // 模态消息循环
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (state.closed || !IsWindow(hwnd)) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    // 若循环因 DestroyWindow 退出，owner 已在 WM_DESTROY 中恢复
}

} // namespace ttplayer::musicfree
