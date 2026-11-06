# 启动性能：慢在哪里、改了什么

## 现象

双击运行后主窗口要等好几秒才显示/才能点。黑盒测量把「窗口出现」和「窗口可交互」
分开计时后发现：窗口其实早就 `ShowWindow` 了，是主线程还堵在启动阶段的同步工作里，
消息循环迟迟没跑起来——用户看到的就是「不显示窗口」或「窗口出来但点不动」。

## 冷启动阶段分解（`TTPLAYER_STARTUP_TRACE=1` 实测，单位 ms）

| 阶段 | 耗时 | 说明 |
| --- | --- | --- |
| i18n / ttpcomm / 单实例 / ttpcomm→ttpres | ~95 | 加载 `ttp_i18n.dll`、`ttpcomm.dll`、`ttpres.dll`、OLE、公共控件 |
| SoundLibraryRuntime::Initialize | 40–457 | 扫描并加载 `AddIn` 下的解码插件 DLL（冷/热缓存差异大）；**本轮已移出启动关键路径** |
| LoadRuntimeSettings | 17–118 | 读 XML 设置与播放列表 |
| LoadStartupSkin | 424–579 | 解析皮肤包、解码位图、载入配色 |
| PlayerWindow::Create | 639–990 | 注册窗口类、创建主窗/播放列表/歌词/均衡器/视觉窗与皮肤控件，`ShowWindow` 在这里 |
| **LoadMusicFreeSourcesOnStartup** | **≈1900** | **部署 JS 依赖库 + 逐个挂载 20 个缓存插件** |
| └ `LoadCached()` 单插件 | 18–310（均值 ~95） | 每个插件新建一个 QuickJS runtime、注册 require/globals、eval 插件脚本，部分还要 eval crypto-js 等大库 |
| └ 协议注册表 `Load()` | ~24 | 洛雪 / REST 音源重建 |
| RestoreStartupMusicBrowserWindow | 0–220 | 上次开着音乐窗时恢复 |
| 恢复播放 / 文件关联检查 | ~20 | |
| **进入消息循环** | 旧版在窗口创建后 **+2200ms** | 这段时间窗口存在但不响应 |

结论：**音源挂载是最大的可消除项**。它是纯 CPU 工作，且结果并不是显示主窗口所必需的。

## 优化

1. **音源重建移到低优先级后台线程**
   `StartMusicFreeSourcesLoading(hwnd, msg)` 启动后台线程；线程先 `Sleep(500)` 让主窗口
   完成首帧，再设 `THREAD_MODE_BACKGROUND_BEGIN`（失败退回 `THREAD_PRIORITY_IDLE`）。
   完成后 `PostMessage(kMsgStartupSourcesReady)` 通知主线程。
   `window_created → message_loop_start` 从 2200ms 降到 ~35ms。

2. **需要音源列表的入口统一等待**
   `WaitForMusicFreeSources()` 在 `SearchAllSources` / `ResolvePlayUrlEx` /
   `ShowMusicFreeBrowser` / `ShowMusicFreeImportDialog` 里调用。后台已完成时是零开销判断；
   用户在加载完成前主动打开音乐窗/导入窗时才阻塞等待，避免显示空列表。
   未启动后台任务的工具进程会自动退化为同步加载一次，并登记完成，避免重复挂载。

3. **依赖音源的启动收尾改为消息驱动**
   恢复音乐窗、恢复上次播放（可能要解析在线曲目）挪到 `kMsgStartupSourcesReady`
   在消息过滤器里处理；自检模式保持同步，保证 `--mf-selftest` / `--dpi-selftest` 的
   确定性与退出码。

4. **去掉启动时的 `SetProcessWorkingSetSize(..., -1, -1)`**
   它主动清空工作集，把刚装入的 ttpcomm/ttpres、皮肤位图、字体立刻换出，首帧绘制
   又得逐页调回，实测拖慢窗口第一次响应几百毫秒。现代 Windows 不需要应用主动 trim。

