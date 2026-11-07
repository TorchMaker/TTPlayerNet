#include "ttplayer/i18n/i18n.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "ttplayer/ui/player_window.h"

#include "player_window_internal.h"
#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/ui/playlist_network_commands.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ttplayer::ui {
using namespace detail;

namespace {

constexpr UINT kReportDialog = 396;
constexpr int kReportUnavailable = 1170;
constexpr int kReportWrongSong = 1171;
constexpr int kReportStatus = 1172;
constexpr int kReportTrack = 2276;
struct ReportDialogState {
    std::wstring track;
};

// ---------------------------------------------------------------- 在线曲目下载
//
// 「下载歌曲」原来是 FreeDB / 音乐窗下载管理器在干活，重建版里没有这两个后端。
// 但对**来自 MusicFree 音源的在线条目**我们确实能下载：条目入列时带了
// Platform + SongId，path 就是解析出来的播放地址。于是这条路改成真的落盘：
// 另存为 → 后台线程拉字节 → 完成消息回到主窗口显示结果。
//
// 同一时刻只允许一个下载（全局负载 + 互斥量）。这样「完成消息该配给哪一次下载」
// 不需要额外携带标识，也不会出现两次下载互相覆盖结果。
struct OnlineDownloadJob {
    bool ok{};
    std::wstring path;
    std::wstring error;
    std::int64_t bytes{};
};

std::mutex g_download_mutex;
std::shared_ptr<OnlineDownloadJob> g_download_job;

// 在线条目「平台:歌曲id → 本地文件路径」的会话内缓存，等价 MusicFree 的
// mediaExtra.localPath / internalSerializeKey.localPath。下载完成或扫到本地副本时
// 写进去，PlayCurrent 解析时优先查它（O(1) 且不依赖目录扫描）。进程级持久性由
// 默认下载目录里那个实际文件保证：重启后文件还在，扫描仍能命中。
std::mutex g_online_local_mutex;
std::map<std::string, std::filesystem::path> g_online_local_cache;

// 后台「边播边落盘」缓存的全局单任务互斥：同时刻只跑一个后台下载，避免和手动「下载
// 歌曲」互相挤占，也避免一批在线曲目同时播放时把磁盘 / 带宽打满。
std::mutex g_background_cache_mutex;
bool g_background_cache_active = false;

std::string OnlineLocalKey(std::string_view platform, std::string_view song_id) {
    std::string key;
    key.reserve(platform.size() + song_id.size() + 1);
    key += platform;
    key += ':';
    key += song_id;
    return key;
}

std::wstring FormatBytes(std::int64_t bytes) {
    if (bytes <= 0) return L"未知";
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0)
        return std::to_wstring(static_cast<long long>(kb + 0.5)) + L" KB";
    // 手算一位小数，不用 printf 家族（宽字符版本在各 CRT 上行为不一致）。
    const long long tenths = static_cast<long long>(kb / 1024.0 * 10.0 + 0.5);
    return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) +
           L" MB";
}

INT_PTR CALLBACK ReportDialogProc(HWND dialog, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG: {
        const auto* state = reinterpret_cast<const ReportDialogState*>(lparam);
        if (!state) return FALSE;
        SetDlgItemTextW(dialog, kReportTrack, state->track.c_str());
        CheckRadioButton(dialog, kReportUnavailable, kReportWrongSong, kReportUnavailable);
        // The resource's 0x84D0 acknowledgement is only valid AFTER an
        // actual submission. The optional reporting backend is absent.
        SetDlgItemTextW(dialog, kReportStatus, i18n::Literal(L"报告服务不可用，尚未提交。"));
        for (int control : {IDOK, kReportUnavailable, kReportWrongSong})
            EnableWindow(GetDlgItem(dialog, control), FALSE);
        SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL) EndDialog(dialog, IDCANCEL);
        return TRUE; // Directly dispatched IDOK must not simulate success.
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lparam) == GetDlgItem(dialog, kReportStatus)) {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, RGB(255, 0, 0));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

} // namespace

