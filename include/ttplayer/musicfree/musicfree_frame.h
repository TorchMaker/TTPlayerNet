#pragma once

// MusicFree 兼容层 —— 音乐窗 / 导入窗共用的「皮肤外框坐标映射」
//
// <browser_window> 段里所有矩形的坐标都是**背景位图坐标**（0,0 在位图左上角）。
// 只有窗口尺寸恰好等于背景位图尺寸时，这些坐标才等于客户区坐标。音乐窗允许缩放
// 之后必须做一次换算；导入窗为了和音乐窗长得完全一样，用的是同一套换算：
//
//   · 内容区 <browser position="l,t,r,b">：保持它到窗口四条边的距离不变 ——
//     窗口变大时内容区跟着变大，这正是「不写死宽高」的关键；
//   · 标题栏上的关闭 / 换音源 / 重新搜索：到**右边缘**的距离保持不变（右锚定），
//     到上边缘的距离不变；
//   · 底部复选行与提示行：到**下边缘**的距离保持不变（下锚定），
//     复选行贴左、提示行贴右。
//
// 当窗口尺寸 == 位图尺寸时所有换算都是恒等变换，所以既有皮肤的外观不会被改动。
//
// 为什么是 header-only：CMakeLists 显式列出源文件，新增 .cpp 要同步改构建脚本；
// 这些函数都很小，放头里更省事。**每个函数体都必须 inline**，否则多个 TU 各出
// 一份强定义，链接期报 LNK2005。

#include <windows.h>

#include <algorithm>

#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_frame.h"

