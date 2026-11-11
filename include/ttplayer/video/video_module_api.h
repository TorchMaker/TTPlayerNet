#pragma once

// ttplayer_video 模块的**插件入口**（C ABI，稳定）。
//
// 视频层编译进独立 DLL `ttplayer_video.dll`，与主程序解耦：
//   · 宿主既可以隐式链接 C++ API（import lib），也可以完全像加载普通插件那样
//     LoadLibrary + GetProcAddress("ttpGetVideoModule") 动态接入 —— 与
//     <exe>\AddIn\ 下 ttp_v_*.dll 的 ttpGetVideoProtocol 约定同构（WINAPI 导出，
//     无需 .def 文件）。
//
// **面向未来的类型扩展**：模块的能力通过 query(api) 按名字取函数指针，而不是
// 把所有入口硬编码在结构体里。未来新增内容类型（短剧 / 漫画 / 小说 / 有声…）
// 只需在模块内新增实现并登记一个新的 api 名字 —— ABI 永不变，老宿主/新模块
// 与新宿主/老模块都能和平共处（未知名字返回 NULL，调用方按能力协商）。
//
// 已登记的 api 名字：
//   "video.cms"            → video_cms.h 的 FetchHome/FetchCategory/…（函数指针）
//   "video.resolve"        → video_resolve.h 的 ResolveVideoUrl
//   "video.source_dialog"  → ShowVideoSourceDialog
//   "video.player_window"  → ShowVideoPlayer
//   "video.source_manager" → VideoSourceManager 构造/析构（见下）
//   "video.web_host"       → SetVideoWebHostFactory / GetVideoWebHostFactory

#include <windows.h>

#define TTPLAYER_VIDEO_MODULE_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TtpVideoModule {
    unsigned size;         /* sizeof(TtpVideoModule)，向前兼容 */
    unsigned abi_version;  /* TTPLAYER_VIDEO_MODULE_ABI_VERSION */
    const char* name;      /* "ttplayer_video" */
    const char* version;   /* 模块版本字符串 */
    /* 按名字取扩展接口（函数指针）；未知/不支持的名字返回 NULL。 */
    void* (*query)(const char* api);
} TtpVideoModule;

/* 插件入口：宿主 GetProcAddress 本符号；失败返回 NULL。 */
__declspec(dllexport) const TtpVideoModule* WINAPI ttpGetVideoModule(void);

#ifdef __cplusplus
}
#endif
