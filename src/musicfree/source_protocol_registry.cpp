// 多协议音源注册表实现
//
// 设计取舍：不改动 MusicFreeSourceManager 的持久化格式与行为，注册表单独
// 维护 <exe>/musicfree/protocols/index.json。导入窗口与播放地址解析只认
// 注册表这一层，因此后续新增协议无需再碰 UI。

#include "ttplayer/musicfree/source_protocol_registry.h"

#include "ttplayer/app/startup_trace.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/lx_source_adapter.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/musicfree/rest_source_adapter.h"

namespace ttplayer::musicfree {
namespace {

std::filesystem::path ExeDir() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(),
                                       static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) return {};
    exe.resize(n);
    return std::filesystem::path(exe).parent_path();
}

std::string HashName(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    std::ostringstream os;
    os << std::hex << h;
    return os.str();
}

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("无法读取文件: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteFile(const std::filesystem::path& p, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法写入文件: " + p.string());
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string GetString(const nlohmann::json& j, const char* key,
                      const std::string& fallback = {}) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>()
                                                 : fallback;
}

SourceProtocol KindFromId(std::string_view id) {
    if (id == "musicfree") return SourceProtocol::kMusicFree;
    if (id == "lx-music") return SourceProtocol::kLxMusic;
    if (id == "listen1") return SourceProtocol::kListen1;
    if (id == "rest") return SourceProtocol::kRest;
    return SourceProtocol::kUnknown;
}

// 报告文案用的窄字符协议标签（ProtocolLabel 返回宽字符，不便拼接）。
const char* ProtocolLabelText(SourceProtocol kind) {
    switch (kind) {
        case SourceProtocol::kLxMusic: return "洛雪音源";
        case SourceProtocol::kRest: return "REST 音源";
        case SourceProtocol::kListen1: return "Listen1 provider";
        case SourceProtocol::kMusicFree: return "MusicFree 插件";
        default: return "音源";
    }
}

} // namespace

struct SourceProtocolRegistry::Impl {
    struct Entry {
        ProtocolSourceInfo info;
        std::unique_ptr<LxSourceAdapter> lx;
        std::unique_ptr<RestSourceAdapter> rest;
        nlohmann::json config;
        std::filesystem::path file;
        std::string content_hash;  // 导入去重：音源文件内容的 FNV-1a 哈希
    };

    std::filesystem::path cache_dir;
    std::filesystem::path index_path;
    std::filesystem::path lib_dir;
    NetworkConfig network;
    mutable std::mutex mutex;
    bool configured = false;
    std::vector<std::unique_ptr<Entry>> entries;

    void Persist() {
        if (cache_dir.empty()) return;
        nlohmann::json root = nlohmann::json::object();
        root["version"] = 1;
        root["sources"] = nlohmann::json::array();
        for (const auto& e : entries) {
            nlohmann::json item = nlohmann::json::object();
            item["kind"] = ProtocolId(e->info.kind);
            item["name"] = e->info.name;
            item["version"] = e->info.version;
            item["author"] = e->info.author;
            item["description"] = e->info.description;
            item["homepage"] = e->info.homepage;
            item["source_url"] = e->info.source_url;
            item["content_hash"] = e->content_hash;
            item["file"] = e->file.string();
            if (!e->config.is_null()) item["config"] = e->config;
            root["sources"].push_back(std::move(item));
        }
        try {
            WriteFile(index_path, root.dump(2));
        } catch (...) {
            // 持久化失败不致命：内存里的音源本次会话仍可用。
        }
    }

    std::filesystem::path SavePayload(const std::string& content,
                                      std::string_view url,
                                      const char* subdir,
                                      const char* ext) {
        const std::string leaf = std::filesystem::path(std::string(url)).filename().string();
        std::string stem = leaf;
        for (char& c : stem)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                c = '_';
        if (stem.size() > 48) stem.resize(48);
        if (stem.empty()) stem = "source";
        const auto path = cache_dir / subdir /
                          (stem + "-" + HashName(content).substr(0, 8) + ext);
        WriteFile(path, content);
        return path;
    }

