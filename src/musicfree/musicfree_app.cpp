// MusicFree 兼容层 —— 应用级入口实现
//
// 推导缓存目录（<exe>/musicfree）、部署内置 JS 依赖库、启动时重建音源，
// 并提供"全部音源在线搜索 / 解析播放地址"的统一入口，供 UI 与播放引擎使用。

#include "ttplayer/musicfree/musicfree_app.h"

#include "ttplayer/app/startup_trace.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/musicfree/musicfree_plugin.h"
#include "ttplayer/musicfree/source_protocol.h"
#include "ttplayer/musicfree/source_protocol_registry.h"

namespace ttplayer::musicfree {

namespace {

std::filesystem::path ExecutableDir() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) return {};
    exe.resize(n);
    return std::filesystem::path(exe).parent_path();
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), n);
    return out;
}

std::string JsonString(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return {};
    return j[key].get<std::string>();
}

// 把「额外保存的 raw」与本次解析的 platform/id 合成插件要的完整对象。
// 以前 ResolvePlayUrl 只用 id/platform 现造一个两字段对象，咪咕插件的
// resolveWithFallback 因此拿不到 contentId / copyrightId，最终报
//「插件调用被拒绝」。这里以 raw 为准，只在缺失时补基础字段。
nlohmann::json BuildItemRaw(const std::string& platform, const std::string& id,
                            const nlohmann::json* raw) {
    nlohmann::json j = raw && raw->is_object() ? *raw
                                               : nlohmann::json::object();
    if (!j.contains("id") || j["id"].is_null()) j["id"] = id;
    if (!j.contains("platform") || j["platform"].is_null())
        j["platform"] = platform;
    return j;
}

// 多协议兜底解析用的统一曲目描述。raw 里的私有字段原样带上，洛雪脚本
// （songmid / hash / copyrightId）与 REST 模板（{id}）都能取到自己要的值。
ProtocolTrack BuildProtocolTrack(const std::string& platform,
                                 const std::string& id,
                                 const nlohmann::json& j) {
    ProtocolTrack track;
    track.platform = platform;
    track.id = id;
    track.raw = j;
    track.title = JsonString(j, "title");
    if (track.title.empty()) track.title = JsonString(j, "name");
    track.artist = JsonString(j, "artist");
    if (track.artist.empty()) track.artist = JsonString(j, "singer");
    track.album = JsonString(j, "album");
    track.artwork = JsonString(j, "artwork");
    if (j.is_object()) {
        if (j.contains("duration_ms") && j["duration_ms"].is_number())
            track.duration_ms = j["duration_ms"].get<std::int64_t>();
        else if (j.contains("duration") && j["duration"].is_number())
            track.duration_ms = static_cast<std::int64_t>(
                j["duration"].get<double>() * 1000);
    }
    return track;
}

std::once_flag g_once;
std::unique_ptr<MusicFreeSourceManager> g_manager;

// 后台音源重建的状态。started/done 受 mutex 保护，worker 只在主线程 join/detach。
struct AsyncSourcesState {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool started = false;
    bool done = false;
    std::atomic<bool> stop{false};
    ~AsyncSourcesState() {
        if (worker.joinable()) worker.detach();
    }
};

AsyncSourcesState& AsyncSources() {
    static AsyncSourcesState state;
    return state;
}

// ---- 解析结果缓存（对齐 MusicFree 的 MediaCache）--------------------------
//
// MusicFree 的 PluginManager.getMediaSource 把 {url, headers, userAgent} 写进
// Cache.update(musicItem, ...)，重复点同一首歌直接命中；本兼容层以前完全没有
// 这一层，导致「音乐窗解析一次 + 播放器再解析一次」以及切歌回来都要把插件
// 与兜底音源全量重跑。实测一次兜底解析串行发 28 个 HTTP 请求、16s 以上。
// CDN 直链有时效，故给 20 分钟 TTL；进程内 map + 磁盘 JSON 双份，进程重启
// 后仍能复用。缓存命中连 WaitForMusicFreeSources() 都不需要等。
constexpr std::int64_t kResolveCacheTtlSeconds = 20 * 60;
constexpr size_t kResolveCacheMaxEntries = 256;

struct ResolveCacheEntry {
    MediaSource source;
    std::int64_t saved_at = 0;
};

std::mutex g_resolve_cache_mutex;
std::map<std::string, ResolveCacheEntry> g_resolve_cache;
bool g_resolve_cache_loaded = false;

std::int64_t ResolveNowSeconds() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path ResolveCachePath() {
    const auto dir = ExecutableDir();
    if (dir.empty()) return {};
    return dir / "musicfree" / "resolve_cache.json";
}