bool PlayerWindow::OnlineTrackIdentity(const playlist::Track& track,
                                       std::string* platform,
                                       std::string* song_id) {
    std::string found_platform;
    std::string found_id;
    for (const auto& entry : track.metadata) {
        if (entry.first == "Platform") found_platform = entry.second;
        else if (entry.first == "SongId") found_id = entry.second;
    }
    if (found_platform.empty() || found_id.empty()) return false;
    if (platform) *platform = found_platform;
    if (song_id) *song_id = found_id;
    return true;
}

bool PlayerWindow::StartOnlineTrackDownload(const playlist::Track& track) {
    std::string platform;
    std::string song_id;
    // 本地文件没有音源身份，「下载」对它们没有意义 —— 返回 false 让调用方继续弹
    // 原来的「无可用下载后端」提示。
    if (!OnlineTrackIdentity(track, &platform, &song_id)) return false;

    // 默认落点：我的音乐目录；文件名用音源返回的标题/歌手，**不要**用 path
    // （在线条目的 path 是 CDN 地址，末段通常是哈希）。
    const std::wstring url_hint = track.path.wstring();
    const std::wstring name = musicfree::DefaultDownloadName(
        track.title, track.artist, url_hint);
    const auto directory = musicfree::DefaultDownloadDirectory();
    const std::wstring initial =
        directory.empty() ? name : (directory / name).wstring();

    std::wstring buffer(initial.size() + 64, L'\0');
    std::copy(initial.begin(), initial.end(), buffer.begin());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = playlist_window_ ? playlist_window_ : window_;
    ofn.lpstrFilter =
        L"音频文件\0*.mp3;*.flac;*.m4a;*.aac;*.wav;*.ogg;*.wma\0所有文件\0*.*\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"下载歌曲到…";
    ofn.lpstrDefExt = L"mp3";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return true;  // 取消：命令已消费，什么都不做

    const std::wstring target(buffer.c_str());
    if (target.empty()) return true;

    musicfree::TraceDiagnostic("PLAYLIST_DOWNLOAD",
        "platform=" + platform + "\nid=" + song_id +
        "\nname=" + core::WideToUtf8(name) +
        "\ntarget=" + core::WideToUtf8(target));

    auto job = std::make_shared<OnlineDownloadJob>();
    job->path = target;
    {
        const std::scoped_lock lock(g_download_mutex);
        if (g_download_job) {
            MessageBoxW(ofn.hwndOwner, L"上一个下载还没结束，请稍候再试。",
                        L"下载歌曲", MB_OK | MB_ICONINFORMATION);
            return true;
        }
        g_download_job = job;
    }

    const HWND notify = window_;
    // 分离线程 + 共享负载：不用 join，也就没有「joinable 的 std::thread 析构」
    // 这种必须 detach 否则 terminate 的坑；结果通过消息回到 UI 线程。
    // 条目里保存的完整音源对象（含插件私有字段）按值捕获进工作线程，
    // 重新解析直链时原样回传给插件 / 洛雪音源。
    const auto source_raw = musicfree::RestoreSourceRaw(track.metadata);
    std::thread([job, notify, url_hint, platform, song_id, source_raw] {
        std::wstring resolved = url_hint;
        std::wstring error;
        // 已经存下来的 path 就是解析结果；不像 http 地址时（例如从旧配置恢复的
        // 条目）再按「音源平台 + 歌曲 id」重新解析一次。
        if (resolved.rfind(L"http", 0) != 0) {
            std::string reason;
            try {
                resolved = core::Utf8ToWide(
                    musicfree::ResolvePlayUrl(platform, song_id, &reason, &source_raw));
            } catch (const std::exception&) {
                resolved.clear();
            }
            if (resolved.empty()) {
                error = reason.empty() ? L"无法解析播放地址"
                                       : core::Utf8ToWide(reason);
            }
        }
        bool ok = false;
        std::int64_t bytes = 0;
        if (error.empty()) {
            std::string reason;
            ok = musicfree::DownloadTrackFile(
                resolved, std::filesystem::path(job->path), &reason);
            if (ok) {
                std::error_code size_error;
                const auto size = std::filesystem::file_size(job->path, size_error);
                if (!size_error) bytes = static_cast<std::int64_t>(size);
            } else {
                error = core::Utf8ToWide(reason);
            }
        }
        {
            const std::scoped_lock lock(g_download_mutex);
            job->ok = ok;
            job->error = std::move(error);
            job->bytes = bytes;
        }
        PostMessageW(notify, detail::kMsgOnlineDownloadDone, 0, 0);
    }).detach();
    return true;
}

