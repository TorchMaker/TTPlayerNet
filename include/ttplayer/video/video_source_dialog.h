#pragma once

// 视频源兼容层 —— 视频源管理对话框（纯 Win32，不依赖 RC 资源）
//
// 与音乐侧的「导入音源」窗口是同一套版式与同一套皮肤外框，操作习惯也一致：
//   * 粘贴一个影视仓 / TVBox 配置 URL，点「导入 URL」；
//   * 或选择本地 JSON 文件；
//   * 列表里能看到**已经导入过的**每个来源（名称、站点数、直播数、版本）；
//   * 选中一行即可「查看详情」（来源地址、每个站点的 kind 与 api、不支持的原因）；
//   * 支持 Ctrl / Shift 复选后「删除所选」，右键菜单同样可查看 / 删除。
//
// 外观：皮肤包带 <browser_window> 段时用与音乐窗**完全相同的背景位图与坐标映射**，
// 没有该段时退回主题自绘的浅色框体。导入结果写在窗口底部的结果框里，不弹
// MessageBox —— 系统灰底消息框在这套皮肤窗里突兀，而且会打断「导入完接着看列表」
// 的操作流。

#include <functional>

#include <windows.h>

#include "ttplayer/video/video_export.h"

namespace ttplayer::skin { class LegacySkin; }

namespace ttplayer::video {

class VideoSourceManager;

// 以模态方式弹出「视频源管理」对话框。
//   owner : 父窗口（可为 nullptr）。
//   mgr   : 已初始化的视频源管理器。
//   skin  : 取当前皮肤的回调；非空且皮肤带 <browser_window> 段时用皮肤外框。
// 返回 true 表示视频源集合发生了改动（导入成功 / 删除成功），调用方应重新聚合首页。
TTPLAYER_VIDEO_API bool ShowVideoSourceDialog(
    HWND owner, VideoSourceManager* mgr,
    std::function<const skin::LegacySkin*()> skin = {});

} // namespace ttplayer::video