std::string MakeResolveCacheKey(const std::string& platform,
                                const std::string& id) {
    // 解析质量固定 QualityKey::kStandard，不必进 key。
    return platform + "\x1f" + id;
}

void LoadResolveCacheLocked() {
    if (g_resolve_cache_loaded) return;
    g_resolve_cache_loaded = true;
    const auto path = ResolveCachePath();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() ||
        !j.contains("entries") || !j["entries"].is_object()) return;
    const std::int64_t now = ResolveNowSeconds();
    for (auto it = j["entries"].begin(); it != j["entries"].end(); ++it) {
        const auto& e = it.value();
        if (!e.is_object()) continue;
        const std::string url = JsonString(e, "url");
        if (url.empty()) continue;
        const std::int64_t saved =
            e.contains("saved_at") && e["saved_at"].is_number()
                ? e["saved_at"].get<std::int64_t>() : 0;
        if (saved <= 0 || now - saved >= kResolveCacheTtlSeconds) continue;
        ResolveCacheEntry entry;
        entry.saved_at = saved;
        entry.source.url = url;
        entry.source.user_agent = JsonString(e, "user_agent");
        if (e.contains("headers") && e["headers"].is_object()) {
            for (auto h = e["headers"].begin(); h != e["headers"].end(); ++h)
                if (h.value().is_string())
                    entry.source.headers[h.key()] = h.value().get<std::string>();
        }
        g_resolve_cache[it.key()] = std::move(entry);
    }
}

void SaveResolveCacheLocked() {
    const auto path = ResolveCachePath();
    if (path.empty()) return;
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& kv : g_resolve_cache) {
        nlohmann::json e = nlohmann::json::object();
        e["url"] = kv.second.source.url;
        e["user_agent"] = kv.second.source.user_agent;
        e["headers"] = kv.second.source.headers;
        e["saved_at"] = kv.second.saved_at;
        entries[kv.first] = std::move(e);
    }
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    root["entries"] = std::move(entries);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::wstring tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(std::filesystem::path(tmp),
                          std::ios::binary | std::ios::trunc);
        if (!out) return;
        const std::string text = root.dump(2);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!MoveFileExW(tmp.c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        std::filesystem::remove(tmp, ec);
}

std::optional<MediaSource> ResolveCacheGet(const std::string& platform,
                                           const std::string& id) {
    std::lock_guard<std::mutex> lock(g_resolve_cache_mutex);
    LoadResolveCacheLocked();
    const auto it = g_resolve_cache.find(MakeResolveCacheKey(platform, id));
    if (it == g_resolve_cache.end()) return std::nullopt;
    if (ResolveNowSeconds() - it->second.saved_at >= kResolveCacheTtlSeconds) {
        g_resolve_cache.erase(it);
        return std::nullopt;
    }
    return it->second.source;
}

void ResolveCachePut(const std::string& platform, const std::string& id,
                     const MediaSource& source) {
    if (source.url.empty()) return;
    std::lock_guard<std::mutex> lock(g_resolve_cache_mutex);
    LoadResolveCacheLocked();
    g_resolve_cache[MakeResolveCacheKey(platform, id)] =
        ResolveCacheEntry{source, ResolveNowSeconds()};
    const std::int64_t now = ResolveNowSeconds();
    for (auto it = g_resolve_cache.begin(); it != g_resolve_cache.end();) {
        if (now - it->second.saved_at >= kResolveCacheTtlSeconds)
            it = g_resolve_cache.erase(it);
        else ++it;
    }
    while (g_resolve_cache.size() > kResolveCacheMaxEntries) {
        auto oldest = g_resolve_cache.begin();
        for (auto it = g_resolve_cache.begin(); it != g_resolve_cache.end(); ++it)
            if (it->second.saved_at < oldest->second.saved_at) oldest = it;
        g_resolve_cache.erase(oldest);
    }
    SaveResolveCacheLocked();
}

} // namespace

void InvalidateResolveCache(const std::string& platform, const std::string& id) {
    std::lock_guard<std::mutex> lock(g_resolve_cache_mutex);
    LoadResolveCacheLocked();
    if (g_resolve_cache.erase(MakeResolveCacheKey(platform, id)) > 0)
        SaveResolveCacheLocked();
}

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
    TT_STARTUP_TRACE("mf_libs_start");
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return;
    const auto dir = ExecutableDir();
    if (dir.empty()) return;
    mgr->InstallBundledLibs(dir / "musicfree" / "libs");
    TT_STARTUP_TRACE("mf_libs_done");
}

