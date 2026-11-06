// MusicFree 兼容层 —— 音源管理器（手动导入 / 持久化）
//
// 对应 MusicFree 的 PluginManager：解析"音源注册表"JSON（如 yuanli.json：
// { "plugins": [ {name,url,version} ] }）或单个插件 JS，下载、挂载、持久化，
// 并在启动时自动重建。导入完全是“手动”的——只消费用户提供的来源。

#include "ttplayer/musicfree/musicfree_source_manager.h"

#include "ttplayer/app/startup_trace.h"
#include "ttplayer/musicfree/musicfree_app.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_plugin.h"

namespace ttplayer::musicfree {

namespace {
std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
void WriteAll(const std::filesystem::path& p, std::string_view s) {
    std::ofstream f(p, std::ios::binary); f.write(s.data(), (std::streamsize)s.size());
}
std::string HashName(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    std::ostringstream os; os << std::hex << h; return os.str();
}

// 从所有持久化记录里摘掉某个缓存文件。同一个 URL 被重复导入时，多条
// ImportedSource 会指向同一个 plugin_<hash>.js，必须一次全摘干净，否则
// 下次启动 LoadCached 还会把它挂回来。plugin_urls 与 plugin_files 平行时
// 同步删掉对应 URL。
void DropPluginFile(std::vector<ImportedSource>& sources,
                    const std::filesystem::path& fp) {
    for (auto it = sources.begin(); it != sources.end();) {
        auto& files = it->plugin_files;
        for (;;) {
            auto f = std::find(files.begin(), files.end(), fp);
            if (f == files.end()) break;
            const std::size_t pos = static_cast<std::size_t>(f - files.begin());
            if (it->plugin_urls.size() == files.size())
                it->plugin_urls.erase(it->plugin_urls.begin() +
                                      static_cast<std::ptrdiff_t>(pos));
            files.erase(f);
        }
        if (files.empty()) it = sources.erase(it);
        else ++it;
    }
}
} // namespace

MusicFreeSourceManager::MusicFreeSourceManager(std::filesystem::path cache_dir)
    : cache_dir_(std::move(cache_dir)) {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_ / "plugins", ec);
    std::filesystem::create_directories(cache_dir_ / "libs", ec);
    index_path_ = cache_dir_ / "imported.json";
}

MusicFreeSourceManager::~MusicFreeSourceManager() = default;

void MusicFreeSourceManager::InstallBundledLibs(const std::filesystem::path& from) {
    static const char* kLibs[] = {"crypto-js.js", "dayjs.js", "qs.js", "big-integer.js", "he.js"};
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_ / "libs", ec);
    for (auto* name : kLibs) {
        std::filesystem::path src = from / name;
        std::filesystem::path dst = cache_dir_ / "libs" / name;
        if (std::filesystem::exists(src) && !std::filesystem::exists(dst)) {
            std::error_code c2;
            std::filesystem::copy_file(src, dst, c2);
        }
    }
}

MusicFreeSourceManager::ImportReport
MusicFreeSourceManager::ImportFromUrl(std::string_view url) {
    ImportReport report;
    try {
        std::string text = DownloadText(std::string(url), network_);
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_discarded() && j.contains("plugins") && j["plugins"].is_array()) {
            report = ImportRegistryJson(j, url, cache_dir_);
        } else {
            // 单个插件 JS
            std::string fname = "plugin_" + HashName(url) + ".js";
            std::filesystem::path fp = cache_dir_ / "plugins" / fname;
            report.source_name = "single:" + std::string(url);
            report.plugins_total = 1;
            // 同一个 URL、同一份缓存文件或同一段脚本内容都算同一个音源。
            // 重复导入既不新增列表行，也不再花几百毫秒重建 QuickJS runtime。
            const std::string hash = HashName(text);
            if (HasPluginUrl(url) || HasContentHash(hash) || HasPluginFile(fp)) {
                report.plugins_skipped = 1;
                report.duplicated = true;
                report.message = "该音源已导入，已跳过重复导入";
                return report;
            }
            WriteAll(fp, text);
            auto plugin = std::make_unique<MusicFreePlugin>(std::string(url));
            plugin->Configure(network_, cache_dir_ / "libs");
            if (plugin->Mount(text, app_version_)) {
                report.plugins_ok = 1;
                ImportedSource src;
                src.name = plugin->meta().name.empty() ? report.source_name : plugin->meta().name;
                src.registry_url = std::string(url);
                src.local_path = fp;
                src.plugin_urls.push_back(std::string(url));
                src.plugin_files.push_back(fp);
                src.version = plugin->meta().version;
                imported_.push_back(std::move(src));
                plugins_.push_back(std::move(plugin));
                plugin_files_.push_back(fp);
                plugin_hashes_.push_back(hash);
            } else {
                report.errors.push_back("挂载失败: " + plugin->last_error());
            }
        }
    } catch (const std::exception& e) {
        report.errors.push_back(std::string(e.what()));
    }
    if (!report.errors.empty() || report.plugins_ok > 0) Persist();
    return report;
}

