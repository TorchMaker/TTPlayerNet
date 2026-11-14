#include "ttplayer/video/video_history.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include <windows.h>

#include "ttplayer/core/text.h"
#include "ttplayer/video/video_cms.h"
#include "ttplayer/video/video_source_manager.h"

namespace ttplayer::video {

namespace {

using json = nlohmann::json;

std::mutex g_mutex;

std::filesystem::path RuntimeDir() {
    static std::filesystem::path cached;
    static std::once_flag once;
    std::call_once(once, [] {
        std::wstring exe_w(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, exe_w.data(),
                                           static_cast<DWORD>(exe_w.size()));
        if (n == 0 || n >= exe_w.size()) return;
        exe_w.resize(n);
        std::error_code ec;
        cached = std::filesystem::path(exe_w).parent_path() / L"video";
        std::filesystem::create_directories(cached, ec);
    });
    return cached;
}

std::filesystem::path HistoryPath() { return RuntimeDir() / L"history.json"; }

std::string NowStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%m-%d %H:%M:%S", &local);
    return buf;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    return core::WideToUtf8(w);
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    return core::Utf8ToWide(s);
}

json EpisodeToJson(const VideoPlayerEpisode& ep) {
    json e;
    e["title"] = ToUtf8(ep.title);
    e["url"] = ToUtf8(ep.url);
    e["line"] = ToUtf8(ep.line);
    json arr = json::array();
    for (const auto& s : ep.sources)
        arr.push_back(json{{"url", ToUtf8(s.url)}, {"name", ToUtf8(s.name)}});
    if (!arr.empty()) e["sources"] = std::move(arr);
    return e;
}

VideoPlayerEpisode EpisodeFromJson(const json& e) {
    VideoPlayerEpisode ep;
    if (!e.is_object()) return ep;
    if (e.contains("title") && e["title"].is_string())
        ep.title = ToWide(e["title"].get<std::string>());
    if (e.contains("url") && e["url"].is_string())
        ep.url = ToWide(e["url"].get<std::string>());
    if (e.contains("line") && e["line"].is_string())
        ep.line = ToWide(e["line"].get<std::string>());
    if (e.contains("sources") && e["sources"].is_array()) {
        for (const auto& s : e["sources"]) {
            if (!s.is_object() || !s.contains("url") || !s["url"].is_string())
                continue;
            VideoPlayerSource src;
            src.url = ToWide(s["url"].get<std::string>());
            if (src.url.empty()) continue;
            if (s.contains("name") && s["name"].is_string())
                src.name = ToWide(s["name"].get<std::string>());
            ep.sources.push_back(std::move(src));
        }
    }
    return ep;
}

json EntryToJson(const VideoHistoryEntry& entry) {
    json j;
    j["id"] = ToUtf8(entry.id);
    j["title"] = ToUtf8(entry.title);
    j["site"] = ToUtf8(entry.site);
    j["detail"] = ToUtf8(entry.detail_id);
    j["cover"] = ToUtf8(entry.cover);
    j["line"] = ToUtf8(entry.line);
    j["current"] = entry.current_index;
    j["position"] = entry.position_ms;
    j["duration"] = entry.duration_ms;
    j["updated"] = entry.updated_at;
    json eps = json::array();
    for (const auto& ep : entry.episodes) eps.push_back(EpisodeToJson(ep));
    if (!eps.empty()) j["episodes"] = std::move(eps);
    return j;
}

VideoHistoryEntry EntryFromJson(const json& j) {
    VideoHistoryEntry entry;
    if (!j.is_object()) return entry;
    auto str = [&j](const char* key) -> std::wstring {
        if (!j.contains(key) || !j[key].is_string()) return {};
        return ToWide(j[key].get<std::string>());
    };
    entry.id = str("id");
    entry.title = str("title");
    entry.site = str("site");
    entry.detail_id = str("detail");
    entry.cover = str("cover");
    entry.line = str("line");
    if (j.contains("current") && j["current"].is_number_integer())
        entry.current_index = j["current"].get<int>();
    if (j.contains("position") && j["position"].is_number_integer())
        entry.position_ms = j["position"].get<int>();
    if (j.contains("duration") && j["duration"].is_number_integer())
        entry.duration_ms = j["duration"].get<int>();
    if (j.contains("updated") && j["updated"].is_number_integer())
        entry.updated_at = j["updated"].get<long long>();
    if (j.contains("episodes") && j["episodes"].is_array()) {
        for (const auto& e : j["episodes"]) {
            auto ep = EpisodeFromJson(e);
            if (!ep.url.empty()) entry.episodes.push_back(std::move(ep));
        }
    }
    return entry;
}

// 标题归一：用于「同一部片只保留一条历史」。跨源回退 / 不同站点会把同一部剧
// 记成两条（例如「现在就出发 第四季」和「现在就出发第4季」），列表里就是两个
// 几乎一样的条目。这里忽略空白与分隔符，并把中文数字换成阿拉伯数字，让
// 「第四季 / 第4季」归一到同一个键；集数不同（第1季 vs 第2季）仍然区分。
std::wstring TitleKey(const std::wstring& title) {
    std::wstring out;
    out.reserve(title.size());
    static const wchar_t kCnDigits[] = L"零一二三四五六七八九";
    for (wchar_t ch : title) {
        if (iswspace(ch)) continue;
        switch (ch) {
            case L'-': case L'_': case L'|': case L'｜': case L'·':
            case L'[': case L']': case L'【': case L'】':
            case L'(': case L')': case L'（': case L'）':
            case L'《': case L'》': case L'"': case L'\'':
                continue;
            default:
                break;
        }
        const wchar_t* digit = wcschr(kCnDigits, ch);
        if (digit) {
            out.push_back(static_cast<wchar_t>(L'0' + (digit - kCnDigits)));
            continue;
        }
        out.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return out;
}

// 同标题只留最近观看的一条（updated_at 大的优先）。调用方持锁。
void DedupeByTitle(std::vector<VideoHistoryEntry>* entries) {
    if (!entries) return;
    std::vector<VideoHistoryEntry> unique;
    unique.reserve(entries->size());
    for (auto& entry : *entries) {
        const std::wstring key = TitleKey(entry.title);
        if (key.empty()) {
            unique.push_back(std::move(entry));
            continue;
        }
        auto found = std::find_if(unique.begin(), unique.end(),
                                  [&key](const VideoHistoryEntry& e) {
                                      return TitleKey(e.title) == key;
                                  });
        if (found == unique.end()) {
            unique.push_back(std::move(entry));
        } else if (entry.updated_at > found->updated_at) {
            *found = std::move(entry);
        }
    }
    *entries = std::move(unique);
}

// 调用方必须持锁。
std::vector<VideoHistoryEntry> LoadLocked() {
    std::vector<VideoHistoryEntry> out;
    try {
        std::ifstream in(HistoryPath(), std::ios::binary);
        if (!in) return out;
        json root;
        in >> root;
        if (!root.is_object() || !root.contains("items") ||
            !root["items"].is_array())
            return out;
        for (const auto& item : root["items"]) {
            auto entry = EntryFromJson(item);
            if (!entry.id.empty()) out.push_back(std::move(entry));
        }
    } catch (...) {
        // 文件损坏：当作空历史，下一次写入会重建。
    }
    // 已经存在的跨源重复条目也在这里折叠成一条（下次写盘时一并清理）。
    DedupeByTitle(&out);
    return out;
}

// 调用方必须持锁。
bool SaveLocked(const std::vector<VideoHistoryEntry>& entries) {
    try {
        json root;
        root["version"] = 1;
        json items = json::array();
        for (const auto& e : entries) items.push_back(EntryToJson(e));
        root["items"] = std::move(items);
        const auto path = HistoryPath();
        if (path.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        auto tmp = path;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << root.dump();
            out.flush();
            if (!out) return false;
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(tmp, path, ec);
        }
        return !ec;
    } catch (...) {
        return false;
    }
}

}  // namespace

void VideoHistoryLog(const std::string& message) {
    try {
        std::lock_guard<std::mutex> lk(g_mutex);
        const auto dir = RuntimeDir();
        if (dir.empty()) return;
        std::ofstream out(dir / L"history.log",
                          std::ios::binary | std::ios::app);
        if (!out) return;
        out << "[" << NowStamp() << "] " << message << "\r\n";
    } catch (...) {
    }
}

std::wstring MakeVideoHistoryId(const std::wstring& site,
                                const std::wstring& key) {
    if (site.empty() && key.empty()) return L"video";
    return site + L"|" + key;
}

std::vector<VideoHistoryEntry> LoadVideoHistory() {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto entries = LoadLocked();
    std::stable_sort(entries.begin(), entries.end(),
                     [](const VideoHistoryEntry& a, const VideoHistoryEntry& b) {
                         return a.updated_at > b.updated_at;
                     });
    return entries;
}

bool UpsertVideoHistory(VideoHistoryEntry entry) {
    if (entry.id.empty()) return false;
    entry.updated_at = static_cast<long long>(std::time(nullptr));
    try {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto entries = LoadLocked();
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&entry](const VideoHistoryEntry& e) {
                                   return e.id == entry.id;
                               });
        if (it != entries.end())
            *it = std::move(entry);
        else
            entries.insert(entries.begin(), std::move(entry));
        // 只保留最近 100 条，避免 history.json 无限膨胀。
        if (entries.size() > 100) entries.resize(100);
        DedupeByTitle(&entries);
        const bool ok = SaveLocked(entries);
        VideoHistoryLog(std::string("UPSERT ok=") + (ok ? "1" : "0") +
                        " id=" + ToUtf8(entry.id) +
                        " episodes=" + std::to_string(entry.episodes.size()) +
                        " cur=" + std::to_string(entry.current_index));
        return ok;
    } catch (...) {
        return false;
    }
}

