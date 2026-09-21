#pragma once

// MusicFree 兼容层 —— 音乐窗 / 导入窗共用的浅色主题
//
// 界面语言：浅色面板 + 青色强调色 + 一像素描边；所有绘制都走 GDI 原语，不依赖
// 任何图片资源。皮肤包里没有对应 skin 段时（音乐窗缺 <browser_window>、导入窗
// 根本没有皮肤段）用它画出风格一致的兜底外框与按钮。
//
// 为什么是 header-only：CMakeLists 里的源文件是显式列出的，新增 .cpp 必须同步改
// 构建脚本；而这些函数都很小，放头里更省事。**每个函数体都必须 inline**，否则多个
// TU 各出一份强定义，链接期报 LNK2005。
//
// 关于字形：按钮文字只用中文字面量，不用 U+27F3(⟳) 之类的符号字符 —— 界面字体
// （Microsoft YaHei UI / Tahoma）没有这些字形，GDI 回退出来的形状是错的（实测
// ⟳ 会画成一个圆圈）。需要图标时用 ◀ ▶ ✕ ✓ 这些字体里确实存在的字符。

#include <windows.h>

#include <algorithm>

namespace ttplayer::musicfree::theme {

// ------------------------------------------------------------------ 调色板
inline constexpr COLORREF kPanel = RGB(255, 255, 255);
inline constexpr COLORREF kPanelSoft = RGB(248, 251, 252);
inline constexpr COLORREF kAccent = RGB(38, 134, 163);
inline constexpr COLORREF kAccentHot = RGB(56, 162, 194);
inline constexpr COLORREF kAccentDown = RGB(26, 104, 128);
inline constexpr COLORREF kAccentDeep = RGB(18, 84, 104);
inline constexpr COLORREF kOutline = RGB(198, 212, 220);
inline constexpr COLORREF kOutlineHot = RGB(142, 172, 188);
inline constexpr COLORREF kText = RGB(38, 54, 62);
inline constexpr COLORREF kSubtleText = RGB(118, 134, 142);
inline constexpr COLORREF kDisabledText = RGB(176, 188, 194);
inline constexpr COLORREF kDisabledFill = RGB(178, 196, 204);
inline constexpr COLORREF kRowAlternate = RGB(247, 251, 253);
inline constexpr COLORREF kRowHot = RGB(232, 244, 250);
inline constexpr COLORREF kRowSelected = RGB(206, 231, 244);
inline constexpr COLORREF kRowSeparator = RGB(238, 243, 246);
inline constexpr COLORREF kHeaderFill = RGB(238, 245, 248);
inline constexpr COLORREF kHeaderSeparator = RGB(214, 228, 236);
inline constexpr COLORREF kFooterFill = RGB(240, 246, 248);
// 强调色底上的按钮文字/图标：常态是浅青，热态是纯白。
inline constexpr COLORREF kOnAccentIdle = RGB(206, 232, 240);
inline constexpr COLORREF kOnAccentDim = RGB(150, 186, 199);
inline constexpr COLORREF kOnAccentText = RGB(255, 255, 255);

// ------------------------------------------------------------------ 图元

inline void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

inline void StrokeSolidRect(HDC dc, const RECT& rect, COLORREF color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

inline void DrawRoundedBox(HDC dc, const RECT& rect, COLORREF fill,
                           COLORREF border, int radius) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const bool outlined = border != CLR_INVALID;
    const bool filled = fill != CLR_INVALID;
    // 不填充时必须显式选 NULL_BRUSH：RoundRect 在没有画笔时会拿 DC 当前画刷去
    // 填内部，而 DC 默认画刷是白色 —— 于是「只描边」的调用会糊出一个白色圆角块。
    const HBRUSH brush = filled ? CreateSolidBrush(fill)
                                : static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    const HPEN pen = outlined ? CreatePen(PS_SOLID, 1, border)
                              : static_cast<HPEN>(GetStockObject(NULL_PEN));
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    if (filled) DeleteObject(brush);
    if (outlined) DeleteObject(pen);
}

inline void DrawTextIn(HDC dc, const RECT& rect, const wchar_t* text,
                       COLORREF color, HFONT font, UINT format) {
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

inline constexpr UINT kLabelFormat = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
inline constexpr UINT kCenterFormat = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
inline constexpr UINT kRightFormat = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;

// ------------------------------------------------------------------ 字体

inline HFONT MakeFont(int height, int weight, const wchar_t* face) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

// 中文界面优先用雅黑；CreateFontW 在字体缺失时会自动替换，无需探测。
inline HFONT CreateUiFont(int height, int weight) {
    HFONT font = MakeFont(height, weight, L"Microsoft YaHei UI");
    if (!font) font = MakeFont(height, weight, L"Tahoma");
    return font;
}

// ------------------------------------------------------------------ 按钮

enum ButtonState {
    kButtonNormal = 0,
    kButtonHot = 1,
    kButtonPressed = 2,
    kButtonDisabled = 3,
};

// 一个按钮在某个状态下的三种颜色。fill 为 CLR_INVALID 表示不填充底色。
struct ButtonSkin {
    COLORREF fill{CLR_INVALID};
    COLORREF border{CLR_INVALID};
    COLORREF text{kText};
};

// 面板上的按钮（搜索结果区、导入窗）：primary 用强调色实心，其余用白底描边。
inline ButtonSkin PanelButtonSkin(bool primary, int state) {
    ButtonSkin skin;
    if (primary) {
        skin.fill = state == kButtonHot ? kAccentHot
                  : state == kButtonPressed ? kAccentDown : kAccent;
        skin.border = skin.fill;
        skin.text = kOnAccentText;
        if (state == kButtonDisabled) {
            skin.fill = kDisabledFill;
            skin.border = kDisabledFill;
            skin.text = kFooterFill;
        }
        return skin;
    }
    skin.fill = state == kButtonHot ? kRowHot
              : state == kButtonPressed ? kRowSelected : kPanel;
    skin.border = state == kButtonHot ? kOutlineHot : kOutline;
    skin.text = state == kButtonHot ? kAccent : kText;
    if (state == kButtonDisabled) {
        skin.fill = kPanelSoft;
        skin.border = kOutline;
        skin.text = kDisabledText;
    }
    return skin;
}

// 强调色标题栏上的按钮：常态不填充，只把文字画成浅青。
inline ButtonSkin TitleButtonSkin(int state) {
    ButtonSkin skin;
    skin.fill = state == kButtonHot ? kAccentDown
              : state == kButtonPressed ? kAccentDeep : CLR_INVALID;
    skin.border = CLR_INVALID;
    skin.text = state == kButtonDisabled ? kOnAccentDim
              : (state == kButtonNormal ? kOnAccentIdle : kOnAccentText);
    return skin;
}

inline void PaintButton(HDC dc, const RECT& rect, const wchar_t* text,
                        HFONT font, const ButtonSkin& skin) {
    DrawRoundedBox(dc, rect, skin.fill, skin.border, 4);
    if (text && *text)
        DrawTextIn(dc, rect, text, skin.text, font, kCenterFormat);
}

// ------------------------------------------------------------------ 兜底窗体

// 兜底外框的分区。导航簇（音源标签 + ◀ ▶ + 重新搜索 + ✕）全部右对齐，
// 标题文字区域裁到导航簇左侧 —— 早期版本把这三个按钮放在标题栏左上角
// (10,7)-(80,23)，正好压在标题文字上，看起来就是「按钮和标题重叠」。
struct FrameChrome {
    RECT client{};
    RECT title{};
    RECT title_text{};
    RECT content{};
    RECT footer{};
    RECT close{};
    RECT navigation_label{};
    RECT prev_source{};
    RECT next_source{};
    RECT refresh{};
};

struct ChromeStates {
    int close{kButtonNormal};
    int prev_source{kButtonNormal};
    int next_source{kButtonNormal};
    int refresh{kButtonNormal};
};

inline FrameChrome BuildFrameChrome(const RECT& client, int pad, int title_height,
                                    int footer_height) {
    FrameChrome chrome;
    chrome.client = client;
    chrome.title = {client.left + 1, client.top + 1, client.right - 1,
                    client.top + title_height};
    chrome.content = {client.left + pad, chrome.title.bottom, client.right - pad,
                      client.bottom - footer_height};
    chrome.footer = {client.left + 1, chrome.content.bottom, client.right - 1,
                     client.bottom - 1};

    const int top = chrome.title.top + 4;
    const int bottom = std::min<LONG>(chrome.title.bottom - 4, top + 20);
    chrome.close = {client.right - pad - 24, top, client.right - pad, bottom};
    chrome.refresh = {chrome.close.left - 8 - 76, top, chrome.close.left - 8, bottom};
    chrome.next_source = {chrome.refresh.left - 12 - 24, top,
                          chrome.refresh.left - 12, bottom};
    chrome.prev_source = {chrome.next_source.left - 2 - 24, top,
                          chrome.next_source.left - 2, bottom};
    chrome.navigation_label = {chrome.prev_source.left - 4 - 34, top,
                               chrome.prev_source.left - 4, bottom};
    chrome.title_text = {client.left + 11, chrome.title.top,
                         std::max<LONG>(client.left + 11, chrome.navigation_label.left - 8),
                         chrome.title.bottom};
    return chrome;
}

// 只画外框的分区与控件，不管业务内容 —— 内容区由使用方填自己的子控件。
inline void PaintFrameChrome(HDC dc, const FrameChrome& chrome,
                             const wchar_t* title, HFONT title_font, HFONT ui_font,
                             const ChromeStates& states) {
    FillSolidRect(dc, chrome.client, kPanelSoft);
    FillSolidRect(dc, chrome.title, kAccent);

    DrawTextIn(dc, chrome.title_text, title ? title : L"", kOnAccentText,
               title_font, kLabelFormat | DT_END_ELLIPSIS);

    PaintButton(dc, chrome.close, L"✕", ui_font, TitleButtonSkin(states.close));
    DrawTextIn(dc, chrome.navigation_label, L"音源", kOnAccentIdle, ui_font,
               kRightFormat);
    PaintButton(dc, chrome.prev_source, L"◀", ui_font,
                TitleButtonSkin(states.prev_source));
    PaintButton(dc, chrome.next_source, L"▶", ui_font,
                TitleButtonSkin(states.next_source));
    // 这里刻意用文字而不是 ⟳：界面字体没有该字形，画出来是个圆圈。
    PaintButton(dc, chrome.refresh, L"重新搜索", ui_font,
                TitleButtonSkin(states.refresh));

    // 内容面板：描边严格落在 content 边界上。早期版本把上边抬高了 4 像素
    // (panel.top -= 4)，那条线因此横穿标题栏，看着像「框把标题盖住了一块」。
    FillSolidRect(dc, chrome.content, kPanel);
    StrokeSolidRect(dc, chrome.content, kOutline);

    FillSolidRect(dc, chrome.footer, kFooterFill);
    StrokeSolidRect(dc, chrome.client, kOutline);
}

// 把窗口客户区坐标的点映射到 chrome 的某个控件上；返回 false 表示没命中。
inline bool HitRect(const RECT& rect, POINT point) {
    return rect.right > rect.left && rect.bottom > rect.top &&
           PtInRect(&rect, point) != FALSE;
}

} // namespace ttplayer::musicfree::theme
