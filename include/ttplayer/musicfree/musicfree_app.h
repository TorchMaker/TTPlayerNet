#pragma once

// MusicFree 兼容层 —— 应用级入口（单例 + 便捷接线）
//
// 把 MusicFreeSourceManager 的创建/缓存目录推导/依赖库部署收敛到一处，供 UI
// 层（player_window / 菜单）调用，避免各处重复推导路径。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

// 启动时重建已导入音源（同步版本，仅供自检/内部使用）。
void LoadMusicFreeSourcesOnStartup();

// ---- 启动加速：把音源重建移出窗口显示的关键路径 ----
//
// LoadCached() 会为每个缓存插件新建一个 QuickJS runtime 并 eval 插件脚本；
// 20 个插件实测约 1.9s，若放在主窗口创建之后、消息循环之前，窗口虽然已经
// ShowWindow 却要等这段同步工作结束才能响应，表现为「启动很慢才显示窗口」。
// 现在由后台线程完成，主线程创建完窗口立即进入消息循环。
//
// notify_hwnd / notify_msg：后台完成后 PostMessage 通知主线程做依赖音源的
// 延后工作（恢复音乐窗、恢复播放）。均可为空。
void StartMusicFreeSourcesLoading(void* notify_hwnd, unsigned notify_msg);

// 等待后台重建完成。所有需要完整音源列表的入口（搜索、解析、导入窗、
// 音乐窗）都会调用它；已完成时是零开销的原子判断，未完成时才阻塞。
// 未调用 StartMusicFreeSourcesLoading（如工具进程）时会同步加载一次。
void WaitForMusicFreeSources();

// 消息循环退出后回收后台线程。会请求取消尚未完成的挂载，最多再等当前
// 这一个插件（约 0.3s）。必须在主线程结束前调用。
void FinishMusicFreeSourcesLoading();

// 供 LoadCached() 在插件之间检查取消请求；进程正在退出时提前收工。
bool ShouldStopMusicFreeLoading();

// ---- 音源自动更新（复刻 MusicFree 的插件更新机制）----

// 检查并应用全部音源更新：MusicFree 插件走 MusicFreeSourceManager，
// 洛雪 / REST / Listen1 走 SourceProtocolRegistry，返回合并报告。
// 复刻原版语义：重下载（no-cache）→ 哈希相同视为已最新 → 版本比较拒绝降级
// → 原地替换并重挂载。供导入窗「检查更新」按钮调用。
SourceUpdateReport CheckAllSourceUpdates();

// 启动自动检查：开关开启且距上次检查 ≥ 24 小时才真正执行，否则立即返回。
// 在启动后台线程里调用（StartMusicFreeSourcesLoading 的收尾段）。
void MaybeAutoUpdateSources();

// 「启动时自动检查音源更新」开关（持久化到 <exe>/musicfree/update_state.json）。
bool AutoUpdateSourcesEnabled();
void SetAutoUpdateSourcesEnabled(bool enabled);

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
    // 音源返回的完整对象（咪咕的 contentId / copyrightId、洛雪的 quality 等
    // 私有字段都在这里）。解析播放地址时原样回传，别只用 id/platform 重建 ——
    // 那正是「插件调用被拒绝」的根因。入列时会一并落进播放列表元数据袋。
    nlohmann::json raw;
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
// raw 为搜索结果/播放列表条目里保存的完整音源对象；传入后插件能拿到私有字段，
// 同时洛雪 / REST 音源可以用 title/artist/album 等元数据做兜底解析。
std::string ResolvePlayUrl(const std::string& platform, const std::string& id,
                           std::string* error = nullptr,
                           const nlohmann::json* raw = nullptr);

// 解析「完整」播放源（url + 自定义请求头 headers + user_agent），供流式播放使用。
// 与 ResolvePlayUrl 不同，这里把插件 getMediaSource 返回的请求头一并带出，因为很多
// CDN（如网易云 m*.music.126.net）的签名直链需要带上 Referer/User-Agent 等头，
// Media Foundation 默认的 URL reader 发不出这些头，必须用自定义字节流喂给解码器。
// 失败返回 nullopt；error 非空时写入失败原因。
std::optional<MediaSource> ResolvePlayUrlEx(const std::string& platform,
                                            const std::string& id,
                                            std::string* error = nullptr,
                                            const nlohmann::json* raw = nullptr);

// 播放或下载失败时丢弃该曲目的解析结果缓存：CDN 直链可能在 TTL 内提前失效，
// 不清掉的话下一次播放会继续拿到同一个坏地址。
void InvalidateResolveCache(const std::string& platform, const std::string& id);

// 从播放列表条目的元数据袋取回音源原始对象。优先读 AddOnlineTrack 写入的
// "SourceRaw"（完整字段）；旧条目没有该键时，用 Title/Artist/Album/Platform/
// SongId 拼一个最小对象，保证洛雪音源至少能按歌名+歌手去取直链。
nlohmann::json RestoreSourceRaw(
    const std::vector<std::pair<std::string, std::string>>& metadata);

// ---- 内容浏览：排行榜 / 推荐歌单（数据来自 MusicFree 插件）----
//
// 洛雪音源协议（user-api）只暴露 musicUrl / lyric / pic，**没有**排行榜、推荐、
// 歌单能力 —— 这些能力只存在于洛雪桌面客户端（它直接调各平台的 API）。因此本兼容层
// 的「推荐 / 封面」内容完全来自 MusicFree 插件（getTopLists / getRecommend* /
// getMusicSheetInfo），与洛雪源协议无关。

enum class SheetKind { kTopList, kSheet };

// 汇总所有支持 getTopLists 的插件，返回带 platform 的排行榜列表。
std::vector<MusicSheet> GetAllTopLists();

// 取某音源某标签下的推荐歌单（getRecommendSheetsByTag）。
std::vector<MusicSheet> GetRecommendSheets(const std::string& platform,
                                           const nlohmann::json& tag, int page = 1);

// 取某音源的推荐歌单标签（getRecommendSheetTags）。
std::vector<SheetTag> GetRecommendTags(const std::string& platform);

// 取歌单 / 排行榜详情里的曲目（getTopListDetail 或 getMusicSheetInfo，由 kind 决定）。
// 返回空 vector 表示失败；错误信息写入 error。
struct SheetTracksOutcome {
    std::vector<MusicItem> items;
    bool is_end = true;
    std::string error;
};
SheetTracksOutcome GetSheetTracks(const std::string& platform,
                                 const nlohmann::json& sheet, SheetKind kind,
                                 int page = 1);

// 列出支持某内容能力的音源 platform 名（供音乐窗下拉框）。
std::vector<std::string> ListTopListPlatforms();    // can_top_lists
std::vector<std::string> ListRecommendPlatforms();  // can_recommend

// ---- 发现页聚合（单页展示所有音源，逐音源渐进加载）----
//
// 每个音源独立完成一次聚合，UI 收到一批就追加一组卡片，慢音源 / 失效音源
// 不会拖住其它音源，也不会阻塞窗口。内部会等待后台插件挂载完成，因此
// 调用方必须在工作线程里使用。
struct DiscoverOutcome {
    std::vector<MusicSheet> sheets;  // 排行榜 + 推荐歌单，带 platform / raw
    std::string error;               // 诊断信息（可为空）
};
std::vector<std::string> ListDiscoverPlatforms();  // can_top_lists || can_recommend
DiscoverOutcome GetDiscoverSheets(const std::string& platform,
                                  int max_top = 12, int max_recommend = 8);

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