bool UpdateVideoHistoryProgress(const std::wstring& id, int current_index,
                                int position_ms, int duration_ms) {
    if (id.empty()) return false;
    try {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto entries = LoadLocked();
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&id](const VideoHistoryEntry& e) {
                                   return e.id == id;
                               });
        if (it == entries.end()) {
            VideoHistoryLog("PROGRESS miss id=" + ToUtf8(id));
            return false;
        }
        it->current_index = current_index;
        it->position_ms = position_ms;
        it->duration_ms = duration_ms;
        it->updated_at = static_cast<long long>(std::time(nullptr));
        const bool ok = SaveLocked(entries);
        VideoHistoryLog(std::string("PROGRESS ok=") + (ok ? "1" : "0") +
                        " id=" + ToUtf8(id) +
                        " cur=" + std::to_string(current_index) +
                        " pos=" + std::to_string(position_ms) +
                        " dur=" + std::to_string(duration_ms));
        return ok;
    } catch (...) {
        return false;
    }
}

bool RemoveVideoHistory(const std::wstring& id) {
    if (id.empty()) return false;
    try {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto entries = LoadLocked();
        // 同标题的隐藏重复条目一并删掉：否则删掉「最近的这一条」之后，
        // 归一后同名的旧记录会重新冒出来（看起来还是删不掉）。
        std::wstring title_key;
        for (const auto& entry : entries) {
            if (entry.id == id) {
                title_key = TitleKey(entry.title);
                break;
            }
        }
        const auto before = entries.size();
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&id, &title_key](const VideoHistoryEntry& e) {
                                         if (e.id == id) return true;
                                         return !title_key.empty() &&
                                                TitleKey(e.title) == title_key;
                                     }),
                      entries.end());
        const bool removed = entries.size() != before;
        const bool ok = removed ? SaveLocked(entries) : true;
        VideoHistoryLog(std::string("REMOVE id=") + ToUtf8(id) +
                        " removed=" + (removed ? "1" : "0") +
                        " ok=" + (ok ? "1" : "0"));
        return removed && ok;
    } catch (...) {
        return false;
    }
}

