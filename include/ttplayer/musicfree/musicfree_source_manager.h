#pragma once

// MusicFree 兼容层 —— 音源管理器（手动导入 / 持久化）
//
// 对应 MusicFree 的 PluginManager：负责
//  * 解析"音源注册表"JSON（如 yuanli.json：{ "plugins": [ {name,url,version} ] }）
//    或单个插件 JS；
//  * 下载并挂载每个插件（过程隔离、可失败跳过）；
//  * 把已导入的音源清单持久化到 TTPlayer 设置目录，下次启动自动加载；
//  * 列出可用音源、按名检索、卸载。
//
// 与 MusicFree 一致：导入是"手动"的——用户粘贴 URL 或选择本地 JSON，
// 本管理器只消费用户提供的来源，不内置任何音源。

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ttplayer/musicfree/musicfree_types.h"

namespace ttplayer::musicfree {

class MusicFreePlugin;

// 已导入音源的持久化记录
struct ImportedSource {
    std::string name;          // 注册表名或插件 platform
    std::string registry_url;  // 来源 URL（本地文件则为空）
    std::filesystem::path local_path; // 本地缓存的 JS/JSON
    std::vector<std::string> plugin_urls; // 该来源包含的插件 URL
    std::vector<std::filesystem::path> plugin_files; // 缓存的插件 JS 文件
    std::string version;
};

class MusicFreeSourceManager {
public:
    explicit MusicFreeSourceManager(std::filesystem::path cache_dir);
    ~MusicFreeSourceManager();

    MusicFreeSourceManager(const MusicFreeSourceManager&) = delete;
    MusicFreeSourceManager& operator=(const MusicFreeSourceManager&) = delete;

    // 设置网络配置（用于下载与插件内 axios）。
    void SetNetworkConfig(NetworkConfig cfg) { network_ = std::move(cfg); }
    // 读回当前网络配置：下载在线曲目（DownloadTrackFile）要走同一条代理/超时链路。
    const NetworkConfig& network() const { return network_; }
    void SetAppVersion(std::string_view v) { app_version_ = v; }

    // 首次启动时把内置的纯 JS 依赖库（crypto-js/dayjs/qs/big-integer/he）复制到
    // cache_dir_/libs，供插件 require 使用。from 为部署时 libs 目录（如 exe 同级的
    // musicfree/libs）。已存在则跳过。
    void InstallBundledLibs(const std::filesystem::path& from);

    // 从一个 URL 导入音源：若内容为注册表 JSON({plugins:[...]}) 则逐个下载挂载；
    // 若为单个插件 JS 则直接挂载。返回导入结果摘要。
    struct ImportReport {
        std::string source_name;
        int plugins_total = 0;
        int plugins_ok = 0;
        std::vector<std::string> errors;
    };
    ImportReport ImportFromUrl(std::string_view url);

    // 从本地文件导入（与 ImportFromUrl 同逻辑，省去首层下载）。
    ImportReport ImportFromFile(const std::filesystem::path& path);

    // 已加载、可使用的插件列表（platform 名 + 能力）。
    std::vector<PluginMeta> ListPlugins() const;

    // 按 platform 名取插件（未找到返回 nullptr）。
    MusicFreePlugin* FindPlugin(std::string_view platform);

    // 卸载某个已导入音源（按注册表/插件名）。
    bool RemoveSource(std::string_view name);

    // 启动时为每个缓存的插件 JS 自动重建挂载。
    void LoadCached();

    const std::vector<ImportedSource>& Imported() const { return imported_; }

private:
    ImportReport ImportRegistryJson(const nlohmann::json& registry,
                                    std::string_view registry_url,
                                    const std::filesystem::path& base);
    void Persist();

    std::filesystem::path cache_dir_;
    std::filesystem::path index_path_;
    NetworkConfig network_;
    std::string app_version_ = "0.1.0";

    mutable std::mutex mutex_;
    std::vector<ImportedSource> imported_;
    std::vector<std::unique_ptr<MusicFreePlugin>> plugins_;
};

} // namespace ttplayer::musicfree