MusicFreeSourceManager::ImportReport
MusicFreeSourceManager::ImportFromFile(const std::filesystem::path& path) {
    ImportReport report;
    try {
        std::string text = ReadAll(path);
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_discarded() && j.contains("plugins") && j["plugins"].is_array()) {
            report = ImportRegistryJson(j, "", path);
        } else {
            std::string fname = "plugin_" + HashName(path.string()) + ".js";
            std::filesystem::path fp = cache_dir_ / "plugins" / fname;
            report.source_name = "file:" + path.string();
            report.plugins_total = 1;
            // 同一份脚本换个路径/文件名复制过来，内容哈希仍然相同，照样判重。
            const std::string hash = HashName(text);
            if (HasContentHash(hash) || HasPluginFile(fp)) {
                report.plugins_skipped = 1;
                report.duplicated = true;
                report.message = "该音源已导入，已跳过重复导入";
                return report;
            }
            WriteAll(fp, text);
            auto plugin = std::make_unique<MusicFreePlugin>(path.string());
            plugin->Configure(network_, cache_dir_ / "libs");
            if (plugin->Mount(text, app_version_)) {
                report.plugins_ok = 1;
                ImportedSource src;
                src.name = plugin->meta().name.empty() ? report.source_name : plugin->meta().name;
                src.local_path = fp;
                src.plugin_files.push_back(fp);
                src.version = plugin->meta().version;
                imported_.push_back(std::move(src));
                plugins_.push_back(std::move(plugin));
                plugin_files_.push_back(fp);
                plugin_hashes_.push_back(hash);
            } else {
                report.errors.push_back("挂载失败: " + plugin->last_error());
            }
        }
    } catch (const std::exception& e) {
        report.errors.push_back(std::string(e.what()));
    }
    if (!report.errors.empty() || report.plugins_ok > 0) Persist();
    return report;
}

MusicFreeSourceManager::ImportReport
MusicFreeSourceManager::ImportRegistryJson(const nlohmann::json& registry,
                                            std::string_view registry_url,
                                            const std::filesystem::path& base) {
    ImportReport report;
    report.source_name = registry.value("name", registry_url.empty()
        ? "registry" : std::string(registry_url));
    ImportedSource src;
    src.name = report.source_name;
    src.registry_url = std::string(registry_url);
    src.local_path = base;

    for (const auto& entry : registry["plugins"]) {
        if (!entry.contains("url") || !entry["url"].is_string()) continue;
        std::string purl = entry["url"].get<std::string>();
        ++report.plugins_total;
        // 注册表整体重复导入时，已存在的插件连下载都省掉：以前这样会
        // 把同一批 JS 重新下 N 遍、挂 N 份实例，列表瞬间翻倍且要等几秒。
        if (HasPluginUrl(purl)) {
            ++report.plugins_skipped;
            continue;
        }
        try {
            std::string code = DownloadText(purl, network_);
            std::string fname = "plugin_" + HashName(purl) + ".js";
            std::filesystem::path fp = cache_dir_ / "plugins" / fname;
            const std::string hash = HashName(code);
            // 换镜像域名但脚本内容一致，仍然只保留一份。
            if (HasContentHash(hash) || HasPluginFile(fp)) {
                ++report.plugins_skipped;
                continue;
            }
            WriteAll(fp, code);
            src.plugin_urls.push_back(purl);
            src.plugin_files.push_back(fp);
            auto plugin = std::make_unique<MusicFreePlugin>(purl);
            plugin->Configure(network_, cache_dir_ / "libs");
            if (plugin->Mount(code, app_version_)) {
                ++report.plugins_ok;
                if (src.version.empty() && entry.contains("version"))
                    src.version = entry["version"].get<std::string>();
                plugins_.push_back(std::move(plugin));
                plugin_files_.push_back(fp);
                plugin_hashes_.push_back(hash);
            } else {
                report.errors.push_back(purl + " 挂载失败: " + plugin->last_error());
            }
        } catch (const std::exception& e) {
            report.errors.push_back(purl + " 下载/挂载失败: " + e.what());
        }
    }
    if (report.plugins_ok > 0 || !report.errors.empty()) {
        imported_.push_back(std::move(src));
    }
    if (report.plugins_skipped > 0) {
        report.message = "跳过已导入的重复插件 " +
                         std::to_string(report.plugins_skipped) + " 个";
        if (report.plugins_ok == 0 && report.errors.empty()) {
            report.duplicated = true;
            report.message += "；该音源已全部导入，无需重复添加";
        }
    }
    return report;
}