bool FindVideoHistory(const std::wstring& id, VideoHistoryEntry* out) {
    if (id.empty() || !out) return false;
    try {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto entries = LoadLocked();
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&id](const VideoHistoryEntry& e) {
                                   return e.id == id;
                               });
        if (it == entries.end()) return false;
        *out = *it;
        return true;
    } catch (...) {
        return false;
    }
}

// ───────────────────────── 从历史打开（含重新解析） ─────────────────────────

namespace {

// 与 bridge_cmds_video.cpp 的 BuildPlayerEpisodes 同口径：以 pick_line 为基准
// 取集名/地址，并按相同下标汇总其它线路的地址（保证换集后仍可切换源）。
std::vector<VideoPlayerEpisode> BuildEpisodesForLine(
    const VideoDetail& detail, const VideoSite& site, int pick_line) {
    std::vector<VideoPlayerEpisode> out;
    if (pick_line < 0 ||
        static_cast<std::size_t>(pick_line) >= detail.lines.size())
        return out;
    auto abs_url = [&site](std::string u) {
        if (!site.play_url.empty() && u.find("://") == std::string::npos)
            u = site.play_url + u;
        return u;
    };
    const auto& base = detail.lines[static_cast<std::size_t>(pick_line)];
    const std::size_t count = std::min<std::size_t>(base.episodes.size(), 400);
    for (std::size_t ei = 0; ei < count; ++ei) {
        VideoPlayerEpisode ep;
        ep.title = ToWide(base.episodes[ei].name);
        ep.line = ToWide(base.name);
        ep.url = ToWide(abs_url(base.episodes[ei].url));
        if (ep.url.empty()) continue;
        for (const auto& ln : detail.lines) {
            if (ei >= ln.episodes.size()) continue;
            const std::string u = abs_url(ln.episodes[ei].url);
            if (u.empty()) continue;
            VideoPlayerSource s;
            s.url = ToWide(u);
            s.name = ToWide(ln.name);
            ep.sources.push_back(std::move(s));
        }
        out.push_back(std::move(ep));
    }
    return out;
}

}  // namespace

