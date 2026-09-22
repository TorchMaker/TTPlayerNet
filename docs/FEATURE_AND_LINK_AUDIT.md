# TTPlayer 重建版：新增功能 / 失效功能 / 链接审计

> 审计对象：`TTPlayer_src/TTPlayer-master`（基于千千静听 5.7.9 的 C++20 重建版）
> 审计日期：2026-09-22
> 方法：源码与 `docs/` 文档静态审查 + 链接实测（Python urllib，浏览器 UA，2026-09-22）
> 置信标注：**[确证]** = 代码/文档直接证据；**[推断]** = 基于证据的合理推论

---

## 0. 前提：一个必须先说清楚的依赖

重建版**不是独立可运行的程序**。README 原文："EXE 同级的原版 DLL、AddIn 与皮肤依然是必需的。"
运行时从 `ttpres.dll` 里按原路径 `FindResourceW`/`LoadResource`/`LockResource` 读取 `<DEFAULT_SKIN>` ZIP **[确证]**。
即：**没有一份原版千千静听 5.7.9 安装目录，本程序跑不起来**；而 CI 里提供给用户的原版 5.7.9 下载链接**已 404**（见 §3.2）。

---

## 一、原版千千静听没有的功能（新增）

### 1.1 README 官方列出的 15 条改进

| # | 新增功能 | 主要实现位置 | 启用条件 |
|---|---|---|---|
| 1 | 扩展皮肤兼容性：6.x BMP/PNG 混合皮肤、逐像素 alpha、四态按钮、悬停过渡 | `src/skin/skin_image.cpp` | 默认开 |
| 2 | 任务栏播放控制（上/播放暂停/下三键） | `src/ui/taskbar_playback.cpp`（`ITaskbarList3::ThumbBarAddButtons`） | 默认开 |
| 3 | Discord 音乐状态同步（含歌词同步选项） | `src/integrations/discord_presence.cpp`、`discord_track_presence.cpp` | 复用原版 MSN 复选框字段 `send_title_to_msn`（默认 **true**）；需本机 Discord 客户端在跑 |
| 4 | 增强在线歌词服务：多服务器增删排序、HTTPS、证书验证、代理 | `src/ui/lyric_service_editor` 等 | 默认开 |
| 5 | 多显示器全屏：可选屏幕、跨屏切换、切走仍保持全屏 | `src/ui/` 全屏相关 | 默认开 |
| 6 | 全屏歌词交互：独立"允许拖拽歌词"选项、拖动定位进度、水平/垂直滚动 | `docs/FULLSCREEN_LYRIC_DRAG.md` | 默认开 |
| 7 | 桌面歌词字号快捷调整（皮肤需提供按钮） | 桌面歌词工具栏 | 依赖皮肤提供对应按钮 |
| 8 | 现代化文件选择窗口（替代原版自绘） | — | 默认开 |
| 9 | 插件故障隔离：元数据/标签/DSP/输出设备探测移入内嵌工作进程 | `src/app/{file_info_worker,dsp_worker,output_device_worker}.cpp` | 默认开 |
| 10 | 独立配置 `TTPlayerRebuild.xml`，首次可导入原版且不覆盖 | `src/` 配置层 | 默认开 |
| 11 | 全屏专辑封面显示（该功能曾在 8.x 出现过） | `docs/FULLSCREEN_ALBUM_BACKGROUND.md` | 默认开 |
| 12 | **SMTC 系统媒体控件**（Win10/11 系统媒体面板） | `src/ui/system_media_controls.cpp`（C++/WinRT + `ISystemMediaTransportControlsInterop::GetForWindow`） | 默认开；**XP/Win7 版被 `TTPLAYER_LEGACY_WINDOWS` 排除** |
| 13 | **MusicFree 音源兼容层**：导入音源插件 `.js` 或注册表 JSON，在线搜索 + 解析播放地址 | `src/musicfree/*`、`third_party/quickjs-0.9.0` | **音源不内置，必须用户手动导入**；必须联网 |
| 14 | **在线音乐窗（音乐窗，F11）**：皮肤化窗口内搜索、双击即播、右键「下载到…」落盘 | `src/musicfree/musicfree_browser_window.cpp` | 需先导入音源 |
| 15 | 「在线搜索」不再跳转 qianqian.com，改为音乐窗内检索 | `src/ui/player_window.cpp:5140` 附近 | 默认开 |

### 1.2 代码层发现、README 未列入的新增项