std::vector<PluginMeta> MusicFreeSourceManager::ListPlugins() const {
    std::vector<PluginMeta> out;
    for (const auto& p : plugins_) out.push_back(p->meta());
    return out;
}

MusicFreePlugin* MusicFreeSourceManager::FindPlugin(std::string_view platform) {
    for (auto& p : plugins_)
        if (p->meta().name == platform) return p.get();
    return nullptr;
}

bool MusicFreeSourceManager::HasContentHash(std::string_view hash) const {
    const std::string key(hash);
    for (const auto& h : plugin_hashes_)
        if (h == key) return true;
    return false;
}

bool MusicFreeSourceManager::HasPluginFile(
    const std::filesystem::path& fp) const {
    return std::find(plugin_files_.begin(), plugin_files_.end(), fp) !=
           plugin_files_.end();
}

bool MusicFreeSourceManager::HasPluginUrl(std::string_view url) const {
    const std::string key(url);
    for (const auto& s : imported_) {
        if (s.registry_url == key) return true;
        if (std::find(s.plugin_urls.begin(), s.plugin_urls.end(), key) !=
            s.plugin_urls.end())
            return true;
    }
    return false;
}

void MusicFreeSourceManager::ErasePluginAt(std::size_t index) {
    if (index >= plugins_.size()) return;
    plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < plugin_files_.size())
        plugin_files_.erase(plugin_files_.begin() +
                            static_cast<std::ptrdiff_t>(index));
    if (index < plugin_hashes_.size())
        plugin_hashes_.erase(plugin_hashes_.begin() +
                             static_cast<std::ptrdiff_t>(index));
}

bool MusicFreeSourceManager::RemoveSource(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(imported_.begin(), imported_.end(),
        [&](const ImportedSource& s) { return s.name == name; });
    if (it == imported_.end()) return false;
    // 先复制一份：后面的 DropPluginFile 会让 imported_ 的迭代器失效。
    const std::vector<std::filesystem::path> files = it->plugin_files;
    for (const auto& fp : files) {
        std::error_code ec; std::filesystem::remove(fp, ec);
    }
    // 按缓存文件定位插件实例，而不是按「音源名 == 插件名」——这两者
    // 从来就不是一个键（音源名是注册表/URL，插件名是 migu/qq/kuwo）。
    for (std::size_t i = plugins_.size(); i-- > 0;) {
        if (i < plugin_files_.size() &&
            std::find(files.begin(), files.end(), plugin_files_[i]) != files.end())
            ErasePluginAt(i);
    }
    for (const auto& fp : files) DropPluginFile(imported_, fp);
    Persist();
    return true;
}

