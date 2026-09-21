// MusicFree 兼容层 —— 音源管理器（手动导入 / 持久化）
//
// 对应 MusicFree 的 PluginManager：解析"音源注册表"JSON（如 yuanli.json：
// { "plugins": [ {name,url,version} ] }）或单个插件 JS，下载、挂载、持久化，
// 并在启动时自动重建。导入完全是“手动”的——只消费用户提供的来源。

#include "ttplayer/musicfree/musicfree_source_manager.h"

#include <algorithm>
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
            WriteAll(fp, text);
            report.source_name = "single:" + std::string(url);
            report.plugins_total = 1;
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
            WriteAll(fp, text);
            report.source_name = "file:" + path.string();
            report.plugins_total = 1;
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
        try {
            std::string code = DownloadText(purl, network_);
            std::string fname = "plugin_" + HashName(purl) + ".js";
            std::filesystem::path fp = cache_dir_ / "plugins" / fname;
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

bool MusicFreeSourceManager::RemoveSource(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(imported_.begin(), imported_.end(),
        [&](const ImportedSource& s) { return s.name == name; });
    if (it == imported_.end()) return false;
    for (const auto& fp : it->plugin_files) {
        std::error_code ec; std::filesystem::remove(fp, ec);
    }
    // 移除对应插件
    plugins_.erase(std::remove_if(plugins_.begin(), plugins_.end(),
        [&](const std::unique_ptr<MusicFreePlugin>& p) {
            return p->meta().source_url == it->registry_url && p->meta().name == it->name;
        }), plugins_.end());
    imported_.erase(it);
    Persist();
    return true;
}

void MusicFreeSourceManager::LoadCached() {
    std::ifstream f(index_path_);
    if (!f) return;
    nlohmann::json idx;
    try { f >> idx; } catch (...) { return; }
    if (!idx.is_array()) return;
    for (const auto& s : idx) {
        ImportedSource is;
        is.name = s.value("name", "");
        is.registry_url = s.value("registry_url", "");
        is.version = s.value("version", "");
        if (s.contains("plugin_files") && s["plugin_files"].is_array())
            for (const auto& pf : s["plugin_files"]) {
                std::filesystem::path fp = pf.get<std::string>();
                if (std::filesystem::exists(fp)) {
                    is.plugin_files.push_back(fp);
                    try {
                        std::string code = ReadAll(fp);
                        auto plugin = std::make_unique<MusicFreePlugin>(
                            is.registry_url.empty() ? pf.get<std::string>() : is.registry_url);
                        plugin->Configure(network_, cache_dir_ / "libs");
                        if (plugin->Mount(code, app_version_)) plugins_.push_back(std::move(plugin));
                    } catch (const std::exception&) { /* 跳过损坏的缓存 */ }
                }
            }
        imported_.push_back(std::move(is));
    }
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
