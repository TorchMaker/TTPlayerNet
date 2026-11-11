#pragma once

// 音乐窗 WebView2 JSON 桥 —— 视频能力命令（Task 7）
//
// 只负责把 4 条 cmd 注册进 MusicFreeBridge（协议层见 musicfree_bridge.h）：
//   video.home    触发视频首页聚合（并发 FetchHome，做法对齐 StartLoadVideo），
//                 完成后经 evt video.home 推回 {sections:[{title, cards:[...]}]}。
//                 分区标题归一与排序复用 musicfree_browser_window.cpp 既有的
//                 FriendlySectionTitle / SectionPriority（同名分区不重复）。
//   video.open    打开视频：payload 带 url/sources/line/title 时直接调既有
//                 ShowVideoPlayer（页面带来的线路数组原样透传）；只带 id+site
//                 （点卡片时页面尚无播放地址）时取详情 FetchDetail，把多路
//                 线路选集回给页面渲染 #episode-list。
//   video.sources 调既有 ShowVideoSourceDialog 弹视频源导入对话框，完成后
//                 回执 + evt source.result（导入报告文案）。
//                 payload.live=true → 直播入口尚无公开接线点，回 ok:false
//                 「直播入口待接线」（见 .cpp 文件头注释的结论）。
//   video.search  全源搜索（并发 FetchSearch）→ evt video.search
//                 {q, cards:[...]}，供右栏搜索条展示结果。
//
// 业务实现全部复用既有函数（FetchHome / FetchDetail / FetchLive 链路、
// ShowVideoPlayer、ShowVideoSourceDialog、FriendlySectionTitle、
// SectionPriority、NormalizeVideoTitle），本模块不新增任何落盘文件、
// 不改任何既有存储格式；handler 不抛异常，失败统一回 ok:false + 中文 error。
//
// 注册时机由宿主决定（通常在音乐窗创建桥之后调用一次）。

#include "musicfree_bridge.h"

namespace ttplayer::musicfree {

// 把 Task 7 的 3 条命令注册到桥上。重复调用以后者为准（Register 自身语义）。
// 要求 bridge 在注册后保持存活，直至在途回执 / 推送事件发送完毕
//（与桥头文件的线程模型约定一致）。
void RegisterVideoCommands(MusicFreeBridge& bridge);

}  // namespace ttplayer::musicfree
