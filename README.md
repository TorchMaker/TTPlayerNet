# 千千静听重建版（Thousand Tunes Player）

[中文](README.md) | [English](README_en.md)

<img width="713" height="569" alt="image" src="https://github.com/user-attachments/assets/a14ee585-7f05-4000-bac7-3a3bf81d7d79" />

本仓库基于 **千千静听 5.7.9** 的行为与文件／ABI 边界，重新写出一套**可编译、可维护**的 C++20 源码。它不是把 Ghidra 反编译出的伪 C 直接丢给编译器，而是按原来的地址边界逐个还原功能，同时保持对原版运行资源的二进制兼容。

逐地址的还原进度（AddIn、原生输出、PCM、淡入淡出、媒体库、标签、CD/VCD、URL、文件关联、设置、启动流程等）记录在
[`docs/MAJOR_FEATURE_RECOVERY.md`](docs/MAJOR_FEATURE_RECOVERY.md)；构建说明与全部还原文档收在 [`docs/`](docs/)。

播放器**不再需要旁挂的辅助 EXE**。文件元数据／封面／标签处理、DSP 扫描与配置、输出设备枚举与详情，全部改成主 EXE 的私有模式内嵌执行（仍然保留进程隔离）。EXE 同级的原版 DLL、AddIn 与皮肤依然是必需的。只构建 `ttplayer_rebuild` 目标即可得到播放器本体；完整的开发者构建会额外产出测试／探针工具，发布时无需分发。
相关文档：[`EMBEDDED_WORKERS.md`](docs/EMBEDDED_WORKERS.md)、[`PORTABLE_FILE_PROPERTIES.md`](docs/PORTABLE_FILE_PROPERTIES.md)。

## 相对原版的改进

1. **扩展皮肤兼容性**：支持 6.x 以上版本的 BMP/PNG 混合皮肤、逐像素透明和按钮动画，同时保留经典 BMP 皮肤的显示方式。

2. **新增任务栏播放控制**：可直接通过 Windows 任务栏缩略图切换上一曲、播放／暂停和下一曲，无需还原播放器窗口。

3. **支持 Discord 音乐状态同步**：显示正在播放的歌曲、歌手、专辑、播放进度和暂停状态，并可选择同步当前歌词。

4. **增强在线歌词服务**：支持添加、编辑和排序多个自定义歌词服务器，增加 HTTPS、证书验证及代理支持。

5. **改善多显示器全屏体验**：支持选择全屏显示的屏幕、在不同显示器之间切换，切换到其他应用时也能保持全屏显示。

6. **增强全屏歌词交互**：新增独立的“允许拖拽歌词”选项，可在歌词文字或显示区域的空白处拖动定位播放进度，支持水平和垂直滚动模式。

7. **新增桌面歌词字号快捷调整**：在提供对应按钮的皮肤中，可直接通过工具栏放大或缩小歌词字号，并保存调整结果。

8. **采用现代 Windows 文件选择窗口**：改善添加音乐、选择文件夹和浏览歌词文件的操作体验，保留多选和文件类型筛选等功能。

9. **增强插件故障隔离**：将文件信息与标签处理、DSP 扫描／配置、输出设备探测放入独立工作进程，加入超时和取消处理，降低这些操作中的插件异常对主界面的影响。

10. **独立保存重建版配置**：使用 `TTPlayerRebuild.xml` 保存设置，首次运行时可导入原版配置而不覆盖原文件；歌词关联记录也独立保存，方便与原版并存。

11. **新增全屏下的专辑封面显示**：在全屏模式下可选择显示专辑封面，该功能曾经在 8.x 版本中存在。

12. **普通版新增 SMTC 系统媒体控件**：在 Windows 10／11 的系统媒体面板显示曲目信息、专辑封面和进度，支持播放、暂停、停止、上一首、下一首及进度跳转。见 [`docs/SMTC.md`](docs/SMTC.md)。

13. **MusicFree 音源兼容层**：手动导入一个 MusicFree 音源（单个插件 `.js`，或注册表 JSON 如 `yuanli.json`）即可在线搜索、解析播放地址并直接播放。插件用内嵌 QuickJS-NG 运行，`axios`/`fetch` 由 WinHTTP 提供真实网络能力。**音源不内置，必须由用户手动导入**，与 MusicFree 的约定一致。见 [`docs/MusicFree_Compatibility.md`](docs/MusicFree_Compatibility.md)。

14. **在线音乐窗（音乐窗）**：皮肤化的独立窗口，在窗内按歌名搜索已导入音源、双击即播；结果列表右键可选「播放／添加到列表／**下载到…**／复制歌曲名」，底部也有「下载」按钮，在线曲目可以直接落盘为音频文件。