bool MusicFreeSourceManager::RemovePluginAt(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= plugins_.size()) return false;
    const std::string name = plugins_[index]->meta().name;
    const std::filesystem::path target =
        index < plugin_files_.size() ? plugin_files_[index]
                                     : std::filesystem::path{};
    // 同一个插件 JS 被重复导入时会挂出多份实例，共用同一个缓存文件；
    // 一次全清，否则残留实例在下次启动时会因文件被删而失效。
    int removed = 0;
    for (std::size_t i = plugins_.size(); i-- > 0;) {
        const bool same_file = !target.empty() && i < plugin_files_.size() &&
                               plugin_files_[i] == target;
        const bool same_name = target.empty() &&
                               plugins_[i]->meta().name == name;
        if (same_file || same_name) { ErasePluginAt(i); ++removed; }
    }
    if (!target.empty()) {
        std::error_code ec; std::filesystem::remove(target, ec);
        DropPluginFile(imported_, target);
    } else {
        // 兜底：没有缓存路径时至少按插件名清掉对应的持久化记录。
        for (auto it = imported_.begin(); it != imported_.end();) {
            if (it->name == name) {
                for (const auto& f : it->plugin_files) {
                    std::error_code ec; std::filesystem::remove(f, ec);
                }
                it = imported_.erase(it);
            } else {
                ++it;
            }
        }
    }
    Persist();
    return removed > 0;
}

std::size_t MusicFreeSourceManager::RemovePluginsAt(
    const std::vector<std::size_t>& indices) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 先把「要删什么」固化下来，再倒序删实例。这样即便同批里选中了共用
    // 同一个缓存文件的重复实例，或删除过程中下标整体前移，也不会错位。
    std::vector<std::filesystem::path> targets;
    std::vector<std::string> names;
    for (const std::size_t idx : indices) {
        if (idx >= plugins_.size()) continue;
        if (idx < plugin_files_.size() && !plugin_files_[idx].empty()) {
            const auto& fp = plugin_files_[idx];
            if (std::find(targets.begin(), targets.end(), fp) == targets.end())
                targets.push_back(fp);
        } else {
            const std::string& name = plugins_[idx]->meta().name;
            if (!name.empty() &&
                std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        }
    }
    if (targets.empty() && names.empty()) return 0;

    std::size_t removed = 0;
    for (std::size_t i = plugins_.size(); i-- > 0;) {
        const bool has_file =
            i < plugin_files_.size() && !plugin_files_[i].empty();
        const bool match =
            has_file ? std::find(targets.begin(), targets.end(),
                                 plugin_files_[i]) != targets.end()
                     : (!plugins_[i]->meta().name.empty() &&
                        std::find(names.begin(), names.end(),
                                  plugins_[i]->meta().name) != names.end());
        if (match) {
            ErasePluginAt(i);
            ++removed;
        }
    }
    for (const auto& fp : targets) {
        std::error_code ec;
        std::filesystem::remove(fp, ec);
        DropPluginFile(imported_, fp);
    }
    for (auto it = imported_.begin(); it != imported_.end();) {
        if (!names.empty() &&
            std::find(names.begin(), names.end(), it->name) != names.end()) {
            for (const auto& f : it->plugin_files) {
                std::error_code ec;
                std::filesystem::remove(f, ec);
            }
            it = imported_.erase(it);
        } else {
            ++it;
        }
    }
    Persist();
    return removed;
}

std::string MusicFreeSourceManager::DescribePlugin(std::size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= plugins_.size()) return {};
    const PluginMeta& m = plugins_[index]->meta();
    const std::filesystem::path file =
        index < plugin_files_.size() ? plugin_files_[index]
                                     : std::filesystem::path{};
    int duplicates = 0;
    if (!file.empty()) {
        for (std::size_t i = 0; i < plugin_files_.size(); ++i)
            if (plugin_files_[i] == file) ++duplicates;
    }
    std::ostringstream os;
    os << "插件名称：" << (m.name.empty() ? "(未命名)" : m.name) << "\r\n"
       << "版本：" << (m.version.empty() ? "-" : m.version) << "\r\n"
       << "来源 URL：" << (m.source_url.empty() ? "(本地文件)" : m.source_url)
       << "\r\n"
       << "缓存文件：" << (file.empty() ? "-" : file.string()) << "\r\n"
       << "内容哈希：" << (m.hash.empty() ? "-" : m.hash) << "\r\n"
       << "能力：";
    bool any = false;
    auto add = [&](bool on, const char* text) {
        if (!on) return;
        os << (any ? " / " : "") << text;
        any = true;
    };
    add(m.can_search, "搜索");
    add(m.can_media_source, "播放地址");
    add(m.can_lyric, "歌词");
    add(m.can_music_info, "歌曲信息");
    add(m.can_album, "专辑");
    add(m.can_sheet, "歌单");
    if (!any) os << "无";
    os << "\r\n";
    if (duplicates > 1)
        os << "注意：该插件被重复导入了 " << duplicates << " 份，删除时会一起清理。";
    return os.str();
}