| 功能 | 实现位置 | 说明 |
|---|---|---|
| **高 DPI 适配（Per-Monitor V2）** | `src/app/main.cpp:145-162` 三级回退 `SetProcessDpiAwarenessContext`；`src/ui/dpi_scale.cpp`；`src/ui/player_window.cpp` `SkinForDpi` / `dpi_mirrors_` / `ApplyDpiScale` | 原版进程完全 DPI-unaware。重建版按 DPI 缓存皮肤镜像。**仍未完成**，见 §2.5 |
| **内嵌 QuickJS-NG JS 引擎** | `third_party/quickjs-0.9.0` + `src/musicfree/quickjs_engine.cpp`；内嵌 `crypto-js / dayjs / qs / big-integer / he` 五个 JS 库 | 原版无任何脚本能力 |
| **XP / Win7 兼容层** | `cmake/xp_runtime.cmake`（YY-Thunks v1.2.2 + VC-LTL 5.3.1）、`cmake/legacy_windows.cmake` | 原版当年原生支持 XP，重建版需额外 Thunk 层才能回去 |
| **WTL 10.01 迁移** | `cmake/wtl.cmake` | 原版用私有旧版 WTL |
| **CLI 编码器一键安装脚本** | `cmake/install_cli_encoders.ps1`（qaac/flac/opus/mpc/tta/tak/mac/vorbis，SHA256 钉死）+ 13 项 FFmpeg PATH 预设写入 `AddIn/ttp_clienc.xml` | opt-in，原版无 |
| **命令行自检开关** | `--mf-selftest="关键词"`、`--dpi-selftest`、`TTPLAYER_MF_TRACE=1`、`TTPLAYER_DPI_OVERRIDE`；日志 `%TEMP%\ttplayer_mf_http.log` | 原版无 |
| **GitHub Actions CI** | `.github/workflows/manual-build.yml`（workflow_dispatch，可发 GitHub/Gitee Release） | 原版无 |
| **任务栏封面缩略图 / Peek 预览** | `src/ui/taskbar_preview.cpp`（`DwmSetIconicThumbnail`、`WM_DWMSENDICONICTHUMBNAIL`） | 文档明写"按用户要求添加的重建版功能" |
| **歌词上传地址改为自建服务器** | `src/ui/lyric_upload_window.cpp:198-209`（取当前搜索服务器 origin + `/dll/lrcup.php`） | 原版硬编码 `qianqian.com`，见 §3.1 |
| **UTF-8 M3U8/LRC 严格读写** | `CP_UTF8` + `MB_ERR_INVALID_CHARS` | 原版主要走 GBK |

---

## 二、无法使用 / 失效的功能

### 2.1 依赖已消亡的外部服务（服务端死了）

| 功能 | 证据 | 用户侧表现 |
|---|---|---|
| **FreeDB / CDDB 查询** | `include/ttplayer/settings/settings.h:272-274` 默认 `http://freedb.freedb.org/~cddb/cddb.cgi`（freedb 已于 2020 停运）；后端**根本没实现**：`src/ui/playlist_network_commands.cpp:75-88` 的 `LegacyBackendUnavailableNotice` 对 freedb 直接返回失败文案 `{0x81f4,0x8165}` **[确证]** | 弹失败提示框，不崩溃。选项里仍可编辑服务器地址（控件 2177），但换了也没后端 |
| **播放列表「在线搜索」→ 音乐窗「report」「download」** | `LegacyBackendUnavailableNotice` 的 `report`/`download` 分支同样只返回失败文案 **[确证]** | 弹失败提示 |
| **皮肤下载站入口** | `src/ui/player_window_options.cpp:243-244` `kSkinDownloadTarget = L"http://ttplayer.qianqian.com/skin.htm"`，**硬编码、不可配置**；控件 2109 点击 → `ShellExecuteW`（`:7262`）**[确证]** | 点一下打开浏览器 → 502/DNS 失败 |
| **关于页内置皮肤链接** | `player_window_options.cpp:6263` `embedded.metadata.url = L"http://www.ttplayer.com"` **[确证]** | 同上 |
| **自动更新 / 推荐列表** | `settings.h:277-280` 保留 `check_update_days`、`last_checked_day`、`accept_recommendation_list` 等字段，`settings.cpp` 只做 XML 读写；全仓 `src/` **无 CheckUpdate 实现** **[确证]** | 选项页控件可见、设置能保存，但永远不生效 **[推断]** |
| **MSN 状态同步** | README「尚未完成／待验证的边界」第 4 条：对已停用的 MSN／更新／FreeDB／推荐／缓存／下载等服务保留明确边界，而不是伪造成功 **[确证]** | 复用该复选框给 Discord，实际 MSN 服务本身不存在 |