void LoadMusicFreeSourcesOnStartup() {
    TT_STARTUP_TRACE("mf_startup_start");
    EnsureMusicFreeLibsDeployed();
    TT_STARTUP_TRACE("mf_loadcached_start");
    if (auto* mgr = AppMusicFreeManager()) mgr->LoadCached();
    TT_STARTUP_TRACE("mf_loadcached_done");
    // 洛雪 / REST / Listen1 音源由注册表单独持久化；依赖库必须先部署完，
    // LX 适配器挂载时才能从 <exe>/musicfree/libs 取到垫片。
    if (auto* registry = AppSourceProtocolRegistry()) {
        registry->SetNetworkConfig(AppMusicFreeManager()
                                       ? AppMusicFreeManager()->network()
                                       : NetworkConfig{});
        TT_STARTUP_TRACE("protocols_load_start");
        registry->Load();
        TT_STARTUP_TRACE("protocols_load_done");
    }
    TT_STARTUP_TRACE("mf_startup_done");
}

void StartMusicFreeSourcesLoading(void* notify_hwnd, unsigned notify_msg) {
    auto& state = AsyncSources();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.started) return;
        state.started = true;
    }
    const HWND hwnd = reinterpret_cast<HWND>(notify_hwnd);
    const UINT msg = static_cast<UINT>(notify_msg);
    state.worker = std::thread([hwnd, msg] {
        auto& s = AsyncSources();
        {
            // 可中断的 500ms 让路：先让主窗口独占 CPU 画完首帧。若期间
            // 用户直接关闭窗口，Finish 会置 stop 并唤醒这里立即收工。
            std::unique_lock<std::mutex> lock(s.mutex);
            s.cv.wait_for(lock, std::chrono::milliseconds(500),
                          [&s] { return s.stop.load(); });
        }
        if (s.stop.load()) {
            TT_STARTUP_TRACE("bg_sources_cancelled");
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.done = true;
            }
            s.cv.notify_all();
            return;
        }
        // 后台模式：调度优先级与页错误优先级一起降，主线程永远优先。
        // THREAD_MODE_BACKGROUND_BEGIN 在 Vista+ 可用，失败再退回 IDLE。
        if (!SetThreadPriority(GetCurrentThread(), 0x00010000))
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
        TT_STARTUP_TRACE("bg_sources_start");
        try {
            LoadMusicFreeSourcesOnStartup();
        } catch (...) {
            // 线程边界不能抛异常；单个音源失败已在 LoadCached/Load 内部消化。
        }
        TT_STARTUP_TRACE("bg_sources_done");
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.done = true;
        }
        s.cv.notify_all();
        if (hwnd && msg) PostMessageW(hwnd, msg, 0, 0);
        // 音源就绪后做一次自动检查更新（复刻 MusicFree 的插件更新机制：
        // 24 小时间隔 + 开关持久化，全部在后台线程，插件之间可被退出中断）。
        try {
            MaybeAutoUpdateSources();
        } catch (...) {
            // 更新失败不影响本次会话：缓存里的插件照常可用。
        }
    });
}

void WaitForMusicFreeSources() {
    auto& state = AsyncSources();
    std::unique_lock<std::mutex> lock(state.mutex);
    if (!state.started) {
        // 没有后台任务（工具进程 / 尚未进入启动流程）时退化为同步加载，
        // 并登记 done，避免后续入口把 20 个插件重复挂载一遍。
        state.started = true;
        lock.unlock();
        try {
            LoadMusicFreeSourcesOnStartup();
        } catch (...) {}
        lock.lock();
        state.done = true;
        state.cv.notify_all();
        return;
    }
    state.cv.wait(lock, [&state] { return state.done; });
}

void FinishMusicFreeSourcesLoading() {
    auto& state = AsyncSources();
    if (!state.worker.joinable()) return;
    // 请求取消并唤醒让路等待；LoadCached() 也会在插件之间检查该标志，
    // 因此 join 只需再等当前这一个插件（约 0.3s），不会拖慢关闭。
    state.stop.store(true);
    state.cv.notify_all();
    state.worker.join();
}