15. **「在线搜索」不再跳转 qianqian.com**：播放列表的「在线搜索」改为按「歌手 - 歌名」组词，直接在音乐窗内检索，不再打开外部浏览器。

## 当前里程碑（0.1）

- 现代版在 Windows 10／11 上的[系统媒体传输控件](docs/SMTC.md)：曲目元数据、内嵌封面、播放状态与进度，播放、暂停、停止、上一首、下一首与进度跳转都接到既有播放器上；

- 在原 `004C0E8F` 边界还原的 Unicode `wWinMain`：不锁版本地加载 EXE 同级的 `ttpcomm.dll`、单实例事件／映射与 `WM_COPYDATA` 转发、TLS/OLE/资源/声音/CoolSB 生命周期，之后进入独立的应用程序会话消息循环；
- 运行时从已加载的 `ttpres.dll` 中按原路径（`FindResourceW`/`LoadResource`/`LockResource`）精确读取 `<DEFAULT_SKIN>` ZIP，同时保留面向外部 `.skn` 包的原始文件分支；容忍 GBK/UTF-8 两种 `Skin.xml` 编码、位图状态条带、颜色键窗口区域与按 XML 定位的控件；
- 6.1.2 版 BMP/PNG 混合皮肤：GDI+ 逐像素 alpha、四态按钮、悬停过渡、播放进度滑块脉冲与 PNG 播放列表工具条；证据与限制见 [`docs/SKIN_PNG_RECOVERY.md`](docs/SKIN_PNG_RECOVERY.md)；
- 文件选择／拖放、播放列表选中、上一曲／播放暂停／下一曲／停止、音量与左右平衡、播放状态显示，以及命令行直接打开音频/播放列表；
- Windows 原生任务栏缩略图的上/播放暂停/下三键，带实时播放状态，且不会把最小化的播放器还原出来；这一主动扩展记录在 [`docs/TASKBAR_PLAYBACK.md`](docs/TASKBAR_PLAYBACK.md)；
- 从 `CPlayerWnd` 还原的主窗口 `WM_CONTEXTMENU`：原 `ttpres.dll` 的菜单层级与文案、动态的当前播放列表与已安装皮肤子菜单、透明度级别、播放模式勾选与命令分发；
- 与原程序比对过的默认皮肤、Let's Vista 与 TT2012 主窗口绘制：精确尺寸／样式、四态位图切片、包图标、停止态 LED／文字，以及右键即时换肤；
- 按原版风格实现的有界输出管线，可选 waveOut、DirectSound、原生 KS 与 ASIO 后端；还原出的 x86 读取器／解码器／缓冲适配器已用 APE、TAK、VQF 与随附 FLAC 验证。AddIn、Media Foundation 的 URL/WAV/MPEG、原生 AIFF/AIFC/AU、精确 CUE 与裸 CD-DA 共用一条 ReplayGain／EQ／环绕／Winamp-DSP／SSRC／输出位深／抖动的处理链，并走还原出的播放／暂停／定位／停止／结束淡出状态机；
- UTF-8 M3U8 播放列表的读写，以及顺序／单曲循环／随机播放导航；
- 还原的播放列表目录 HWND 与自绘契约：隐藏的 `TreeCtrl(0x2800)`、可见的 `ListCtrl(0x2801/0x2802)`、活动列表与焦点选中两种状态、选中图／渐变／焦点框，以及工具条与关闭按钮原有的进入／离开行为；
- 还原的普通／迷你 `TTPlayer_LyricWnd`：皮肤包的 `lyric_window`/`Lyric.xml` 布局、独立模式的几何／可见性／置顶、迷你模式实线边框与横向缩放契约、普通模式固定框与九宫格弹窗、原有子窗口 HWND 与命令集、资源驱动的菜单与提示、UTF-8/UTF-16/ANSI 本地 LRC 加载、关联／重载／复制、TOML 支撑的 RichEdit 编辑器（受保护的实时标签着色、资源工具条、焦点作用域的 Ctrl+X/C/V/Z/Y 快捷键）、按时间多行绘制、平滑滚动、捕获式行拖动、方向键定位与 500 ms 歌词时间轴滚轮微调；运行时一致性证据见 [`docs/LYRIC_WINDOW_RECOVERY.md`](docs/LYRIC_WINDOW_RECOVERY.md)；
- 还原的全屏状态机：既有 `VisualCtrl` 与 `LyricCtrl` 按原有样式切换被摘下／重新挂载；不透明歌词、桌面透明歌词、分离与叠加组合布局各自使用独立的 `*FS` 设置，以及原生资源 143/145/390 的右键菜单。见 [`docs/FULLSCREEN_RECOVERY.md`](docs/FULLSCREEN_RECOVERY.md) 与可复现的 `tools/windows_sandbox/` 对比工具；
- 还原的 x86 `ttpGetSoundAddIn` 适配器：四类真实的读取器／解码器／编码器／歌词检索提供者，保留模块／工厂／会话生命周期、`IStream`、完整可扩展格式、六槽 PCM 缓冲、读／EOF／定位／复位，以及可执行的编码器 Configure/Open/Start/Write/Finalize；文件选择器与转换器只在其创建者成功后才暴露对应格式；
- 还原的读取器元数据与缩略图读写槽位、隔离的文件信息事务、内置 MP3 ID3v1/ID3v2/APEv2 保留、ReplayGain、一基的 CUE 子轨、CDA 转换／抓轨来源、URL 对话框与来源，以及 EXE 同级被监视的 `Music.library`；详细证据见 [`docs/AUDIO_RECOVERY.md`](docs/AUDIO_RECOVERY.md) 与 [`docs/MAJOR_FEATURE_RECOVERY.md`](docs/MAJOR_FEATURE_RECOVERY.md)；
- 还原的 `CSkinParser_ParseEqualizerWindow` 布局与独立的 `WS_POPUP/WS_EX_TOOLWINDOW` 均衡器：启用／配置／复位／关闭按钮，左右平衡、环绕、前置放大、十段均衡、预设、配置文件，捕获式拖动／轨道点击／悬停／Esc／方向键输入，按原尺寸裁剪的滑块填充，自绘配置菜单，窗口吸附，换肤，以及运行时序号 103/104 的 DSP 刷新；真实的 `SkinButton`/`SkinSlider` 子窗口 HWND 还原了原有逐控件捕获路径。拖动滑块时会临时把资源支持的平衡／环绕／均衡值写进主皮肤 `status` 字段，松手后恢复播放状态。资源驱动的悬停提示也按原注册方式拆分：主窗口／均衡器回调提示、播放列表原生的八个提示、以及其共享的显式字符串提示，走还原出的翻译前 `MSG` 中继，不再有重复的提示子类化／周期刷新路径；证据见 [`docs/EQUALIZER_RECOVERY.md`](docs/EQUALIZER_RECOVERY.md)；
- 使用 `TTPlayerRebuild.xml` 保存设置，并可一次性导入旧版 `TTPlayer.xml`；播放器、播放、设备、转换、媒体库、历史、热键、可视化、歌词与桌面歌词各项设置都有对应的运行时消费者；`/reg` 与 `/unreg` 走安全的按用户文件关联事务；
- `.skn` ZIP 中央目录检查与兼容性校验；
- 为应用程序根目录与 `AddIn` 分别还原了只含资源的 `ttpres.dll` 目标，保留全部原始资源载荷与语言 ID。

