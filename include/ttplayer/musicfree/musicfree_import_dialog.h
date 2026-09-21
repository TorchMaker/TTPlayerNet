#pragma once

// MusicFree 兼容层 —— 手动导入对话框（纯 Win32，不依赖 RC 资源）
//
// 与 MusicFree 一致：音源不内置，必须由用户手动导入。提供两种方式：
//   * 粘贴一个 URL（单个插件 JS，或像 yuanli.json 那样的注册表 JSON）
//   * 选择本地文件（注册表 JSON 或单个插件 JS）
// 导入后写入 TTPlayer 设置目录，下次启动自动 LoadCached 重建。

#include <windows.h>

namespace ttplayer::musicfree {

class MusicFreeSourceManager;

// 以模态方式弹出“导入 MusicFree 音源”对话框。
// owner 为父窗口（可为 nullptr）；mgr 为已初始化的音源管理器。
void ShowMusicFreeImportDialog(HWND owner, MusicFreeSourceManager* mgr);

} // namespace ttplayer::musicfree
