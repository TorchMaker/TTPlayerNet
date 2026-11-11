#pragma once

// 音乐窗 WebView2 JSON 桥 —— 播放控制与状态回推命令（Task 6）
//
// 只负责把 6 条 cmd 注册进 MusicFreeBridge（协议层见 musicfree_bridge.h），
// 并启动 evt player.state 的周期推送：
//   player.play     播放：payload 可带曲目 / 列表；不带参数时等同主窗「播放」
//   player.pause    暂停
//   player.next     下一首
//   player.prev     上一首
//   player.seek     定位：{ pos: 秒 } 或 { pos_ms: 毫秒 }
//   player.volume   音量：{ vol: 0..100 }
//
// 实现全部复用既有播放链路（绝不自造播放器）：
//   · 暂停 / 播放 / 上一首 / 下一首 / 定位 / 音量 / 播放模式 —— 直接调用
//     主播放窗 PlayerWindow 的既有成员实现（与 WM_COMMAND 0xC350..0xC355
//     即 kCmdPlay/kCmdPause/kCmdPrevious/kCmdNext 及皮肤进度条提交同一份代码）；
//   · 带曲目的播放 —— 先走 musicfree_app.cpp 的 ResolvePlayUrl 解析地址，
//     再走 PlayerWindow::AddOnlineTrack（即音乐窗「播放所选」的
//     on_track 回调链，player_window.cpp:ShowMusicFreeBrowserWindow）。
//
// 【player.state 契约（页面按此渲染控制条）】
//   { playing: bool, state: "playing|paused|stopped|opening|failed",
//     title, artist, art: string, pos/dur: 秒(int), pos_ms/dur_ms: 毫秒(int),
//     mode: 0..4（单曲/单曲循环/顺序/列表循环/随机）, vol: 0..100, mute: bool }
//   推送时机：注册后先连推若干次做初始同步（防 sender 未注入时丢失），
//   之后按「签名有变化即推」——播放中 pos 每秒进位正好每秒一推，
//   播放状态 / 播放模式 / 音量等变化最多 500ms 内到达页面。
//   高频节流由桥内置（100ms 合并），本模块只管触发。
//
// 【状态与控制的取值路径】主窗 GWLP_USERDATA 存着 PlayerWindow*（见
// src/ui/wtl_window.h WindowBinding::Start），本文件通过 player_window.h /
// audio_engine.h 已 friend 声明的 ttplayer::testing::ProgressSeekAccess
// （原为测试保留，全工程唯一定义在本文件）访问其私有成员；不改任何既有文件。
// 状态轮询定时器与跨线程回 UI 的投递挂在音乐窗窗口的子类化过程上
// （SetWindowSubclass，运行期注入，同样不改既有文件）。
//
// 注册时机由宿主决定（通常在音乐窗创建桥之后调用一次）；
// 重复调用按最后一次为准并重新挂子类 / 定时器。

#include "musicfree_bridge.h"

namespace ttplayer::musicfree {

// 把 Task 6 的 6 条播放命令注册到桥上，并启动 player.state 周期推送。
// 要求 bridge 在注册后保持存活，直至在途回执 / 事件发送完毕
//（与桥的线程模型一致）；音乐窗窗口销毁时本模块的子类化过程会自行收尾。
void RegisterMusicCommands(MusicFreeBridge& bridge);

}  // namespace ttplayer::musicfree
