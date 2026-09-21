#pragma once

// MusicFree 兼容层 —— 应用级入口（单例 + 便捷接线）
//
// 把 MusicFreeSourceManager 的创建/缓存目录推导/依赖库部署收敛到一处，供 UI
// 层（player_window / 菜单）调用，避免各处重复推导路径。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

// 诊断日志：仅在 TTPLAYER_MF_TRACE=1 时把一条自有记录追加到
// %TEMP%\ttplayer_mf_http.log（与 axios 追踪同一个文件）。用它把
// 「搜索结果 → 解析出的地址 → 入列后条目实际带上的字段」串成一条
// 可逐行核对的链路，而不必靠截图判断界面显示了什么。
void TraceDiagnostic(const std::string& tag, const std::string& text);

} // namespace ttplayer::musicfree
