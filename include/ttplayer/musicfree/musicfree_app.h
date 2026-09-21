#pragma once

// MusicFree 兼容层 —— 应用级入口（单例 + 便捷接线）
//
// 把 MusicFreeSourceManager 的创建/缓存目录推导/依赖库部署收敛到一处，供 UI
// 层（player_window / 菜单）调用，避免各处重复推导路径。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ttplayer/musicfree/musicfree_types.h"

namespace ttplayer::musicfree {

class MusicFreeSourceManager;

// 返回进程级音源管理器（首次调用时初始化）：
//  * 缓存目录：<exe 同级>/musicfree
//  * 依赖库目录：<exe 同级>/musicfree/libs（由构建阶段 stage 而来）
// 未初始化成功时返回 nullptr。
MusicFreeSourceManager* AppMusicFreeManager();

// 若 exe 同级存在 musicfree/libs 且缓存目录尚未填充，则把内置 JS 依赖库复制过去。
void EnsureMusicFreeLibsDeployed();

// 启动时重建已导入音源（调用于播放器初始化阶段）。
void LoadMusicFreeSourcesOnStartup();

// 在线搜索：由已导入音源逐一搜索并汇总。
// platform 为空时使用全部已挂载插件。
struct OnlineSearchHit {
    std::string platform;
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork;
    std::int64_t duration_ms = 0;
};

std::vector<OnlineSearchHit> SearchAllSources(const std::string& query,
                                              const std::string& platform = {},
                                              int page = 1);

// 搜索诊断：一次「全部音源搜索」里每个音源的失败原因/空结果情况。
// 音乐窗用它把“为什么没有结果”直接显示给用户，而不是只报一句“没有找到”。
struct OnlineSearchDiagnostics {
    int sources = 0;                 // 参与搜索的音源数
    int sources_with_hits = 0;       // 有结果的音源数
    std::vector<std::string> errors; // "音源名: 错误文本"
};

std::vector<OnlineSearchHit> SearchAllSources(const std::string& query,
                                              const std::string& platform,
                                              int page,
                                              OnlineSearchDiagnostics* diagnostics);

// 解析播放地址（用于把在线曲目交给音频引擎）。
// 返回空串表示失败；error 非空时写入失败原因（音乐窗会把它显示出来）。
std::string ResolvePlayUrl(const std::string& platform, const std::string& id,
                           std::string* error = nullptr);

// 解析「完整」播放源（url + 自定义请求头 headers + user_agent），供流式播放使用。
// 与 ResolvePlayUrl 不同，这里把插件 getMediaSource 返回的请求头一并带出，因为很多
// CDN（如网易云 m*.music.126.net）的签名直链需要带上 Referer/User-Agent 等头，
// Media Foundation 默认的 URL reader 发不出这些头，必须用自定义字节流喂给解码器。
// 失败返回 nullopt；error 非空时写入失败原因。
std::optional<MediaSource> ResolvePlayUrlEx(const std::string& platform,
                                            const std::string& id,
                                            std::string* error = nullptr);

// 把在线曲目的播放地址下载到本地文件（音乐窗的「下载到…」）。
//   url  : ResolvePlayUrl 解析出来的地址（宽字符，与音乐窗内部一致）。
//   path : 目标文件路径，写之前认为父目录已存在。
// 成功返回 true；失败返回 false 并把原因写入 error（若提供）。走与插件搜索同一条
// 网络链路（代理 / 超时 / 跟随重定向），二进制写入，不经过 std::string。
bool DownloadTrackFile(const std::wstring& url, const std::filesystem::path& path,
                       std::string* error = nullptr);

// ---- 下载落点的取名规则（音乐窗的「下载到…」与播放列表的「下载歌曲」共用）----

// 把 Windows 文件名里非法的字符换成 '_'，并去掉结尾的空格与点（这类名字资源管理器
// 会拒绝创建）。音源返回的歌名带 " / \ : ? * 的情况很常见。
std::wstring SanitizeFileName(const std::wstring& text);

// 从播放地址推断扩展名。CDN 地址常带 ?query 或干脆没有扩展名，推不出来按 mp3。
std::wstring GuessExtension(const std::wstring& url);

// 默认文件名「歌手 - 歌名.ext」。用音源返回的元数据，**不要**从 CDN 地址反推 ——
// 地址末段通常是哈希（播放列表那边踩过这个坑，列表里显示成一串乱码）。
std::wstring DefaultDownloadName(const std::string& title,
                                 const std::string& artist,
                                 const std::wstring& url);

// 默认下载目录：用户主目录下的 Music / 音乐 / 我的音乐，都没有就退回主目录。
// 返回空路径表示连主目录都取不到。
std::filesystem::path DefaultDownloadDirectory();

// 诊断日志：仅在 TTPLAYER_MF_TRACE=1 时把一条自有记录追加到
// %TEMP%\ttplayer_mf_http.log（与 axios 追踪同一个文件）。用它把
// 「搜索结果 → 解析出的地址 → 入列后条目实际带上的字段」串成一条
// 可逐行核对的链路，而不必靠截图判断界面显示了什么。
void TraceDiagnostic(const std::string& tag, const std::string& text);

} // namespace ttplayer::musicfree