    ProtocolImportReport AddContent(const std::string& content,
                                    const std::string& source_url,
                                    const std::filesystem::path& hint_path) {
        const std::string hint = hint_path.empty() ? source_url
                                                   : hint_path.string();
        const SourceProtocol kind = DetectSourceProtocol(content, hint);
        ProtocolImportReport report;
        report.kind = kind;
        report.source_name = source_url.empty() ? hint_path.filename().string()
                                                : source_url;

        if (kind == SourceProtocol::kMusicFree) {
            auto* mgr = AppMusicFreeManager();
            if (!mgr) {
                report.errors.push_back("MusicFree 管理器未初始化");
                report.message = "导入失败";
                return report;
            }
            MusicFreeSourceManager::ImportReport mf =
                source_url.empty() ? mgr->ImportFromFile(hint_path)
                                   : mgr->ImportFromUrl(source_url);
            report.plugins_total = mf.plugins_total;
            report.plugins_ok = mf.plugins_ok;
            report.plugins_skipped = mf.plugins_skipped;
            report.duplicated = mf.duplicated;
            report.errors = std::move(mf.errors);
            report.ok = mf.plugins_ok > 0;
            report.source_name = mf.source_name;
            report.message = std::string("MusicFree 插件：成功 ") +
                             std::to_string(mf.plugins_ok) + " / " +
                             std::to_string(mf.plugins_total);
            if (mf.plugins_skipped > 0)
                report.message += "，跳过重复 " +
                                  std::to_string(mf.plugins_skipped) + " 个";
            if (mf.duplicated && !mf.message.empty())
                report.message = mf.message;
            return report;
        }

        if (kind == SourceProtocol::kUnknown) {
            report.errors.push_back("无法识别音源协议（既不是 MusicFree 插件、"
                                    "洛雪音源，也不是 REST 配置 / Listen1 provider）");
            report.message = "导入失败：协议未识别";
            return report;
        }

        // 导入去重：内容完全相同 → 直接跳过；同一 URL 但内容变了 → 视为
        // 更新，先把旧条目摘掉再挂载新的，列表里始终只留一条。
        const std::string content_hash = HashName(content);
        constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
        std::size_t replace_index = kNoIndex;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& e = *entries[i];
                const bool same_content =
                    !e.content_hash.empty() && e.content_hash == content_hash;
                const bool same_url =
                    !source_url.empty() && e.info.source_url == source_url;
                if (!same_content && !same_url) continue;
                if (same_content) {
                    report.duplicated = true;
                    report.source_name = e.info.name;
                    report.message =
                        "该音源已导入（内容相同），已跳过重复导入：" + e.info.name;
                    return report;
                }
                replace_index = i;
                break;
            }
        }

        auto entry = std::make_unique<Entry>();
        entry->content_hash = content_hash;
        entry->info.kind = kind;
        entry->info.source_url = source_url;
        std::string error;

        if (kind == SourceProtocol::kLxMusic) {
            entry->file = SavePayload(content, source_url, "lx", ".js");
            entry->info.cache_path = entry->file;
            auto adapter = std::make_unique<LxSourceAdapter>();
            if (!adapter->Mount(content, network, lib_dir, source_url, &error)) {
                report.errors.push_back(error);
                report.message = "洛雪音源挂载失败";
                std::error_code ec;
                std::filesystem::remove(entry->file, ec);
                return report;
            }
            entry->info = adapter->info();
            entry->info.cache_path = entry->file;
            entry->lx = std::move(adapter);
            report.source_name = entry->info.name;
        } else if (kind == SourceProtocol::kRest) {
            nlohmann::json config = nlohmann::json::parse(content, nullptr, false);
            if (config.is_discarded()) {
                report.errors.push_back("REST 音源配置不是有效 JSON");
                report.message = "导入失败";
                return report;
            }
            entry->file = SavePayload(content, source_url, "rest", ".json");
            entry->config = config;
            auto adapter = std::make_unique<RestSourceAdapter>();
            if (!adapter->Mount(config, network, source_url, &error)) {
                report.errors.push_back(error);
                report.message = "REST 音源挂载失败";
                std::error_code ec;
                std::filesystem::remove(entry->file, ec);
                return report;
            }
            entry->info = adapter->info();
            entry->info.cache_path = entry->file;
            entry->rest = std::move(adapter);
            report.source_name = entry->info.name;
        } else if (kind == SourceProtocol::kListen1) {
            // Listen1 provider 依赖浏览器宿主对象（get/post/ajax/angular），
            // 目前先落盘并登记元数据，运行时桥接见 docs/SOURCE_PROTOCOLS.md。
            entry->file = SavePayload(content, source_url, "listen1", ".js");
            const nlohmann::json header = ParseScriptHeader(content);
            entry->info.name = GetString(header, "name", "Listen1 Provider");
            entry->info.version = GetString(header, "version");
            entry->info.author = GetString(header, "author");
            entry->info.description = GetString(header, "description");
            entry->info.cache_path = entry->file;
            entry->info.sources = {"listen1"};
            entry->info.can_search = false;
            entry->info.can_resolve = false;
            report.message =
                "已识别 Listen1 provider 并登记；其运行时依赖浏览器宿主对象，"
                "当前版本仅保留元数据（详见 docs/SOURCE_PROTOCOLS.md）";
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (replace_index != kNoIndex && replace_index < entries.size()) {
                std::error_code ec;
                std::filesystem::remove(entries[replace_index]->file, ec);
                entries.erase(entries.begin() +
                              static_cast<std::ptrdiff_t>(replace_index));
                report.updated = true;
            }
            entries.push_back(std::move(entry));
            Persist();
        }
        report.ok = true;
        const std::string detail = report.message;
        report.message = std::string(report.updated ? "已更新 " : "已导入 ") +
                         ProtocolLabelText(kind) + "：" + report.source_name;
        if (!detail.empty()) report.message += "\r\n" + detail;
        return report;
    }
};