bool OpenVideoFromHistory(const std::wstring& id, HWND owner,
                          const skin::BrowserSkin* browser_skin) {
    VideoHistoryEntry entry;
    if (!FindVideoHistory(id, &entry)) {
        VideoHistoryLog("OPEN miss id=" + ToUtf8(id));
        return false;
    }
    VideoHistoryLog("OPEN id=" + ToUtf8(id) + " title=" + ToUtf8(entry.title) +
                    " site=" + ToUtf8(entry.site) +
                    " detail=" + ToUtf8(entry.detail_id) +
                    " eps=" + std::to_string(entry.episodes.size()) +
                    " cur=" + std::to_string(entry.current_index) +
                    " pos=" + std::to_string(entry.position_ms) +
                    " dur=" + std::to_string(entry.duration_ms));

    auto usable = [&entry] {
        if (entry.current_index < 0 ||
            entry.current_index >= static_cast<int>(entry.episodes.size()))
            return false;
        return !entry.episodes[static_cast<std::size_t>(entry.current_index)]
                    .url.empty();
    };

    bool re_resolved = false;
    if (!usable() && !entry.site.empty()) {
        // 保存的直链全失效 / 历史里没有选集：按站点详情重新解析一次。
        try {
            VideoSourceManager mgr(RuntimeDir());
            musicfree::NetworkConfig net;
            net.user_agent =
                L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                L"(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
            mgr.SetNetworkConfig(std::move(net));
            mgr.LoadCached();
            VideoSite site;
            bool found = false;
            for (const auto& s : mgr.AllSites()) {
                if ((!entry.site.empty() &&
                     (ToWide(s.key) == entry.site || ToWide(s.name) == entry.site))) {
                    site = s;
                    found = true;
                    break;
                }
            }
            if (!found) {
                VideoHistoryLog("RESOLVE site-miss site=" + ToUtf8(entry.site));
            } else {
                VideoItem item;
                item.id = ToUtf8(entry.detail_id);
                item.title = ToUtf8(entry.title);
                item.site_key = site.key;
                const VideoDetail detail = FetchDetail(site, item, mgr.network());
                int pick = -1;
                for (std::size_t li = 0; li < detail.lines.size(); ++li) {
                    if (ToWide(detail.lines[li].name) == entry.line) {
                        pick = static_cast<int>(li);
                        break;
                    }
                }
                if (pick < 0) {
                    for (std::size_t li = 0; li < detail.lines.size(); ++li) {
                        if (!detail.lines[li].episodes.empty() &&
                            !detail.lines[li].episodes.front().url.empty()) {
                            pick = static_cast<int>(li);
                            break;
                        }
                    }
                }
                auto episodes = BuildEpisodesForLine(detail, site, pick);
                VideoHistoryLog("RESOLVE detail lines=" +
                                std::to_string(detail.lines.size()) +
                                " pick=" + std::to_string(pick) + " eps=" +
                                std::to_string(episodes.size()));
                if (!episodes.empty()) {
                    entry.episodes = std::move(episodes);
                    entry.current_index = 0;
                    entry.position_ms = 0;
                    entry.duration_ms = 0;
                    if (!detail.pic.empty()) entry.cover = ToWide(detail.pic);
                    if (pick >= 0)
                        entry.line = ToWide(detail.lines[static_cast<std::size_t>(pick)].name);
                    re_resolved = true;
                }
            }
        } catch (const std::exception& e) {
            VideoHistoryLog(std::string("RESOLVE fail ") + e.what());
        } catch (...) {
            VideoHistoryLog("RESOLVE fail unknown");
        }
    }

    if (!usable()) {
        VideoHistoryLog("OPEN no-playable id=" + ToUtf8(id));
        return false;
    }
    if (re_resolved) UpsertVideoHistory(entry);

    auto& ep = entry.episodes[static_cast<std::size_t>(entry.current_index)];
    std::vector<VideoPlayerSource> sources = ep.sources;
    const std::wstring line = ep.line.empty() ? entry.line : ep.line;
    if (sources.empty()) sources.push_back({ep.url, line});
    VideoHistoryLog("OPEN play title=" + ToUtf8(entry.title) + " line=" +
                    ToUtf8(line) + " url=" + ToUtf8(ep.url) +
                    " resume=" + std::to_string(entry.position_ms) +
                    " skin=" + (browser_skin ? "1" : "0"));
    ShowVideoPlayer(owner, ep.url, entry.title, line, browser_skin, {}, sources,
                    {}, {}, {}, entry.episodes, entry.position_ms, entry.id);
    return true;
}

}  // namespace ttplayer::video
