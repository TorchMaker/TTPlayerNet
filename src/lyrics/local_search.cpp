#include "ttplayer/lyrics/local_search.h"
#include "ttplayer/lyrics/association.h"
#include "ttplayer/lyrics/online_search.h"
#include "ttplayer/audio/archive_member.h"
#include <algorithm>
#include <functional>
#include <set>
#include <thread>
#include <windows.h>

namespace ttplayer::lyrics {
namespace {
struct PathLess {
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
bool IsFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}
SearchResult SplitName(const std::wstring& name) {
    const auto dash = name.find(L'-'); // 004444E3: first hyphen, not only " - ".
    return dash == name.npos ? SearchResult{L"", name} : SearchResult{name.substr(0, dash), name.substr(dash + 1)};
}
}

Lyrics LoadLocalLyric(const std::filesystem::path& path, HMODULE module,
                     const std::function<bool()>& canceled) {
    audio::ArchiveMemberPath member;
    if (!audio::ParseArchiveMemberPath(path.native(), member)) return LoadLrc(path);
    const auto bytes = audio::ReadArchiveMember(member, module, canceled);
    return DecodeLrcBytes(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::vector<LocalSearchRoot> LocalSearchRoots(const std::filesystem::path& media,
    const std::filesystem::path& runtime, const std::filesystem::path& download,
    const std::vector<std::wstring>& configured) {
    std::vector<LocalSearchRoot> result;
    for (auto value : configured) {
        if (value.empty()) continue;
        const bool recursive = value.front() == L'*';
        if (recursive) value.erase(value.begin());
        std::filesystem::path path;
        if (value == L"<Sound Folder>") {
            audio::ArchiveMemberPath member;
            if (audio::ParseArchiveMemberPath(media.native(), member)) {
                auto folder = std::filesystem::path(member.member).parent_path();
                result.push_back({audio::MakeArchiveMemberPath(member.archive,
                    folder.empty() ? L"." : folder.native()), recursive});
                continue;
            }
            if (media.native().find(L"://") == std::wstring::npos) path = media.parent_path();
        } else if (value == L"<Lyrics Download Folder>") path = download;
        else path = value;
        if (!path.empty()) result.push_back({(path.is_absolute() ? path : runtime / path).lexically_normal(), recursive});
    }
    return result;
}

LocalSearchResult SearchLocalLyrics(const LocalSearchRequest& request, const std::atomic_bool& canceled) {
    LocalSearchResult result;
    auto load = [&](const std::filesystem::path& path) {
        if (canceled || path.empty()) return false;
        try {
            auto lyric = LoadLocalLyric(path, request.archive_module, [&]{ return canceled.load(); });
            if (lyric.lines.empty()) return false;
            result.loaded_path = path; result.lyric = std::move(lyric); return true;
        } catch (...) { return false; }
    };
    if (!request.all_matches && load(request.associated)) return result;
    if (!request.all_matches && request.associated == kNoLyric) return result;
    std::set<std::filesystem::path, PathLess> seen;
    int best{};
    std::filesystem::path best_path;
    audio::ArchiveMemberPath media_member;
    const auto file_title = audio::ParseArchiveMemberPath(request.media.native(), media_member)
        ? std::filesystem::path(media_member.member).stem().wstring() : request.media.stem().wstring();
    const auto exact_name = request.artist.empty() ? file_title : request.artist + L" - " + request.title;
    const auto accept = [&](const std::filesystem::path& path, int score) {
        if (score < (request.partial ? 1 : 4)) return;
        if (request.all_matches) {
            if (seen.insert(path.lexically_normal()).second) result.matches.push_back(path);
        } else if (score > best) { best = score; best_path = path; }
    };
    std::function<void(const std::filesystem::path&, unsigned, bool)> scan;
    scan = [&](const std::filesystem::path& root, unsigned depth, bool recursive) {
        if (canceled || (!request.all_matches && best == 9)) return;
        if (!request.all_matches) {
            for (const auto& [name, score] : {std::pair{exact_name, 9}, std::pair{file_title, 4}}) {
                // Filename metadata must never escape the configured root.
                if (name.empty() || std::filesystem::path(name).filename() != name || name.find_first_of(L"<>:\"/\\|?*") != name.npos) continue;
                for (const auto* extension : {L".lrc", L".txt"}) {
                    const auto file = root / (name + extension);
                    if (IsFile(file)) { accept(file, score); break; }
                }
                if (best == 9) return;
            }
        }
        std::vector<std::filesystem::path> subdirectories;
        std::error_code error;
        for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && it != end && !canceled; it.increment(error)) {
            const auto& entry = *it;
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (recursive && depth < 4 && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) subdirectories.push_back(entry.path());
                continue;
            }
            const auto extension = entry.path().extension().wstring();
            if (_wcsicmp(extension.c_str(), L".lrc") && _wcsicmp(extension.c_str(), L".txt")) continue;
            const auto stem = entry.path().stem().wstring();
            accept(entry.path(), ScoreLyricMatch(SplitName(stem), request.artist, request.title, file_title, request.partial));
            if (!request.all_matches && best == 9) return;
        }
        for (const auto& path : subdirectories) scan(path, depth + 1, recursive);
    };
    for (const auto& root : request.roots) {
        if (canceled || (!request.all_matches && best == 9)) break;
        audio::ArchiveMemberPath archive_root;
        if (!audio::ParseArchiveMemberPath(root.path.native(), archive_root)) {
            scan(root.path, 1, root.recursive); continue;
        }
        try {
            auto folder = std::filesystem::path(archive_root.member).lexically_normal().native();
            if (folder == L".") folder.clear();
            if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
            const auto names = audio::ListArchiveMembers(archive_root.archive, request.archive_module,
                [&]{ return canceled.load(); });
            std::function<void(std::wstring,unsigned)> scan_archive;
            scan_archive = [&](std::wstring prefix, unsigned depth) {
                if (canceled || (!request.all_matches && best == 9)) return;
                if (!request.all_matches) for (const auto& [name, score] :
                    {std::pair{exact_name,9}, std::pair{file_title,4}}) {
                    if (name.empty() || name.find_first_of(L"<>:\"/\\|?*") != name.npos) continue;
                    for (const auto* extension : {L".lrc", L".txt"}) {
                        const auto wanted = prefix + name + extension;
                        const auto found = std::find_if(names.begin(), names.end(), [&](const auto& n){return !_wcsicmp(n.c_str(),wanted.c_str());});
                        if (found != names.end()) { accept(audio::MakeArchiveMemberPath(archive_root.archive,*found),score); break; }
                    }
                    if (best == 9) return;
                }
                std::set<std::wstring> visited;
                std::vector<std::wstring> subdirectories;
                for (const auto& name : names) {
                    if (canceled) return;
                    if (name.size() <= prefix.size() || _wcsnicmp(name.c_str(),prefix.c_str(),prefix.size())) continue;
                    const auto slash = name.find(L'\\',prefix.size());
                    if (slash != name.npos) {
                        auto sub = name.substr(0,slash+1);
                        if (root.recursive && depth < 4 && visited.insert(sub).second) subdirectories.push_back(std::move(sub));
                        continue;
                    }
                    const std::filesystem::path file(name);
                    const auto extension = file.extension().native();
                    if (_wcsicmp(extension.c_str(),L".lrc") && _wcsicmp(extension.c_str(),L".txt")) continue;
                    accept(audio::MakeArchiveMemberPath(archive_root.archive,name),
                        ScoreLyricMatch(SplitName(file.stem().native()),request.artist,request.title,file_title,request.partial));
                    if (!request.all_matches && best == 9) return;
                }
                for (const auto& sub : subdirectories) scan_archive(sub,depth+1);
            };
            scan_archive(folder,1);
        } catch (...) { /* A missing/canceled archive must not prevent other configured roots. */ }
    }
    if (!request.all_matches && best >= 4) load(best_path);
    return result;
}

std::shared_ptr<LocalSearchJob> SearchLocalLyricsAsync(LocalSearchRequest request) {
    auto job = std::make_shared<LocalSearchJob>();
    if (!request.archive_module) request.archive_module=GetModuleHandleW(L"ttpcomm.dll");
    HMODULE retained{};
    if (request.archive_module && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(request.archive_module), &retained)) { job->result=LocalSearchResult{}; return job; }
    auto lease = std::shared_ptr<void>(retained, [](void* module){ if(module) FreeLibrary(static_cast<HMODULE>(module)); });
    std::thread([job, lease = std::move(lease), request = std::move(request)] {
        LocalSearchResult result;
        try { result = SearchLocalLyrics(request, job->canceled); } catch (...) {}
        std::lock_guard lock(job->mutex);
        if (!job->canceled) job->result = std::move(result);
    }).detach();
    return job;
}
} // namespace ttplayer::lyrics
