#pragma once

// 视频源兼容层 —— CMS provide/vod 与直播客户端
//
// 实现影视仓 / TVBox 系最核心的内容链路：
//   * CMS（type 0/1/3，api 形如 .../api.php/provide/vod）：home / category /
//     search / detail，解析 MacCMS 的 list / class / vod_play_from / vod_play_url；
//   * 直播（m3u_vod / m3u / txt）：解析分组与频道。
// 失败即抛 std::runtime_error，由上层（聚合器）按「跳过异常源」策略处理。

#include <string>
#include <string_view>
#include <vector>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/video/video_export.h"
#include "ttplayer/video/video_types.h"

namespace ttplayer::video {

// NetworkConfig 完整定义在 ttplayer::musicfree；引入以使用其完整类型。
using ttplayer::musicfree::NetworkConfig;

// 合并站点的 UA / header 到请求头表。default_ua 取 NetworkConfig::user_agent（wstring）。
TTPLAYER_VIDEO_API std::map<std::string, std::string> BuildSiteHeaders(
    const VideoSite& site, std::wstring_view default_ua);

// 拼 CMS 请求 URL（处理 ? 与 &，并补 ext）。
TTPLAYER_VIDEO_API std::string BuildCmsUrl(
    const VideoSite& site, const std::map<std::string, std::string>& params);

// 首页：返回分类 + 第一页卡片。
TTPLAYER_VIDEO_API VideoListing FetchHome(const VideoSite& site, const NetworkConfig& cfg);

// 分类翻页。
TTPLAYER_VIDEO_API VideoListing FetchCategory(const VideoSite& site,
                                              std::string_view category_id,
                                              int page, const NetworkConfig& cfg);

// 搜索翻页。
TTPLAYER_VIDEO_API VideoListing FetchSearch(const VideoSite& site,
                                            std::string_view keyword,
                                            int page, const NetworkConfig& cfg);

// 详情：取选集（多路线路）。
TTPLAYER_VIDEO_API VideoDetail FetchDetail(const VideoSite& site, const VideoItem& item,
                                           const NetworkConfig& cfg);

// 直播：解析 m3u / txt，返回分组与频道（频道即播放地址）。
struct LiveChannel {
    std::string name;
    std::string group;
    std::string url;
};
struct LiveListing {
    std::vector<LiveChannel> channels;
    std::string error;
};
TTPLAYER_VIDEO_API LiveListing FetchLive(const VideoLive& live, const NetworkConfig& cfg);

} // namespace ttplayer::video