5. **启动淡入改为即时到位**：`Create()` 里临时关闭 `fade_windows` 再调用
   `BeginSkinWindowFade`，让它走即时 `FinishSkinWindowFade()` 分支（目标 alpha、
   pending 标志、启动最小化都照常处理），随后恢复设置。省掉启动时对主窗/歌词窗/
   播放列表/均衡器的逐帧 `RDW_UPDATENOW` 同步重绘；运行期间的窗口淡入不受影响。

6. **后台加载可取消、关闭不等待**：`FinishMusicFreeSourcesLoading()` 置停止标志并
   唤醒让路等待，`LoadCached()` 在插件之间检查该标志，因此「刚启动就关窗口」
   时最多再等当前这一个插件（约 0.3s）即可 join，不会拖慢退出，也避免 CRT
   静态析构时后台线程仍在跑。

7. **AddIn 音效库延后到窗口显示后**（满足「未必马上播放」的场景）
   `SoundLibraryRuntime` 改为 `SetDirectory()` + 幂等 `EnsureLoaded()`；`wWinMain`
   启动段只登记 `AddIn` 路径，不再逐个 `LoadLibrary`。`PlayerWindow::Create()`
   完成后排一个 350ms 一次性定时器，消息过滤器在空闲时段执行加载；若用户
   提前按键/点击/拖文件，则在放行输入前同步兜底加载，保证格式表不会是空的。
   加载完**必须重新调用 `SetSoundLibrary()`**：它在调用瞬间复制 `ReaderFormats`
   到播放器，窗口创建前那次复制发生在懒加载之前。文件关联检查依赖这份格式表，
   也一并移到加载之后。自检 / `--reg` / `--unreg` 仍同步加载，保持确定性。
   AddIn DLL 的工厂函数会注册窗口类、初始化 COM，所以刻意留在主线程。

8. **皮肤启动路径加速，但不整体延后**
   皮肤决定了主窗口大小、区域（HRGN）、所有控件坐标和首帧位图，`Create()`
   里 `CreateWindowExW` → `WM_CREATE` 就要用；整体挪到窗口显示后会先出现默认
   外观再跳变一次，反而更显眼。因此只做无视觉影响的加速：
   - `SkinPackage::ExtractTo` 加 `.ttp-extracted` 完成标记。缓存目录名已含包
     时间戳 + 内容指纹，命中时 O(1) 返回（首尾条目抽查防缓存被清理），省掉
     对上百个皮肤文件逐个 `is_regular_file` + `file_size`；
   - `SkinPackage::Parse` 的 EOCD 回扫限定在末尾 22 + 65535 字节内，不再对
     几 MB 的大皮肤包逐字节全量扫描；
   - 新增 `skin_extract_begin` / `skin_extracted` / `skin_parsed` 三个埋点，把
     原来一个 424–579ms 的 `skin_loaded` 拆开，便于定位剩余开销属于解压、
     MSXML 解析还是位图解码。
   皮肤目录扫描（`StartSkinMenuCatalogLoad`）本来就是右键菜单打开时的异步任务，
   不在启动链路，皮肤装得多不会拖慢启动。

## 诊断与验证方法

- 阶段日志：设环境变量 `TTPLAYER_STARTUP_TRACE=1` 后启动，日志在
  `%TEMP%\ttplayer_startup.log`，每行是「自进程入口起的毫秒数 + 阶段名」，
  相邻两行之差就是该阶段耗时。未设置该变量时每个埋点只是一次已缓存的环境变量
  查询，无文件 I/O。
- 黑盒测量：`.test/startup_probe.py [exe]`，测「进程启动 → 主窗口 HWND 出现 →
  `SendMessageTimeout(WM_NULL)` 成功」三个时间点及进程 CPU 时间；启动、测量、关闭
  都在同一个脚本内完成。
