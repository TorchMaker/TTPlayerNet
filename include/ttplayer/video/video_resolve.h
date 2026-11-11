#pragma once

// 视频播放地址解析（影视仓 / TVBox 的 parses + 播放页爬虫）
//
// 影视仓（TVBox 系）的内容链路是三段式：
//   ① spider 爬虫   —— 从站点列表/详情里拿到的是**播放页地址**（HTML 页面）
//   ② parses 解析器 —— 把播放页地址变成**真正的媒体直链**（m3u8 / mp4）
//   ③ 播放器         —— IJK/Exo 原生流式播 m3u8（所以能秒进）
//
// 本程序只消费用户提供的配置，不内置爬虫包；但 ①② 两段里能通用化的部分
// 这里做了实现：
//   · parses 解析接口按 TVBox 约定调用（type 1=JSON / 2=HTML / 3=直链前缀）
//   · 播放页兜底爬虫：直接抓播放页 HTML，正则提取 m3u8/mp4 直链
//     （覆盖绝大多数 CMS 源；影视仓的 spider 本质也是做这件事）
// 真正需要执行站点私有 JS 的 spider（csp_XXX / drpy）仍不在范围内 —— 那需要
// 完整的 TVBox 爬虫运行时。

#include <map>
#include <string>
#include <vector>

#include "ttplayer/musicfree/musicfree_types.h"  // NetworkConfig 完整定义
#include "ttplayer/video/video_export.h"
#include "ttplayer/video/video_types.h"

namespace ttplayer::video {

// NetworkConfig 完整定义在 ttplayer::musicfree；在此引入，避免被同名前向声明遮蔽。
using ttplayer::musicfree::NetworkConfig;

struct VideoResolveResult {
    std::string url;      // 解析出的媒体直链（空串 = 没解析出来）
    std::string method;   // 命中的方式：direct / player / parse3 / parse1 / parse2 / page
    std::string detail;   // 诊断信息（失败原因、命中的解析器名等）
    // 播放器 / 解析接口要求附加的请求头（Referer、UA、鉴权…）。调用方必须把它
    // 并进播放请求的 open_headers，否则 m3u8 主索引能下、切片 403。
    std::map<std::string, std::string> headers;
};

// 解析一个播放地址 → 媒体直链。
//   raw_url    —— 选集原始地址（可能是 CMS 的 /play/xxx 播放页，也可能是直链）
//   parses     —— 该来源配置里的解析接口列表（按优先级顺序尝试）
//   headers    —— 站点自定义请求头（播放页/解析接口都要带）
//   flag       —— vod_play_from 的线路标识（TVBox 用它派发播放器）；空=无
//   player_url —— 订阅 playerUrl / 站点 player（自定义播放器 JS）；空=无
//   base_url   —— player_url 为相对路径时的拼接基准（订阅地址 / 站点 api）
// 全部失败时返回空 url，detail 写明原因。
TTPLAYER_VIDEO_API VideoResolveResult ResolveVideoUrl(
    const std::string& raw_url,
    const std::vector<VideoParse>& parses,
    const NetworkConfig& cfg,
    const std::map<std::string, std::string>& headers = {},
    const std::string& flag = {},
    const std::string& player_url = {},
    const std::string& base_url = {});

// 判断地址是否已经是可直接播放的媒体直链（m3u8/mp4/flv/mkv/ts…）。
// 与播放窗里的 LooksPlayableMediaUrl 同义，这里独立一份避免跨模块依赖。
TTPLAYER_VIDEO_API bool IsDirectMediaUrl(const std::string& url);

// 剥掉 TVBox 播放串里内联的请求头，返回纯播放地址：
//   https://x/a.m3u8${"Referer":"https://y/"}
//   https://x/a.m3u8@Referer=https://y/@User-Agent=Mozilla/5.0
//   https://x/a.m3u8&{"Referer":" https://y/"}
// 这些是 catvod spider 的 playerContent 常见返回形态。若整串带着后缀就被
// 当成 URL 请求，CDN 必然 403/404 —— 必须从串里拆出来并进 open_headers。
// 无内联头时原样返回 url，headers 不变。
TTPLAYER_VIDEO_API std::string ParseInlinePlayHeaders(
    const std::string& url, std::map<std::string, std::string>& headers);

// 判断地址是否是网盘分享页（夸克 / UC / 百度 / 阿里 / 115 / 123 / 天翼 /
// 迅雷 / 蓝奏…）。这类地址必须登录网盘（还要转存）才可能出视频 —— 用户明确
// 不要：详情聚合会剔除整条全为网盘链接的线路，播放窗网页兜底也不会打开它们。
TTPLAYER_VIDEO_API bool IsCloudShareUrl(const std::string& url);

// 从一段 HTML / JSON 文本里提取媒体直链（正则兜底爬虫）。
// 返回空串表示没找到。
TTPLAYER_VIDEO_API std::string ExtractMediaUrlFromText(const std::string& text);

// 判断地址是否是「数据接口」——请求它返回 JSON（如 6huo 的
// ?view=api&mode=download-data、CMS 的 ac=detail），而不是网页播放页。
// 这类地址绝不能交给 WebView2 兜底（用户只会看到一屏 JSON 文本）。
TTPLAYER_VIDEO_API bool LooksLikeJsonApiUrl(const std::string& url);

// 直接请求一个数据接口，把返回 JSON 里的媒体直链挖出来
// （movurl / url / playurl / m3u8 / mp4 …，也支持正则兜底）。
// 返回空串表示没拿到直链。
TTPLAYER_VIDEO_API std::string ResolveJsonApiMedia(
    const std::string& url, const NetworkConfig& cfg,
    const std::map<std::string, std::string>& headers = {});

} // namespace ttplayer::video