### 2.2 桩实现 / 未接线（点了没反应）

- **AddIn 的非 reader 分类全部不执行**：`src/plugins/plugin_manager.cpp:23-25` 注释明写 "writer/processor/encoder labels were placeholders and left every non-reader category non-executable"；`:2023` 等对无法识别接口的模块记 `E_NOTIMPL` 并 `FreeLibrary` **[确证]**
  → **第三方编码器插件、歌词搜索插件加载了但不会跑**。
- **MusicFree 半截工程**（`docs/MusicFree_Compatibility.md:201-206` 原文列出「后续可延伸（未包含在本次改动）」）**[确证]**：
  1. `SearchAllSources` 结果**未接入播放列表 UI**（`player_window_playlist_network.cpp` 的"网络"视图已存在但没接）
  2. Options 里**没有「音源管理」入口**
  3. `GetLyric` 已可用但**未接入歌词子系统**
  4. `env.getUserVariables` 返回空对象
- **`AddIn/` 目录在当前仓库中不存在** **[确证]**；Winamp DSP 走 `src/audio/winamp_dsp.cpp`，缺 `dsp_*.dll` 即无效果。
- 其余 `E_NOTIMPL`（`http_stream_source.cpp:254-275`、`player_window_playlist.cpp:1073-1082`、`player_window_library.cpp:551-560`）属于 COM 未使用方法的标准空实现，**不是缺陷**。

### 2.3 被编译开关关掉的功能

`CMakeLists.txt:4-13` 的 `option()`：`BUILD_TESTING=OFF`、`TTPLAYER_STAGE_RUNTIME=ON`、**`TTPLAYER_LEGACY_WINDOWS=OFF`**、`TTPLAYER_OPTIMIZE_SIZE=ON`，5 个 `TTPLAYER_BUILD_*_TESTS=OFF`。全仓**无 `#if 0`** **[确证]**。

只有 `TTPLAYER_LEGACY_WINDOWS` 会切掉功能，见 §2.4。

### 2.4 XP / Win7 版（`TTPLAYER_LEGACY_WINDOWS=ON`）必然缺失

| 缺失项 | 证据 |
|---|---|
| SMTC 系统媒体控件 | `CMakeLists.txt:175` 仅 `NOT TTPLAYER_LEGACY_WINDOWS` 时编译 `src/ui/system_media_controls.cpp`；`include/ttplayer/ui/player_window.h:24/280/888` 用 `#if !defined(TTPLAYER_LEGACY_WINDOWS)` 包住成员与 `HandleSystemMediaCommand` |
| 任务栏缩略图按钮 / DWM 合成 | `docs/LEGACY_WINDOWS.md:23` 明写 XP 不提供 |
| 随机播放三轮阈值 | `include/ttplayer/playlist/playback_order.h:15` 在 legacy 下置 0（`docs/RANDOM_PLAYBACK_ROUNDS.md:18`） |

### 2.5 依赖缺失即失败 / 明确未完成

| 项目 | 状态 |
|---|---|
| **高 DPI** | 骨架已落地（PMv2 + `SkinForDpi`），但 README TODO 仍挂着「高 DPI 支持（目前受限于图片皮肤）」；`docs/HIGH_DPI_REMAINING_PLAN.md:40/:124` 明写**现网皮肤包没有任何 @Nx 高清资源**，该项阻塞待生态 **[确证]** |
| **MIDI** | `include/ttplayer/audio/midi_player.h:42-43` 走 DirectShow；`docs/MIDI_PLAYBACK_ANALYSIS.md:152` 确证该路径**不产出 PCM**，EQ/DSP/ReplayGain/编码器都拿不到数据；`:169` `ApplyVolumeLocked` 无 MCI 分支 → **MIDI 音量/平衡调节无效** **[确证]** |
| **外部 CLI 编码器** | `docs/EXTERNAL_ENCODERS.md:5/:22`：`Encoders\ffmpeg.exe` 是**显式相对路径，缺失时不回退 PATH 搜索**；`:123` 即使 EXE 齐全也可能在 Start 阶段失败 → 转换任务报错（不静默） |
| **编码器脚本 SHA256 钉死** | `cmake/install_cli_encoders.ps1:22` 校验不过即 `throw "SHA256 mismatch"`。实测 `https://monkeysaudio.com/x86` 现重定向到 `MAC_1326.exe`，**上游一更新版本就会 mismatch 中断安装** **[确证 + 推断]** |
| **CD-DA / KS / ASIO** | README「尚未完成／待验证的边界」第 1 条：待在兼容的物理硬件上验证 **[确证]** |
| **TTA/MPC/Real/MOD 等格式** | 同上第 2 条：样本验证待扩展 **[确证]** |
| **歌词提供者 ABI** | 同上第 3 条：原服务已停用，尚未还原或用中立客户端替换 **[确证]** |
| **私有 WTL 命令条/按钮/CBT 包装** | 同上第 5 条：无法仅凭伪 C 推断，改用自有 Win32 实现保留可观察职责 **[确证]** |
| **`docs/STATUS.md` 标记的未完成项** | `:62` FreeDB、音乐窗下载/report 后端、**原生查找 UI**；`:67` 歌词 live shared-reader 标签编辑、完整 `LyricSaveMode` leave-document 事务、CUE/自动写标签生命周期；`:547` Nero AAC 与外部 CLI 预设**未验证**；`PLAYLIST_MARQUEE_MENU_AUDIT.md:122` 命令 `0x7EFA`（freedb 查询）标为未完成 |
| **鼠标在控件上的样式 / 音乐窗交互细节** | README TODO List 明确挂 `- [ ]` **[确证]** |