bool ShouldStopMusicFreeLoading() {
    return AsyncSources().stop.load();
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

nlohmann::json RestoreSourceRaw(
    const std::vector<std::pair<std::string, std::string>>& metadata) {
    for (const auto& kv : metadata) {
        if (kv.first != "SourceRaw" || kv.second.empty()) continue;
        auto j = nlohmann::json::parse(kv.second, nullptr, false);
        if (!j.is_discarded() && j.is_object()) return j;
    }
    // 旧版条目没有 SourceRaw：用元数据袋里已有的字段拼一个最小对象，
    // 至少让洛雪音源能按「歌名 + 歌手」去找直链。
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : metadata) {
        if (kv.second.empty()) continue;
        if (kv.first == "Title") j["title"] = kv.second;
        else if (kv.first == "Artist") j["artist"] = kv.second;
        else if (kv.first == "Album") j["album"] = kv.second;
        else if (kv.first == "Artwork") j["artwork"] = kv.second;
        else if (kv.first == "Platform") j["platform"] = kv.second;
        else if (kv.first == "SongId") j["id"] = kv.second;
    }
    return j;
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
    if (query.empty()) return out;
    WaitForMusicFreeSources();
    auto* mgr = AppMusicFreeManager();

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
            hit.raw = item.raw;  // 咪咕 contentId / copyrightId 等私有字段
            out.push_back(std::move(hit));
        }
    };

    if (mgr) {
        if (!platform.empty()) {
            collect(mgr->FindPlugin(platform));
        } else {
            for (const auto& meta : mgr->ListPlugins())
                collect(mgr->FindPlugin(meta.name));
        }
    }

    // 声明式 REST 音源也参与「全部音源搜索」。洛雪音源按协议只负责把歌曲
    // 信息换成直链，不提供搜索，因此不在这里出现；它的价值体现在解析兜底。
    // 用户明确选中了某个 MusicFree 插件时不再混入其它音源，避免结果串台。
    auto* registry = AppSourceProtocolRegistry();
    const bool musicfree_selected = mgr && !platform.empty() &&
                                    mgr->FindPlugin(platform) != nullptr;
    if (registry && !musicfree_selected) {
        if (diagnostics) {
            for (const auto& info : registry->List())
                if (info.can_search) ++diagnostics->sources;
        }
        const std::size_t before = out.size();
        for (const auto& track : registry->SearchAll(query, page)) {
            if (!platform.empty() && track.platform != platform) continue;
            OnlineSearchHit hit;
            hit.platform = track.platform;
            hit.id = track.id;
            hit.title = track.title;
            hit.artist = track.artist;
            hit.album = track.album;
            hit.artwork = track.artwork;
            hit.duration_ms = track.duration_ms;
            hit.raw = track.raw;
            out.push_back(std::move(hit));
        }
        if (diagnostics && out.size() > before) ++diagnostics->sources_with_hits;
    }
    return out;
}

std::optional<MediaSource> ResolvePlayUrlEx(const std::string& platform,
                                            const std::string& id,
                                            std::string* error,
                                            const nlohmann::json* raw) {
    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();

    // 第 0 路：解析结果缓存（对齐 MusicFree 的 MediaCache）。命中直接返回，
    // 连 WaitForMusicFreeSources() 都不必等 —— 冷启动早期点已解析过的歌也能秒开。
    if (auto cached = ResolveCacheGet(platform, id)) {
        TraceDiagnostic("RESOLVE_TIMING",
            "route=cache_hit\nplatform=" + platform + "\nid=" + id +
            "\ncache=hit\ntotal_ms=0");
        return cached;
    }

    WaitForMusicFreeSources();
    const auto t_wait_done = Clock::now();
    const nlohmann::json item_raw = BuildItemRaw(platform, id, raw);
    const ProtocolTrack track = BuildProtocolTrack(platform, id, item_raw);
    std::string primary_error;

    // 分段计时（默认零文件 I/O，仅 TTPLAYER_MF_TRACE=1 时落日志）。
    auto emit_timing = [&](const char* route, Clock::time_point t_plugin_done,
                           Clock::time_point t_fallback_done) {
        const auto ms = [](Clock::time_point a, Clock::time_point b) {
            return std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
        };
        TraceDiagnostic("RESOLVE_TIMING",
            std::string("route=") + route + "\nplatform=" + platform +
            "\nid=" + id + "\ncache=miss" +
            "\nwait_ms=" + ms(t_start, t_wait_done) +
            "\nplugin_ms=" + ms(t_wait_done, t_plugin_done) +
            "\nfallback_ms=" + ms(t_plugin_done, t_fallback_done) +
            "\ntotal_ms=" + ms(t_start, t_fallback_done));
    };

    // 第一路：MusicFree 插件。完整 raw 回传后，咪咕/酷狗这类依赖私有字段的
    // 插件不再因为缺 contentId、hash 而在 resolveWithFallback 里被拒。
    auto* mgr = AppMusicFreeManager();
    MusicFreePlugin* plugin = mgr ? mgr->FindPlugin(platform) : nullptr;
    if (plugin && plugin->IsMounted()) {
        MusicItem item;
        item.id = id;
        item.platform = platform;
        item.raw = item_raw;
        item.title = track.title;
        item.artist = track.artist;
        item.album = track.album;
        item.artwork = track.artwork;
        item.duration_ms = track.duration_ms;
        if (auto ms = plugin->GetMediaSource(item, QualityKey::kStandard,
                                             &primary_error)) {
            ResolveCachePut(platform, id, *ms);
            const auto now = Clock::now();
            emit_timing("plugin", now, now);
            return *ms;
        }
    } else {
        primary_error = "找不到 MusicFree 音源「" + platform + "」";
    }
    const auto t_plugin_done = Clock::now();

    // 第二路：其它协议音源。REST 音源本身就可能是曲目的来源平台；洛雪音源
    // 则在 MusicFree 插件解析失败时按歌名/歌手/私有 id 补一条可用直链。
    // 兜底脚本会串行试探多条线路（实测 28 个请求），失效线路若按全局 20s
    // 超时等待会把一次解析拖到十几秒；这里只把兜底段的单请求超时压到 6s。
    std::string fallback_error;
    {
        ScopedHttpTimeout short_timeout(6000);
        if (auto* registry = AppSourceProtocolRegistry()) {
            try {
                ProtocolResolveResult result = registry->ResolveFallback(track);
                if (result.ok()) {
                    MediaSource ms;
                    ms.url = std::move(result.url);
                    ms.headers = std::move(result.headers);
                    ms.user_agent = std::move(result.user_agent);
                    ResolveCachePut(platform, id, ms);
                    TraceDiagnostic("PROTOCOL_FALLBACK",
                        "platform=" + platform + "\nid=" + id +
                        "\nvia=" + result.via + "\nurl=" + ms.url);
                    emit_timing("fallback", t_plugin_done, Clock::now());
                    return ms;
                }
                fallback_error = std::move(result.error);
            } catch (const std::exception& e) {
                fallback_error = e.what();
            }
        }
    }

    emit_timing("failed", t_plugin_done, Clock::now());
    if (error) {
        *error = primary_error.empty() ? "无法解析播放地址" : primary_error;
        if (!fallback_error.empty()) *error += "；其它协议音源：" + fallback_error;
    }
    return std::nullopt;
}

