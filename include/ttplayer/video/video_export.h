#pragma once

// ttplayer_video.dll 的符号导出宏。
//
// 视频层（src/video/**：视频源管理、CMS/直播拉取、地址解析、spider 引擎、
// 播放窗）整体编译进独立 DLL `ttplayer_video.dll`，避免这些代码（尤其十几个
// spider 引擎）把主程序撑大。只有被 DLL 之外使用的公开 API（include/ttplayer/
// video 下的自由函数与 VideoSourceManager 的非 inline 成员）才需要本宏：
//
//   · 编译 ttplayer_video 目标时定义 TTPLAYER_VIDEO_BUILDING_DLL → dllexport
//   · 其它任何使用方（主 exe / ttplayer_core 静态库）不加该定义 → dllimport
//
// 纯虚接口（如 VideoWebHost）与 header-only 类型（video_types.h 的结构体等）
// 不需要标注。

// XP / Win7 兼容构建把视频层静态编进主 EXE：此时既没有 DLL 导出也没有
// 导入（旧系统审计禁止任何外部 DLL 启动导入和延迟导入）。
#if defined(TTPLAYER_VIDEO_STATIC)
#define TTPLAYER_VIDEO_API
#elif defined(TTPLAYER_VIDEO_BUILDING_DLL)
#define TTPLAYER_VIDEO_API __declspec(dllexport)
#else
#define TTPLAYER_VIDEO_API __declspec(dllimport)
#endif
