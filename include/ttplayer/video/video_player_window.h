#pragma once

// 视频播放窗口：弹出式，外框复用音乐窗主题，内部为视频渲染面 + 传输控制。

#include <map>
#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/video/video_export.h"

namespace ttplayer::skin {
struct BrowserSkin;  // 仅前向声明；完整定义由调用方提供的 skin.h 引入
}

namespace ttplayer::video {

// 一条可切换的播放源（线路）：影视仓式「同一集的多线路」。
struct VideoPlayerSource {
    std::wstring url;
    std::wstring name;   // 线路名（站点名），空则显示「线路 N」
};

// 解析接口（影视仓 parses）：播放窗在拿到「播放页地址」时用它换真实媒体直链。
struct VideoParse;

// 弹出视频播放窗口。url 为当前源的可播放直链；title 显示片名；line_label 显示
// 当前线路/选集；headers 为可选请求头（best-effort）。sources 为同一集的
// 全部可切换线路（含当前源，为空表示单一线路、不显示切换按钮）。
// parses 为解析接口列表：url 不是媒体直链时，播放窗会在后台调用它们把地址
// 解析成 m3u8/mp4（影视仓的 parses 机制），解析期间显示「正在解析播放地址…」。
// 外框复用音乐窗的 <browser_window> 皮肤边框（browser_skin 非空时）；
// 为空则退回自绘深色边框。
TTPLAYER_VIDEO_API void ShowVideoPlayer(
    HWND owner, const std::wstring& url, const std::wstring& title,
    const std::wstring& line_label,
    const skin::BrowserSkin* browser_skin = nullptr,
    const std::map<std::string, std::string>& headers = {},
    const std::vector<VideoPlayerSource>& sources = {},
    const std::vector<VideoParse>& parses = {},
    const std::string& player_url = {},
    const std::string& player_base_url = {});

} // namespace ttplayer::video