std::string ResolvePlayUrl(const std::string& platform, const std::string& id,
                           std::string* error, const nlohmann::json* raw) {
    const auto ms = ResolvePlayUrlEx(platform, id, error, raw);
    return ms ? ms->url : std::string{};
}

// ---- 内容浏览：排行榜 / 推荐歌单 -------------------------------------------

// 发现页：列出具备任一内容能力的音源。调用方必须在工作线程里执行，
// 因为这里要等后台插件挂载完成；挂载完成后是无锁的零开销判断。
std::vector<std::string> ListDiscoverPlatforms() {
    WaitForMusicFreeSources();
    std::vector<std::string> out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return out;
    for (const auto& meta : mgr->ListPlugins()) {
        if (meta.can_top_lists || meta.can_recommend)
            out.push_back(meta.name);
    }
    return out;
}

// 单个音源的「榜单 + 推荐」聚合。任何一步失败都只记诊断、不影响其它音源；
// 返回的 sheet 保留插件原始 raw，打开详情时才能还原 contentId 等私有字段。
DiscoverOutcome GetDiscoverSheets(const std::string& platform,
                                  int max_top, int max_recommend) {
    WaitForMusicFreeSources();
    DiscoverOutcome out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr) { out.error = "音源管理器不可用"; return out; }
    MusicFreePlugin* plugin = mgr->FindPlugin(platform);
    if (!plugin || !plugin->IsMounted()) {
        out.error = "音源未挂载";
        return out;
    }
    const auto& meta = plugin->meta();

    if (meta.can_top_lists) {
        try {
            auto r = plugin->GetTopLists();
            if (r) {
                for (auto& s : r->items) {
                    if (max_top-- <= 0) break;
                    if (s.platform.empty()) s.platform = platform;
                    const std::string origin = s.group;
                    s.group = origin.empty() ? "榜单" : "榜单 · " + origin;
                    out.sheets.push_back(std::move(s));
                }
                if (!r->error.empty()) out.error = r->error;
            }
        } catch (const std::exception& e) {
            out.error = e.what();
        } catch (...) {
            out.error = "榜单读取异常";
        }
    }

    // 推荐只取第一个标签的第一页：多个标签会成倍增加网络请求，慢音源
    // 不该拖住整个发现页（用户仍可从卡片进入对应歌单）。
    if (meta.can_recommend) {
        try {
            auto tags = plugin->GetRecommendSheetTags();
            if (tags && !tags->tags.empty()) {
                const SheetTag& tag = tags->tags.front();
                auto r = plugin->GetRecommendSheetsByTag(tag.raw, 1);
                if (r) {
                    const std::string prefix = "推荐" +
                        (tag.title.empty() ? std::string() : " · " + tag.title);
                    for (auto& s : r->items) {
                        if (max_recommend-- <= 0) break;
                        if (s.platform.empty()) s.platform = platform;
                        const std::string origin = s.group;
                        s.group = origin.empty() ? prefix : prefix + " · " + origin;
                        out.sheets.push_back(std::move(s));
                    }
                    if (!r->error.empty() && out.error.empty()) out.error = r->error;
                }
            }
        } catch (const std::exception& e) {
            if (out.error.empty()) out.error = e.what();
        } catch (...) {
            if (out.error.empty()) out.error = "推荐歌单读取异常";
        }
    }
    return out;
}