namespace ttplayer::musicfree::frame {

// 外框各个部位在**当前客户区坐标**下的矩形。
struct Metrics {
    bool skinned{};
    SIZE natural{};      // 背景位图尺寸（= 自然窗口尺寸）
    RECT resize_rect{};  // 九宫格拉伸区（位图坐标，即 browser 内容区）
    RECT content{};      // 内容区（客户区坐标）
    RECT close{};
    RECT backward{};
    RECT forward{};
    RECT refresh{};
    RECT check_box{};
    RECT check_label{};
    RECT link{};
};

// 皮肤包级透明色键；节点级 transparent_color 优先。
inline COLORREF TransparentColor(const skin::LegacySkin* legacy,
                                 const skin::BrowserSkin& browser) {
    if (browser.transparent_color != CLR_INVALID) return browser.transparent_color;
    if (legacy) return legacy->TransparentColor();
    return RGB(255, 0, 255);
}

// 把皮肤坐标映射到当前客户区。client 是客户区矩形（left/top 通常为 0）。
inline Metrics Map(const RECT& client, const skin::BrowserSkin& browser) {
    Metrics metrics;
    metrics.skinned = true;
    metrics.natural = browser.background.size;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int natural_width =
        metrics.natural.cx > 0 ? metrics.natural.cx : width;
    const int natural_height =
        metrics.natural.cy > 0 ? metrics.natural.cy : height;
    const int grow_x = width - natural_width;
    const int grow_y = height - natural_height;

    // 内容区：四边内缩量不变。皮肤没写 <browser> 时按常用边框厚度推一个。
    RECT content = browser.content_bounds;
    if (content.right <= content.left || content.bottom <= content.top)
        content = RECT{8, 31, natural_width - 8, natural_height - 24};
    metrics.resize_rect = content;
    metrics.content = content;
    metrics.content.right += grow_x;
    metrics.content.bottom += grow_y;
    // 兜底：极端情况下内容区不能被压成负数边长。
    metrics.content.right =
        std::max<LONG>(metrics.content.right, metrics.content.left + 64);
    metrics.content.bottom =
        std::max<LONG>(metrics.content.bottom, metrics.content.top + 64);

    // 标题栏按钮：右锚定。X 平移 grow_x，Y 不动。
    const auto anchor_top = [grow_x](const skin::SkinElement& element) {
        RECT rect = element.bounds;
        rect.left += grow_x;
        rect.right += grow_x;
        return rect;
    };
    metrics.close = anchor_top(browser.close);
    metrics.backward = anchor_top(browser.backward);
    metrics.forward = anchor_top(browser.forward);
    metrics.refresh = anchor_top(browser.refresh);

    // 底部一行：随高度一起下移。复选行贴左，提示行贴右。
    RECT box = browser.check.bounds;
    box.top += grow_y;
    box.bottom += grow_y;
    metrics.check_box = box;
    metrics.check_label = {box.right + 6, box.top, box.right + 6 + 76, box.bottom};

    RECT link = browser.link.bounds;
    link.left += grow_x;
    link.right += grow_x;
    link.top += grow_y;
    link.bottom += grow_y;
    metrics.link = link;
    return metrics;
}

// 按九宫格把外框位图铺到客户区。拉伸区取内容区 —— 边框、标题栏、页脚因此保持
// 原始像素厚度，只有内容区随窗口变大。
inline void PaintBackground(HDC dc, const skin::BrowserSkin& browser,
                            const Metrics& metrics, const RECT& client) {
    skin::DrawFrameBackground(dc, browser.background, metrics.resize_rect,
                              client.right - client.left,
                              client.bottom - client.top, false);
}

// 鼠标可抓取的边框厚度（缩放带宽度）。必须**小于**皮肤边框的原始厚度，否则会把
// 内容区（尤其是列表右侧的滚动条）的点击抢走。实测内置皮肤：左 8px、右 9px、
// 上 31px、下 31px —— 取 6 既好抓又完全落在边框里。
inline constexpr int kResizeBand = 6;

// 鼠标落在窗口边框的缩放带上时返回对应的 HT*，否则返回 HTCLIENT。
//
// 纯算术、不碰 HWND —— 这样「哪条边认不认」可以在 --mf-selftest 里用几个合成尺寸
// 直接算出来核对，不必真的去拖窗口。
//
// ★ **宽度锁定：只允许高度方向缩放**。音乐窗的皮肤标题栏与关闭/换音源按钮都是
// 固定宽度的像素图，一旦横向拉伸就会糊成变形条、文字错位；所以左右边与四个角一律
// 返回 HTCLIENT（即不认横向缩放）。只有上边 HTTOP / 下边 HTBOTTOM 可以抓，四个角
// 也被归到上/下边（拖角只改高度，不横向拉宽）。宽度方向再用 WM_GETMINMAXINFO 把
// ptMinTrackSize.x 与 ptMaxTrackSize.x 同时钉在皮肤自然宽度上兜底 —— 即便鼠标没停
// 在边上，双击标题栏之类的系统行为也无法把窗口拉宽。
//
// band 必须**小于**皮肤边框厚度，否则会把内容区（尤其列表右侧滚动条）的点击抢走。
inline int ResizeHitCode(SIZE client, int band, POINT point) {
    if (client.cx <= 0 || client.cy <= 0 || band <= 0) return HTCLIENT;
    const bool top = point.y < band;
    const bool bottom = point.y >= client.cy - band;
    // 仅高度方向可缩放：上/下边抓到返回 HTTOP/HTBOTTOM，四个角也归到上/下边；
    // 左/右方向一律当作客户区，窗口宽度由 WM_GETMINMAXINFO 钉死，永不改变。
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTCLIENT;
}

// 把 HT* 码转成 trace 里好读的名字。
inline const char* HitCodeName(int code) {
    switch (code) {
        case HTCLIENT: return "HTCLIENT";
        case HTLEFT: return "HTLEFT";
        case HTRIGHT: return "HTRIGHT";
        case HTTOP: return "HTTOP";
        case HTBOTTOM: return "HTBOTTOM";
        case HTTOPLEFT: return "HTTOPLEFT";
        case HTTOPRIGHT: return "HTTOPRIGHT";
        case HTBOTTOMLEFT: return "HTBOTTOMLEFT";
        case HTBOTTOMRIGHT: return "HTBOTTOMRIGHT";
        default: return "OTHER";
    }
}

// 由同一个九宫格映射生成窗口区域（BMP 走颜色键，PNG 走 alpha 掩码），
// 保证拉伸后的边框不会被区域裁掉。
inline void ApplyWindowRegion(HWND hwnd, const skin::BrowserSkin& browser,
                              const Metrics& metrics, const RECT& client,
                              COLORREF transparent) {
    if (!hwnd) return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HRGN region = skin::CreateFrameRegion(browser.background, metrics.resize_rect,
                                          width, height, false, transparent);
    RECT bounds{};
    if (!region || GetRgnBox(region, &bounds) == NULLREGION ||
        !SetWindowRgn(hwnd, region, TRUE)) {
        if (region) DeleteObject(region);
    }
}

} // namespace ttplayer::musicfree::frame
