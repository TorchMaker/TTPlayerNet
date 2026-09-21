// MusicFree 兼容层 —— 手动导入对话框实现（纯 Win32，自建窗口，无需 RC）
//
// 控件全部用 CreateWindow 创建，消息循环在 ShowMusicFreeImportDialog 内自包含，
// 因此接入方无需改动任何 .rc 资源，也无需 ATL/WTL 依赖。

#include "ttplayer/musicfree/musicfree_import_dialog.h"

#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <vector>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"

namespace ttplayer::musicfree {

namespace {

const int kIdUrlEdit = 1001;
const int kIdImportUrl = 1002;
const int kIdImportFile = 1003;
const int kIdList = 1004;
const int kIdRemove = 1005;
const int kIdClose = 1006;

struct DlgState {
    MusicFreeSourceManager* mgr = nullptr;
    HWND hwnd = nullptr;
    HWND urlEdit = nullptr;
    HWND list = nullptr;
    HWND owner = nullptr;
    bool closed = false;
};

void RefreshList(DlgState* s) {
    if (!s->list || !s->mgr) return;
    SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
    auto plugins = s->mgr->ListPlugins();
    for (const auto& p : plugins) {
        std::wstring line = L"[插件] " + std::wstring(p.name.begin(), p.name.end());
        if (!p.version.empty()) line += L"  v" + std::wstring(p.version.begin(), p.version.end());
        line += p.can_search ? L"  搜" : L"";
        line += p.can_media_source ? L"  播" : L"";
        line += p.can_lyric ? L"  词" : L"";
        SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
}

void Report(DlgState* s, const std::wstring& msg) {
    MessageBoxW(s->hwnd, msg.c_str(), L"MusicFree 音源", MB_OK | MB_ICONINFORMATION);
}

void DoImportUrl(DlgState* s) {
    wchar_t buf[4096] = {0};
    GetWindowTextW(s->urlEdit, buf, 4096);
    std::wstring wurl(buf);
    if (wurl.empty()) { Report(s, L"请先粘贴音源 URL"); return; }
    std::string url(wurl.begin(), wurl.end());
    auto report = s->mgr->ImportFromUrl(url);
    std::wstring info = L"音源：" + std::wstring(report.source_name.begin(), report.source_name.end())
        + L"\n成功挂载插件：" + std::to_wstring(report.plugins_ok) + L" / "
        + std::to_wstring(report.plugins_total);
    for (const auto& e : report.errors) {
        info += L"\n- " + std::wstring(e.begin(), e.end());
    }
    Report(s, info);
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
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    // path 是 wchar_t 数组，需走项目的宽→UTF-8 转换，不能用 std::string 构造。
    std::wstring wp(path);
    auto report = s->mgr->ImportFromFile(std::filesystem::path(wp));
    std::wstring info = L"音源：" + std::wstring(report.source_name.begin(), report.source_name.end())
        + L"\n成功挂载插件：" + std::to_wstring(report.plugins_ok) + L" / "
        + std::to_wstring(report.plugins_total);
    for (const auto& e : report.errors) {
        info += L"\n- " + std::wstring(e.begin(), e.end());
    }
    Report(s, info);
    RefreshList(s);
}

void DoRemove(DlgState* s) {
    int idx = (int)SendMessageW(s->list, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) { Report(s, L"请先在列表中选择要移除的音源"); return; }
    auto plugins = s->mgr->ListPlugins();
    if (idx < 0 || idx >= (int)plugins.size()) return;
    if (s->mgr->RemoveSource(plugins[idx].name)) RefreshList(s);
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
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == kIdImportUrl) DoImportUrl(s);
            else if (id == kIdImportFile) DoImportFile(s);
            else if (id == kIdRemove) DoRemove(s);
            else if (id == kIdClose) { s->closed = true; DestroyWindow(hwnd); }
            return 0;
        }
        case WM_CLOSE:
            s->closed = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (s->owner) EnableWindow(s->owner, TRUE);
            // 不要 PostQuitMessage：本对话框是播放器主消息循环里的嵌套模态循环，
            // 投递 WM_QUIT 会连带把整个应用的消息循环一起结束。
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowMusicFreeImportDialog(HWND owner, MusicFreeSourceManager* mgr) {
    if (!mgr) return;

    DlgState state;
    state.mgr = mgr;
    state.owner = owner;

    // 窗口类名（一次性）
    static const wchar_t* kCls = L"TTPlayerMusicFreeImport";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    if (owner) EnableWindow(owner, FALSE);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kCls, L"导入 MusicFree 音源",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 360,
        owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!hwnd) { if (owner) EnableWindow(owner, TRUE); return; }

    // 控件
    CreateWindowW(L"STATIC", L"音源 URL（单个插件 JS，或注册表 JSON 如 yuanli.json）：",
                  WS_CHILD | WS_VISIBLE, 12, 12, 430, 20, hwnd, nullptr, nullptr, nullptr);
    state.urlEdit = CreateWindowW(L"EDIT", L"",
                  WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                  12, 36, 340, 22, hwnd, (HMENU)kIdUrlEdit, nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"导入 URL", WS_CHILD | WS_VISIBLE,
                  360, 34, 84, 26, hwnd, (HMENU)kIdImportUrl, nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"选择本地文件…", WS_CHILD | WS_VISIBLE,
                  12, 66, 150, 26, hwnd, (HMENU)kIdImportFile, nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"移除所选", WS_CHILD | WS_VISIBLE,
                  170, 66, 110, 26, hwnd, (HMENU)kIdRemove, nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE,
                  360, 66, 84, 26, hwnd, (HMENU)kIdClose, nullptr, nullptr);
    CreateWindowW(L"STATIC", L"已导入音源 / 插件：",
                  WS_CHILD | WS_VISIBLE, 12, 102, 430, 18, hwnd, nullptr, nullptr, nullptr);
    state.list = CreateWindowW(L"LISTBOX", L"",
                  WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                  12, 124, 432, 170, hwnd, (HMENU)kIdList, nullptr, nullptr);

    // 控件建好之后才刷新（WM_INITDIALOG 不会到达这里的普通窗口）。
    RefreshList(&state);

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