bool PlayerWindow::HandleOnlineDownloadComplete(LPARAM) {
    std::shared_ptr<OnlineDownloadJob> job;
    {
        const std::scoped_lock lock(g_download_mutex);
        job = std::move(g_download_job);
    }
    if (!job) return true;

    const HWND owner = playlist_window_ ? playlist_window_ : window_;
    if (!job->ok) {
        musicfree::TraceDiagnostic("PLAYLIST_DOWNLOAD",
            "ok=0 error=" + core::WideToUtf8(job->error));
        const std::wstring message = job->error.empty()
            ? std::wstring(L"下载失败。") : L"下载失败：" + job->error;
        MessageBoxW(owner, message.c_str(), L"下载歌曲", MB_OK | MB_ICONWARNING);
        return true;
    }

    musicfree::TraceDiagnostic("PLAYLIST_DOWNLOAD",
        "ok=1 bytes=" + std::to_string(job->bytes));
    const std::wstring message = L"已下载到：\n" + job->path +
        L"\n\n大小：" + FormatBytes(job->bytes) + L"\n\n是否打开所在文件夹？";
    if (MessageBoxW(owner, message.c_str(), L"下载歌曲",
                    MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        const std::wstring arguments = L"/select,\"" + job->path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(),
                      nullptr, SW_SHOWNORMAL);
    }
    return true;
}

bool PlayerWindow::HandleLegacyPlaylistNetworkCommand(UINT command) {
    LegacyPlaylistNetworkAction action{};
    switch (command) {
    case kPlaylistFreeDb:
        action = LegacyPlaylistNetworkAction::freedb;
        break;
    case kPlaylistDownload:
        action = LegacyPlaylistNetworkAction::download;
        break;
    case kPlaylistReportOnline:
        action = LegacyPlaylistNetworkAction::report;
        break;
    default:
        return false;
    }

    // All three original handlers resolve the focused Files row first.  A
    // stale command with no valid row is a consumed no-op.
    std::optional<size_t> row = playlist_selection_;
    if (!row && !playlist_selected_rows_.empty())
        row = *playlist_selected_rows_.begin();
    const auto* track = row ? VisiblePlaylistTrack(*row) : nullptr;
    if (!track) return true;

    if (!LegacyNetworkActionAcceptsTrack(action, *track)) return true;

    if (action == LegacyPlaylistNetworkAction::report) {
        ReportDialogState state;
        state.track = BuildLegacyReportTrackText(
            *track, ResourceText(0x84d1));
        const INT_PTR result = ShowWtlModalDialog(
            ResourceModule(), MAKEINTRESOURCEW(kReportDialog),
            playlist_window_ ? playlist_window_ : window_, ReportDialogProc,
            reinterpret_cast<LPARAM>(&state));
        // Cancel is a consumed no-op in FUN_004878A4.  A missing/corrupt
        // dialog resource falls through to the explicit backend notice.
        if (result != IDYES && result != -1) return true;
    }

    // 在线条目（来自 MusicFree 音源）能真的下载到本地：另存为 → 后台拉字节 →
    // 完成消息。开始下载即视为命令已被消费。
    if (action == LegacyPlaylistNetworkAction::download &&
        StartOnlineTrackDownload(*track))
        return true;

    // 其余情况：TTPlayer delegated these operations to a FreeDB HTTP worker or its
    // optional Music Window/download-manager module.  Neither private
    // service is present in the rebuild.  Never wait on a retired endpoint
    // or claim success; expose the executable's resource-backed failure.
    const auto notice = LegacyBackendUnavailableNotice(action);
    auto caption = ResourceText(notice.caption_resource);
    auto message = ResourceText(notice.message_resource);
    if (caption.empty()) caption = ResourceText(0x80);
    if (message.empty()) message = ResourceText(0x828e);
    MessageBoxW(playlist_window_ ? playlist_window_ : window_,
        message.c_str(), caption.c_str(),
        MB_OK | (action == LegacyPlaylistNetworkAction::freedb
            ? MB_ICONEXCLAMATION : MB_ICONINFORMATION));
    return true;
}

