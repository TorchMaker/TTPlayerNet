// 视频源兼容层 —— 视频源管理对话框实现（纯 Win32，自建窗口，无需 RC）
//
// 版式、配色、子控件子类化（按钮悬停）、皮肤外框映射全部与
// src/musicfree/musicfree_import_dialog.cpp 一致 —— 两个窗口在真机上叠在一起时
// 边框厚度、关闭按钮、背景位图必须逐像素相同，否则切换页签会「跳框」。
//
// 与音乐侧唯一的差别是内容：音乐源的列表项是一个插件，视频源的列表项是**一份
// 导入过的配置**（影视仓 / TVBox JSON），里面可能含十几个站点；因此「查看详情」
// 展示的是这份配置的来源地址 + 站点清单（kind / api / 不支持的原因）。

#include "ttplayer/video/video_source_dialog.h"

#include <commdlg.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <windowsx.h>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_frame.h"
#include "ttplayer/musicfree/musicfree_theme.h"
#include "ttplayer/skin/skin.h"
#include "ttplayer/ui/dpi_scale.h"
#include "ttplayer/video/video_source_manager.h"

namespace ttplayer::video {

namespace {

namespace theme = ttplayer::musicfree::theme;
namespace frame = ttplayer::musicfree::frame;

const int kIdUrlEdit = 1101;
const int kIdImportUrl = 1102;
const int kIdImportFile = 1103;
const int kIdList = 1104;
const int kIdRemove = 1105;
const int kIdClose = 1106;
const int kIdReport = 1107;
const int kIdView = 1108;
// 列表右键菜单命令（TPM_RETURNCMD 直接取回，不必绕 WM_COMMAND）。
const int kCtxView = 2101;
const int kCtxDelete = 2102;

// 皮肤包没有 <browser_window> 段时的兜底版式。视频源详情比音源详情长，高度比
// 音乐导入窗多 46px。
constexpr int kWindowWidth = 468;
constexpr int kWindowHeight = 520;
constexpr int kTitleHeight = 30;
constexpr int kPad = 12;
// 详情框高度：视频源的详情是「来源 + 每个站点的 kind/接口」，比音乐侧的插件
// 简介长得多，留 140px（约 7 行）才能一屏看完常见配置的几个站点。
constexpr int kReportHeight = 140;

struct DlgState;

struct ChildProbe {
    DlgState* state = nullptr;
    bool tracking_leave = false;
    WNDPROC base = nullptr;
};

struct DlgState {
    VideoSourceManager* mgr = nullptr;
    // 列表内容快照：RefreshList 时重取一次，行下标 = 这里的下标。
    std::vector<ImportedVideoSource> sources;
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    std::function<const skin::LegacySkin*()> skin;
    HWND url_edit = nullptr;
    HWND list = nullptr;
    HWND report = nullptr;
    HWND label_url = nullptr;
    HWND label_list = nullptr;
    HWND button_import_url = nullptr;
    HWND button_import_file = nullptr;
    HWND button_view = nullptr;
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
    // 有任何导入 / 删除成功就置位，退出对话框时回传给调用方（首页要重新聚合）。
    bool changed = false;
};

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
                                 ttplayer::ui::ResolveScaleForWindow(s->hwnd));
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

Layout CurrentLayout(const DlgState* s) {
    RECT client{};
    if (s && s->hwnd) GetClientRect(s->hwnd, &client);
    return ComputeLayout(s, client);
}

// ---------------------------------------------------------------- 文本组装

std::wstring KindLabel(VideoSourceKind kind) {
    switch (kind) {
        case VideoSourceKind::kCms: return L"CMS";
        case VideoSourceKind::kLive: return L"直播";
        case VideoSourceKind::kSpider: return L"SPIDER";
        case VideoSourceKind::kNativeSpider: return L"原生";
        default: return L"不支持";
    }
}

// 真能取内容的站点数（CMS + 直播）。SPIDER 不算：它们的 api 是 TVBox 里的
// Java 类名，没有对应运行库就永远拉不到数据。
int SupportedCount(const ImportedVideoSource& source) {
    int ok = 0;
    for (const auto& site : source.sites)
        if (site.CanFetch()) ++ok;
    return ok;
}

int SpiderCount(const ImportedVideoSource& source) {
    int n = 0;
    for (const auto& site : source.sites)
        if (site.kind == VideoSourceKind::kSpider) ++n;
    return n;
}

// 列表一行：名称 + 站点统计 + 直播 + 版本。URL 不放在行里 —— 一行放不下，
// 详情里能看到完整地址。
std::wstring RowText(const ImportedVideoSource& source) {
    std::wstring line = source.name.empty() ? L"（未命名来源）"
                                            : core::Utf8ToWide(source.name);
    line += L"    可播 " + std::to_wstring(SupportedCount(source)) + L"/" +
            std::to_wstring(source.sites.size());
    if (SpiderCount(source) > 0)
        line += L"（SPIDER " + std::to_wstring(SpiderCount(source)) + L"）";
    if (!source.lives.empty())
        line += L"    直播 " + std::to_wstring(source.lives.size());
    if (!source.version.empty())
        line += L"    v" + core::Utf8ToWide(source.version);
    return line;
}

// 「查看详情」：来源信息 + 站点清单。站点多时截断，避免结果框被几十行灌满
// （结果框可滚动，但用户第一眼要看的是来源本身）。
std::wstring DetailsOf(const ImportedVideoSource& source) {
    std::wstring info = L"来源：" + (source.name.empty()
                                        ? std::wstring(L"（未命名来源）")
                                        : core::Utf8ToWide(source.name));
    if (!source.source_url.empty())
        info += L"\r\n地址：" + core::Utf8ToWide(source.source_url);
    else if (!source.local_path.empty())
        info += L"\r\n地址：本地文件 " + source.local_path.wstring();
    if (!source.version.empty())
        info += L"\r\n版本：" + core::Utf8ToWide(source.version);
    const int ok = SupportedCount(source);
    info += L"\r\n站点：" + std::to_wstring(ok) + L" / " +
            std::to_wstring(source.sites.size()) + L" 可播放";
    if (!source.lives.empty())
        info += L"，直播源 " + std::to_wstring(source.lives.size()) + L" 个";
    info += L"\r\n";

    constexpr std::size_t kMaxSites = 40;
    std::size_t shown = 0;
    for (const auto& site : source.sites) {
        if (shown++ >= kMaxSites) break;
        info += L"\r\n[" + KindLabel(site.kind) + L"] ";
        info += site.name.empty() ? std::wstring(L"（未命名站点）")
                                  : core::Utf8ToWide(site.name);
        if (site.CanFetch() && !site.api.empty())
            info += L"\r\n    接口：" + core::Utf8ToWide(site.NormalizedApi());
        else if (!site.why_unsupported.empty())
            info += L"\r\n    原因：" + core::Utf8ToWide(site.why_unsupported);
        else if (!site.api.empty())
            info += L"\r\n    api：" + core::Utf8ToWide(site.api);
    }
    if (source.sites.size() > kMaxSites)
        info += L"\r\n… 另有 " +
                std::to_wstring(source.sites.size() - kMaxSites) + L" 个站点未显示";
    if (source.sites.empty())
        info += L"\r\n该配置里没有站点。";
    return info;
}

std::wstring ImportSummary(const VideoSourceManager::ImportReport& report) {
    std::wstring info = L"来源：" + (report.source_name.empty()
                                        ? std::wstring(L"（未命名来源）")
                                        : core::Utf8ToWide(report.source_name));
    if (report.duplicated)
        info += L"\r\n结果：该配置已存在，已用新内容覆盖";
    else if (report.sources_ok > 0)
        info += L"\r\n结果：导入成功";
    else
        info += L"\r\n结果：导入失败";
    // 「可播放」= CMS 站点 + 直播站点。SPIDER 站点单独点出来：它们不是配置写错，
    // 而是需要 TVBox 那套 JS/Java 运行库，本程序执行不了 —— 不写清原因，用户只会
    // 觉得「导入成功却什么都没有」。
    info += L"\r\n站点：" + std::to_wstring(report.sites_supported) + L" / " +
            std::to_wstring(report.sites_total) + L" 可播放";
    if (report.sites_cms > 0)
        info += L"（CMS " + std::to_wstring(report.sites_cms) + L" 个）";
    if (report.sites_native > 0)
        info += L"，原生 spider " + std::to_wstring(report.sites_native) + L" 个";
    if (report.sites_spider > 0)
        info += L"，SPIDER " + std::to_wstring(report.sites_spider) +
                L" 个（需 TVBox 运行库，本程序暂不支持）";
    if (report.lives_total > 0)
        info += L"；直播源 " + std::to_wstring(report.lives_total) + L" 个";
    if (!report.message.empty())
        info += L"\r\n" + core::Utf8ToWide(report.message);
    for (const auto& e : report.errors)
        info += L"\r\n- " + core::Utf8ToWide(e);
    return info;
}

// ---------------------------------------------------------------- 列表 / 报告

void SetReport(DlgState* s, const std::wstring& text) {
    if (!s->report) return;
    SetWindowTextW(s->report, text.c_str());
    // 长长的站点清单更新后滚回顶部，否则会停在上一份内容的滚动位置。
    SendMessageW(s->report, EM_SETSEL, 0, 0);
    SendMessageW(s->report, EM_SCROLLCARET, 0, 0);
}

void RefreshList(DlgState* s) {
    if (!s->list) return;
    SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
    s->sources.clear();
    if (s->mgr) s->sources = s->mgr->ListSources();
    for (const auto& source : s->sources) {
        const std::wstring line = RowText(source);
        SendMessageW(s->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
}

int SelectedRow(DlgState* s) {
    return static_cast<int>(SendMessageW(s->list, LB_GETCURSEL, 0, 0));
}

std::vector<int> SelectedRows(DlgState* s) {
    std::vector<int> rows;
    const int count = static_cast<int>(SendMessageW(s->list, LB_GETSELCOUNT, 0, 0));
    if (count > 0) {
        rows.resize(static_cast<std::size_t>(count));
        const int got = static_cast<int>(SendMessageW(
            s->list, LB_GETSELITEMS, static_cast<WPARAM>(count),
            reinterpret_cast<LPARAM>(rows.data())));
        rows.resize(static_cast<std::size_t>(std::max(0, got)));
        return rows;
    }
    const int cur = SelectedRow(s);
    if (cur >= 0) rows.push_back(cur);
    return rows;
}

void DoView(DlgState* s, int idx) {
    if (idx < 0) {
        SetReport(s, L"请先在列表中选择一个视频源。");
        return;
    }
    if (idx >= static_cast<int>(s->sources.size())) {
        SetReport(s, L"该视频源已不存在，请重新打开本窗口。");
        return;
    }
    SetReport(s, DetailsOf(s->sources[static_cast<std::size_t>(idx)]));
}

void DoRemoveRows(DlgState* s, const std::vector<int>& rows) {
    if (rows.empty()) {
        SetReport(s, L"请先在列表中选择要删除的视频源（可按住 Ctrl / Shift 复选）。");
        return;
    }
    if (!s->mgr) return;
    std::vector<std::string> names;
    for (const int idx : rows) {
        if (idx < 0 || idx >= static_cast<int>(s->sources.size())) continue;
        const std::string& name = s->sources[static_cast<std::size_t>(idx)].name;
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }
    std::size_t removed = 0;
    for (const auto& name : names)
        if (s->mgr->RemoveSource(name)) ++removed;

    if (removed > 0) s->changed = true;
    SetReport(s, removed > 0
        ? L"已删除 " + std::to_wstring(removed) + L" 个视频源（选中 " +
              std::to_wstring(rows.size()) + L" 项）。\r\n"
              L"索引文件与原始配置缓存已同步清理；回到视频页会自动重新聚合首页。"
        : L"删除失败：所选视频源已不存在，请重新打开本窗口。");
    RefreshList(s);
}

// 导入完成后的统一收尾：写结果、刷新列表、选中并展示刚导入的那一条。
void AfterImport(DlgState* s, const VideoSourceManager::ImportReport& report) {
    SetReport(s, ImportSummary(report));
    RefreshList(s);
    // 有源入库就算「变了」：哪怕全是 SPIDER 站点、首页取不到内容，也要让视频页
    // 重新聚合一次（直播源可能新增，空态提示也要跟着更新）。
    if (report.sources_ok > 0) s->changed = true;
    // 选中刚导入/命中的那一行，用户能立刻看到它出现在列表里。
    const std::wstring target = core::Utf8ToWide(report.source_name);
    for (int i = 0; i < static_cast<int>(s->sources.size()); ++i) {
        if (core::Utf8ToWide(s->sources[static_cast<std::size_t>(i)].name) == target) {
            SendMessageW(s->list, LB_SETSEL, FALSE, -1);
            SendMessageW(s->list, LB_SETSEL, TRUE, static_cast<LPARAM>(i));
            SendMessageW(s->list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
            break;
        }
    }
}

void DoImportUrl(DlgState* s) {
    wchar_t buf[4096] = {0};
    GetWindowTextW(s->url_edit, buf, 4096);
    const std::wstring wurl(buf);
    const std::size_t first = wurl.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        SetReport(s, L"请先粘贴影视仓 / TVBox 配置地址（或每行一条 CMS 接口），或点「选择本地文件…」。");
        return;
    }
    if (!s->mgr) {
        SetReport(s, L"视频源管理器未初始化。");
        return;
    }
    SetReport(s, L"正在下载并解析视频源配置…（网络较慢时请稍候）");
    UpdateWindow(s->hwnd);  // 同步下载前先把提示画出来，否则窗口会僵在那
    const std::string url = core::WideToUtf8(wurl.substr(first));
    AfterImport(s, s->mgr->ImportFromUrl(url));
}

void DoImportFile(DlgState* s) {
    wchar_t path[4096] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(std::size(path));
    ofn.lpstrFilter =
        L"影视仓 / TVBox 配置 (*.json;*.txt)\0*.json;*.txt\0所有文件\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        SetReport(s, L"未选择配置文件，导入已取消。");
        return;
    }
    if (!s->mgr) {
        SetReport(s, L"视频源管理器未初始化。");
        return;
    }
    SetReport(s, L"正在读取并解析视频源配置…");
    UpdateWindow(s->hwnd);
    AfterImport(s, s->mgr->ImportFromFile(std::filesystem::path(std::wstring(path))));
}

// ---------------------------------------------------------------- 布局

void LayoutChildren(DlgState* s) {
    if (!s->hwnd) return;
    const Layout layout = CurrentLayout(s);
    const RECT area = layout.content;
    const int left = area.left;
    const int right = area.right;
    const int width = std::max(120, right - left);

    // 行 1：说明 + URL 输入框 + 「导入 URL」
    const int url_label_y = area.top;
    const int url_edit_y = url_label_y + 20;
    const int button_w = 92;
    const int url_button_x = right - button_w;
    if (s->label_url)
        MoveWindow(s->label_url, left, url_label_y, width, 16, TRUE);
    if (s->url_edit)
        MoveWindow(s->url_edit, left, url_edit_y, std::max(60, url_button_x - left - 8),
                   24, TRUE);
    if (s->button_import_url)
        MoveWindow(s->button_import_url, url_button_x, url_edit_y - 1, button_w, 26, TRUE);

    // 行 2：本地文件 / 查看详情 / 删除所选
    const int tools_y = url_edit_y + 34;
    if (s->button_import_file)
        MoveWindow(s->button_import_file, left, tools_y, 132, 26, TRUE);
    if (s->button_view)
        MoveWindow(s->button_view, left + 140, tools_y, 88, 26, TRUE);
    if (s->button_remove)
        MoveWindow(s->button_remove, left + 236, tools_y, 100, 26, TRUE);

    // 行 3：列表 + 底部结果框
    const int list_label_y = tools_y + 38;
    const int list_y = list_label_y + 20;
    const int report_y = area.bottom - kReportHeight;
    const int list_height = report_y - 10 - list_y;
    if (s->label_list)
        MoveWindow(s->label_list, left, list_label_y, width, 16, TRUE);
    if (s->list)
        MoveWindow(s->list, left, list_y, width,
                   list_height > 48 ? list_height : 48, TRUE);
    if (s->report)
        MoveWindow(s->report, left, report_y, width, kReportHeight, TRUE);
}

// ---------------------------------------------------------------- 子控件子类化

LRESULT CALLBACK ChildProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam) {
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

int CloseButtonState(const DlgState* s) {
    return s->close_pressed ? 2 : (s->close_hot ? 1 : 0);
}

// ---------------------------------------------------------------- 窗口过程

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
        case WM_CREATE:
            s->ui_font = theme::CreateUiFont(-12, FW_NORMAL);
            s->title_font = theme::CreateUiFont(-14, FW_SEMIBOLD);
            s->panel_brush = CreateSolidBrush(theme::kPanel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            const Layout layout = ComputeLayout(s, client);
            const skin::BrowserSkin* browser = ActiveSkin(s);
            if (browser) {
                const skin::LegacySkin* legacy = s->skin ? s->skin() : nullptr;
                const COLORREF transparent = frame::TransparentColor(legacy, *browser);
                theme::FillSolidRect(dc, client, theme::kPanel);
                frame::PaintBackground(dc, *browser, layout.skin, client);
                skin::DrawFrameButton(dc, browser->close, layout.close,
                                      CloseButtonState(s), transparent);
            } else {
                theme::FillSolidRect(dc, client, theme::kPanel);
                theme::FillSolidRect(dc, layout.title, theme::kAccent);
                theme::DrawTextIn(dc, layout.title_text, L"视频源",
                                  theme::kOnAccentText, s->title_font,
                                  theme::kLabelFormat | DT_END_ELLIPSIS);
                theme::PaintButton(dc, layout.close, L"✕", s->ui_font,
                                   theme::TitleButtonSkin(CloseButtonState(s)));
                theme::StrokeSolidRect(dc, client, theme::kOutline);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, theme::kText);
            SetBkColor(dc, theme::kPanel);
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
                int state = theme::kButtonNormal;
                if (disabled) state = theme::kButtonDisabled;
                else if (item->itemState & ODS_SELECTED) state = theme::kButtonPressed;
                else if (s->hot_button == static_cast<int>(item->CtlID))
                    state = theme::kButtonHot;
                wchar_t text[128] = {0};
                GetWindowTextW(item->hwndItem, text, 128);
                theme::PaintButton(item->hDC, item->rcItem, text, s->ui_font,
                                   theme::PanelButtonSkin(primary, state));
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX) {
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                theme::FillSolidRect(item->hDC, item->rcItem,
                                     selected ? theme::kRowSelected : theme::kPanel);
                wchar_t text[512] = {0};
                if (SendMessageW(item->hwndItem, LB_GETTEXT,
                                 static_cast<WPARAM>(item->itemID),
                                 reinterpret_cast<LPARAM>(text)) != LB_ERR) {
                    RECT row = item->rcItem;
                    row.left += 8;
                    theme::DrawTextIn(item->hDC, row, text,
                                      selected ? theme::kAccent : theme::kText,
                                      s->ui_font,
                                      theme::kLabelFormat | DT_END_ELLIPSIS);
                }
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == kIdImportUrl) DoImportUrl(s);
            else if (id == kIdImportFile) DoImportFile(s);
            else if (id == kIdView) DoView(s, SelectedRow(s));
            else if (id == kIdRemove) DoRemoveRows(s, SelectedRows(s));
            else if (id == kIdList &&
                     (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
                // 单击就把详情写进结果框：用户问的「查看已经导入的信息」通常就是
                // 「我导进去了哪些站点」，不该再要求先点「查看详情」。
                DoView(s, SelectedRow(s));
            }
            else if (id == kIdClose) { s->closed = true; DestroyWindow(hwnd); }
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (reinterpret_cast<HWND>(wParam) != s->list) break;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = -1;
            if (point.x == -1 && point.y == -1) {
                GetCursorPos(&point);
                idx = SelectedRow(s);
                if (idx < 0) {
                    SetReport(s, L"请先用左键选中一个视频源，再按菜单键。");
                    return 0;
                }
            } else {
                POINT client = point;
                ScreenToClient(s->list, &client);
                const DWORD hit = static_cast<DWORD>(SendMessageW(
                    s->list, LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y)));
                if (HIWORD(hit) == 0)
                    idx = static_cast<int>(static_cast<short>(LOWORD(hit)));
            }
            if (idx < 0) return 0;
            const bool hit_selected =
                SendMessageW(s->list, LB_GETSEL, static_cast<WPARAM>(idx), 0) > 0;
            std::vector<int> rows = hit_selected ? SelectedRows(s) : std::vector<int>{};
            if (rows.empty()) {
                SendMessageW(s->list, LB_SETSEL, FALSE, -1);
                SendMessageW(s->list, LB_SETSEL, TRUE, static_cast<LPARAM>(idx));
                SendMessageW(s->list, LB_SETCURSEL, static_cast<WPARAM>(idx), 0);
                rows.push_back(idx);
            }
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            AppendMenuW(menu, MF_STRING, kCtxView, L"查看视频源详情(&V)");
            const std::wstring delete_label =
                rows.size() > 1
                    ? L"删除所选 " + std::to_wstring(rows.size()) + L" 个视频源(&D)"
                    : L"删除该视频源(&D)";
            AppendMenuW(menu, MF_STRING, kCtxDelete, delete_label.c_str());
            const int cmd = TrackPopupMenuEx(
                menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                point.x, point.y, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == kCtxView) DoView(s, idx);
            else if (cmd == kCtxDelete) DoRemoveRows(s, rows);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Layout layout = CurrentLayout(s);
            if (theme::HitRect(layout.close, point)) {
                s->close_pressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &layout.close, FALSE);
            } else if (!theme::HitRect(layout.content, point)) {
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
            const bool hot = theme::HitRect(layout.close, point);
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
                if (theme::HitRect(layout.close, point)) {
                    s->closed = true;
                    DestroyWindow(hwnd);
                }
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
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                const Layout layout = CurrentLayout(s);
                if (!theme::HitRect(layout.content, point) &&
                    !theme::HitRect(layout.close, point)) {
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
            // 不投递 WM_QUIT：本对话框是主消息循环里的嵌套模态循环。
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool ShowVideoSourceDialog(HWND owner, VideoSourceManager* mgr,
                           std::function<const skin::LegacySkin*()> skin) {
    if (!mgr) return false;

    DlgState state;
    state.mgr = mgr;
    state.owner = owner;
    state.skin = std::move(skin);

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

    static const wchar_t* kCls = L"TTPlayerVideoSourceDialog";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kCls;
        wc.hbrBackground = nullptr;  // 全部在 WM_PAINT 里画
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            registered = true;
        else
            return false;
    }

    if (owner) EnableWindow(owner, FALSE);

    // 样式必须与音乐窗、音乐导入窗逐字一致（WS_POPUP + WS_EX_TOOLWINDOW）。
    // 加 WS_CAPTION 之类的非客户区会让客户区比皮肤位图小，背景被整幅缩放。
    const DWORD frame_style = WS_POPUP | WS_CLIPCHILDREN;
    const DWORD frame_ex_style = WS_EX_TOOLWINDOW;
    RECT frame_rect{0, 0, width, height};
    AdjustWindowRectEx(&frame_rect, frame_style, FALSE, frame_ex_style);
    const int outer_width = frame_rect.right - frame_rect.left;
    const int outer_height = frame_rect.bottom - frame_rect.top;

    // WS_POPUP 不能用 CW_USEDEFAULT；显式居中到 owner 再夹进工作区。
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

    HWND hwnd = CreateWindowExW(frame_ex_style, kCls, L"视频源", frame_style, x, y,
                                outer_width, outer_height, owner, nullptr,
                                GetModuleHandleW(nullptr), &state);
    if (!hwnd) {
        if (owner) EnableWindow(owner, TRUE);
        return false;
    }

    if (browser) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const Layout layout = ComputeLayout(&state, client);
        frame::ApplyWindowRegion(hwnd, *browser, layout.skin, client,
                                 frame::TransparentColor(legacy, *browser),
                                 ttplayer::ui::ResolveScaleForWindow(hwnd));
    }

    HFONT ui_font = state.ui_font;

    state.label_url = CreateWindowW(
        L"STATIC", L"配置地址（影视仓/TVBox JSON，或每行一条 CMS 接口）：",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10, hwnd,
        nullptr, nullptr, nullptr);
    state.url_edit = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(kIdUrlEdit), nullptr, nullptr);
    state.button_import_url = CreateWindowW(L"BUTTON", L"导入 URL",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(kIdImportUrl), nullptr, nullptr);
    state.button_import_file = CreateWindowW(L"BUTTON", L"选择本地文件…",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(kIdImportFile), nullptr, nullptr);
    state.button_view = CreateWindowW(L"BUTTON", L"查看详情",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(kIdView), nullptr, nullptr);
    state.button_remove = CreateWindowW(L"BUTTON", L"删除所选",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(kIdRemove), nullptr, nullptr);
    state.label_list = CreateWindowW(
        L"STATIC", L"已导入视频源（点一行看详情；Ctrl / Shift 复选，右键：查看 / 删除）：",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10, hwnd,
        nullptr, nullptr, nullptr);
    state.list = CreateWindowW(L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_EXTENDEDSEL,
        0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdList), nullptr, nullptr);
    state.report = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
            ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdReport), nullptr, nullptr);

    for (HWND control : {state.label_url, state.label_list, state.url_edit,
                         state.button_import_url, state.button_import_file,
                         state.button_view, state.button_remove, state.list,
                         state.report}) {
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font), TRUE);
    }
    TrackButton(&state, state.button_import_url);
    TrackButton(&state, state.button_import_file);
    TrackButton(&state, state.button_view);
    TrackButton(&state, state.button_remove);

    LayoutChildren(&state);
    RefreshList(&state);

    // 打开就能看到已有源的信息：自动选中第一行并把它的详情铺进结果框，
    // 一个源都没有时才显示导入引导。
    if (!state.sources.empty()) {
        SendMessageW(state.list, LB_SETSEL, TRUE, 0);
        SendMessageW(state.list, LB_SETCURSEL, 0, 0);
        DoView(&state, 0);
    } else {
        SetReport(&state,
                  L"还没有导入任何视频源。\r\n"
                  L"把影视仓 / TVBox 配置地址、或 CMS 接口（每行一条）粘贴到上面的输入框，点「导入 URL」；"
                  L"也可以点「选择本地文件…」导入本地 JSON。\r\n"
                  L"导入后列表会列出每份配置及其站点数量，点一行即可查看站点清单。");
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (state.closed || !IsWindow(hwnd)) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return state.changed;
}

} // namespace ttplayer::video