- A/B 对比：`.test/startup_bench.py <旧exe> <新exe> [次数]`，每个二进制各跑 N 次
  并丢弃第一次（Defender / 页缓存预热），输出中位数。

## 验证结果（2026-11-06，本机 A/B）

`.test/startup_bench.py` 各跑 6 次并丢弃预热，取中位数；基线是本轮改动前、
仅带计时埋点的同源代码（`.bench_baseline.exe`）：

| 指标 | 优化前 | 优化后 |
| --- | --- | --- |
| 主窗口 HWND 出现 | 580 ms | 641 ms |
| **窗口可交互**（`SendMessageTimeout(WM_NULL)` 成功） | **1762 ms** | **1325 ms** |
| 进程 CPU 时间（探测点采样） | 1391 ms | 992 ms |

冷启动/ Defender 首次扫描时波动很大（出现过 3–9 秒的预热值），已按预热丢弃。
用户最初反馈的「要等好几秒才显示窗口」对应的是冷启动叠加同步挂载，优化后
窗口创建到进入消息循环只剩 ~35ms（优化前 ~2200ms），窗口不会再「存在但点不动」。

功能回归 `.test/e2e_protocols.py`：主窗、音乐窗、导入窗均可打开；后台加载完成后
音源列表里 MusicFree / 洛雪 LX Music / REST 三类齐全，右键「查看音源详情 /
删除该音源」文案正确。

构建产物 `TTPlayer.exe` sha256 `3ec9d3170a8f0c7f300de9971e43f7bc6c0ff88452e7a74b3b49534b1104bc30`，
build2 产物 / 部署 exe / 安装包解出的 exe 三方一致。

## 隔离实验与仍然存在的开销

把 `musicfree/imported.json` 临时移走、让后台加载完全无事可做，再测一次：
窗口可交互仍是约 1.30s（有音源时 1.24s）。**这证明剩下约 0.6s 与音源加载
无关**，属于主窗口自身创建之后的固定工作，主要来自 `WM_CREATE` 里的播放列表
载入、媒体库监控/刷新、歌词窗/均衡器窗/播放列表窗创建、250ms UI 定时器，
以及窗口激活时的 `opaque_when_active` 动画。

音效库 DLL 加载、皮肤包解析、主窗口创建是显示主界面必需的工作。后续第二轮
已把「非首屏附属窗口」从这条关键路径移走，见下一节。

## 第二轮：非首屏辅助窗口延迟创建（2026-11-06 下午）

新增 `skin_*` / `wm_create_*` / `window_create_*` / `aux_windows_*` 细分埋点后，
冷启动的一次完整分解（单位 ms）如下：

| 阶段 | 耗时 | 说明 |
| --- | --- | --- |
| `skin_extract_begin → skin_extracted` | ~20 | 皮肤解压（完成标记命中） |
| `skin_xml_decoded → skin_xml_parsed` | ~9 | 4 个 XML 的 MSXML 解析 |
| `skin_bitmap_begin → skin_load_end` | ~312 | **76 次 `SkinImage::Load`**，累计 290ms；默认皮肤是 55 张 24bpp BMP |
| `window_create_enter → wm_create_enter` | ~50–100 | 图标加载 + 注册窗口类 |
| `wm_create_library → wm_create_aux_windows` | **~483** | 歌词窗 216 + 均衡器窗 47 + 播放列表窗 220 |
| `window_create_shadow → window_create_first_paint` | ~316 | `ShowWindow` + `UpdateWindow` 首帧绘制 |
| `window_create_first_paint → window_create_done` | ~66 | 显示辅助窗、启动淡入即时收尾 |

结论：**三个辅助顶层窗（歌词 / 均衡器 / 播放列表）启动时全都不参与主窗口首帧，
却占了 WM_CREATE 的 483ms**，是当时最大的可消除项。

### 做法

