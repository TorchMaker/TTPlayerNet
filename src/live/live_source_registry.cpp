#include "ttplayer/live/live_source_registry.h"

#include <algorithm>

#include "../plugins/content_plugin_loader.h"

namespace ttplayer::live {

namespace {

using ttplayer::plugins::AppendContentLog;
using ttplayer::plugins::FindContentPlugins;
using ttplayer::plugins::GetPluginFactory;
using ttplayer::plugins::SetPluginError;

void WINAPI EmitChannel(void* ctx, const TtpLiveChannel* channel) {
    auto* out = static_cast<std::vector<LiveChannel>*>(ctx);
    if (!ctx || !channel || !channel->url || !*channel->url) return;
    LiveChannel item;
    if (channel->name) item.name = channel->name;
    item.url = channel->url;
    if (channel->group) item.group = channel->group;
    if (channel->logo) item.logo = channel->logo;
    out->push_back(std::move(item));
}

std::string Utf8(const std::wstring& value) {
    return ttplayer::core::WideToUtf8(value);
}

}  // namespace

LiveSourceRegistry& LiveSourceRegistry::Instance() {
    static LiveSourceRegistry instance;
    return instance;
}

LiveSourceRegistry::~LiveSourceRegistry() {
    for (auto& plugin : plugins_) {
        if (plugin.module) FreeLibrary(plugin.module);
    }
}

void LiveSourceRegistry::LoadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return;
    loaded_ = true;
    const auto files = FindContentPlugins(L"ttp_l_");
    for (const auto& path : files) {
        Plugin plugin;
        plugin.module = LoadLibraryW(path.c_str());
        if (!plugin.module) {
            AppendContentLog(L"live", "LOAD fail " + Utf8(path.filename().wstring()) +
                                          " err=" +
                                          std::to_string(GetLastError()));
            continue;
        }
        using Factory = HRESULT(WINAPI*)(TtpLiveSourceApi*);
        auto factory = reinterpret_cast<Factory>(
            GetPluginFactory(plugin.module, "ttpGetLiveSource"));
        if (!factory) {
            AppendContentLog(L"live", "LOAD no-factory " +
                                          Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
            continue;
        }
        TtpLiveSourceApi api{};
        if (FAILED(factory(&api)) || !api.api_version ||
            api.api_version() != TTPLAYER_LIVE_SOURCE_ABI_VERSION ||
            !api.enumerate) {
            AppendContentLog(L"live", "LOAD abi-mismatch " +
                                          Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
            continue;
        }
        TtpLiveSourceInfo info{};
        if (api.info) api.info(&info);
        plugin.info.id = info.id ? info.id : "";
        plugin.info.name = info.name ? info.name : plugin.info.id;
        plugin.info.version = info.vendor ? info.vendor : "";
        plugin.info.path = path.wstring();
        plugin.api = api;
        plugin.usable = !plugin.info.id.empty();
        if (plugin.usable) {
            AppendContentLog(L"live", "LOAD ok id=" + plugin.info.id + " name=" +
                                          plugin.info.name + " file=" +
                                          Utf8(path.filename().wstring()));
            plugins_.push_back(std::move(plugin));
        } else {
            AppendContentLog(L"live", "LOAD no-id " +
                                          Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
        }
    }
    AppendContentLog(L"live",
                     "LOAD total=" + std::to_string(plugins_.size()));
}

std::vector<LiveSourcePluginInfo> LiveSourceRegistry::Sources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LiveSourcePluginInfo> out;
    out.reserve(plugins_.size());
    for (const auto& plugin : plugins_) out.push_back(plugin.info);
    return out;
}

LiveSourceRegistry::Plugin* LiveSourceRegistry::FindLocked(
    const std::string& plugin_id) {
    for (auto& plugin : plugins_) {
        if (plugin.usable && plugin.info.id == plugin_id) return &plugin;
    }
    return nullptr;
}

const LiveSourceRegistry::Plugin* LiveSourceRegistry::FindLocked(
    const std::string& plugin_id) const {
    for (const auto& plugin : plugins_) {
        if (plugin.usable && plugin.info.id == plugin_id) return &plugin;
    }
    return nullptr;
}

bool LiveSourceRegistry::Enumerate(const std::string& plugin_id,
                                   const std::wstring& location,
                                   std::vector<LiveChannel>* out,
                                   std::string* error, int timeout_ms) {
    if (!out) return false;
    out->clear();
    LoadAll();
    std::lock_guard<std::mutex> lock(mutex_);
    Plugin* plugin = FindLocked(plugin_id);
    if (!plugin) {
        if (error) *error = "未安装直播源插件: " + plugin_id;
        AppendContentLog(L"live", "ENUM miss-plugin id=" + plugin_id);
        return false;
    }
    char buffer[512]{};
    const int rc = plugin->api.enumerate(location.c_str(), EmitChannel, out,
                                         buffer, sizeof(buffer), timeout_ms);
    if (rc != 0) {
        if (error) *error = buffer[0] ? buffer : "解析直播源失败";
        AppendContentLog(L"live", "ENUM fail id=" + plugin_id + " url=" +
                                      Utf8(location) + " err=" +
                                      (buffer[0] ? buffer : "unknown") +
                                      " channels=" + std::to_string(out->size()));
        return false;
    }
    AppendContentLog(L"live", "ENUM ok id=" + plugin_id + " url=" +
                                  Utf8(location) +
                                  " channels=" + std::to_string(out->size()));
    return true;
}

bool LiveSourceRegistry::EnumerateAuto(const std::wstring& location,
                                       std::vector<LiveChannel>* out,
                                       std::string* error,
                                       std::string* used_plugin,
                                       int timeout_ms) {
    if (!out) return false;
    out->clear();
    LoadAll();
    std::string best_id;
    int best_score = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& plugin : plugins_) {
            if (!plugin.usable) continue;
            int score = 1;
            if (plugin.api.probe) score = plugin.api.probe(location.c_str());
            if (score > best_score) {
                best_score = score;
                best_id = plugin.info.id;
            }
        }
    }
    if (best_id.empty()) {
        if (error) *error = "没有插件认识这个直播源";
        AppendContentLog(L"live", "ENUM auto no-plugin url=" + Utf8(location));
        return false;
    }
    if (used_plugin) *used_plugin = best_id;
    return Enumerate(best_id, location, out, error, timeout_ms);
}

}  // namespace ttplayer::live
