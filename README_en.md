# Thousand Tunes Player 2026

[中文](README.md) | [English](README_en.md)

**A from-scratch reimplementation of the classic TTPlayer, wired up to MusicFree's pluggable music sources.**

Play your local library as before; search online through the sources *you* import.
Skins, mini mode, desktop lyrics and the equalizer all keep the original feel — the online layer is plugins.

`C++20` · `Win32 / x86` · `QuickJS-NG` · `MIT`

---

## Contents

- [What this is](#what-this-is)
- [Why TTPlayer × MusicFree](#why-ttplayer--musicfree)
- [How far the integration goes](#how-far-the-integration-goes)
- [What's next](#whats-next)
- [Changes over the classic player](#changes-over-the-classic-player)
- [Copyright, licensing and compliance](#copyright-licensing-and-compliance)
- [Credits](#credits)
- [Building](#building)
- [How this project was written](#how-this-project-was-written)
- [Known issues and roadmap](#known-issues-and-roadmap)
- [Documentation index](#documentation-index)

---

## What this is

In one line: **the whole TTPlayer 5.7.9 experience rewritten in modern C++, with online music handed over to MusicFree's plugin system.**

Three things to say about it:

- **It works the way it always did.** `.skn` skins (including mixed BMP/PNG 6.x skins), mini mode,
  desktop lyrics, a 10-band equalizer with surround and preamp, the audio path (waveOut / DirectSound / KS / ASIO),
  ReplayGain, Winamp DSP, format conversion, media library, CUE tracks, taskbar playback controls,
  System Media Transport Controls (SMTC).
- **There is one new thing.** A **skinnable online music window**. Import a MusicFree source, then search,
  preview, play and download inside it. Entries arrive with real title/artist/album metadata instead of
  the tail of a CDN URL.
- **It is clean.** The source repository contains **only code written for this project**.
  No original runtime binaries ship with it — see [compliance](#copyright-licensing-and-compliance).

The core judgement behind this repository: **what TTPlayer lacks is not a player. It is a music source that is still alive.**

---

## Why TTPlayer × MusicFree

The two strengths are complementary. Together they make a complete player.

### TTPlayer's strength is the completeness of a *local* player

The skin engine, the lyric system (scrolling, drag-to-seek, KTV highlighting), the effect chain,
the format coverage (APE/TAK/VQF/TTA/MPC/MOD/MIDI…), format conversion, media library, CD ripping —
this is desktop software polished by two decades, and plenty of details still have no equal.
Compressing the whole thing down to a single playback bar tucked against a screen edge is something
modern players simply stopped doing.

Its real weakness is not features, it is **services**: the online lyric servers, recommendations, FreeDB,
downloads and the `qianqian.com` online search are long gone. This project **does not resurrect dead services,
and does not fake a successful response**. It marks those boundaries clearly, then takes a route that is still alive.

### MusicFree's strength is *pluggable sources*

MusicFree itself **bundles no music source from any platform**. Search, resolution, playlists and lyrics
are all plugin-based. The practical consequence is direct: **what you can listen to depends on which plugins
you installed, not on which platform the player signed an exclusive deal with.**

A plugin is a CommonJS module satisfying the plugin protocol, exporting async methods such as
`search` / `getMediaSource` / `getLyric`.

### Putting them together

| | What it provides |
| --- | --- |
| **TTPlayer (this project)** | The player itself: skins and interface, audio pipeline, lyrics and effects, media library |
| **MusicFree (upstream protocol)** | The source plugin protocol, the plugin ecosystem, the import/management conventions |

So the goal here is not to build another MusicFree. It is to **let TTPlayer hear online music again**,
while handing the decision of *what to listen to* back to the user.

---

## How far the integration goes

- **Manual source import.** Paste a registry JSON (such as `yuanli.json`) or pick a single plugin `.js`.
  Plugins are cached in `<exe>/musicfree/plugins/`, the import record goes to `imported.json`,
  and everything reloads on next launch. **Sources are not bundled and must be imported by you** —
  exactly as MusicFree intends.
- **Online music window.** `View → Music Window` (`F11`, or the button on the main window). It toggles.
  Its frame is drawn from the skin's `<browser_window>` section and reuses the main window's
  drag-snap and position memory (10-pixel magnet, workspace clamping).
- **Persisted window state.** The music window's **open state and position are remembered together**
  (`Player/BrowserVisible` + `BrowserWnd`, double-written to the skin sidecar and `TTPlayerRebuild.xml`).
  Open at exit means open at next launch; closing it with its own close button is remembered correctly too.
  A missing property in an old config is treated as "closed" — it will not pop up out of nowhere.
- **Search / resolve / play / enqueue.** Online entries are enqueued with the source's title, artist and album.
  Two playback paths: local file when the track can be committed to disk, streaming HTTP decode otherwise.
- **Download to disk.** "Download to…" from the context menu, or the button at the bottom.
  Resolve → save dialog → background fetch and write, with wide-character paths so non-ASCII directories
  do not end up garbled.
- **"Online search" no longer opens qianqian.com.** The playlist's online search now composes
  "artist - title" and queries inside the music window instead of launching an external browser.
- **Sandbox and network.** Plugins run inside an embedded **QuickJS-NG**; `axios` / `fetch` get real
  network capability from **WinHTTP**. The code only consumes URLs and files you provide —
  it never goes looking for sources on its own.
- **Headless self-test.** The whole `SearchAllSources → ResolvePlayUrl → AddOnlineTrack` chain can run
  without a UI, so you can check it line by line instead of by screenshot (see
  [Building → Self-test](#self-test-and-logs)).

Full architecture and the staged verification record: [`docs/MusicFree_Compatibility.md`](docs/MusicFree_Compatibility.md).

---

## What's next

The integration is not finished. In priority order:

- [ ] **Playlist import** — wire up the protocol's `importSheet`, album detail and artist detail,
      so importing a playlist is a single click
- [ ] **Lyrics through plugins** — use a plugin's `getLyric` and merge it with local LRC and the existing
      lyric servers into one priority-ordered chain
- [ ] **Concurrent multi-source aggregation** — query several sources at once, merge and de-duplicate results,
      grouped by source, instead of waiting on the first one serially
- [ ] **Music window interaction** — sorting, multi-select, batch enqueue/download, secondary filtering in results
- [ ] **Source availability probing** — validate on import that a plugin can `mount`, and flag the ones
      that cannot instead of failing at search time
- [ ] **High DPI** — currently limited by bitmap skins; stages 0–3 are done, remainder in
      [`docs/HIGH_DPI_REMAINING_PLAN.md`](docs/HIGH_DPI_REMAINING_PLAN.md)
- [ ] Consistent mouse cursors over controls

---

## Changes over the classic player

Beyond the integration above, this edition also changes the following (full item-by-item list in
[`docs/MAJOR_FEATURE_RECOVERY.md`](docs/MAJOR_FEATURE_RECOVERY.md)):

**Interface and skins**

1. 6.x mixed BMP/PNG skins with per-pixel alpha and button animation, while classic BMP skins keep rendering as before
2. Falls back to a native dark window when a skin fails to load, instead of crashing
3. Album cover display in full screen (a feature 8.x had, later removed)
4. Quick desktop-lyric font size adjustment, in skins that provide the buttons

**Playback and integration**

5. Taskbar thumbnail controls (previous / play-pause / next) with live state that **do not restore a minimized player**
6. Discord rich presence, optionally including the current lyric line
7. System Media Transport Controls: track metadata, embedded artwork and timeline in the Windows 10/11 media
   panel, with play, pause, stop, previous, next and seek — see [`docs/SMTC.md`](docs/SMTC.md)
8. Modern Windows file pickers for adding music, choosing folders and browsing lyric files,
   keeping multi-select and type filtering
9. Enhanced online lyric service: add, edit and order multiple custom servers, with HTTPS,
   certificate validation and proxy support

**Stability**

10. Plugin fault isolation: file metadata/tag jobs, DSP discovery/configuration and output-device probing run
    in dedicated worker processes with timeouts and cancellation
11. Settings saved to a separate `TTPlayerRebuild.xml`; the original `TTPlayer.xml` can be imported once
    without overwriting the original file

**Other**

12. Enhanced full-screen lyric interaction: drag the lyric text or the empty area to seek, with horizontal
    and vertical scrolling modes
13. Multi-monitor full screen: choose the display, switch between them, and stay full screen when switching apps
14. "Related links" now point at this project and upstream MusicFree
    (`Sonic853/TTPlayer` / `maotoumao/MusicFree`) instead of the original "Github repository / Send feedback";
    the longer labels make the options window's bottom row wrap automatically so the buttons stay clickable

---

## Copyright, licensing and compliance

Please read this before redistributing. The project's position has three layers.

### 1. No original executables or runtime libraries in the source repository

The repository contains **only C++ source, CMake scripts and documentation written for this project**.
Every original TTPlayer runtime resource is excluded through `.gitignore` and does not ship with the source:

| Category | Excluded |
| --- | --- |
| Runtime DLLs | `ttpcomm.dll` `ttpres.dll` `mp3PRO.dll` `msdmo.dll` `tak_deco_lib.dll` `WMADMOD.DLL` |
| Main program and resources | `TTPlayer.exe` `TTPlayer.rll` `TTPlayerRebuild.rll` `TTPlayerSet.exe` `uninstall.exe` `卸载.exe` `FAQ.txt` |
| Directories | `AddIn/` `Icons/` `Lyrics/` `PlayList/` `RealCodec/` `Skin/*.xml` |
| Reverse-engineering workspace | `/reverse` (Ghidra project and decompiler output) |

In other words: **what you see after a clone was essentially all written here.** You can verify every line
in `.gitignore`.

**The only exception is three icons**: `src/app/assets/{TTPlayer,AudioFile,PlaylistFile}.ico` (application and
file-association icons) are versioned so that a locally built EXE has an icon. They come from the original
program, their **copyright belongs to the original author**, and they are used for identification only.
If you intend to redistribute this source, consider replacing them with your own.

### 2. Running it needs your own copy of the original runtime directory

The reconstruction **cannot run standalone**: it works against the original behaviour and ABI boundaries,
so `ttpcomm.dll`, `ttpres.dll`, `AddIn` and `Skin` must be present next to `TTPlayer.exe`.

**Please take these from your own lawfully obtained installation of TTPlayer 5.7.9.** This project does not
redistribute original binaries on your behalf. Just drop the built `TTPlayer.exe` into that directory;
nothing original needs to be overwritten.

### 3. Sources, plugins and content are never bundled

Consistent with MusicFree's conventions:

- **No sources or plugins are bundled.** You import your own, or write your own. Judge the safety and legality
  of third-party plugins yourself — do not install things from unknown origins.
- **No lyrics, album art or music files are bundled.**
- **No cracking of any kind.** No VIP bypass, no lifting of preview limits, no capability to obtain paid tracks.
  This project only hands addresses you can already reach to the decoder.
- Data produced by third-party plugins is unrelated to this project. **Delete anything that may be
  copyright-infringing**, and use this software only within what is legal where you live.

### Name and trademark

"千千静听" (Thousand Tunes; formerly MP3随身听 / 芊芊静听) is the name of software independently developed by
**Nanling Zheng (郑南岭, "nanling")** in 2002. It was acquired by Baidu in July 2006, renamed
"百度音乐" in July 2013, and a new brand "千千音乐" launched in 2018 with no direct lineage to the original software.

**This project has no affiliation, authorisation or partnership with Nanling Zheng, Baidu, or Taihe Music Group.**
The name is used only to describe what is being reimplemented. It is not an official build and should not be
mistaken for one.

### About the word "reconstruction"

This project reimplements against **observable behaviour** (window class names, control IDs, resource IDs,
message ordering, file formats, settings keys) rather than compiling decompiler output directly.
Artwork, skins, icons and fonts from the original remain the **copyright of their original authors**;
pay attention to the provenance of any skin assets you use with the program.

### License

The source follows the upstream `Sonic853/TTPlayer` **MIT** licence — see [`LICENSE`](LICENSE)
(Copyright © 2026 Sonic853). Keep the original copyright notice when you use, modify or redistribute it.

Third-party components:

| Component | Licence | Location |
| --- | --- | --- |
| QuickJS-NG (embedded JS engine) | MIT | `third_party/quickjs-0.9.0/LICENSE` |
| WTL (Windows Template Library) | Microsoft Public License | `docs/licenses/WTL-MS-PL.txt` |
| YY-Thunks (XP/Win7 edition only) | MIT | `docs/licenses/` |
| VC-LTL (XP/Win7 edition only) | Eclipse Public License 2.0 | `docs/licenses/VC-LTL-LICENSE.txt` |

---

## Credits

### Thank you, Nanling Zheng

**The original author of TTPlayer, Nanling Zheng (网名 nanling)**, wrote it alone in 2002 and named it after
Teresa Teng's song *Qianqian Quege*.

It was free, ad-free and incredibly light; you could swap skins, drag the lyrics, it had all the effects you
needed and it swallowed almost every format. In an era when bandwidth was expensive, it was the only music
software on a great many computers. It was later acquired, renamed and discontinued — and that "just right"
desktop player never really got a replacement.

**What this project wants is simple: let it still open on Windows in 2026.**

### Thank you, upstream projects

| Project | Description | Link |
| --- | --- | --- |
| **Sonic853/TTPlayer** | Thousand Tunes Player, the community reconstruction. **The source base and MIT licence origin of this repository** | https://github.com/Sonic853/TTPlayer |
| **maotoumao/MusicFree** | A plugin-based, customisable, ad-free free music player. The source compatibility layer reimplements its plugin protocol | https://github.com/maotoumao/MusicFree |
| **maotoumao/MusicFreeDesktop** | The MusicFree desktop edition; reference for the source import interaction | https://github.com/maotoumao/MusicFreeDesktop |

### And the authors of these libraries

- **QuickJS / QuickJS-NG** — Fabrice Bellard, Charlie Gordon, Ben Noordhuis, Saúl Ibarra Corretgé
  → https://github.com/quickjs-ng/quickjs
- **WTL** — Windows Template Library, Microsoft Public License
- **YY-Thunks / VC-LTL** — Chuyu-Team → https://github.com/Chuyu-Team

---

## Building

> **Windows XP / Windows 7 users should use the EXE from `TTPlayerRebuild-XP-Win7.zip`.**
> The default modern edition is built with a current MSVC and does not run on those systems.
> For the legacy edition's build and feature scope, see [`docs/LEGACY_WINDOWS.md`](docs/LEGACY_WINDOWS.md).

### Prerequisites

| Item | Requirement |
| --- | --- |
| Toolchain | Visual Studio 2026 with the "Desktop development with C++" workload, plus CMake 4.2+ |
| Platform | **Win32 / x86 only — do not select x64.** All legacy AddIns and `ttpcomm.dll` are x86 |
| Runtime assets | A lawfully obtained original runtime directory (`ttpcomm.dll` / `ttpres.dll` / `AddIn` / `Skin`) |

### One-shot player build

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build build --config Release --target ttplayer_rebuild --parallel 4
```

Output:

```
build/Release/TTPlayerRebuild.exe
```

This builds **the player only** — no tests, no copying of original runtime assets. To run it, place the EXE
into a directory that has the original runtime assets.

### Daily incremental build (local Ninja tree)

For iterating during debugging, the Ninja path is faster:

```powershell
ninja -C build2 ttplayer_rebuild
```

The output is `build2/TTPlayerRebuild.exe`. Copy it over `TTPlayer.exe` in your player directory and verify
in place. `build2/` is excluded in `.gitignore`.

### Full reconstruction workspace (tests and resource DLLs)

```powershell
cmake -S rebuild -B rebuild/build -A Win32
cmake --build rebuild/build --config Debug
ctest --test-dir rebuild/build -C Debug --output-on-failure
```

The reconstructed resource DLLs are written to:

```
rebuild/build/resources/root/ttpres.dll
rebuild/build/resources/AddIn/ttpres.dll
```

### Running

Runtime files are always resolved **relative to the executable**, independent of the working directory:

```powershell
.\rebuild\build\Debug\TTPlayerRebuild.exe
```

You can also pass a supported audio or playlist path as an argument. The program first paints the original
327x141 default skin, and only falls back to a native dark window if the skin package fails to load.

### Manual CI build

Once the workflow is on the default branch, trigger it from **Actions → Manual Windows Build → Run workflow**.
It produces an x86 player EXE and **does not package original DLLs or private configuration**.
Inputs and clean-checkout builds are documented in [`docs/BUILDING.md`](docs/BUILDING.md).

### Self-test and logs

The music window chain can be self-tested **without a UI**, writing to `%TEMP%\ttplayer_mf_http.log`
so you can check it line by line instead of by screenshot:

```powershell
$env:TTPLAYER_MF_TRACE = "1"
$env:TTPLAYER_MF_SELFTEST_DOWNLOAD = "1"   # optional: also exercise the download path (fetches several MB)
.\TTPlayer.exe --mf-selftest="告白气球"
```

The log contains these sections: `BROWSER_SKIN` (music window frame skin section), `MF_SELFTEST`
(begin/end and whether the track was enqueued), `MF_SELFTEST_SEARCH` (per-source hit counts and errors),
`MF_SELFTEST_RESOLVE` (resolution result), `MF_SELFTEST_DOWNLOAD` (bytes written), and
`ONLINE_TRACK_STORED` (fields read back from the playlist).

---

## How this project was written

The integration work here was carried out by **WorkBuddy + DeepSeek V4.1 Flash** together — reading the original
behaviour, inferring boundaries, writing the C++, then compiling, packaging, running end-to-end verification
and writing the documentation.

Roughly, the division of labour is:

- **WorkBuddy** handles engineering orchestration: editing code, compiling, packaging, **launching the real
  program and reading real window text back for verification**, cleaning up test environments, maintaining docs.
  It has the local filesystem and a terminal, so the loop
  "edit → compile → install → run → read the UI → regress" is actually closed.
- **DeepSeek V4.1 Flash** handles code and reasoning: inferring interface boundaries from disassembly fragments
  and observable behaviour, writing the C++20 implementation, and chasing concrete problems in skin painting,
  layout geometry, the audio chain and encoding conversion.

One hard rule: **never guess from screenshots**. Every change has to pass
"compiles → runs → real state read back from the running process". When verifying the "related links" change,
for example, the approach was to launch the installed program, find the main window by class name, open the
options window through the application's own message hook, then enumerate every child window and read
`GetWindowTextW` plus rectangle coordinates. That is how we found the new label covering the "Save All" button
by 42 px — the link static is created later, sits above in z-order, and that strip of the button was neither
visible nor clickable. A screenshot would not have shown it; only reading coordinates does.

The address-by-address reconstruction records (80+ documents under `docs/`) came out of the same process.

---

## Known issues and roadmap

- Mouse cursor styles over controls are not yet unified
- The music window is wired to MusicFree sources; interaction details still need work (see [What's next](#whats-next))
- High DPI is limited by bitmap skins; stages 0–3 are complete, remainder in
  [`docs/HIGH_DPI_REMAINING_PLAN.md`](docs/HIGH_DPI_REMAINING_PLAN.md)
- XP / Win7 edition validation is recorded in [`docs/LEGACY_WINDOWS_VALIDATION.md`](docs/LEGACY_WINDOWS_VALIDATION.md)

**Boundaries kept explicit** (marked, rather than faked as success):

1. Verify CD-DA, KS and ASIO on compatible physical hardware;
2. Extend real-sample validation to TTA / MPC / Real / MOD and uncommon multichannel and valid-bit layouts;
3. Restore the lyric provider request/result/cancel ABI, or replace the retired service with a provider-neutral
   client that explicitly declares itself non-original;
4. Keep explicit boundaries for retired services (MSN / update / FreeDB / recommendations / cache / download);
5. The old private WTL command bar, button and CBT wrapper object layouts cannot be inferred from pseudo-C alone;
   their observable responsibilities are preserved with an in-house Win32 implementation instead.

---

## Documentation index

`docs/` is organised by topic; common entry points:

| Document | Contents |
| --- | --- |
| [`MusicFree_Compatibility.md`](docs/MusicFree_Compatibility.md) | **MusicFree source compatibility layer and online music window (the integration mainline)** |
| [`MAJOR_FEATURE_RECOVERY.md`](docs/MAJOR_FEATURE_RECOVERY.md) | Address-by-address reconstruction progress |
| [`STATUS.md`](docs/STATUS.md) | Current status overview |
| [`BUILDING.md`](docs/BUILDING.md) | Build inputs, outputs and clean-checkout builds |
| [`AUDIO_ENGINE.md`](docs/AUDIO_ENGINE.md) / [`AUDIO_RECOVERY.md`](docs/AUDIO_RECOVERY.md) | Audio engine and recovery evidence |
| [`LYRIC_DISPLAY.md`](docs/LYRIC_DISPLAY.md) / [`LYRIC_WINDOW_RECOVERY.md`](docs/LYRIC_WINDOW_RECOVERY.md) | Lyric rendering and lyric window |
| [`SKIN_PNG_RECOVERY.md`](docs/SKIN_PNG_RECOVERY.md) / [`SKIN_ALIGNMENT.md`](docs/SKIN_ALIGNMENT.md) | PNG skins and skin alignment |
| [`EQUALIZER_RECOVERY.md`](docs/EQUALIZER_RECOVERY.md) | Equalizer |
| [`FULLSCREEN_RECOVERY.md`](docs/FULLSCREEN_RECOVERY.md) | Full-screen state machine |
| [`SMTC.md`](docs/SMTC.md) / [`TASKBAR_PLAYBACK.md`](docs/TASKBAR_PLAYBACK.md) / [`DISCORD_PRESENCE.md`](docs/DISCORD_PRESENCE.md) | System integration |
| [`HIGH_DPI_REMAINING_PLAN.md`](docs/HIGH_DPI_REMAINING_PLAN.md) | High DPI remainder |
| [`LEGACY_WINDOWS.md`](docs/LEGACY_WINDOWS.md) | XP / Win7 edition |
| [`licenses/`](docs/licenses/) | Full third-party licence texts |

### Source layout (UI)

`PlayerWindow` remains a coordinator class so that HWND ownership, message ordering and private state stay
ABI-neutral; each native window has its own implementation unit:

- `src/ui/player_window.cpp` — main player, skinning, shared menus, persistence, playback coordination
- `src/ui/player_window_lyrics.cpp` — `TTPlayer_LyricWnd` and `LyricCtrl`
- `src/ui/player_window_playlist.cpp` — `TTPlayer_PlayListWnd`, list controls and tip/edit handling
- `src/ui/player_window_equalizer.cpp` — `TTPlayer_EqualizerWnd`, `SkinButton` and `SkinSlider`
- `src/ui/player_window_visual.cpp` — `VisualCtrl`, PCM-driven dream/spectrum/oscilloscope renderers,
  album artwork and the full-screen state machine
- `src/ui/player_window_visual_options.cpp` — visualisation settings commands and live refresh
- `src/ui/player_window_internal.h` — private command IDs and shared declarations, **not public API**
- `src/musicfree/` — **MusicFree source compatibility layer and online music window**
  (`musicfree_source_manager` / `musicfree_plugin` / `musicfree_http` / `musicfree_browser_window` /
  `musicfree_import_dialog` / `quickjs_engine`), with shared appearance in
  `include/ttplayer/musicfree/musicfree_theme.h`

---

## License

MIT — see [`LICENSE`](LICENSE) (Copyright © 2026 Sonic853).
Third-party component licences are in [`docs/licenses/`](docs/licenses/).
