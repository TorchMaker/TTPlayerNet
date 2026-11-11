#pragma once

// 视频源兼容层 —— 视频源管理器（手动导入 / 持久化）
//
// 与 MusicFree 音源管理器一致：用户粘贴影视仓 / TVBox 配置 URL 或选择本地 JSON，
// 本管理器只消费用户提供的来源，不内置任何视频源。导入后把站点清单持久化到
// <exe>/video/protocols/index.json，下次启动自动加载；支持失败跳过、去重、卸载。

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/video/video_export.h"
#include "ttplayer/video/video_types.h"

namespace ttplayer::video {

// NetworkConfig 完整定义在 ttplayer::musicfree；在此引入，避免被同名前向声明遮蔽。
using ttplayer::musicfree::NetworkConfig;

// 只有跨 DLL 边界调用的非 inline 成员才标注 TTPLAYER_VIDEO_API（成员级导出，
// 避免整类导出带来的 C4251）。inline 成员在每个使用方 TU 内本地实例化。
class VideoSourceManager {
public:
    TTPLAYER_VIDEO_API explicit VideoSourceManager(std::filesystem::path cache_dir);
    TTPLAYER_VIDEO_API ~VideoSourceManager();

    VideoSourceManager(const VideoSourceManager&) = delete;
    VideoSourceManager& operator=(const VideoSourceManager&) = delete;

    void SetNetworkConfig(NetworkConfig cfg) { network_ = std::move(cfg); }
    const NetworkConfig& network() const { return network_; }
    const std::filesystem::path& cache_dir() const { return cache_dir_; }

    // 后台刷新线程写完 index.json 后，在 UI 线程重新载入（不下载）。
    TTPLAYER_VIDEO_API void Reload();

    struct ImportReport {
        std::string source_name;
        int sources_total = 0;
        int sources_ok = 0;
        int sites_total = 0;
        // 本程序真能取内容的站点数（CMS + 直播）。SPIDER 不算 —— 那些 api 是
        // TVBox 里的 Java 类名，没有对应运行库就永远拉不到数据，报「受支持」
        // 只会让人以为导入成功却一片空白。
        int sites_supported = 0;
        int sites_cms = 0;
        int sites_spider = 0;
        // 已复刻成 C++ 原生实现、可直接拉取的 csp_* 站点数（计入 sites_supported，
        // 但不计入 sites_spider）。
        int sites_native = 0;
        // 由加密 ext 自动还原成可播 CMS 的站点数。
        int sites_converted = 0;
        int lives_total = 0;
        bool duplicated = false;
        // 本次导入命中了哪种解码方式（json_plain / image_marker_base64 /
        // aes_cbc_self_2423 / …），空串表示走了裸列表分支。
        std::string decode_kind;
        std::string message;
        std::vector<std::string> errors;
    };

    TTPLAYER_VIDEO_API ImportReport ImportFromUrl(std::string_view url);
    TTPLAYER_VIDEO_API ImportReport ImportFromFile(const std::filesystem::path& path);

    // 所有已导入来源。
    TTPLAYER_VIDEO_API std::vector<ImportedVideoSource> ListSources() const;

    // 所有能拉首页内容的站点（CMS + 原生 spider；SPIDER / 直播不走这条链路）。
    TTPLAYER_VIDEO_API std::vector<VideoSite> AllSites() const;

    // 所有直播源（lives），group 为空时用所属来源名兜底，供首页「直播」分组用。
    TTPLAYER_VIDEO_API std::vector<VideoLive> AllLives() const;

    // 所有来源的解析接口（parses）合并去重，按「直链 → JSON → HTML」排序。
    // 播放时用它把「播放页地址」变成真实媒体直链（影视仓的 parses 机制）。
    TTPLAYER_VIDEO_API std::vector<VideoParse> AllParses() const;

    // 所有来源的订阅级 playerUrl（TVBox 自定义播放器 JS）合并去重。
    TTPLAYER_VIDEO_API std::vector<std::string> AllPlayerUrls() const;

    // 某站点播放时应使用的 player JS：优先站点级 player 字段，否则回退到它
    // 所属来源的订阅级 playerUrl。找不到返回空串。
    TTPLAYER_VIDEO_API std::string PlayerUrlForSite(std::string_view site_key) const;

    // 按 name 卸载整个来源。
    TTPLAYER_VIDEO_API bool RemoveSource(std::string_view name);

    // 启动时重建：读入 index.json 中的所有站点（无需重新下载）。
    TTPLAYER_VIDEO_API void LoadCached();

    struct RefreshReport {
        int checked = 0;    // 有 source_url、被检查的源数
        int updated = 0;    // 重新下载并成功解密、已替换的源数
        int failed = 0;     // 下载/解析失败的源数（旧数据保持不变）
        std::vector<std::string> errors;
    };

    // 自动更新：按 source_url 重新下载并走同一套自动解密链，成功后原地替换
    // 站点清单。供后台线程调用；单个源失败不影响其它源，也不会清空已有数据。
    // 返回后调用方应在 UI 线程重新聚合首页。
    TTPLAYER_VIDEO_API RefreshReport RefreshAll();

    const std::vector<ImportedVideoSource>& Imported() const { return imported_; }

private:
    ImportReport ImportText(std::string_view text, std::string_view origin,
                            std::string_view source_url,
                            const std::filesystem::path& local_path);
    void Persist();
    void LoadFromIndex();
    void SaveRaw(const std::filesystem::path& path, std::string_view text);

    std::filesystem::path cache_dir_;
    std::filesystem::path index_path_;
    NetworkConfig network_;
    mutable std::mutex mutex_;
    std::vector<ImportedVideoSource> imported_;
};

} // namespace ttplayer::video