SourceProtocolRegistry::SourceProtocolRegistry()
    : impl_(std::make_unique<Impl>()) {}
SourceProtocolRegistry::~SourceProtocolRegistry() = default;

void SourceProtocolRegistry::Configure(std::filesystem::path cache_dir,
                                       NetworkConfig network,
                                       std::filesystem::path lib_dir) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cache_dir = std::move(cache_dir);
    impl_->index_path = impl_->cache_dir / "index.json";
    impl_->lib_dir = std::move(lib_dir);
    impl_->network = std::move(network);
    impl_->configured = true;
}

void SourceProtocolRegistry::SetNetworkConfig(NetworkConfig network) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->network = std::move(network);
}

ProtocolImportReport SourceProtocolRegistry::ImportFromUrl(
    std::string_view url) {
    ProtocolImportReport report;
    report.source_name = std::string(url);
    try {
        // 先只做一次下载用于嗅探；MusicFree 内容会由原管理器自行再取一次。
        const std::string content = DownloadText(std::string(url), impl_->network);
        return impl_->AddContent(content, std::string(url), {});
    } catch (const std::exception& e) {
        report.errors.push_back(e.what());
        report.message = "下载音源失败";
        return report;
    }
}

ProtocolImportReport SourceProtocolRegistry::ImportFromFile(
    const std::filesystem::path& path) {
    ProtocolImportReport report;
    report.source_name = path.filename().string();
    try {
        return impl_->AddContent(ReadFile(path), {}, path);
    } catch (const std::exception& e) {
        report.errors.push_back(e.what());
        report.message = "读取音源文件失败";
        return report;
    }
}

void SourceProtocolRegistry::Load() {
    TT_STARTUP_TRACE("protocols_load_enter");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->configured || !std::filesystem::exists(impl_->index_path)) return;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(ReadFile(impl_->index_path), nullptr, false);
    } catch (...) {
        return;
    }
    if (root.is_discarded() || !root.is_object() ||
        !root.contains("sources") || !root["sources"].is_array())
        return;
    TT_STARTUP_TRACE("protocols_index_parsed");
    impl_->entries.clear();
    // 旧版 index.json 里若有重复条目，启动时一并清理并回写。
    std::vector<std::string> seen;
    bool cleaned = false;
    for (const auto& item : root["sources"]) {
        if (!item.is_object()) continue;
        const auto kind = KindFromId(GetString(item, "kind"));
        const std::string file = GetString(item, "file");
        if (file.empty()) continue;
        std::filesystem::path path(file);
        if (!std::filesystem::exists(path)) continue;

        auto entry = std::make_unique<Impl::Entry>();
        entry->file = path;
        entry->info.kind = kind;
        entry->info.name = GetString(item, "name");
        entry->info.version = GetString(item, "version");
        entry->info.author = GetString(item, "author");
        entry->info.description = GetString(item, "description");
        entry->info.homepage = GetString(item, "homepage");
        entry->info.source_url = GetString(item, "source_url");
        entry->content_hash = GetString(item, "content_hash");
        entry->info.cache_path = path;
        if (item.contains("config")) entry->config = item["config"];

        try {
            const std::string code = ReadFile(path);
            if (entry->content_hash.empty())
                entry->content_hash = HashName(code);
            if (std::find(seen.begin(), seen.end(), entry->content_hash) !=
                seen.end()) {
                cleaned = true;
                continue;
            }
            seen.push_back(entry->content_hash);
            if (kind == SourceProtocol::kLxMusic) {
                auto adapter = std::make_unique<LxSourceAdapter>();
                std::string error;
                if (adapter->Mount(code, impl_->network, impl_->lib_dir,
                                   entry->info.source_url, &error)) {
                    entry->info = adapter->info();
                    entry->info.cache_path = path;
                    entry->lx = std::move(adapter);
                }
            } else if (kind == SourceProtocol::kRest) {
                nlohmann::json cfg = item.contains("config")
                                         ? item["config"]
                                         : nlohmann::json::parse(code, nullptr, false);
                auto adapter = std::make_unique<RestSourceAdapter>();
                std::string error;
                if (adapter->Mount(cfg, impl_->network, entry->info.source_url,
                                   &error)) {
                    entry->info = adapter->info();
                    entry->info.cache_path = path;
                    entry->rest = std::move(adapter);
                }
            } else if (kind == SourceProtocol::kListen1) {
                entry->info.sources = {"listen1"};
            }
        } catch (...) {
            // 单个音源重建失败不影响其它音源。
        }
        impl_->entries.push_back(std::move(entry));
    }
    if (cleaned) impl_->Persist();
    TT_STARTUP_TRACE("protocols_entries_done");
}

