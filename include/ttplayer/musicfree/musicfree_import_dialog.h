#pragma once

// MusicFree 兼容层 —— 手动导入对话框（纯 Win32，不依赖 RC 资源）
//
// 与 MusicFree 一致：音源不内置，必须由用户手动导入。提供两种方式：
//   * 粘贴一个 URL（单个插件 JS，或像 yuanli.json 那样的注册表 JSON）
//   * 选择本地文件（注册表 JSON 或单个插件 JS）
// 导入后写入 TTPlayer 设置目录，下次启动自动 LoadCached 重建。
//
// 外观与音乐窗**完全一致**：皮肤包带 <browser_window> 段时用同一幅背景位图、
// 同一个皮肤关闭按钮、同一套坐标映射（见 musicfree_frame.h）；没有皮肤段时才退回
// 主题自绘的强调色标题栏。

#include <functional>

#include <windows.h>

namespace ttplayer::skin { class LegacySkin; }

namespace ttplayer::musicfree {

class MusicFreeSourceManager;

// 以模态方式弹出「导入 MusicFree 音源」对话框。
//   owner : 父窗口（可为 nullptr）。
//   mgr   : 已初始化的音源管理器。
//   skin  : 取当前皮肤的可选回调。若非空且皮肤包带 <browser_window> 段，对话框就
//           用与音乐窗相同的皮肤外框绘制；否则退回主题自绘框。写成回调而不是裸
//           指针，这样运行期换肤不需要重新打开对话框。
void ShowMusicFreeImportDialog(HWND owner, MusicFreeSourceManager* mgr,
                               std::function<const skin::LegacySkin*()> skin = {});

} // namespace ttplayer::musicfree