std::vector<MusicSheet> GetAllTopLists() {
    WaitForMusicFreeSources();
    std::vector<MusicSheet> out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return out;
    for (const auto& meta : mgr->ListPlugins()) {
        MusicFreePlugin* plugin = mgr->FindPlugin(meta.name);
        if (!plugin || !plugin->IsMounted() || !plugin->meta().can_top_lists)
            continue;
        auto result = plugin->GetTopLists();
        if (!result || result->items.empty()) continue;
        for (auto& sheet : result->items) {
            if (sheet.platform.empty()) sheet.platform = meta.name;
            out.push_back(std::move(sheet));
        }
    }
    return out;
}

std::vector<std::string> ListTopListPlatforms() {
    std::vector<std::string> out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return out;
    for (const auto& meta : mgr->ListPlugins())
        if (meta.can_top_lists) out.push_back(meta.name);
    return out;
}

std::vector<std::string> ListRecommendPlatforms() {
    std::vector<std::string> out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return out;
    for (const auto& meta : mgr->ListPlugins())
        if (meta.can_recommend) out.push_back(meta.name);
    return out;
}

std::vector<SheetTag> GetRecommendTags(const std::string& platform) {
    WaitForMusicFreeSources();
    std::vector<SheetTag> out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return out;
    MusicFreePlugin* plugin = mgr->FindPlugin(platform);
    if (!plugin || !plugin->IsMounted() || !plugin->meta().can_recommend)
        return out;
    auto result = plugin->GetRecommendSheetTags();
    if (result) out = std::move(result->tags);
    return out;
}

std::vector<MusicSheet> GetRecommendSheets(const std::string& platform,
                                           const nlohmann::json& tag, int page) {
    WaitForMusicFreeSources();
    std::vector<MusicSheet> out;
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return out;
    MusicFreePlugin* plugin = mgr->FindPlugin(platform);
    if (!plugin || !plugin->IsMounted() || !plugin->meta().can_recommend)
        return out;
    auto result = plugin->GetRecommendSheetsByTag(tag, page);
    if (result) {
        for (auto& sheet : result->items) {
            if (sheet.platform.empty()) sheet.platform = platform;
            out.push_back(std::move(sheet));
        }
    }
    return out;
}

// 打开歌单诊断：仅当 TTPLAYER_SHEET_TRACE=1 时写 %TEMP%\ttplayer_sheet.log，
// 默认零文件 I/O。用于定位「打开歌单没有返回曲目」这类插件侧静默失败。
void SheetTrace(const std::string& line) {
    if (GetEnvironmentVariableW(L"TTPLAYER_SHEET_TRACE", nullptr, 0) == 0) return;
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return;
    const std::filesystem::path path = std::filesystem::path(tmp) / L"ttplayer_sheet.log";
    std::ofstream f(path, std::ios::app);
    if (!f) return;
    f << line << '\n';
}