// ----------------------------------------------------- 在线曲目「本地优先」播放
//
// 根因：播放列表里的在线条目，path 存的是 CDN 地址（http(s)），而 TTPlayer 的原生
// 解码器只能打开本地文件 —— PlayCurrent 直接 audio_->Play(http_url) 必然失败，于是
// 「列表里的文件再点播放就放不出来」。MusicFree 不会把地址当成「可播放路径」：它的
// trackPlayer.play() 在每次播放时才去 pluginManager.getMediaSource 解析真源，且
// getMediaSource 第一步就 getLocalPath —— 本地文件若存在就直接用本地文件，否则才
// 调插件现拉流地址。下面三个函数就是这套逻辑在 TTPlayer 的等价物。

bool PlayerWindow::ResolveOnlineTrackLocalCopy(const playlist::Track& track,
                                               std::filesystem::path& out_local) {
    std::string platform;
    std::string song_id;
    if (!OnlineTrackIdentity(track, &platform, &song_id)) return false;

    const std::wstring path = track.path.wstring();
    // 已经不是 http 地址（本地文件 / file:// 等）→ 直接用，和 MusicFree 里
    // getLocalPath 直接返回 musicItem.url 一致。
    if (path.rfind(L"http", 0) != 0) {
        out_local = track.path;
        return true;
    }

    const std::string key = OnlineLocalKey(platform, song_id);
    // 1) 会话内缓存（下载完成 / 之前扫到副本时记下的本地路径）。
    {
        const std::scoped_lock lock(g_online_local_mutex);
        const auto it = g_online_local_cache.find(key);
        if (it != g_online_local_cache.end()) {
            std::error_code ec;
            if (std::filesystem::exists(it->second, ec)) {
                out_local = it->second;
                return true;
            }
            g_online_local_cache.erase(it);  // 文件没了，下次重新解析
        }
    }

    // 2) 扫默认下载目录：文件名里带歌曲 id，或文件名（去扩展名）等于默认命名。
    const auto dir = musicfree::DefaultDownloadDirectory();
    if (!dir.empty()) {
        std::error_code ec;
        std::wstring default_name =
            musicfree::DefaultDownloadName(track.title, track.artist, path);
        // DefaultDownloadName 已 SanitizeFileName，比较时用去扩展名的基名。
        const std::wstring default_base =
            default_name.substr(0, default_name.rfind(L'.') == std::wstring::npos
                ? default_name.size() : default_name.rfind(L'.'));
        std::uint64_t best_mtime = 0;
        std::filesystem::path best;
        for (const auto& entry :
             std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto& f = entry.path();
            const std::wstring ext = f.extension().wstring();
            const bool audio = ext == L".mp3" || ext == L".flac" || ext == L".m4a" ||
                ext == L".aac" || ext == L".wav" || ext == L".ogg" || ext == L".wma" ||
                ext == L".ape" || ext == L".opus";
            if (!audio) continue;
            const std::wstring base = f.stem().wstring();
            const bool match = base.find(core::Utf8ToWide(song_id)) !=
                std::wstring::npos || (default_base.size() >= 2 &&
                base.find(default_base) != std::wstring::npos);
            if (!match) continue;
            // 同名多版本取最新修改的，贴近「最近一次下载的副本」。
            std::error_code mec;
            const auto mt = std::filesystem::last_write_time(f, mec);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                mt.time_since_epoch()).count();
            if (ms > static_cast<std::int64_t>(best_mtime) || best.empty()) {
                best = f;
                best_mtime = static_cast<std::uint64_t>(ms);
            }
        }
        if (!best.empty()) {
            const std::scoped_lock lock(g_online_local_mutex);
            g_online_local_cache[key] = best;
            out_local = best;
            return true;
        }
    }
    return false;
}