void MusicFreeSourceManager::LoadCached() {
    TT_STARTUP_TRACE("cached_index_open");
    std::ifstream f(index_path_);
    if (!f) return;
    nlohmann::json idx;
    try { f >> idx; } catch (...) { return; }
    if (!idx.is_array()) return;
    TT_STARTUP_TRACE("cached_index_parsed");
    int mounted = 0;
    // 历史版本的 imported.json 里可能已经积累了重复记录（同一个插件被导入
    // 多次，或从两个 URL 下载了同一份脚本）。启动时按内容哈希去重，一次性
    // 把索引也写干净，列表里不再出现多份同名音源。
    std::vector<std::string> seen;
    bool cleaned = false;
    for (const auto& s : idx) {
        // 进程正在退出时不再继续建 QuickJS runtime，让关闭立刻完成。
        if (ShouldStopMusicFreeLoading()) break;
        ImportedSource is;
        is.name = s.value("name", "");
        is.registry_url = s.value("registry_url", "");
        is.version = s.value("version", "");
        std::vector<std::string> urls;
        if (s.contains("plugin_urls") && s["plugin_urls"].is_array())
            for (const auto& pu : s["plugin_urls"])
                urls.push_back(pu.get<std::string>());
        if (s.contains("plugin_files") && s["plugin_files"].is_array()) {
            const auto& files = s["plugin_files"];
            const bool paired = urls.size() == files.size();
            for (std::size_t fi = 0; fi < files.size(); ++fi) {
                if (ShouldStopMusicFreeLoading()) break;
                std::filesystem::path fp = files[fi].get<std::string>();
                if (!std::filesystem::exists(fp)) continue;
                try {
                    std::string code = ReadAll(fp);
                    const std::string hash = HashName(code);
                    if (std::find(seen.begin(), seen.end(), hash) != seen.end()) {
                        cleaned = true;
                        continue;  // 重复缓存文件不再挂载、不再登记
                    }
                    seen.push_back(hash);
                    is.plugin_files.push_back(fp);
                    // URL 与文件必须一一对应；去重时按同一索引丢弃，
                    // 否则 DropPluginFile 会删错 URL 导致重复判重失效。
                    if (paired) is.plugin_urls.push_back(urls[fi]);
                    auto plugin = std::make_unique<MusicFreePlugin>(
                        is.registry_url.empty() ? fp.string() : is.registry_url);
                    plugin->Configure(network_, cache_dir_ / "libs");
                    if (plugin->Mount(code, app_version_)) {
                        plugins_.push_back(std::move(plugin));
                        plugin_files_.push_back(fp);
                        plugin_hashes_.push_back(hash);
                    }
                    char label[64];
                    std::snprintf(label, sizeof(label), "cached_plugin_%02d", ++mounted);
                    TT_STARTUP_TRACE(label);
                } catch (const std::exception&) { /* 跳过损坏的缓存 */ }
            }
        }
        // 若旧记录里 URL 与文件数量不等就保持 URL 列表为空，避免错位配对。
        imported_.push_back(std::move(is));
    }
    if (cleaned) Persist();
    TT_STARTUP_TRACE("cached_all_done");
}