- `WM_CREATE` 里不再同步调用 `CreateLyricWindow/CreateEqualizerWindow/CreatePlaylistWindow`，
  改为 `PostMessageW(window_, kMsgCreateStartupAuxWindows, 0, 0)`；消息循环启动后
  由 `HandleMessage` 调用幂等的 `CreateStartupAuxWindows()` 补建，保持原来的
  Lyric → Equalizer → PlayList 顺序（决定重叠时 z 序）。
- 补建后统一补齐 `ApplyWindowShadow()`、逐窗 `ApplySkinWindowAlpha()`、
  `ApplyActiveLyricWindowState()` 以及均衡器/播放列表的可见状态；主窗已最小化时
  不单独显示 owned 窗口。
- 用户比该消息更早输入时，`PlayerFilter::PreTranslateMessage` 会先调用
  `CreateStartupAuxWindows()` 兜底；各命令入口本来就有
  `if (!handle && !CreateXxxWindow()) return;` 按需创建保护。
- 计时埋点本身只在 `TTPLAYER_STARTUP_TRACE=1` 时写日志，正常启动零文件 I/O。

### 皮肤位图解码缓存：实测后改为默认关闭

上一轮加的解码缓存只覆盖 PNG/GDI+ 分支，而默认皮肤 55 张全是 24bpp BMP，
走 `LoadImageW(LR_CREATEDIBSECTION)`，因此缓存文件数一直是 0。本轮把它扩展到
BMP（缓存 DIB 原始行，保留 `biHeight` 符号与 stride，`CreateDIBSection` 原样重建，
不改变色键绘制路径），然后实测：

- 热命中 6 轮 A/B：位图加载中位数 `293.3ms`（关闭）→ `291.0ms`（开启），**只省 2.3ms**；
- 但清空缓存后首次启动要写 74 个文件（1.93 MiB），`skin_bitmap_total_ms` 高达 4326ms。
  根因是「写临时文件 → 删目标 → 重命名」：本机 74 次耗时 1836ms，直接写只要 222ms
  （实时防护把每次 rename 放大约 25ms）。改成直接写最终文件 + 尾部完整性标记、
  magic 升到 `02` 让旧缓存自动失效后，冷启动仍要 1514ms。

因此 `TTPLAYER_SKIN_CACHE` 语义改为**默认关闭，设为 `1` 才启用读+写**。
默认皮肤没有理由为 ~2ms 收益付出 1.2s 冷启动代价；PNG 皮肤很多时才值得显式开启。
`.test/skin_cache_bench.py` 已同步改成显式 `=1` 做冷/热测量。

### 验证结果

`.test/startup_bench.py <改动前> <改动后> 4`（丢弃预热，取中位数）：

| 指标 | 改动前 | 改动后 |
| --- | --- | --- |
| 窗口可交互 | 1142.5 ms | **752.3 ms**（快 390 ms / 34%） |
| 进程 CPU 时间 | 812.5 ms | 500.0 ms |

阶段日志确认 `message_loop_start` 从 ~1469ms 提前到 ~906ms，三个辅助窗在
909–1315ms 的空闲时段补建；`startup_lazy_check.py` 全部断言通过。

回归：`.test/e2e_links.py` / `e2e_batch.py` / `e2e_protocols.py` 与改动前基线
结果逐项一致（批量删除 22→20 行、导入去重、全选、三类音源、右键菜单文案均正常；
`delete_report` / `detail_text` 为空是跨进程输入进不了菜单模态循环的既有自动化限制，
基线同样为空，不是回归）。四个顶层窗的 rect / visible / alpha 在两版本完全一致。

最终产物 sha256 `0fe5985682120851f427f01b259f6084ae11663ded254598135e4ca498cf721a`，
build2 产物 / 部署 `TTPlayer.exe` / 安装包解出的 exe 三方一致；
安装包 `dist/千千静听2026-Setup.exe` sha256
`608e75fc02964244fefbd4de95c369d88c34028b1bb01af1511bbbd47866feba`。
