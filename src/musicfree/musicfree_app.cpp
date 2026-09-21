// MusicFree 兼容层 —— 应用级入口实现
//
// 推导缓存目录（<exe>/musicfree）、部署内置 JS 依赖库、启动时重建音源，
// 并提供"全部音源在线搜索 / 解析播放地址"的统一入口，供 UI 与播放引擎使用。

#include "ttplayer/musicfree/musicfree_app.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/musicfree/musicfree_plugin.h"

namespace ttplayer::musicfree {

namespace {

std::filesystem::path ExecutableDir() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) return {};
    exe.resize(n);
    return std::filesystem::path(exe).parent_path();
}

std::once_flag g_once;
std::unique_ptr<MusicFreeSourceManager> g_manager;

} // namespace

MusicFreeSourceManager* AppMusicFreeManager() {
    std::call_once(g_once, [] {
        const auto dir = ExecutableDir();
        if (dir.empty()) return;
        auto mgr = std::make_unique<MusicFreeSourceManager>(dir / "musicfree");
        mgr->SetAppVersion("0.1.0");
        g_manager = std::move(mgr);
    });
    return g_manager.get();
}

void EnsureMusicFreeLibsDeployed() {
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return;
    const auto dir = ExecutableDir();
    if (dir.empty()) return;
    mgr->InstallBundledLibs(dir / "musicfree" / "libs");
}

void LoadMusicFreeSourcesOnStartup() {
    EnsureMusicFreeLibsDeployed();
    if (auto* mgr = AppMusicFreeManager()) mgr->LoadCached();
}

void TraceDiagnostic(const std::string& tag, const std::string& text) {
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"TTPLAYER_MF_TRACE", flag, 8) == 0) return;
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0) return;
    const std::wstring path =
        std::wstring(temp, length) + L"ttplayer_mf_http.log";
    FILE* file = _wfopen(path.c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "----\n%s\n%s\n", tag.c_str(), text.c_str());
    std::fclose(file);
}

std::vector<OnlineSearchHit> SearchAllSources(const std::string& query,
                                              const std::string& platform,
                                              int page) {
    return SearchAllSources(query, platform, page, nullptr);
}

std::vector<OnlineSearchHit> SearchAllSources(const std::string& query,
                                              const std::string& platform,
                                              int page,
                                              OnlineSearchDiagnostics* diagnostics) {
    std::vector<OnlineSearchHit> out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr || query.empty()) return out;

    auto collect = [&](MusicFreePlugin* plugin) {
        if (!plugin || !plugin->IsMounted() || !plugin->meta().can_search) return;
        if (diagnostics) ++diagnostics->sources;
        SearchResult r = plugin->Search(query, page, "music");
        if (!r.error.empty() && diagnostics)
            diagnostics->errors.push_back(plugin->meta().name + ": " + r.error);
        if (!r.items.empty() && diagnostics) ++diagnostics->sources_with_hits;
        for (const auto& item : r.items) {
            OnlineSearchHit hit;
            hit.platform = item.platform.empty() ? plugin->meta().name : item.platform;
            hit.id = item.id;
            hit.title = item.title;
            hit.artist = item.artist;
            hit.album = item.album;
            hit.artwork = item.artwork;
            hit.duration_ms = item.duration_ms;
            out.push_back(std::move(hit));
        }
    };

    if (!platform.empty()) {
        collect(mgr->FindPlugin(platform));
    } else {
        for (const auto& meta : mgr->ListPlugins()) collect(mgr->FindPlugin(meta.name));
    }
    return out;
}

std::string ResolvePlayUrl(const std::string& platform, const std::string& id,
                           std::string* error) {
    auto* mgr = AppMusicFreeManager();
    if (!mgr) {
        if (error) *error = "MusicFree 管理器未初始化";
        return {};
    }
    MusicFreePlugin* plugin = mgr->FindPlugin(platform);
    if (!plugin || !plugin->IsMounted()) {
        if (error) *error = "找不到音源「" + platform + "」";
        return {};
    }
    MusicItem item;
    item.id = id;
    item.platform = platform;
    item.raw = nlohmann::json::object();
    item.raw["id"] = id;
    item.raw["platform"] = platform;
    auto ms = plugin->GetMediaSource(item, QualityKey::kStandard, error);
    return ms ? ms->url : std::string{};
}

} // namespace ttplayer::musicfree