SourceUpdateReport SourceProtocolRegistry::CheckForUpdates() {
    SourceUpdateReport report;
    // 快照（持锁）：url / hash / 名称 / 版本 / 协议 / 网络配置。下载在锁外做，
    // 避免检查更新期间搜索、解析、删除全被卡住。
    struct Snap {
        std::string url;
        std::string hash;
        std::string name;
        std::string version;
        std::string protocol;
    };
    std::vector<Snap> snaps;
    NetworkConfig network;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        network = impl_->network;
        for (const auto& e : impl_->entries) {
            if (e->info.source_url.empty()) {
                SourceUpdateInfo info;
                info.protocol = ProtocolLabelText(e->info.kind);
                info.name = e->info.name;
                info.old_version = e->info.version;
                info.status = "no_url";
                info.detail = "本地导入，没有更新源";
                report.items.push_back(std::move(info));
                continue;
            }
            snaps.push_back({e->info.source_url, e->content_hash, e->info.name,
                             e->info.version,
                             ProtocolLabelText(e->info.kind)});
        }
    }

    for (auto& s : snaps) {
        SourceUpdateInfo info;
        info.protocol = s.protocol;
        info.name = s.name;
        info.url = s.url;
        info.old_version = s.version;
        ++report.checked;
        try {
            std::map<std::string, std::string> hdrs;
            hdrs["cache-control"] = "no-cache";
            auto bytes = DownloadBinary(s.url, network, hdrs);
            const std::string text(bytes.begin(), bytes.end());
            if (HashName(text) == s.hash) {
                info.status = "latest";
                ++report.latest;
                report.items.push_back(std::move(info));
                continue;
            }
            // 内容变化：确认条目仍在（检查期间可能被用户删掉），然后走导入
            // 路径——AddContent 会按 source_url 匹配旧条目并替换，列表不新增。
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                bool still_there = false;
                for (const auto& e : impl_->entries)
                    if (e->info.source_url == s.url) { still_there = true; break; }
                if (!still_there) {
                    info.status = "no_url";
                    info.detail = "检查期间该音源已被删除";
                    report.items.push_back(std::move(info));
                    continue;
                }
            }
            ProtocolImportReport r = impl_->AddContent(text, s.url, {});
            if (r.ok && r.updated) {
                // 替换完成后回读新条目的版本号给报告用。
                {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    for (const auto& e : impl_->entries)
                        if (e->info.source_url == s.url)
                            info.new_version = e->info.version;
                }
                info.status = "updated";
                ++report.updated;
                if (!r.message.empty()) info.detail = r.message;
            } else if (r.ok) {
                // 导入成功但没走替换分支（例如内容又变回与其它条目相同）。
                info.status = "latest";
                ++report.latest;
                if (!r.message.empty()) info.detail = r.message;
            } else {
                info.status = "failed";
                ++report.failed;
                for (const auto& e : r.errors)
                    info.detail += (info.detail.empty() ? "" : "; ") + e;
                if (info.detail.empty()) info.detail = r.message;
            }
        } catch (const std::exception& e) {
            info.status = "failed";
            info.detail = e.what();
            ++report.failed;
        }
        report.items.push_back(std::move(info));
    }
    return report;
}

