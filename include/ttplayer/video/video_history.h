#pragma once

// 视频观看历史（播放列表「视频」栏目 / 续播进度）。
//
// 记录的是**解析后的完整信息**（整部剧的选集、每集的多线路、当前集、进度），
// 不是单纯的播放直链：再次打开时可原样恢复「切换源 / 选集」，直链失效时也能
// 按 site + detail_id + 片名重新走一遍详情解析（见 bridge_cmds_video.cpp 的
// OpenVideoFromHistory）。
//
// 落盘位置：<exe 目录>\video\history.json（原子写：.tmp → rename）。
// 诊断日志：<exe 目录>\video\history.log（每次记录 / 打开 / 续播 / 重解析）。

#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/video/video_export.h"
#include "ttplayer/video/video_types.h"        // VideoParse（真正定义）
#include "ttplayer/video/video_player_window.h"

namespace ttplayer::skin {
struct BrowserSkin;  // 前向声明：外框皮肤由调用方提供（与 ShowVideoPlayer 同款）
}

namespace ttplayer::video {

struct VideoHistoryEntry {
    std::wstring id;          // 稳定键：site|detail_id（同片重复观看只留一条）
    std::wstring title;       // 片名 / 剧名
    std::wstring site;        // 站点 key（重新解析用）
    std::wstring detail_id;   // 站点内的详情编号（vod_id，重新解析用）
    std::wstring cover;       // 封面图 URL
    std::wstring line;        // 当前线路名
    std::vector<VideoPlayerEpisode> episodes;  // 整部剧（含每集多线路）
    int current_index{};      // 当前集下标（电影为 0）
    int position_ms{};        // 已看进度（毫秒）
    int duration_ms{};        // 总时长（毫秒，未知为 0）
    long long updated_at{};   // Unix 秒，列表按它倒序
};

// 组装稳定键：site + "|" + key。key 优先用详情编号，没有就用播放地址。
TTPLAYER_VIDEO_API std::wstring MakeVideoHistoryId(const std::wstring& site,
                                                   const std::wstring& key);

// 读取全部历史（最近观看在前）。
TTPLAYER_VIDEO_API std::vector<VideoHistoryEntry> LoadVideoHistory();

// 新增 / 合并一条历史（按 id），并把时间戳刷新为当前时间。
TTPLAYER_VIDEO_API bool UpsertVideoHistory(VideoHistoryEntry entry);

// 只更新进度（播放中的定时落盘走这里；id 不存在时忽略）。
TTPLAYER_VIDEO_API bool UpdateVideoHistoryProgress(const std::wstring& id,
                                                   int current_index,
                                                   int position_ms,
                                                   int duration_ms);

// 删除一条历史（播放列表「视频」栏目里删除行时调用，避免下次刷新又冒出来）。
TTPLAYER_VIDEO_API bool RemoveVideoHistory(const std::wstring& id);

// 按 id 取一条（找不到返回 false）。
TTPLAYER_VIDEO_API bool FindVideoHistory(const std::wstring& id,
                                         VideoHistoryEntry* out);

// 诊断日志：写 <exe>\video\history.log（线程安全，不抛异常）。
TTPLAYER_VIDEO_API void VideoHistoryLog(const std::string& message);

// 从历史打开播放窗（主窗口播放列表「视频」栏目双击时调用）。
// 优先用保存的解析结果直接播；选集/地址缺失时按 site + detail_id + 片名
// 重新走一遍详情解析（重新解析），成功则回写历史。返回 false 表示找不到
// 记录或重新解析后仍无可用地址（原因见 history.log / player_ui.log）。
// browser_skin 与音乐窗打开视频时传的是同一个皮肤（<browser_window> 段），
// 保证两条入口出来的播放窗外观完全一致。
TTPLAYER_VIDEO_API bool OpenVideoFromHistory(
    const std::wstring& id, HWND owner,
    const skin::BrowserSkin* browser_skin = nullptr);

}  // namespace ttplayer::video