SheetTracksOutcome GetSheetTracks(const std::string& platform,
                                  const nlohmann::json& sheet, SheetKind kind,
                                  int page) {
    WaitForMusicFreeSources();
    SheetTracksOutcome out;
    std::string sheet_dump = sheet.dump();
    if (sheet_dump.size() > 400) sheet_dump.resize(400);
    SheetTrace("CALL platform=" + platform +
               " kind=" + (kind == SheetKind::kTopList ? "toplist" : "sheet") +
               " page=" + std::to_string(page) + " sheet=" + sheet_dump);
    auto* mgr = AppMusicFreeManager();
    if (!mgr) return out;
    MusicFreePlugin* plugin = mgr->FindPlugin(platform);
    if (!plugin || !plugin->IsMounted()) {
        out.error = "找不到音源「" + platform + "」";
        SheetTrace("NO_PLUGIN " + out.error);
        return out;
    }
    // 榜单优先 getTopListDetail、歌单优先 getMusicSheetInfo；但不少插件只实现
    // 其中一个，或只对特定 id 走 getTopListDetail 的某个分支（其余分支隐式返回
    // undefined）。因此首选方法缺失 / 返回空时，自动用另一个方法兜底再试一次。
    auto primary = [&]() -> std::optional<SheetTracksResult> {
        return kind == SheetKind::kTopList
                   ? plugin->GetTopListDetail(sheet, page)
                   : plugin->GetMusicSheetInfo(sheet, page);
    };
    auto alternate = [&]() -> std::optional<SheetTracksResult> {
        return kind == SheetKind::kTopList
                   ? plugin->GetMusicSheetInfo(sheet, page)
                   : plugin->GetTopListDetail(sheet, page);
    };
    // 插件刚挂载完成时偶尔返回 pending Promise 或空列表（冷启动点第一张卡片
    // 尤其明显）：这类是瞬态失败，稍后再试一次即可拿到数据。过去的表现是
    // 「第一次点没曲目、第二次点同一张就有了」，这里把它做成自动重试。
    const auto transient = [](const std::string& error) {
        return error.empty() ||
               error.find("Promise 未决") != std::string::npos ||
               error.find("超时") != std::string::npos ||
               error.find("异步任务异常") != std::string::npos ||
               error.find("then 调用异常") != std::string::npos;
    };
    std::optional<SheetTracksResult> result;
    for (int attempt = 0; attempt < 3; ++attempt) {
        result = primary();
        if (!result) {
            result = alternate();
            if (result) SheetTrace("FALLBACK_METHOD_MISSING");
        }
        if (result && result->items.empty()) {
            const std::string first_error = result->error;
            auto fallback = alternate();
            if (fallback && !fallback->items.empty()) {
                result = std::move(fallback);
                SheetTrace("FALLBACK_EMPTY first_error=" + first_error);
            }
        }
        const bool empty = !result || result->items.empty();
        const std::string reason = result ? result->error : std::string();
        if (!empty || !transient(reason) || attempt == 2) break;
        SheetTrace("RETRY attempt=" + std::to_string(attempt + 1) +
                   " reason=" + reason);
        result.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!result) {
        out.error = "音源「" + platform + "」不支持该内容方法";
        SheetTrace("NO_METHOD " + out.error);
        return out;
    }
    out.items = std::move(result->items);
    out.is_end = result->is_end;
    // 这里曾经写成 `if (!out.error.empty())`，条件恰好反了：out.error 初始为空，
    // 于是插件抛出的异常（网络失败 / Promise 未决 / 参数拒绝）永远被吞掉，
    // UI 只能显示「该歌单/排行榜没有返回曲目」这句没有任何线索的话。
    if (out.error.empty()) out.error = result->error;
    // 插件正常返回但曲目为空时，也给一条可诊断的提示。
    if (out.items.empty() && out.error.empty())
        out.error = "插件返回了空的曲目列表（该榜单暂无数据或网络请求失败）";
    // 与 Search 保持一致：插件返回的曲目常不带 platform，不回填会导致
    // 随后双击播放时 ResolvePlayUrl 找不到音源。raw 里也要带上。
    for (auto& item : out.items) {
        if (item.platform.empty()) {
            item.platform = platform;
            if (item.raw.is_object()) item.raw["platform"] = platform;
        }
    }
    std::string first;
    for (size_t i = 0; i < out.items.size() && i < 3; ++i)
        first += (i ? " | " : "") + out.items[i].title;
    SheetTrace("RESULT items=" + std::to_string(out.items.size()) +
               " error=" + out.error + " first=" + first);
    return out;
}

bool DownloadTrackFile(const std::wstring& url, const std::filesystem::path& path,
                       std::string* error) {
    auto* mgr = AppMusicFreeManager();
    if (!mgr) {
        if (error) *error = "MusicFree 管理器未初始化";
        return false;
    }
    if (url.empty()) {
        if (error) *error = "播放地址为空，无法下载";
        return false;
    }
    if (path.empty()) {
        if (error) *error = "未指定保存路径";
        return false;
    }

    // 下载与写盘都放在调用方的工作线程里（音乐窗就是这么用的），这里只做同步 IO。
    std::vector<std::uint8_t> bytes;
    try {
        bytes = DownloadBinary(WideToUtf8(url), mgr->network());
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = "下载时发生未知错误";
        return false;
    }

    // 用 _wfopen/_wremove 直接吃宽路径：目标目录常含中文，走 std::ofstream 在某些
    // 区域设置下会把路径按 ANSI 降级成 '?'。
    const std::wstring native = path.wstring();
    FILE* file = _wfopen(native.c_str(), L"wb");
    if (!file) {
        if (error) *error = "无法写入文件：" + WideToUtf8(native);
        return false;
    }
    const size_t written =
        bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool size_ok = written == bytes.size();
    std::fclose(file);
    if (!size_ok) {
        _wremove(native.c_str());  // 半截文件比没有文件更糟，直接清掉。
        if (error) *error = "写入文件时字节数不完整（磁盘空间不足？）";
        return false;
    }
    return true;
}

// ---- 下载落点的取名规则 ----------------------------------------------------
//
// 这三段逻辑原本只长在音乐窗里；播放列表的「下载歌曲」也要用同一套命名，所以提到
// 这里共用，免得两处各写一份、行为慢慢分叉。