---

## 三、链接统计

共扫描到 **330 个唯一 URL**，其中第三方库自带文档链接（nlohmann/json.hpp 的 API 文档、quickjs 的 package-lock）约 190 个，与业务无关，已排除。业务相关 **137 个**，其中真正会被用户/构建触碰的约 45 个，实测如下。

### 3.1 被有意改写过的链接 **[确证]**

| 位置 | 原版 | 重建版改为 | 说明 |
|---|---|---|---|
| `src/ui/project_links.h:24-26` | 原版菜单 0x9d 占位符指向官网／论坛 | `https://github.com/Sonic853/TTPlayer`（"Github仓库"）、`.../issues`（"提交反馈"），命令 `0xb650`/`0xb651` | 主右键菜单与选项窗口共用 |
| `src/ui/lyric_upload_window.cpp:198-209` | 硬编码 `http://www.qianqian.com/dll/lrcup.php` | **动态**：取当前歌词搜索服务器的 origin + `/dll/lrcup.php`（`WinHttpCrackUrl` 解析） | 旧址仅存于 `docs/LYRIC_MENU_RECOVERY.md:17` |
| `src/ui/player_window.cpp` 在线搜索 | `ShellExecute` 打开 qianqian.com | 改为音乐窗内检索，不再跳浏览器 | README 第 15 条 |
| `src/musicfree/musicfree_http.cpp:189` | — | 注释示例：`http://yinyue.haitangw.net/...` 自动升级为 `https://` | 仅注释，非硬编码 |

### 3.2 实测死链（**必须处理**）

| 链接 | 位置 | 实测 | 影响 |
|---|---|---|---|
| `http://www.ttplayer.com` | `player_window_options.cpp:6263`、`docs/OPTIONS_RECOVERY.md` | **502** | 关于页内置皮肤 URL，点击即失败 |
| `http://ttplayer.qianqian.com/skin.htm` | `player_window_options.cpp:243`（硬编码，不可配置） | **502** | 「下载皮肤」入口完全失效 |
| `http://www.qianqian.com/dll/lrcup.php` | `docs/LYRIC_MENU_RECOVERY.md:17`（历史记录） | **502** | 仅文档残留，代码已改用动态 origin |
| `http://freedb.freedb.org/~cddb/cddb.cgi` | `settings.h:274`（默认值） | **502** | freedb 2020 停运；且后端未实现（§2.1） |
| `http://ftp6.nero.com/tools/NeroAACCodec-1.5.1.zip` | `docs/EXTERNAL_ENCODERS.md` | **404** | Nero 原站已下线；脚本已改用 archive.org 镜像 |
| `https://github.com/Sonic853/TTPlayer_Tests` | `docs/BUILDING.md` | **404** | 文档引用的测试仓库不存在或已私有化 |
| `https://github.com/user-attachments/files/32415122/TTPlayer579.zip` | `.github/workflows/manual-build.yml:239` | **404** | **最严重**：Release 说明里告诉用户"安装前确保已装 5.7.9"并给此链接，而重建版运行必需原版 DLL/皮肤（§0）。新用户拿不到前置包 |

### 3.3 受限 / 需人工确认

