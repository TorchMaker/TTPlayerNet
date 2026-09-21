#pragma once

#include "ttplayer/skin/skin.h"

// 窗口框体绘制：把皮肤包里的位图铺到任意尺寸的 HWND 上，并生成对应的
// 透明区域。主窗口/歌词窗/播放列表窗/均衡器窗和音乐窗共用这套实现，
// 这样新加一个皮肤化窗口时不必再复制一份九宫格与颜色键逻辑。

namespace ttplayer::skin {

// 按九宫格（resize_rect 之外的四条边保持原始像素，中心区拉伸或平铺）把
// 背景位图铺满 width x height。resize_rect 为空或目标小于原始尺寸时退化为
// 1:1 缩放。scale 把源分割与可拉伸区换算到当前显示器物理像素。
void DrawFrameBackground(HDC target, const SkinBitmap& bitmap, RECT resize_rect,
                         int width, int height, bool tile,
                         const ui::DpiScale& scale = ui::DpiScale{});

// 由框体位图生成窗口区域。BMP 用颜色键判定透明，PNG 用 alpha 掩码。
// 失败返回 nullptr；调用方在 SetWindowRgn 失败时需要自行 DeleteObject。
[[nodiscard]] HRGN CreateFrameRegion(const SkinBitmap& bitmap, RECT resize_rect,
                                     int width, int height, bool tile,
                                     COLORREF transparent,
                                     const ui::DpiScale& scale = ui::DpiScale{});

// 绘制多帧按钮的指定状态帧。帧宽 = 位图宽 / frames，帧高始终是位图原始
// 高度（XML 矩形允许带几像素的留白，不参与拉伸）。scale 缩放目标帧。
void DrawFrameButton(HDC target, const SkinElement& element, RECT bounds,
                     int state, COLORREF transparent,
                     const ui::DpiScale& scale = ui::DpiScale{});

} // namespace ttplayer::skin