std::wstring SanitizeFileName(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
            ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || ch < 0x20)
            out.push_back(L'_');
        else
            out.push_back(ch);
    }
    // 结尾的空格和点会被资源管理器直接吞掉（或拒绝创建），先剪干净。
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    return out.empty() ? std::wstring(L"未命名曲目") : out;
}

std::wstring GuessExtension(const std::wstring& url) {
    std::wstring path = url;
    const size_t query = path.find_first_of(L"?#");
    if (query != std::wstring::npos) path.erase(query);
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L'/');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return L".mp3";
    std::wstring extension = path.substr(dot);
    if (extension.size() > 6) return L".mp3";
    for (wchar_t& ch : extension) ch = static_cast<wchar_t>(towlower(ch));
    for (const wchar_t* known : {L".mp3", L".flac", L".m4a", L".aac", L".wav",
                                L".ogg", L".wma"})
        if (extension == known) return extension;
    return L".mp3";
}

std::wstring DefaultDownloadName(const std::string& title,
                                 const std::string& artist,
                                 const std::wstring& url) {
    const std::wstring wide_title = Utf8ToWide(title);
    const std::wstring wide_artist = Utf8ToWide(artist);
    std::wstring name;
    if (!wide_artist.empty() && !wide_title.empty())
        name = wide_artist + L" - " + wide_title;
    else
        name = wide_title.empty() ? wide_artist : wide_title;
    return SanitizeFileName(name) + GuessExtension(url);
}

std::filesystem::path DefaultDownloadDirectory() {
    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return {};
    const std::filesystem::path home(profile);
    for (const wchar_t* name : {L"Music", L"音乐", L"我的音乐"}) {
        std::error_code error;
        const auto candidate = home / name;
        if (std::filesystem::is_directory(candidate, error) && !error)
            return candidate;
    }
    return home;
}

// ---- 音源自动更新（复刻 MusicFree 的插件更新机制）----
//
// 状态持久化在 <exe>/musicfree/update_state.json：
//   { "auto_check": true, "last_check": <unix 秒> }
// 开关默认开启；间隔 24 小时。检查动作本身在 CheckForUpdates（manager/registry）
// 里逐音源进行，进程退出时通过 ShouldStopMusicFreeLoading() 提前收工。

namespace {

std::filesystem::path UpdateStatePath() {
    const auto dir = ExecutableDir();
    if (dir.empty()) return {};
    return dir / "musicfree" / "update_state.json";
}

nlohmann::json LoadUpdateState() {
    nlohmann::json j = nlohmann::json::object();
    const auto path = UpdateStatePath();
    if (!path.empty()) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            try { f >> j; } catch (...) { j = nlohmann::json::object(); }
        }
    }
    if (!j.is_object()) j = nlohmann::json::object();
    return j;
}

void SaveUpdateState(const nlohmann::json& j) {
    const auto path = UpdateStatePath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) {
        try { f << j.dump(2); } catch (...) {}
    }
}

void MergeUpdateReport(SourceUpdateReport& total, SourceUpdateReport&& part) {
    total.checked += part.checked;
    total.updated += part.updated;
    total.latest += part.latest;
    total.failed += part.failed;
    for (auto& item : part.items) total.items.push_back(std::move(item));
    for (auto& e : part.errors) total.errors.push_back(std::move(e));
}

} // namespace

SourceUpdateReport CheckAllSourceUpdates() {
    SourceUpdateReport total;
    if (auto* mgr = AppMusicFreeManager())
        MergeUpdateReport(total, mgr->CheckForUpdates());
    if (auto* registry = AppSourceProtocolRegistry())
        MergeUpdateReport(total, registry->CheckForUpdates());
    return total;
}

void MaybeAutoUpdateSources() {
    const nlohmann::json state = LoadUpdateState();
    if (!state.value("auto_check", true)) return;
    const long long last = state.value("last_check", 0LL);
    const long long now = static_cast<long long>(::time(nullptr));
    if (last > 0 && now - last < 24LL * 3600LL) return;
    // 无论成功与否都记下时间：网络不可用时不在每次启动都重试拖慢启动，
    // 用户随时可以在导入窗里手动「检查更新」。
    try { CheckAllSourceUpdates(); } catch (...) {}
    nlohmann::json out = LoadUpdateState();
    out["auto_check"] = state.value("auto_check", true);
    out["last_check"] = now;
    SaveUpdateState(out);
}

bool AutoUpdateSourcesEnabled() {
    return LoadUpdateState().value("auto_check", true);
}

void SetAutoUpdateSourcesEnabled(bool enabled) {
    nlohmann::json j = LoadUpdateState();
    j["auto_check"] = enabled;
    SaveUpdateState(j);
}

} // namespace ttplayer::musicfree