| 链接 | 实测 | 判断 |
|---|---|---|
| `https://downloads.sourceforge.net/project/wtl/WTL%2010/.../WTL10_01_Release.zip`（`cmake/wtl.cmake`） | 403 | SourceForge 对非浏览器 UA 的反爬，**浏览器/U 盘下载通常正常**。构建前置依赖，需人工确认 |
| `https://downloads.sourceforge.net/project/tta/tta/ttaenc-win/ttaenc-3.4.1.zip` | 403 | 同上 |
| `https://sourceforge.net/projects/wtl/files/WTL%2010/...` | 403 | 同上 |
| `http://yinyue.haitangw.net/` | 403 | MusicFree 第三方音源站，拒绝直连（需特定 UA/Referer 或已关闭）。仅出现在代码注释里 |
| `https://web.archive.org/web/20160222054250id_/http://ftp6.nero.com/...` | 本环境不可达 | 作为 Nero 404 的替代镜像，需人工确认 |
| `https://github.com/Chuyu-Team/VC-LTL5/releases/download/v5.3.1/VC-LTL-Binary.7z` | 本环境代理 502 | release 页存在性未直接验证；YY-Thunks v1.2.2 release 页**已验证存在**，此链接大概率可用 |
| `https://github.com/user-attachments/assets/a14ee585-...`（README 头图） | 本环境代理 502 | README/README_en 首屏截图，需人工确认 |
| `https://github.com/actions/runner-images/issues/14017` | 本环境代理 502 | 外部 issue，非构建依赖 |

### 3.4 实测可用

`github.com/Sonic853/TTPlayer`（200）· `.../issues`（200）· `Sonic853/gitee-release-cli-rust`（200，含 `blob/9993cd7.../docs/rust-cli.md` 深度链接，200）· `13413.kstore.vip/yuanli/yuanli.json`（200，MusicFree 音源注册表）· `interface.music.163.com/eapi/batch`（200）· `nu774/qaac`、`xiph/flac`、`knik0/faac` 的 GitHub release 下载包（均 200）· `foobar2000.org/downloads/Free_Encoder_Pack-2026-03-13.exe`（200）· `www.foobar2000.org/encoderpack`（200）· `www.thbeck.de/Download/TAK_2.3.3.zip`（200）· `files.musepack.net/windows/musepack_windows_sv7.zip`（200）· `archive.mozilla.org/pub/opus/win32/opus-tools-0.2-win32.zip`（200）· `secure-appldnld.apple.com/.../iTunesSetup.exe`（200，Apple 直链居然还活着）· `support.apple.com/en-us/106373`（200）· `raw.githubusercontent.com/lameproject/lame/master/Dll/BladeMP3EncDLL.h`（200）· `cmake.org/.../Visual Studio 18 2026.html`（200）· `monkeysaudio.com/x86`（200，但**重定向到 MAC_1326.exe**，见 §2.5 校验和风险）

### 3.5 文档占位符（不是真链接，无需处理）

`https://lyrics.example:8443/api/search/?key=1`、`https://lyrics.example:8443/dll/lrcup.php`（`LYRIC_MENU_RECOVERY.md`）· `https://example.test/lyrics`、`https://example.com/`（`LYRIC_SERVICE_EDITOR.md`）· `http://your-server/lyrics`、`http://your-other-server/lyrics`（`LYRIC_SEARCH_RECOVERY.md`）· `https://host/song.flac`、`http://host/song.ape`（`src/audio/audio_engine.cpp` 注释）

---

## 四、风险排序与建议

| 优先级 | 问题 | 建议 |
|---|---|---|
| **P0** | 原版 5.7.9 下载链接 404，而重建版运行**必需**原版 DLL + 皮肤 | 重新上传安装包并更新 `manual-build.yml:239`，或在 README 明确替代获取途径 |
| **P0** | 「下载皮肤」入口硬编码死链，不可配置 | 改为可配置 URL，或指向现存的皮肤归档站 |
| **P1** | FreeDB 后端未实现 + 默认服务器已停运 | 要么移除入口，要么按 README 第 3/4 条给出"明确边界"提示 |
| **P1** | MusicFree 歌词/播放列表 UI 未接线 | 按 `MusicFree_Compatibility.md` §9 收尾 |
| **P2** | 编码器脚本 SHA256 钉死，上游一更新就中断 | 改为"校验失败时警告并询问"，或提供 hash 更新流程 |
| **P2** | SourceForge 链接 403（WTL/TTA） | 说明需浏览器手动下载，或改用镜像 |
| **P3** | `docs/BUILDING.md` 引用 `TTPlayer_Tests` 404 | 删除或替换 |
| **P3** | 高 DPI 缺 @Nx 皮肤资源 | 阻塞待生态，README TODO 已记录 |