void PlayerWindow::StartOnlineTrackResolveAndPlay(size_t playlist_index,
                                                  size_t row,
                                                  const playlist::Track& track) {
    std::string platform;
    std::string song_id;
    if (!OnlineTrackIdentity(track, &platform, &song_id)) return;

    const std::wstring url_hint = track.path.wstring();
    const HWND notify = window_;
    // 复用下载的「单任务」互斥，避免和用户点「下载歌曲」撞车。
    auto job = std::make_shared<OnlineDownloadJob>();
    {
        const std::scoped_lock lock(g_download_mutex);
        if (g_download_job) {
            musicfree::TraceDiagnostic("PLAYLIST_PLAY_RESOLVE",
                "skip: another download in progress");
            return;
        }
        g_download_job = job;
    }

    const auto source_raw = musicfree::RestoreSourceRaw(track.metadata);
    std::thread([job, notify, url_hint, platform, song_id,
                 playlist_index, row, source_raw] {
        std::wstring resolved = url_hint;
        std::wstring error;
        // path 是 http 地址时，按「音源平台 + 歌曲 id」重新解析（等价于 MusicFree
        // getMediaSource 调插件现拉）。从旧配置恢复的条目 path 可能已是本地文件，
        // 但这里只会在线条目（http）走到这，所以一律重新解析更稳。
        std::string reason;
        resolved = core::Utf8ToWide(
            musicfree::ResolvePlayUrl(platform, song_id, &reason, &source_raw));
        if (resolved.empty()) {
            error = reason.empty() ? L"无法解析播放地址"
                                   : core::Utf8ToWide(reason);
        }
        bool ok = false;
        if (error.empty()) {
            const auto dir = musicfree::DefaultDownloadDirectory();
            const std::wstring ext = musicfree::GuessExtension(resolved);
            const std::wstring target =
                (dir.empty() ? std::filesystem::temp_directory_path().wstring()
                             : dir.wstring()) +
                L"\\" + core::Utf8ToWide(platform) + L"_" + core::Utf8ToWide(song_id) +
                ext;
            ok = musicfree::DownloadTrackFile(resolved,
                std::filesystem::path(target), &reason);
            if (ok) {
                const std::string key = OnlineLocalKey(platform, song_id);
                const std::scoped_lock lock(g_online_local_mutex);
                g_online_local_cache[key] = std::filesystem::path(target);
            } else {
                error = core::Utf8ToWide(reason);
            }
        }
        {
            const std::scoped_lock lock(g_download_mutex);
            job->ok = ok;
            job->error = std::move(error);
            job->path = resolved;
        }
        auto* payload = new detail::PlayResolvedPayload{playlist_index, row};
        PostMessageW(notify, detail::kMsgOnlinePlayResolved, 0,
                     reinterpret_cast<LPARAM>(payload));
    }).detach();
}

bool PlayerWindow::HandleOnlinePlayResolved(LPARAM payload) {
    auto* data = reinterpret_cast<detail::PlayResolvedPayload*>(payload);
    if (!data) return true;
    const size_t playlist_index = data->playlist_index;
    const size_t row = data->row;
    delete data;
    if (playlist_index >= playlists_.Size() ||
        row >= playlists_.At(playlist_index).Tracks().size())
        return true;
    // 重选同一行：这次 PlayCurrent 会经 ResolveOnlineTrackLocalCopy 命中刚写下的
    // 本地缓存，于是 audio_->Play 拿到的是本地文件，真正出声。
    SelectTrackFrom(playlist_index, row, true);
    return true;
}

bool PlayerWindow::HandleOnlineCacheReady(LPARAM payload) {
    auto* data = reinterpret_cast<detail::CacheReadyPayload*>(payload);
    if (!data) return true;
    const auto info = *data;
    delete data;
    if (info.playlist_index >= playlists_.Size() ||
        info.row >= playlists_.At(info.playlist_index).Tracks().size())
        return true;
    // 用户已经切到别的曲目 / 列表，就不要打扰当前播放。
    if (!playing_playlist_index_ ||
        *playing_playlist_index_ != info.playlist_index ||
        !current_ || *current_ != info.row) return true;
    const auto& track = playlists_.At(info.playlist_index).Tracks()[info.row];
    std::string platform, song_id;
    if (!OnlineTrackIdentity(track, &platform, &song_id) ||
        platform != info.platform || song_id != info.song_id) return true;
    std::filesystem::path local;
    if (!ResolveOnlineTrackLocalCopy(track, local)) return true;
    // 流式已经在正常推进就别从头重播；否则用本地副本真正出声。
    const bool advancing = audio_->State() == audio::PlaybackState::playing &&
        audio_->Position().count() > 0;
    if (!advancing) SelectTrackFrom(info.playlist_index, info.row, true);
    return true;
}

