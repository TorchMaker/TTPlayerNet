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

// 一集（剧集）：播放窗「选集」菜单据此在不同剧集之间切换。
// sources 是该集自己的多线路（换集后「切换源」列表跟着换，而不是沿用上一集）。
struct VideoPlayerEpisode {
    std::wstring title;                      // 集名（菜单显示）
    std::wstring url;                        // 该集播放地址（当前线路）
    std::wstring line;                       // 线路名
    std::vector<VideoPlayerSource> sources;  // 该集的全部线路
};

// 弹出视频播放窗口。url 为当前源的可播放直链；title 显示片名；line_label 显示
// 当前线路/选集；headers 为可选请求头（best-effort）。sources 为同一集的
// 全部可切换线路（含当前源，为空表示单一线路、不显示切换按钮）。
// parses 为解析接口列表：url 不是媒体直链时，播放窗会在后台调用它们把地址
// 解析成 m3u8/mp4（影视仓的 parses 机制），解析期间显示「正在解析播放地址…」。
// 外框复用音乐窗的 <browser_window> 皮肤边框（browser_skin 非空时）；
// 为空则退回自绘深色边框。
// start_ms > 0：首帧就绪后跳转到该毫秒位置（播放列表「视频」栏目的续播）。
// history_id 非空：播放中/关窗时把进度写回观看历史（见 video_history.h）。
TTPLAYER_VIDEO_API void ShowVideoPlayer(
    HWND owner, const std::wstring& url, const std::wstring& title,
    const std::wstring& line_label,
    const skin::BrowserSkin* browser_skin = nullptr,
    const std::map<std::string, std::string>& headers = {},
    const std::vector<VideoPlayerSource>& sources = {},
    const std::vector<VideoParse>& parses = {},
    const std::string& player_url = {},
    const std::string& player_base_url = {},
    const std::vector<VideoPlayerEpisode>& episodes = {},
    int start_ms = 0, const std::wstring& history_id = {});

} // namespace ttplayer::video
