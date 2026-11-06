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
#include <fstream>
#include <memory>
#include <mutex>
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
    WaitForMusicFreeSources();
    const nlohmann::json item_raw = BuildItemRaw(platform, id, raw);
    const ProtocolTrack track = BuildProtocolTrack(platform, id, item_raw);
    std::string primary_error;

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
                                             &primary_error))
            return *ms;
    } else {
        primary_error = "找不到 MusicFree 音源「" + platform + "」";
    }

    // 第二路：其它协议音源。REST 音源本身就可能是曲目的来源平台；洛雪音源
    // 则在 MusicFree 插件解析失败时按歌名/歌手/私有 id 补一条可用直链。
    std::string fallback_error;
    if (auto* registry = AppSourceProtocolRegistry()) {
        try {
            ProtocolResolveResult result = registry->ResolveFallback(track);
            if (result.ok()) {
                MediaSource ms;
                ms.url = std::move(result.url);
                ms.headers = std::move(result.headers);
                ms.user_agent = std::move(result.user_agent);
                TraceDiagnostic("PROTOCOL_FALLBACK",
                    "platform=" + platform + "\nid=" + id +
                    "\nvia=" + result.via + "\nurl=" + ms.url);
                return ms;
            }
            fallback_error = std::move(result.error);
        } catch (const std::exception& e) {
            fallback_error = e.what();
        }
    }

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
