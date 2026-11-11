#pragma once

// 音乐窗 WebView2 JSON 桥 —— 搜索 / 收藏 / 历史 / 配置 命令（Task 8）
//
// 只负责把 4 条 cmd 注册进 MusicFreeBridge（协议层见 musicfree_bridge.h）：
//   search.query  搜索：既有 QuickJS 音源聚合搜索，结果经 evt search.result 推回
//   fav.set       收藏/取消收藏（含读取当前收藏）
//   cfg.get       读既有配置 / 历史（TTPlayerRebuild.xml、musicfree/update_state.json）
//   cfg.set       写既有配置（沿用既有保存函数，落盘文件与格式不变）
//
// 业务实现全部复用既有函数（SearchAllSources / AutoUpdateSourcesEnabled /
// settings::LoadRuntimeSettings + settings::SaveWindowState），本模块不新增
// 任何落盘文件，也不改变既有存储格式。
// 注册时机由宿主决定（通常在音乐窗创建桥之后调用一次）。

#include "musicfree_bridge.h"

namespace ttplayer::musicfree {

// 把 Task 8 的 4 条命令注册到桥上。重复调用以后者为准（Register 自身语义）。
// 只要求 bridge 在注册后保持存活，直至在途回执/事件发送完毕（与桥的线程模型一致）。
void RegisterMiscCommands(MusicFreeBridge& bridge);

}  // namespace ttplayer::musicfree