SourceUpdateReport MusicFreeSourceManager::CheckForUpdates() {
    SourceUpdateReport report;
    // 全程持锁：更新会原地替换 plugins_ / plugin_hashes_，与删除、列表读取
    // 互斥。下载量 = 插件个数（每个几百 KB），几秒内完成；进程退出时靠
    // ShouldStopMusicFreeLoading() 在插件之间提前收工。
    std::lock_guard<std::mutex> lock(mutex_);

    // 快照每个插件的更新源。URL 记在 ImportedSource.plugin_urls 里，与
    // plugin_files 平行；先按缓存文件反查所属来源，再取同下标的 URL。
    struct Pending {
        std::size_t index;
        std::string url;
        std::string hash;
        std::string version;
        std::string name;
    };
    std::vector<Pending> pending;
    for (std::size_t i = 0; i < plugins_.size(); ++i) {
        PluginMeta m = plugins_[i]->meta();
        std::string url;
        for (const auto& s : imported_) {
            for (std::size_t k = 0; k < s.plugin_files.size(); ++k) {
                if (s.plugin_files[k] != plugin_files_[i]) continue;
                if (k < s.plugin_urls.size() && !s.plugin_urls[k].empty())
                    url = s.plugin_urls[k];
            }
        }
        if (url.empty()) {
            SourceUpdateInfo info;
            info.protocol = "MusicFree";
            info.name = m.name;
            info.old_version = m.version;
            info.status = "no_url";
            info.detail = "本地导入的插件没有更新源";
            report.items.push_back(std::move(info));
            continue;
        }
        pending.push_back({i, std::move(url), plugin_hashes_[i],
                           std::move(m.version), std::move(m.name)});
    }

    for (auto& p : pending) {
        if (ShouldStopMusicFreeLoading()) break;
        SourceUpdateInfo info;
        info.protocol = "MusicFree";
        info.name = p.name;
        info.url = p.url;
        info.old_version = p.version;
        ++report.checked;
        try {
            // 与 MusicFree installPluginFromUrl 一致：no-cache 防中间缓存
            // 把旧脚本再吐回来。
            std::map<std::string, std::string> hdrs;
            hdrs["cache-control"] = "no-cache";
            auto bytes = DownloadBinary(p.url, network_, hdrs);
            const std::string text(bytes.begin(), bytes.end());
            const std::string new_hash = HashName(text);
            if (new_hash == p.hash) {
                info.status = "latest";
                ++report.latest;
                report.items.push_back(std::move(info));
                continue;
            }
            // 探针挂载读新版本：不进正式列表，挂载失败视作远端脚本损坏。
            auto probe = std::make_unique<MusicFreePlugin>("update-probe");
            probe->Configure(network_, cache_dir_ / "libs");
            if (!probe->Mount(text, app_version_)) {
                info.status = "failed";
                info.detail = "远端脚本挂载失败: " + probe->last_error();
                ++report.failed;
                report.items.push_back(std::move(info));
                continue;
            }
            info.new_version = probe->meta().version;
            // 防降级（compare-versions 语义）：本地版本较新就拒绝覆盖。
            if (CompareSourceVersions(info.new_version, p.version) < 0) {
                info.status = "downgrade";
                info.detail = "本地已是更新版本";
                report.items.push_back(std::move(info));
                continue;
            }
            // 应用更新：缓存文件名由 URL 哈希决定，路径不变，原地覆写后重挂。
            const std::filesystem::path fp = plugin_files_[p.index];
            WriteAll(fp, text);
            auto fresh = std::make_unique<MusicFreePlugin>(p.url);
            fresh->Configure(network_, cache_dir_ / "libs");
            if (!fresh->Mount(text, app_version_)) {
                info.status = "failed";
                info.detail = "更新后挂载失败: " + fresh->last_error();
                ++report.failed;
                report.items.push_back(std::move(info));
                continue;
            }
            plugins_[p.index] = std::move(fresh);
            plugin_hashes_[p.index] = new_hash;
            info.status = "updated";
            ++report.updated;
            report.items.push_back(std::move(info));
        } catch (const std::exception& e) {
            info.status = "failed";
            info.detail = e.what();
            ++report.failed;
            report.items.push_back(std::move(info));
        }
    }
    if (report.updated > 0) Persist();
    return report;
}

void MusicFreeSourceManager::Persist() {
    nlohmann::json idx = nlohmann::json::array();
    for (const auto& s : imported_) {
        nlohmann::json o;
        o["name"] = s.name;
        o["registry_url"] = s.registry_url;
        o["version"] = s.version;
        o["local_path"] = s.local_path.string();
        o["plugin_urls"] = s.plugin_urls;
        o["plugin_files"] = s.plugin_files;
        idx.push_back(o);
    }
    WriteAll(index_path_, idx.dump(2));
}

} // namespace ttplayer::musicfree
