#pragma once

// 多协议音源注册表
//
// 单一入口：导入时嗅探协议 → 交给对应适配器 → 持久化到
// <exe>/musicfree/protocols/index.json → 启动时重建。对上层只暴露
// List / Describe / RemoveAt / ResolveFallback / SearchAll，导入窗口与
// 播放地址解析都不需要知道具体是哪一种协议。
//
// MusicFree 插件仍由 MusicFreeSourceManager 管理（保持原有行为不变）；
// 注册表发现 MusicFree 内容时会转交给它，并把结果包装成统一报告。

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/musicfree/source_protocol.h"

namespace ttplayer::musicfree {

struct ProtocolImportReport {
    SourceProtocol kind = SourceProtocol::kUnknown;
    std::string source_name;
    bool ok = false;
    int plugins_total = 0;   // 仅 MusicFree 注册表 JSON 有意义
    int plugins_ok = 0;
    int plugins_skipped = 0;  // 已存在而跳过的重复插件
    bool duplicated = false;  // 内容相同的音源已存在，跳过重复导入
    bool updated = false;     // 同 URL 但内容变化：替换旧条目
    std::vector<std::string> errors;
    std::string message;
};

class SourceProtocolRegistry {
public:
    SourceProtocolRegistry();
    ~SourceProtocolRegistry();
    SourceProtocolRegistry(const SourceProtocolRegistry&) = delete;
    SourceProtocolRegistry& operator=(const SourceProtocolRegistry&) = delete;

    // 首次调用时推导缓存目录（<exe>/musicfree/protocols）与依赖库目录。
    void Configure(std::filesystem::path cache_dir, NetworkConfig network,
                   std::filesystem::path lib_dir);

    void SetNetworkConfig(NetworkConfig network);

    // 导入（自动嗅探协议）。MusicFree 内容转交原管理器。
    ProtocolImportReport ImportFromUrl(std::string_view url);
    ProtocolImportReport ImportFromFile(const std::filesystem::path& path);

    // 启动时重建已持久化的非 MusicFree 音源。
    void Load();

    // 检查并应用更新（复刻 MusicFree 语义）：对每个有 source_url 的洛雪 / REST /
    // Listen1 音源带 Cache-Control: no-cache 重新下载；内容哈希相同 → 已是最新；
    // 变化 → 走导入路径按 source_url 替换旧条目（列表不新增）。MusicFree 插件的
    // 更新由 MusicFreeSourceManager::CheckForUpdates 负责，这里不碰。
    SourceUpdateReport CheckForUpdates();

    std::vector<ProtocolSourceInfo> List() const;
    std::size_t Count() const;
    bool RemoveAt(std::size_t index);
    // 批量删除（倒序处理索引、一次持久化），返回成功移除的条数。
    std::size_t RemoveMany(const std::vector<std::size_t>& indices);
    std::string Describe(std::size_t index) const;

    // MusicFree 插件解析失败时的兜底：按注册表顺序尝试洛雪 / REST 音源。
    ProtocolResolveResult ResolveFallback(const ProtocolTrack& track);

    // 可搜索的 REST 音源汇总（洛雪不提供搜索）。
    std::vector<ProtocolTrack> SearchAll(const std::string& query, int page = 1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 进程级单例（与 AppMusicFreeManager 并列）。首次调用完成路径推导。
SourceProtocolRegistry* AppSourceProtocolRegistry();

} // namespace ttplayer::musicfree