## MusicFree 在线音乐窗

- 音源由用户手动导入：粘贴注册表 JSON 或选择单个插件 `.js`。插件缓存在 `<exe>/musicfree/plugins/`，导入记录写入 `imported.json`，下次启动自动加载。
- 「查看 → 音乐窗」（快捷键 `F11`，或主窗口上的音乐窗按钮）打开音乐窗；窗口是开关语义，再按一次关闭。音乐窗使用皮肤包的 `<browser_window>` 段自绘外框，并通过宿主回调复用主窗口已有的拖动吸附与位置记忆（十像素磁吸与工作区约束）。
- 在线搜索、解析播放地址、双击播放或加入播放列表；在线条目会带着音源的标题／歌手／专辑入列，而不是显示成 CDN 地址的最后一段。
- 下载：结果列表右键「下载到…」或底部「下载」按钮。先解析地址，再弹保存对话框，然后在后台拉取字节写入磁盘（宽字符路径，避免中文目录乱码）。
- 完整架构与分阶段验证记录见 [`docs/MusicFree_Compatibility.md`](docs/MusicFree_Compatibility.md)。

## 构建

**Windows XP / Windows 7 用户请使用 `TTPlayerRebuild-XP-Win7.zip` 中的 EXE。**
默认现代版由新版 MSVC 构建，不适用于这些系统。旧系统版的构建、安装和功能范围见
[XP / Win7 兼容版说明](docs/LEGACY_WINDOWS.md)。Actions 会同时提供两个版本。

需要安装 Visual Studio 2026 的“使用 C++ 的桌面开发”组件和 CMake 4.2+。**当前项目必须使用 Win32/x86，不能选择 x64**（所有旧版 AddIn 与 `ttpcomm.dll` 都是 x86）。

