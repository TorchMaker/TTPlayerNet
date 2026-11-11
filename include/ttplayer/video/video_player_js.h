#pragma once

// TVBox / catvod / drpy「自定义播放器」执行器（逆向复刻）
//
// TVBox 的播放是两段式：取详情拿到 vod_play_url 只是第一段，第二段必须按
// vod_play_from 里的 flag 把播放串交给「播放器」解析：
//   · 普通 flag（m3u8 / direct / 空）→ 直接播；
//   · 匹配 parses 名的 flag      → 走对应解析接口；
//   · 其它自定义 flag            → 执行 playerUrl 指向的 JS（或 spider 的
//                                  playerContent），把加密串 / 播放页解成
//                                  真正的媒体直链 + 必须要带的请求头。
//
// 本文件实现后者：用项目里的 QuickJS 沙箱搭一套 catvod/drpy 兼容宿主，加载
// playerUrl 的 JS 并执行 playerContent(flag, id, vipFlags) / 以 flag 命名的
// 全局函数，取回 { url, header, parse }。没有 playerUrl 时整条链路不介入，
// 不影响既有的 parses / 播放页正则兜底。
//
// 复刻的宿主 API（覆盖 catvod Java spider 与 drpy/hipy 的常见子集）：
//   java.ajax / java.get / java.post / java.md5 / java.base64
//   $.fetch / $.get / $.post / $.request / $.md5 / $.base64
//   request / print / log / base64Encode / base64Decode / md5 / sha256
//   atob / btoa / localStorage(getItem,setItem) / sleep / getTimestamp
//   require("axios") / require("crypto-js")（复用 MusicFree 兼容层）
//   URL / console / env / process（复用 MusicFree 兼容层）
// 另外注入全局 flag / id / url / vipFlags / PARAMS / UA —— 部分播放器脚本
// 直接读全局变量而不是收参数。

#include <map>
#include <string>

#include "ttplayer/musicfree/musicfree_types.h"  // NetworkConfig 完整定义
#include "ttplayer/video/video_export.h"
#include "ttplayer/video/video_types.h"

namespace ttplayer::video {

using ttplayer::musicfree::NetworkConfig;

// 站点 header 字符串 → 请求头 map。
// TVBox 的 site.header 有两种写法：
//   JSON：{"Referer":"https://x.com/","User-Agent":"..."}
//   文本："Referer: https://x.com/,User-Agent: Mozilla/5.0"
// site.ua 非空时作为 User-Agent 兜底。空 key 一律丢弃。
TTPLAYER_VIDEO_API std::map<std::string, std::string> ParseVideoSiteHeaders(
    const VideoSite& site);

struct VideoPlayerJsResult {
    std::string url;                            // 解析出的最终播放地址
    std::map<std::string, std::string> headers; // 播放器要求附加的请求头
    int parse = 0;       // 1 = 返回的 url 还需再走 parses / 页面解析
    std::string detail;  // 诊断（命中的函数 / 失败原因）
    bool ok() const { return !url.empty(); }
};

// 加载并执行自定义播放器 JS。
//   player_spec —— playerUrl（http(s) / 相对路径 / 本地文件 / 直接 JS 源码 / base64）
//                  也支持用逗号分隔的多个候选，逐个尝试。
//   base_url    —— 相对路径的拼接基准（订阅地址或站点 api）
//   flag        —— vod_play_from 里的线路标识
//   play_url    —— vod_play_url 里的播放串
//   headers     —— 站点请求头（作为 HTTP 请求的默认值）
// 失败返回空 url，detail 写明原因。整体有超时与异常隔离，绝不抛出。
TTPLAYER_VIDEO_API VideoPlayerJsResult RunVideoPlayerJs(
    const std::string& player_spec, const std::string& base_url,
    const std::string& flag, const std::string& play_url,
    const NetworkConfig& cfg,
    const std::map<std::string, std::string>& headers = {});

// 判断 flag 是否属于「无需 JS 播放器」的内置类型。
// 为 true 时 ResolveVideoUrl 不会去跑 playerUrl，避免给普通 m3u8 线路平白
// 增加一次 JS 加载 + 网络往返。
TTPLAYER_VIDEO_API bool IsBuiltinPlayerFlag(const std::string& flag);

} // namespace ttplayer::video