// 后台「边播边落盘」缓存：在线曲目走流式播放的同时，在另一线程把同一地址下载到本地
// 默认下载目录，成功后写进 g_online_local_cache。下一次重播时 ResolveOnlineTrackLocalCopy
// 会优先命中这个本地副本，从而零网络直接出声 —— 正是用户要的「能下载到本地就下载，下次
// 从本地直接播放」。与 StartOnlineTrackResolveAndPlay 的区别：这里**不**重选、不阻塞
// 播放，纯粹把流式播放顺手缓存一份；下载失败（例如需要自定义头才能下、或本身就是 HLS
// 切片流）就静默跳过，流式播放本身不受影响。同时刻只跑一个后台缓存，避免和手动「下载
// 歌曲」互相挤占，也避免一批在线曲目同时播放时把磁盘 / 带宽打满。
void PlayerWindow::StartOnlineTrackBackgroundCache(
    const playlist::Track& track, const std::string& resolved_url,
    const std::map<std::string, std::string>& headers,
    const std::string& user_agent) {
    std::string platform;
    std::string song_id;
    // headers / user_agent 目前未直接传给 DownloadTrackFile（它走与插件搜索同一条
    // 网络链路，无需逐条重复头）。保留参数是为了让 PlayCurrent 的双路调用签名统一，
    // 且将来若要为受 Referer/UA 保护的链接也做后台落盘，可直接在这里接上。
    (void)headers;
    (void)user_agent;
    if (!OnlineTrackIdentity(track, &platform, &song_id)) return;
    // 已经命中本地副本（path 非 http）就没有缓存的必要了。
    if (track.path.wstring().rfind(L"http", 0) != 0) return;

    const std::filesystem::path target_dir = musicfree::DefaultDownloadDirectory();
    if (target_dir.empty()) return;

    const std::wstring url_w = core::Utf8ToWide(resolved_url);
    const std::wstring base =
        musicfree::DefaultDownloadName(track.title, track.artist, url_w);
    const std::wstring target =
        target_dir / (base + musicfree::GuessExtension(url_w));

    // 单任务互斥：已经在后台缓存别的曲目就跳过这次，下次重播还会再触发。
    {
        const std::scoped_lock lock(g_background_cache_mutex);
        if (g_background_cache_active) return;
        g_background_cache_active = true;
    }

    // 把需要跨线程的字段按值捕获，避免引用悬空。
    const HWND notify = window_;
    const size_t target_playlist = playlists_.ActiveIndex();
    const size_t target_row = current_.value_or(0);
    std::thread([platform, song_id, target, resolved_url, notify,
                 target_playlist, target_row] {
        std::string reason;
        const bool ok = musicfree::DownloadTrackFile(
            core::Utf8ToWide(resolved_url), std::filesystem::path(target),
            &reason);
        if (ok) {
            const std::string key = OnlineLocalKey(platform, song_id);
            {
                const std::scoped_lock lock(g_online_local_mutex);
                g_online_local_cache[key] = std::filesystem::path(target);
            }
            // 本地副本就绪：让 UI 线程判断是否需要「用本地文件重播」兜底。
            auto* ready = new detail::CacheReadyPayload{
                target_playlist, target_row, platform, song_id};
            if (!PostMessageW(notify, detail::kMsgOnlineCacheReady, 0,
                              reinterpret_cast<LPARAM>(ready)))
                delete ready;
            musicfree::TraceDiagnostic("ONLINE_BACKGROUND_CACHE",
                "ok=1 key=" + key + "\ntarget=" + core::WideToUtf8(target));
        } else {
            musicfree::TraceDiagnostic("ONLINE_BACKGROUND_CACHE",
                "ok=0 key=" + OnlineLocalKey(platform, song_id) +
                "\nerror=" + reason);
        }
        const std::scoped_lock lock(g_background_cache_mutex);
        g_background_cache_active = false;
    }).detach();
}

} // namespace ttplayer::ui