在项目中打开终端，执行以下命令进行构建：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build build --config Release --target ttplayer_rebuild --parallel 4
```

生成文件位于：

```
build/Release/TTPlayerRebuild.exe
```

这组命令只构建播放器，不构建测试、不复制原版运行资源。运行时，将 EXE 放到包含 `ttpcomm.dll`、`ttpres.dll`、`AddIn`、`Skin` 的原播放器目录。

需要完整还原工作区（含测试与资源 DLL）时，在 Visual Studio 开发者命令行里执行：

```powershell
cmake -S rebuild -B rebuild/build -A Win32
cmake --build rebuild/build --config Debug
ctest --test-dir rebuild/build -C Debug --output-on-failure
```

运行时文件一律相对可执行文件解析，与当前工作目录无关。启动重建版界面：

```powershell
.\rebuild\build\Debug\TTPlayerRebuild.exe
```

也可以直接把受支持的音频或播放列表路径作为参数传入。程序会先绘制原版 327x141 默认皮肤；只有在皮肤包加载失败时，才退回原生深色窗口作为兜底。

还原出的资源 DLL 输出在：

```text
rebuild/build/resources/root/ttpres.dll
rebuild/build/resources/AddIn/ttpres.dll
```

### 本机 Ninja / MSVC 构建树

除 CMake 生成器外，本机还保留了一条 Ninja 增量构建路径，日常改动用它更快：

```powershell
ninja -C build2 ttplayer_rebuild
```

产物为 `build2/TTPlayerRebuild.exe`，把它复制成播放器目录里的 `TTPlayer.exe` 即可就地验证。`build2/` 已在 `.gitignore` 中排除。

### 手动 GitHub Actions 构建

工作流进入默认分支后，可从 **Actions → Manual Windows Build → Run workflow** 手动触发。该工作流产出 x86 播放器 EXE，不打包原版 DLL 与私有配置。输入项、产物与干净检出构建见 [`docs/BUILDING.md`](docs/BUILDING.md)。

## 自检与日志

音乐窗链路可以在无界面模式下自检，结果写进 `%TEMP%\ttplayer_mf_http.log`，便于逐行核对而不依赖截图：

```powershell
$env:TTPLAYER_MF_TRACE = "1"
$env:TTPLAYER_MF_SELFTEST_DOWNLOAD = "1"   # 可选：连下载链路一起验（会拉取数 MB）
.\TTPlayer.exe --mf-selftest="告白气球"
```

自检走的是真实的 `SearchAllSources → ResolvePlayUrl → AddOnlineTrack` 流程，日志里会出现这几段：

- `BROWSER_SKIN` —— 音乐窗外框皮肤段是否加载成功（`valid=` 与背景位图尺寸、内容区）；
- `MF_SELFTEST` —— 自检起止与最终是否入列（`begin query=… / end committed=…`）；
- `MF_SELFTEST_SEARCH` —— 各音源的命中统计与错误；
- `MF_SELFTEST_RESOLVE` —— 解析结果（platform/id/title/artist/url/error）；
- `MF_SELFTEST_DOWNLOAD` —— 仅在设了 `TTPLAYER_MF_SELFTEST_DOWNLOAD=1` 时出现，记录落盘字节数；
- `ONLINE_TRACK_STORED` —— 入列后从播放列表真实读回的字段（path/title/artist/album/duration_ms/元数据袋）。

## 源码布局（UI 部分）

`PlayerWindow` 仍然是一个协调者类，以保证其 HWND 归属、消息顺序与还原出的私有状态保持 ABI 中立；但每个原生窗口都有自己的实现单元：

- `src/ui/player_window.cpp`：主播放器、换肤、共享菜单支持、持久化与播放协调；
- `src/ui/player_window_lyrics.cpp`：`TTPlayer_LyricWnd` 与 `LyricCtrl`；
- `src/ui/player_window_playlist.cpp`：`TTPlayer_PlayListWnd`、列表控件及其提示／编辑处理；
- `src/ui/player_window_equalizer.cpp`：`TTPlayer_EqualizerWnd`、`SkinButton` 与 `SkinSlider`；
- `src/ui/player_window_visual.cpp`：`VisualCtrl`、由 PCM 驱动的梦幻／频谱／示波渲染器、读取器所有的专辑封面与全屏状态机；
- `src/ui/player_window_visual_options.cpp`：资源对话框 253、可视化设置／配置命令及其即时刷新路径；
- `src/ui/player_window_internal.h`：私有命令 ID 与共享的绘制／菜单／几何声明，不属于公开 API；
- `src/musicfree/`：MusicFree 音源兼容层与在线音乐窗（`musicfree_source_manager`、`musicfree_plugin`、`musicfree_http`、`musicfree_browser_window`、`musicfree_import_dialog`），共用外观在头文件 `include/ttplayer/musicfree/musicfree_theme.h` 中。

这次拆分只是结构性的：窗口类名、控件 ID、资源 ID、消息分发顺序与还原出的方法体都没有改动。

## 兼容性策略

- 新模块一律暴露带类型的 C++ 接口。
- 旧版 DLL 继续藏在 x86 声音库适配器之后；私有对象与缓冲区的虚表绝不越过插件／音频边界。
- `.skn`、M3U8／LRC，以及选定的 `TTPlayer.xml` 设置优先保全。
- 私有元数据与处理槽位保持在受保护的 x86 适配器之内；只有创建者与初始化都真正成功，格式或处理器才会被暴露出来。
- 接受的专辑封面载荷先走 WIC，仅在 WIC 解码失败时才调用 `OleLoadPicture`；读取器选择与旧版 MIME 准入规则不变。封面帧在离屏合成后原子转移；父级皮肤绘制会排除活着的 `VisualCtrl`，避免其 250 ms 的 UI 刷新暴露只有背景的帧。分析与 Windows Sandbox 回归见 [`docs/COVER_FLICKER_RECOVERY.md`](docs/COVER_FLICKER_RECOVERY.md)。
- 可视化的还原证据与精确的命令／配置映射记录在 [`docs/VISUALIZATION_RECOVERY.md`](docs/VISUALIZATION_RECOVERY.md)。

## 尚未完成／待验证的边界

1. 在兼容的物理硬件上验证 CD-DA、KS 与 ASIO；
2. 把真实样本验证扩展到 TTA/MPC/Real/MOD 以及不常见的多声道／有效位布局；
3. 还原歌词提供者的 request/result/cancel ABI，或用一个明确声明「非原版」的提供者中立客户端替换已停用的服务；
4. 对已停用的 MSN／更新／FreeDB／推荐／缓存／下载等服务保留明确的边界，而不是伪造成功；
5. 旧的私有 WTL 命令条／按钮／CBT 包装对象布局与精确 C++ 源码无法仅凭伪 C 推断；改为用自有的 Win32 实现保留其可观察职责。

## TODO List

- [ ] 鼠标在控件上的样式
- [ ] 音乐窗（已接入 MusicFree 音源，待继续完善交互细节）
- [ ] 高 DPI 支持（目前受限于图片皮肤）

## 文档索引

`docs/` 下按主题分类，常用入口：

| 文档 | 内容 |
| --- | --- |
| [`MAJOR_FEATURE_RECOVERY.md`](docs/MAJOR_FEATURE_RECOVERY.md) | 逐地址的总体还原进度 |
| [`BUILDING.md`](docs/BUILDING.md) | 构建输入、产物与干净检出构建 |
| [`STATUS.md`](docs/STATUS.md) | 当前状态总表 |
| [`AUDIO_ENGINE.md`](docs/AUDIO_ENGINE.md) / [`AUDIO_RECOVERY.md`](docs/AUDIO_RECOVERY.md) | 音频引擎与还原证据 |
| [`MusicFree_Compatibility.md`](docs/MusicFree_Compatibility.md) | MusicFree 音源兼容层与在线音乐窗 |
| [`LYRIC_DISPLAY.md`](docs/LYRIC_DISPLAY.md) / [`LYRIC_WINDOW_RECOVERY.md`](docs/LYRIC_WINDOW_RECOVERY.md) | 歌词绘制与歌词窗还原 |
| [`SKIN_PNG_RECOVERY.md`](docs/SKIN_PNG_RECOVERY.md) / [`SKIN_ALIGNMENT.md`](docs/SKIN_ALIGNMENT.md) | PNG 皮肤与皮肤对齐 |
| [`FULLSCREEN_RECOVERY.md`](docs/FULLSCREEN_RECOVERY.md) | 全屏状态机 |
| [`EQUALIZER_RECOVERY.md`](docs/EQUALIZER_RECOVERY.md) | 均衡器还原 |
| [`MEDIA_LIBRARY_AUDIT.md`](docs/MEDIA_LIBRARY_AUDIT.md) / [`MEDIA_LIBRARY_FIXES.md`](docs/MEDIA_LIBRARY_FIXES.md) | 媒体库审计与修正 |
| [`LEGACY_WINDOWS.md`](docs/LEGACY_WINDOWS.md) | XP / Win7 兼容版 |
