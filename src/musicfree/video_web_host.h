#pragma once
// WebView2 网页兜底宿主的**实现侧**声明（只给 exe / 探针工具包含）。
// 核心库只认 include/ttplayer/video/video_web_host.h 的纯接口，不包含本头。

#include <memory>

#include <windows.h>

#include "ttplayer/video/video_web_host.h"

namespace ttplayer::musicfree {

// 创建视频窗网页兜底宿主；签名与 ttplayer::video::VideoWebHostFactory 一致，
// 因此可直接 &CreateVideoWebHost 注入。失败返回 nullptr（不抛异常）。
std::unique_ptr<ttplayer::video::VideoWebHost> CreateVideoWebHost(
    HWND parent, RECT content, const std::wstring& url);

}  // namespace ttplayer::musicfree
