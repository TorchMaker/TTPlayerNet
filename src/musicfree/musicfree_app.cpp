// MusicFree 兼容层 —— 应用级入口实现
//
// 推导缓存目录（<exe>/musicfree）、部署内置 JS 依赖库、启动时重建音源，
// 并提供"全部音源在线搜索 / 解析播放地址"的统一入口，供 UI 与播放引擎使用。

#include "ttplayer/musicfree/musicfree_app.h"

#include <cstdio>
#include <cwctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/musicfree/musicfree_http.h"
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

std::optional<MediaSource> ResolvePlayUrlEx(const std::string& platform,
                                            const std::string& id,
                                            std::string* error) {
    auto* mgr = AppMusicFreeManager();
    if (!mgr) {
        if (error) *error = "MusicFree 管理器未初始化";
        return std::nullopt;
    }
    MusicFreePlugin* plugin = mgr->FindPlugin(platform);
    if (!plugin || !plugin->IsMounted()) {
        if (error) *error = "找不到音源「" + platform + "」";
        return std::nullopt;
    }
    MusicItem item;
    item.id = id;
    item.platform = platform;
    item.raw = nlohmann::json::object();
    item.raw["id"] = id;
    item.raw["platform"] = platform;
    auto ms = plugin->GetMediaSource(item, QualityKey::kStandard, error);
    if (!ms) return std::nullopt;
    return *ms;
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

} // namespace ttplayer::musicfree
