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

#include <cstddef>
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
        int plugins_skipped = 0;   // 内容/URL 已存在而跳过的重复插件
        bool duplicated = false;   // 整个来源已导入过，没有新增
        std::string message;       // 去重 / 更新提示
        std::vector<std::string> errors;
    };
    ImportReport ImportFromUrl(std::string_view url);

    // 从本地文件导入（与 ImportFromUrl 同逻辑，省去首层下载）。
    ImportReport ImportFromFile(const std::filesystem::path& path);

    // 已加载、可使用的插件列表（platform 名 + 能力）。
    std::vector<PluginMeta> ListPlugins() const;

    // 按 platform 名取插件（未找到返回 nullptr）。
    MusicFreePlugin* FindPlugin(std::string_view platform);

    // 卸载某个已导入音源（按注册表/插件名，整批删除）。
    bool RemoveSource(std::string_view name);

    // 按 ListPlugins() 的索引卸载**单个插件实例**：同时清掉它的缓存文件、
    // 内存实例与持久化记录。列表里重复的同源插件会一起清理（它们共用同一个
    // 缓存文件，留着会在下次启动时被 LoadCached 重新挂载）。
    // 索引越界返回 false。
    bool RemovePluginAt(std::size_t index);

    // 按 ListPlugins() 的下标一次删除多行（导入窗口的复选批量删除）。
    // 共用同一缓存文件的重复实例会一并清掉，索引漂移在这里统一处理，
    // 并且只在最后 Persist() 一次。返回实际移除的插件实例数。
    std::size_t RemovePluginsAt(const std::vector<std::size_t>& indices);

    // 返回某个插件的详细信息（UTF-8 多行文本），供导入窗口的「查看」使用。
    // 索引越界返回空串。
    std::string DescribePlugin(std::size_t index) const;

    // 启动时为每个缓存的插件 JS 自动重建挂载。
    void LoadCached();

    // 检查并应用更新（复刻 MusicFree 的 updatePlugin：对每个有来源 URL 的插件
    // 带 Cache-Control: no-cache 重新下载；内容哈希相同 → 已是最新；本地版本
    // 更新 → 拒绝降级；否则原地替换缓存 JS 并重挂载）。本地导入（无 URL）的
    // 插件记为 no_url 跳过。会写回 imported.json。
    SourceUpdateReport CheckForUpdates();

    const std::vector<ImportedSource>& Imported() const { return imported_; }

private:
    ImportReport ImportRegistryJson(const nlohmann::json& registry,
                                    std::string_view registry_url,
                                    const std::filesystem::path& base);
    // plugins_ / plugin_files_ / plugin_hashes_ 必须**始终等长**：挂载一个插件
    // 就登记它的缓存路径与内容哈希，删除时三个数组同一个下标一起移除。
    // 导入窗口的列表顺序就是 ListPlugins() 的顺序，因此右键/按钮可以直接把
    // 下标交给 RemovePluginAt / RemovePluginsAt。
    void ErasePluginAt(std::size_t index);
    // 导入去重：内容哈希、缓存文件、插件 URL 任一命中即视为同一个音源。
    bool HasContentHash(std::string_view hash) const;
    bool HasPluginFile(const std::filesystem::path& fp) const;
    bool HasPluginUrl(std::string_view url) const;
    void Persist();

    std::filesystem::path cache_dir_;
    std::filesystem::path index_path_;
    NetworkConfig network_;
    std::string app_version_ = "0.1.0";

    mutable std::mutex mutex_;
    std::vector<ImportedSource> imported_;
    std::vector<std::unique_ptr<MusicFreePlugin>> plugins_;
    std::vector<std::filesystem::path> plugin_files_;  // 与 plugins_ 一一对应
    std::vector<std::string> plugin_hashes_;          // 与 plugins_ 一一对应
};

} // namespace ttplayer::musicfree