std::vector<ProtocolSourceInfo> SourceProtocolRegistry::List() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<ProtocolSourceInfo> out;
    out.reserve(impl_->entries.size());
    for (const auto& e : impl_->entries) out.push_back(e->info);
    return out;
}

std::size_t SourceProtocolRegistry::Count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->entries.size();
}

bool SourceProtocolRegistry::RemoveAt(std::size_t index) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->entries.size()) return false;
    std::error_code ec;
    std::filesystem::remove(impl_->entries[index]->file, ec);
    impl_->entries.erase(impl_->entries.begin() +
                         static_cast<std::ptrdiff_t>(index));
    impl_->Persist();
    return true;
}

std::size_t SourceProtocolRegistry::RemoveMany(
    const std::vector<std::size_t>& indices) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::size_t> sorted(indices);
    std::sort(sorted.begin(), sorted.end(), std::greater<std::size_t>());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::size_t removed = 0;
    // 倒序删除：每次 erase 只影响下标更大的条目，而这些已经处理过了。
    for (const std::size_t index : sorted) {
        if (index >= impl_->entries.size()) continue;
        std::error_code ec;
        std::filesystem::remove(impl_->entries[index]->file, ec);
        impl_->entries.erase(impl_->entries.begin() +
                             static_cast<std::ptrdiff_t>(index));
        ++removed;
    }
    if (removed > 0) impl_->Persist();
    return removed;
}

std::string SourceProtocolRegistry::Describe(std::size_t index) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->entries.size()) return {};
    const auto& e = *impl_->entries[index];
    if (e.lx) return e.lx->Describe();
    if (e.rest) return e.rest->Describe();
    if (e.info.kind == SourceProtocol::kListen1) {
        std::string text = "协议：Listen1 Provider\n名称：" + e.info.name + "\n";
        if (!e.info.version.empty()) text += "版本：" + e.info.version + "\n";
        if (!e.info.author.empty()) text += "作者：" + e.info.author + "\n";
        text += "缓存：" + e.file.string() + "\n";
        text += "状态：已识别并登记元数据，运行时桥接尚未启用。\n";
        text += ProtocolDescription(SourceProtocol::kListen1);
        return text;
    }
    return "协议：" + std::string(ProtocolId(e.info.kind)) +
           "\n名称：" + e.info.name;
}

ProtocolResolveResult SourceProtocolRegistry::ResolveFallback(
    const ProtocolTrack& track) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ProtocolResolveResult out;
    std::vector<std::string> failures;
    for (const auto& e : impl_->entries) {
        ProtocolResolveResult r;
        try {
            if (e->lx && e->info.can_resolve)
                r = e->lx->ResolveUrl(track);
            else if (e->rest && e->info.can_resolve)
                r = e->rest->ResolveUrl(track);
        } catch (const std::exception& ex) {
            r.error = ex.what();
        }
        if (r.ok()) return r;
        if (!r.error.empty())
            failures.push_back(e->info.name + ": " + r.error);
    }
    out.error = failures.empty() ? "没有可用的其它协议音源"
                                 : "其它协议音源尝试结果：" ;
    for (const auto& f : failures) out.error += "\n- " + f;
    return out;
}

std::vector<ProtocolTrack> SourceProtocolRegistry::SearchAll(
    const std::string& query, int page) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<ProtocolTrack> out;
    for (const auto& e : impl_->entries) {
        if (!e->rest || !e->info.can_search) continue;
        auto hits = e->rest->Search(query, page);
        for (auto& h : hits) out.push_back(std::move(h));
    }
    return out;
}

SourceProtocolRegistry* AppSourceProtocolRegistry() {
    static std::once_flag flag;
    static std::unique_ptr<SourceProtocolRegistry> instance;
    std::call_once(flag, [] {
        instance = std::make_unique<SourceProtocolRegistry>();
        const std::filesystem::path dir = ExeDir();
        NetworkConfig network;
        if (auto* mgr = AppMusicFreeManager()) network = mgr->network();
        instance->Configure(dir / "musicfree" / "protocols", network,
                            dir / "musicfree" / "libs");
    });
    return instance.get();
}

} // namespace ttplayer::musicfree
