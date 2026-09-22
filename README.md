# 千千静听 2026

[中文](README.md) | [English](README_en.md)

**把二十年前的千千静听重新实现一遍，再给它接上 MusicFree 的插件化音源。**

本地音乐照放，在线音乐由你导入的音源来搜 —— 皮肤、迷你模式、桌面歌词、均衡器都还是原来的手感，
音源这一层交给插件。

`C++20` · `Win32 / x86` · `QuickJS-NG` · `MIT`

---

## 目录

- [这是什么](#这是什么)
- [为什么是 TTPlayer × MusicFree](#为什么是-ttplayer--musicfree)
- [融合已经做到哪一步](#融合已经做到哪一步)
- [下一步要做的](#下一步要做的)
- [相对经典版的改动](#相对经典版的改动)
- [版权、许可与合规](#版权许可与合规)
- [致谢](#致谢)
- [构建](#构建)
- [关于「这个项目是怎么写出来的」](#关于这个项目是怎么写出来的)
- [已知问题与路线图](#已知问题与路线图)
- [文档索引](#文档索引)

---

## 这是什么

一句话：**用现代 C++ 把千千静听 5.7.9 的整套体验重写，然后把在线音乐这一块交给 MusicFree 的插件体系。**

分三条讲：

- **像原来那样用**：`.skn` 皮肤（含 BMP/PNG 混合的 6.x 皮肤）、迷你模式、桌面歌词、
  十段均衡器与环绕/前置放大、播放链路（waveOut / DirectSound / KS / ASIO）、
  ReplayGain、Winamp DSP、格式转换、媒体库、CUE 音轨、任务栏播放控制、系统媒体控件（SMTC）。
- **多了一块新东西**：皮肤化的**在线音乐窗**。导入一个 MusicFree 音源，就能在窗内搜索、试听、
  直接播放、下载落盘，条目带着歌名/歌手/专辑入列，而不是一串 CDN 地址。
- **干净**：源码仓库里**只有自己写的代码**。原版运行资源一个都不带（见[版权一节](#版权许可与合规)）。

这个仓库最核心的判断是：**千千静听缺的从来不是播放器，是一个还活着的音源。**

---

## 为什么是 TTPlayer × MusicFree

这两样东西的强项刚好互补，拼在一起才是完整的一台播放器。

### 千千静听强在「本地播放器的完成度」

皮肤引擎、歌词系统（滚动/拖动定位/KTV 着色）、音效链、格式覆盖（APE/TAK/VQF/TTA/MPC/MOD/MIDI…）、
格式转换、媒体库、CD 抓轨 —— 这是一套被二十年时间打磨过的桌面软件，很多细节到今天依然没有对手。
迷你模式压缩成一条播放栏退到屏幕边缘这种事，现在的播放器基本不做了。

它真正的短板不是功能，是**服务**：在线歌词服务器、推荐、FreeDB、下载、`qianqian.com` 的在线搜索，
这些早就不在了。本项目**不复刻已经停掉的服务**，也不伪造成功返回，而是把这些边界明确标出来 ——
然后换一条还活着的路。

### MusicFree 强在「插件化音源」

MusicFree 本身**不集成任何平台的任何音源**，搜索、解析、歌单、歌词全部基于插件。
这带来一个很实际的结果：**能不能听，取决于你装了什么插件，而不是播放器签到哪家的独家。**

插件就是满足约定协议的 CommonJS 模块，导出 `search` / `getMediaSource` / `getLyric` 等异步方法。

### 接起来

| | 提供什么 |
| --- | --- |
| **TTPlayer（本项目）** | 播放器本体、皮肤与界面体验、音频链路、歌词与音效、媒体库 |
| **MusicFree（上游协议）** | 音源插件协议、插件生态、导入/管理约定 |

所以这个项目的方向不是「再造一个 MusicFree」，而是**让千千静听重新听见在线音乐**，
同时把「听什么」的决定权交回给用户自己。

---

## 融合已经做到哪一步

- **音源手动导入**：粘贴注册表 JSON（如 `yuanli.json`），或选择一个插件 `.js`。
  插件缓存到 `<exe>/musicfree/plugins/`，导入记录写进 `imported.json`，下次启动自动加载。
  **音源不内置，必须由你手动导入** —— 和 MusicFree 的约定完全一致。
- **在线音乐窗**：`查看 → 音乐窗`（`F11`，或主窗口的音乐窗按钮），开关语义。
  外框走皮肤包的 `<browser_window>` 段自绘，复用主窗口的拖动吸附与位置记忆（十像素磁吸 + 工作区约束）。
- **窗口状态持久化**：音乐窗的**开关与位置一起记住**（`Player/BrowserVisible` + `BrowserWnd`，
  随皮肤侧车与 `TTPlayerRebuild.xml` 双写）。上次退出时开着，下次启动自动开；用窗自己的关闭按钮关掉也能正确记住。
  旧配置里没这个属性时按「关闭」处理，不会凭空弹出。
- **搜索 / 解析 / 播放 / 入列**：在线条目带着音源的标题、歌手、专辑入列。
  双路播放：能在本地落盘的走本地，不能的走流式 HTTP 解码。
- **下载到磁盘**：「下载到…」（右键）或底部「下载」。先解析地址 → 弹保存对话框 → 后台拉字节写盘，
  路径按宽字符处理，中文目录不乱码。
- **「在线搜索」不再跳 qianqian.com**：播放列表的「在线搜索」改为按「歌手 - 歌名」组词，
  直接在音乐窗里检索，不再打开外部浏览器。
- **沙箱与网络**：插件在内嵌 **QuickJS-NG** 里跑，`axios` / `fetch` 由 **WinHTTP** 提供真实网络能力。
  代码只消费你提供的 URL 与文件，不自己去找音源。
- **无头自检**：整条 `SearchAllSources → ResolvePlayUrl → AddOnlineTrack` 链路可以不开界面跑，
  逐行核对日志而不用截图（见 [构建 → 自检](#自检与日志)）。

完整架构与分阶段验证记录：[`docs/MusicFree_Compatibility.md`](docs/MusicFree_Compatibility.md)。

---

## 下一步要做的

融合还没结束。按优先级：

- [ ] **歌单导入**：接上 MusicFree 协议的 `importSheet`、专辑详情、歌手详情，把「导入一个歌单」变成一次点击
- [ ] **歌词交给插件**：用插件的 `getLyric` 取词，和本地 LRC、现有歌词服务器合成一条有优先级的歌词链
- [ ] **多音源并发聚合**：同时问多个音源，结果合并去重、按音源分组，而不是串行等第一个
- [ ] **音乐窗交互补齐**：排序、多选、批量入列/批量下载、结果内二次过滤
- [ ] **音源可用性探测**：导入时校验插件能否 `mount`，不能用的直接标出来而不是等搜索时才失败
- [ ] **高 DPI**：目前受限于图片皮肤，已做阶段 0~3，遗留项见 [`docs/HIGH_DPI_REMAINING_PLAN.md`](docs/HIGH_DPI_REMAINING_PLAN.md)
- [ ] 控件鼠标指针样式统一

---

## 相对经典版的改动

除了上面的融合，本版在原有功能上也做了这些（完整逐项见 [`docs/MAJOR_FEATURE_RECOVERY.md`](docs/MAJOR_FEATURE_RECOVERY.md)）：

**界面与皮肤**

1. 支持 6.x 的 BMP/PNG 混合皮肤、逐像素透明、按钮动画，同时保留经典 BMP 皮肤的显示方式
2. 皮肤包加载失败时退回原生深色窗口兜底，不再直接崩
3. 全屏模式新增专辑封面显示（8.x 曾有，后消失）
4. 桌面歌词新增字号快捷调整（皮肤提供按钮时）

**播放与集成**

5. 任务栏缩略图三键（上一曲/播放暂停/下一曲），带实时状态，**不会把最小化的窗口还原出来**
6. 支持 Discord 音乐状态同步（可同步当前歌词）
7. 系统媒体控件（SMTC）：系统媒体面板显示曲目、内嵌封面、进度，支持播放/暂停/停止/切歌/跳转 —— 见 [`docs/SMTC.md`](docs/SMTC.md)
8. 现代 Windows 文件选择窗口（添加音乐、选文件夹、找歌词文件），保留多选与类型筛选
9. 增强在线歌词服务：可添加/编辑/排序多个自定义歌词服务器，支持 HTTPS、证书校验与代理

**稳定性**

10. 插件故障隔离：文件信息/标签处理、DSP 扫描配置、输出设备探测放进独立工作进程，带超时与取消
11. 配置文件独立为 `TTPlayerRebuild.xml`，首次运行可导入原版 `TTPlayer.xml` 而不覆盖原文件

**其他**

12. 增强全屏歌词交互：可拖拽歌词或空白处定位进度，支持水平/垂直滚动
13. 多显示器全屏：可选屏幕、可切换、切到别的应用仍保持全屏
14. 「相关链接」指向本项目与上游 MusicFree（`Sonic853/TTPlayer` / `maotoumao/MusicFree`），
    不再使用原版的「Github仓库 / 提交反馈」；标签变长后选项窗底部会自动改竖排，不会盖住按钮

---

## 版权、许可与合规

这一节请务必看完再分发。本项目对版权的态度分三层，逐条说清楚。

### 1. 源码仓库里没有原版可执行文件与运行库

仓库只包含**自行编写的 C++ 源码、CMake 脚本与文档**。原版千千静听的运行资源一律通过 `.gitignore` 排除，
不随源码分发：

| 类别 | 排除项 |
| --- | --- |
| 运行库 DLL | `ttpcomm.dll` `ttpres.dll` `mp3PRO.dll` `msdmo.dll` `tak_deco_lib.dll` `WMADMOD.DLL` |
| 主程序与资源 | `TTPlayer.exe` `TTPlayer.rll` `TTPlayerRebuild.rll` `TTPlayerSet.exe` `uninstall.exe` `卸载.exe` `FAQ.txt` |
| 目录 | `AddIn/` `Icons/` `Lyrics/` `PlayList/` `RealCodec/` `Skin/*.xml` |
| 反汇编工作区 | `/reverse`（Ghidra 工程与反编译产物） |

也就是说，**clone 下来看到的东西基本都是这个项目自己写的**。想核对可以直接翻 `.gitignore`，逐条都能对上。

**唯一的例外是三个图标**：`src/app/assets/{TTPlayer,AudioFile,PlaylistFile}.ico`（程序图标与文件关联图标）
随源码入库，这样本地构建出来的 EXE 才带图标。它们取自原版程序，**版权归原作者**，仅作识别用途；
如果你打算再分发这份源码，建议替换成自己的图标。

### 2. 运行需要一份你自己的原版运行目录

重建版**不能单独运行**：它按原版的行为与 ABI 边界工作，运行时需要 `TTPlayer.exe` 同级存在
`ttpcomm.dll`、`ttpres.dll`、`AddIn`、`Skin`。

**这些请从你自己合法获得的千千静听 5.7.9 安装中取**，本项目不代替你分发原版二进制。
做法是把编译出的 `TTPlayer.exe` 放进那个目录即可，不必覆盖任何原文件。

### 3. 音源、插件、内容都不内置

和 MusicFree 的约定一致：

- **不内置任何音源或插件**。音源由用户自己导入，或者自己写。第三方插件的安全性与合法性请自行判断，
  不要安装来路不明的东西。
- **不内置任何歌词、专辑封面、音乐文件**。
- **不做任何破解**：不绕过 VIP、不解除试听限制、不提供付费歌曲的获取能力。
  这个项目只是把「用户已经能访问的地址」交给解码器。
- 第三方插件产生的数据与本项目无关；**可能涉及版权的内容请及时删除**，
  请仅在你所在地区合法的范围内使用。

### 名称与商标

「千千静听」（英文 Thousand Tunes，曾用名 MP3随身听 / 芊芊静听）是原作者 **郑南岭（nanling）** 于 2002 年
独立开发的软件名称，2006 年 7 月被百度收购，2013 年 7 月更名为「百度音乐」，2018 年后「千千音乐」
作为新品牌启用，与原软件已无直接传承关系。

**本项目与郑南岭、百度、太合音乐集团均无任何隶属、授权或合作关系。** 使用这个名字只是为了说明
它在复刻什么，不代表官方版本，也不应被理解为官方产品。

### 关于「重建」这件事

本项目按**可观察行为**（窗口类名、控件 ID、资源 ID、消息顺序、文件格式、设置项）重新实现，
而不是把反编译产物直接丢给编译器。原版界面、皮肤、图标、字体等**美术与资源版权仍归原作者**，
随程序使用的皮肤资源请注意来源。

### 许可

本项目源码沿用上游 `Sonic853/TTPlayer` 的 **MIT** 协议，见 [`LICENSE`](LICENSE)（Copyright © 2026 Sonic853）。
使用、修改、分发请保留原始版权声明。

第三方组件各自许可：

| 组件 | 许可 | 位置 |
| --- | --- | --- |
| QuickJS-NG（内嵌 JS 引擎） | MIT | `third_party/quickjs-0.9.0/LICENSE` |
| WTL（窗口模板库） | Microsoft Public License | `docs/licenses/WTL-MS-PL.txt` |
| YY-Thunks（仅 XP/Win7 兼容版） | MIT | `docs/licenses/` |
| VC-LTL（仅 XP/Win7 兼容版） | Eclipse Public License 2.0 | `docs/licenses/VC-LTL-LICENSE.txt` |

---

## 致谢

### 谢谢郑南岭

**千千静听的原作者郑南岭（网名 nanling）**，2002 年一个人把它写出来，名字取自陈慧娴的《千千阙歌》。

它免费、无广告、占资源极小；皮肤能换，歌词能拖，音效够用，格式几乎全吃。在流量还很贵的年代，
它是很多人电脑上唯一的音乐软件。它后来被收购、改名、消失，但那个「做得刚刚好」的桌面播放器
一直没有真正的替代品。

**这个项目想做的事很简单：让它在 2026 年的 Windows 上还能打开。**

### 谢谢上游项目

| 项目 | 说明 | 链接 |
| --- | --- | --- |
| **Sonic853/TTPlayer** | 千千静听社区复刻版（Thousand Tunes Player）。**本仓库的源码基础与 MIT 许可来源** | https://github.com/Sonic853/TTPlayer |
| **maotoumao/MusicFree** | 插件化、定制化、无广告的免费音乐播放器。本项目音源兼容层复刻的就是它的插件协议 | https://github.com/maotoumao/MusicFree |
| **maotoumao/MusicFreeDesktop** | MusicFree 桌面版，音源导入交互的参考 | https://github.com/maotoumao/MusicFreeDesktop |

### 也谢谢这些库的作者

- **QuickJS / QuickJS-NG** — Fabrice Bellard、Charlie Gordon、Ben Noordhuis、Saúl Ibarra Corretgé
  → https://github.com/quickjs-ng/quickjs
- **WTL** — Windows Template Library，Microsoft Public License
- **YY-Thunks / VC-LTL** — Chuyu-Team → https://github.com/Chuyu-Team

---

## 构建

> **Windows XP / Windows 7 用户请用 `TTPlayerRebuild-XP-Win7.zip` 里的 EXE。**
> 默认现代版由新版 MSVC 构建，不适用于这些系统；旧系统版的构建与功能范围见
> [`docs/LEGACY_WINDOWS.md`](docs/LEGACY_WINDOWS.md)。

### 前置条件

| 项 | 要求 |
| --- | --- |
| 工具链 | Visual Studio 2026「使用 C++ 的桌面开发」组件 + CMake 4.2+ |
| 平台 | **必须 Win32 / x86，不能选 x64** —— 所有旧版 AddIn 与 `ttpcomm.dll` 都是 x86 |
| 运行资源 | 一份你自己合法获得的原版运行目录（`ttpcomm.dll` / `ttpres.dll` / `AddIn` / `Skin`） |

### 一条命令构建播放器

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build build --config Release --target ttplayer_rebuild --parallel 4
```

产物：

```
build/Release/TTPlayerRebuild.exe
```

这组命令**只构建播放器**，不构建测试、不复制原版运行资源。运行时把 EXE 放进装有原版运行资源的目录即可。

### 日常增量构建（本机 Ninja 树）

调试期间改代码用 Ninja 路径更快：

```powershell
ninja -C build2 ttplayer_rebuild
```

产物是 `build2/TTPlayerRebuild.exe`，复制成播放器目录里的 `TTPlayer.exe` 即可就地验证。
`build2/` 已在 `.gitignore` 中排除。

### 完整还原工作区（含测试与资源 DLL）

```powershell
cmake -S rebuild -B rebuild/build -A Win32
cmake --build rebuild/build --config Debug
ctest --test-dir rebuild/build -C Debug --output-on-failure
```

还原出的资源 DLL 输出在：

```
rebuild/build/resources/root/ttpres.dll
rebuild/build/resources/AddIn/ttpres.dll
```

### 启动

运行时文件一律**相对可执行文件**解析，与当前工作目录无关：

```powershell
.\rebuild\build\Debug\TTPlayerRebuild.exe
```

也可以直接把音频或播放列表路径当参数传进去。程序会先绘制原版 327x141 默认皮肤；
只有在皮肤包加载失败时才退回原生深色窗口兜底。

### 自检与日志

音乐窗链路可以在**无界面**模式下自检，结果写进 `%TEMP%\ttplayer_mf_http.log`，
逐行核对而不依赖截图：

```powershell
$env:TTPLAYER_MF_TRACE = "1"
$env:TTPLAYER_MF_SELFTEST_DOWNLOAD = "1"   # 可选：连下载链路一起验（会拉取数 MB）
.\TTPlayer.exe --mf-selftest="告白气球"
```

日志里会出现这几段：`BROWSER_SKIN`（音乐窗外框皮肤段）、`MF_SELFTEST`（起止与是否入列）、
`MF_SELFTEST_SEARCH`（各音源命中与错误）、`MF_SELFTEST_RESOLVE`（解析结果）、
`MF_SELFTEST_DOWNLOAD`（落盘字节数）、`ONLINE_TRACK_STORED`（从播放列表读回的字段）。

---

## 关于「这个项目是怎么写出来的」

这个项目的整合工作是 **WorkBuddy + DeepSeek V4.1 Flash** 一起完成的 ——
从读原版行为、推边界、写 C++，到编译、打包、跑端到端验证、写文档，都是这个组合在推进。

具体分工大致是：

- **WorkBuddy** 负责工程编排：改代码、编译、装包、**启动真实程序读回真实窗口文本做验证**、清理测试环境、
  维护文档。它手上有本地文件系统和终端，所以「改完立刻编译 → 装一遍 → 跑起来 → 读 UI → 回归」这条链是闭环的。
- **DeepSeek V4.1 Flash** 负责代码与推理：从反汇编片段与可观察行为倒推接口边界、写 C++20 实现、
  排查皮肤绘制 / 布局几何 / 音频链路 / 编码转换这类具体的坑。

一条硬规则是：**不靠截图猜**。每处改动都要过「能编译 → 能跑起来 → 从运行中的进程读回真实状态」三步。
比如验证「相关链接」这一处改动时，做法是启动装好的程序、按窗口类名找到主窗、用应用自己的消息钩子
打开选项窗、枚举所有子窗口读 `GetWindowTextW` 与矩形坐标 —— 结果发现新标签把「全部保存」按钮盖住了 42px
（链接静态控件后创建，z 序在上，那截按钮既看不见也点不到）。这个问题在截图上是看不出来的，
只有读坐标才会暴露。

写下来的那些逐地址还原记录（`docs/` 下有 80 多份）也是这个流程的产物。

---

## 已知问题与路线图

- 鼠标在控件上的指针样式尚未统一
- 音乐窗已接入 MusicFree 音源，交互细节待继续完善（见[下一步](#下一步要做的)）
- 高 DPI 受限于图片皮肤，阶段 0~3 已完成，遗留项见 [`docs/HIGH_DPI_REMAINING_PLAN.md`](docs/HIGH_DPI_REMAINING_PLAN.md)
- XP / Win7 兼容版的验证记录见 [`docs/LEGACY_WINDOWS_VALIDATION.md`](docs/LEGACY_WINDOWS_VALIDATION.md)

**待验证的边界**（明确保留边界，而不是伪造成功）：

1. 在兼容的物理硬件上验证 CD-DA、KS 与 ASIO；
2. 把真实样本验证扩展到 TTA / MPC / Real / MOD 及不常见的多声道、有效位布局；
3. 恢复歌词提供者的 request/result/cancel ABI，或用一个明确声明「非原版」的提供者中立客户端
   替换已停用的服务；
4. 对已停用的 MSN / 更新 / FreeDB / 推荐 / 缓存 / 下载等服务保留明确边界；
5. 旧的私有 WTL 命令条 / 按钮 / CBT 包装对象布局无法仅凭伪 C 推断，改以自有的 Win32 实现
   保留其可观察职责。

---

## 文档索引

`docs/` 下按主题分类，常用入口：

| 文档 | 内容 |
| --- | --- |
| [`MusicFree_Compatibility.md`](docs/MusicFree_Compatibility.md) | **MusicFree 音源兼容层与在线音乐窗（融合主线）** |
| [`MAJOR_FEATURE_RECOVERY.md`](docs/MAJOR_FEATURE_RECOVERY.md) | 逐地址的总体还原进度 |
| [`STATUS.md`](docs/STATUS.md) | 当前状态总表 |
| [`BUILDING.md`](docs/BUILDING.md) | 构建输入、产物与干净检出构建 |
| [`AUDIO_ENGINE.md`](docs/AUDIO_ENGINE.md) / [`AUDIO_RECOVERY.md`](docs/AUDIO_RECOVERY.md) | 音频引擎与还原证据 |
| [`LYRIC_DISPLAY.md`](docs/LYRIC_DISPLAY.md) / [`LYRIC_WINDOW_RECOVERY.md`](docs/LYRIC_WINDOW_RECOVERY.md) | 歌词绘制与歌词窗还原 |
| [`SKIN_PNG_RECOVERY.md`](docs/SKIN_PNG_RECOVERY.md) / [`SKIN_ALIGNMENT.md`](docs/SKIN_ALIGNMENT.md) | PNG 皮肤与皮肤对齐 |
| [`EQUALIZER_RECOVERY.md`](docs/EQUALIZER_RECOVERY.md) | 均衡器还原 |
| [`FULLSCREEN_RECOVERY.md`](docs/FULLSCREEN_RECOVERY.md) | 全屏状态机 |
| [`SMTC.md`](docs/SMTC.md) / [`TASKBAR_PLAYBACK.md`](docs/TASKBAR_PLAYBACK.md) / [`DISCORD_PRESENCE.md`](docs/DISCORD_PRESENCE.md) | 系统集成 |
| [`HIGH_DPI_REMAINING_PLAN.md`](docs/HIGH_DPI_REMAINING_PLAN.md) | 高 DPI 遗留项 |
| [`LEGACY_WINDOWS.md`](docs/LEGACY_WINDOWS.md) | XP / Win7 兼容版 |
| [`licenses/`](docs/licenses/) | 第三方许可全文 |

### 源码布局（UI 部分）

`PlayerWindow` 仍是协调者类，以保证 HWND 归属、消息顺序与私有状态保持 ABI 中立；
每个原生窗口有自己的实现单元：

- `src/ui/player_window.cpp` —— 主播放器、换肤、共享菜单、持久化与播放协调
- `src/ui/player_window_lyrics.cpp` —— `TTPlayer_LyricWnd` 与 `LyricCtrl`
- `src/ui/player_window_playlist.cpp` —— `TTPlayer_PlayListWnd`、列表控件与提示/编辑
- `src/ui/player_window_equalizer.cpp` —— `TTPlayer_EqualizerWnd`、`SkinButton` 与 `SkinSlider`
- `src/ui/player_window_visual.cpp` —— `VisualCtrl`、PCM 驱动的梦幻/频谱/示波渲染、专辑封面与全屏状态机
- `src/ui/player_window_visual_options.cpp` —— 可视化设置命令与即时刷新
- `src/ui/player_window_internal.h` —— 私有命令 ID 与共享声明，**不属于公开 API**
- `src/musicfree/` —— **MusicFree 音源兼容层与在线音乐窗**
  （`musicfree_source_manager` / `musicfree_plugin` / `musicfree_http` / `musicfree_browser_window` /
  `musicfree_import_dialog` / `quickjs_engine`），共用外观在
  `include/ttplayer/musicfree/musicfree_theme.h`

---

## 许可

MIT，见 [`LICENSE`](LICENSE)（Copyright © 2026 Sonic853）。第三方组件许可见 [`docs/licenses/`](docs/licenses/)。
