# 高 DPI 剩余问题分析与实施计划

> 生成时间：2026-09-22
> 依据：`docs/HIGH_DPI_SKIN_SPEC.md` §11.2 / §11.3，加上本次对代码的重新核对（所有结论都给了 `文件:行号` 证据）
> 当前基线：提交 `e71300d`，`ninja -C build2 ttplayer_rebuild` BUILD_EXIT=0，`TTPLAYER_DPI_OVERRIDE=96/144/192` 三档 `--mf-selftest` 均 EXIT=0

---

## 0.1 执行结果（2026-09-22，本计划已执行完毕）

**构建**：`ninja -C build2 ttplayer_rebuild` → **BUILD_EXIT=0，零 error 零 warning**。
**部署**：`TTPlayer/TTPlayer.exe`（3,575,808 字节）。
**验证**：

```
--dpi-selftest : DPI=96  EXIT=0   ok=1  skin_for_dpi 144=483x248 192=644x330
                DPI=144 EXIT=0   ok=1
                DPI=192 EXIT=0   ok=1
--mf-selftest  : DPI=96/144/192 全部 MF_EXIT=0（在线链路未回归）
```

已落地：P0-1（N1 桌面歌词）、P0-2（N3 拓扑变化）、P0-3 + P2-1（策略 C 每窗口独立 DPI）、P1（`--dpi-selftest`）。
决策点最终选 **C 完全独立**（非最初推荐的 A）。

### 执行中修正的一处设计错误（值得记住）

第一版 `--dpi-selftest` 用 `AtDpi(120).AtDpi(96)` 的链式结果去断言「反累乘」，结果 17 条 FAIL —— **但那是断言写错了，不是代码错了**：`AtDpi()` 是「按因子缩放」，`AtDpi(96)` 是恒等变换，链式当然保留 120 的值。
正确的反累乘断言是**走已发货代码路径**：`ApplyDpiScale(192)` → `ApplyDpiScale(96)` → 与起点几何**逐字段相等**。这才真正守住「有人改成从 `skin_` 派生而不是 `logical_skin_`」的回归。已改。

---

## 0. 结论先行

原清单 4 项里，**只有 1 项值得立刻做**，另外 3 项建议分别降级、阻塞、拆分：

| 原项 | 新判定 | 一句话理由 |
|---|---|---|
| 1 每窗口独立 DPI（148 处 `skin_->`） | **降级 + 拆分** | 拆出「补三窗消息处理」（小、立即做）与「真正独立镜像」（大、暂缓） |
| 2 `RenderSkinPreview` 位图级高清 | **不做（won't-fix）** | 5 个辅助函数源/目标尺寸混用，GDI+ 分支无视 DC 映射模式；收益只是一个选项页缩略图的锐度 |
| 3 @Nx 换 DPI 后不重解码 | **阻塞（等生态）** | 现网皮肤包没有任何 @Nx 资源，改完无法验证；等出现 @2x 皮肤再做（约 30 行） |
| 4 真机 150%/200% 目视验收 | **拆成「可自动化」+「必须人工」** | 新增无头几何回归把可量化部分变成断言，只留「是否清晰/是否裁切」给真人 |

同时**新发现 3 个原清单外的问题**（N1~N3），其中 N1 是本轮改造自己引入的真实 bug。

---

## 1. 新发现的问题（原清单外）

### N1 ★ 桌面歌词窗的 `scale_` 会过期 —— 第二轮改造引入的真实 bug

**证据链**

- `desktop_lyrics.cpp:383` `void RefreshScale() { scale_ = ui::ResolveScaleForWindow(control_); }`
- 调用点只有两处：`:308`（`Create()` 里 HWND 建好后）、`:377`（`SetSkin()`）
- `WM_WINDOWPOSCHANGED`（`:853`）只做 `SyncOwnedWindows()`
- `WM_DISPLAYCHANGE` / `WM_SETTINGCHANGE`（`:855-857`）走 `PositionWindows(bounds, true)`，而 `PositionWindows()`（`:1631`）**不刷新 `scale_`**

**后果**：把桌面歌词拖到 DPI 不同的显示器 → `MinWidth()/MaxWidth()`、工具栏按钮矩形（`DefaultBarRect()`）、行高约束全部按旧 DPI 计算。PMv2 下系统**不会**位图拉伸，所以是永久错位，直到换肤或重启才会恢复。

**修法（约 15 行，零风险）**：新增 `WM_DPICHANGED` 分支 → `RefreshScale()` + `PositionWindows(当前矩形, false)` + `ApplyBarSkin()` + `RenderLayered()`；并在 `WM_WINDOWPOSCHANGED` 里加一句兜底：若 `ResolveScaleForWindow(control_).dpi != scale_.dpi` 就刷新。兜底是必要的 —— 拖过屏边界时 `WM_WINDOWPOSCHANGED` 一定到，`WM_DPICHANGED` 的时序不保证早于重绘。

---

### N2 歌词窗 / 列表窗 / 均衡器窗完全没有 DPI 消息处理

**证据链**

- 全仓 `WM_DPICHANGED` 只出现在 `player_window.cpp:2666`（主窗）
- 三窗都是 `WS_POPUP` 的 **owned popup**，即 top-level：`lyrics:1051`、`playlist:2917`（`WS_EX_TOOLWINDOW`）、`equalizer:409`
- PMv2 下 Windows 会给每个 top-level 窗口**单独**发 `WM_DPICHANGED`

**当前后果**：拖到异 DPI 屏 → 无响应 → 保持旧物理尺寸、且不被系统拉伸 → 视觉上「变小或变大」。

**为什么不常触发**：三窗默认位置都是 `player.left + layout.position.left` 锚定主窗（`lyrics:1001`、`playlist:2971`、`equalizer:525`），主窗不动时它们同屏。但列表窗有独立拖拽命中测试（`player_window.cpp:4097`），确实可以被拖走。

**与「每窗口独立 DPI」的关系**：这是同一件事的两面 —— 先有消息处理，才谈得上独立镜像。见 §3.1a 与 §4 的决策点。

---

### N3 主窗的 `WM_DISPLAYCHANGE` / `WM_SETTINGCHANGE` 不重解析 DPI

**证据链**

- `player_window.cpp:2643` `WM_DISPLAYCHANGE`：只在 `fullscreen_mode_ != 0` 时更新全屏布局
- `player_window.cpp:2681` `WM_SETTINGCHANGE`：只在 `SPI_SETWORKAREA` 且全屏时更新
- 两者都没有 `ApplyDpiScale(ui::ResolveScaleForWindow(window_))`

**后果**：拔掉副屏、改显示器主次等拓扑变化，主窗可能落到另一块不同 DPI 的屏上而**只收到 `WM_DISPLAYCHANGE`**（不保证有 `WM_DPICHANGED`）→ scale 不更新。

**修法（约 5 行）**：这两个 case 末尾各补一次 `ApplyDpiScale(ui::ResolveScaleForWindow(window_))`（`ApplyDpiScale` 内部已有 `dpi 相同则直接 return` 的短路，无副作用）。

---

## 2. 原清单 4 项的重新定级（详细理由）

### 2.1 每窗口独立 DPI → 降级 + 拆分

`skin_->` 引用分布（实测）：

| 文件 | 引用数 | 是否 top-level | 现状 |
|---|---|---|---|
| `player_window_lyrics.cpp` | 66 | 是（owned popup） | 共用主窗 `current_scale_` |
| `player_window_playlist.cpp` | 56 | 是（owned popup） | 共用 |
| `player_window_equalizer.cpp` | 26 | 是（owned popup） | 共用 |
| `player_window_skin_animation.cpp` | 7 | 否（画在主窗上） | 共用，正确 |
| `desktop_lyrics.cpp` | 4 | 是 | **已独立**（但见 N1） |

彻底方案是 `SkinForDpi(int)` 缓存（`std::map<int, LegacySkin>`，从 `logical_skin_` 派生）+ 逐窗口分发，把 148 处 `skin_->` 改成 `SkinForWindow(hwnd)->`。

**为什么暂缓**：这三窗的几何**全部由主窗皮肤镜像推导**（位置是 `player.left + skin_->Xxx().position.left`，尺寸是 `skin_->Xxx().background.size`）。若它们用独立 DPI 的镜像，锚定公式会与主窗边框脱节 —— 主窗 100%、列表窗 200% 时，列表窗会被推到离主窗两倍远的地方。要修正就得同时改锚定逻辑，改动量和回归风险都翻倍。而触发条件（把附属窗拖到异 DPI 屏）是边缘用法。

### 2.2 `RenderSkinPreview` 高清 → 不做

- 5 个辅助函数（`DrawPreviewSlider` / `DrawSkinLed` / `DrawScrollingSkinInfo` / `DrawSkinText` 等）把 `bar_size` / `fill_size` / `thumb_size` / `image_size` **同时当源尺寸和目标尺寸**用，必须逐个拆开
- `SkinImage::Draw` 的 GDI+ 分支**无视 DC 映射模式** → 「设个 `MM_ANISOTROPIC` 一键放大」这条路走不通（会变成背景没缩放、控件缩放的错位图）
- 已完成的部分（绘制端 `min(dpi系数, 适配比例)`）已经消除了「高 DPI 下缩略图只占半格」的观感问题，剩下的是纯锐度

**判定**：收益 / 成本比过低，记为 won't-fix。

### 2.3 @Nx 换 DPI 不重解码 → 阻塞

- `ApplyDpiScale`（`player_window.cpp:2186-2200`）会 `ui::SetSessionDpi(scale.dpi)`，但只重新派生镜像（`skin_.emplace(logical_skin_->AtDpi(...))`），**不会重解码位图**
- 要重解码必须在 `PlayerWindow` 留存皮肤源路径（当前**没有**该成员：`LoadSkin(SkinPackage, cache_path)` 只留包对象）
- 而且现网皮肤包**没有任何 @Nx 资源**，`source_scale_` 恒为 1，改完无法验证

**判定**：等出现 @2x/@3x 皮肤再做（约 30 行：加路径成员 + 换档时重载）。当前记为「阻塞（待生态）」。

### 2.4 真机目视验收 → 拆分

「目视」里其实大部分是**可量化的几何不变量**，可以无头断言：

1. **幂等性 / 反累乘**（最有价值）：`AtDpi(144)` 后再 `AtDpi(96)` 必须逐字段等于直接从 96 派生的结果 —— 直接验证本方案的核心风险「二次累乘」
2. **线性**：`WindowSize()` / `MiniWindowSize()` 各轴 ≈ `round(96值 × dpi/96)`，误差 ≤ 1px，且随 dpi 单调不减
3. **矩形合法**：所有段 `bounds` 不反转（`left < right && top < bottom`）
4. **字号**：`lfHeight` 符号保持、`|值|` 随 dpi 单调不减
5. **区域非空**：`CreateWindowRegion()` 返回非 `nullptr`（`CreateFrameRegion` 走 `CreateRectRgn`，不需要真实 HWND）
6. **配置迁移往返**：`MigrateSavedWindowGeometry` 从 96→192 后尺寸/位置 ×2，再 192→96 回到原值

剩下「九宫格边框是否清晰」「文字是否裁切」确实只能真人看 —— 但这已经是小得多的一块。

---

## 3. 执行计划

### P0 —— 两处明确 bug + 附属窗消息（改动量：3 文件，约 40 行）

| # | 内容 | 位置 | 说明 |
|---|---|---|---|
| P0-1 | N1 桌面歌词 DPI 刷新 | `desktop_lyrics.cpp` | 加 `WM_DPICHANGED` 分支 + `WM_WINDOWPOSCHANGED` 兜底 |
| P0-2 | N3 主窗拓扑变化重解析 | `player_window.cpp:2643` / `:2681` | 各补一次 `ApplyDpiScale(ResolveScaleForWindow(window_))` |
| P0-3 | N2 三窗 `WM_DPICHANGED` | `player_window_lyrics/playlist/equalizer.cpp` | 策略见下方决策点 |

**验收**：`ninja` 零 error 零 warning + 三档 `--mf-selftest` EXIT=0。

### P1 —— 无头几何回归（改动量：1 新文件 + 2 处接线，约 200 行）

| # | 内容 | 位置 |
|---|---|---|
| P1-1 | `PlayerWindow::RunDpiSelfTest()` | 新文件 `src/ui/player_window_dpi_selftest.cpp` |
| P1-2 | 命令行 `--dpi-selftest` | `src/app/application.cpp:45` 附近、`include/ttplayer/app/application.h:20` |
| P1-3 | 固化 96/120/144/168/192/240 六档基线 | 文档记录 |

断言项 = §2.4 的 1~6。**其中断言 1（幂等/反累乘）是整套改造最值钱的回归保护。**

### P2 —— 视情况

- P2-1：`SkinForDpi(int)` 缓存 + 148 处改指向（真正独立 DPI）
- P2-2：@Nx 重解码（等出现 @2x 皮肤）

### 不做

- `RenderSkinPreview` 位图级高清（§2.2）

---

## 4. 需要你拍板的一个决策点：附属窗跨屏怎么办

P0-3 有三种策略，行为差异明显：

| 策略 | 行为 | 代价 |
|---|---|---|
| **A 归位**（推荐） | 附属窗落到异 DPI 屏时，按主窗 DPI 重算尺寸并重新锚定到主窗旁 | 跨屏拖动会被弹回主窗所在屏 |
| **B 保持现状**（显式忽略） | 收到 `WM_DPICHANGED` 直接 `return 0`，即维持今天的行为 | 附属窗在异 DPI 屏上尺寸不对（偏小/偏大） |
| **C 完全独立**（= P2-1） | 各窗持自己的皮肤镜像 | 148 处改动 + 锚定公式要同步改，回归风险高 |

推荐 **A**：附属窗本来就是锚定主窗的附属物，「拖到别的屏」不在设计范围内；A 让行为可预期（弹回）而不是静默错位，且改动最小。若你经常把列表窗拖到副屏用，就选 C。

---

## 0.2 现在还剩什么

计划内已全部完成。**剩下的只有 3 项，且都不该现在做**：

1. **`RenderSkinPreview` 位图级高清** —— won't-fix（§2.2）。
2. **@Nx 换 DPI 后重解码** —— 阻塞待生态（§2.3）。等出现含 `@2x` 资源的皮肤包再做，约 30 行。
3. **真机目视验收的「人工部分」** —— 自检已覆盖可量化的几何不变量；只剩「九宫格边框是否清晰」「文字是否裁切」需要真人看。
   另需真机确认「把附属窗拖到异 DPI 屏」的实际观感 —— `--dpi-selftest` 只能证明镜像本身正确，证明不了拖动体验。

## 5. 风险登记（回顾）

| 风险 | 触发 | 缓解 |
|---|---|---|
| P0-1 刷新 `scale_` 后行高/宽度跳变 | 桌面歌词跨屏 | `PositionWindows` 里已 clamp 到 `MinWidth()/MaxWidth()`，且先把矩形存下来再重排 |
| P0-2 重复调用 `ApplyDpiScale` | 拓扑频繁变化 | 函数内 `dpi 相同直接 return`，且换 DPI 会重建区域（有开销但幂等） |
| P0-3 策略 A 与拖拽冲突 | 用户跨屏拖列表窗 | 只在 `WM_DPICHANGED`（真正跨 DPI）时归位，同 DPI 跨屏拖动不受影响 |
| P1 断言过严导致误报 | 皮肤尺寸非整数倍 | 线性断言给 ±1px 容差；幂等断言要求**逐字段相等**（这个是严格可满足的） |

> 实际执行选的是 **策略 C**（不是最初推荐的 A），因此「归位与拖拽冲突」这条风险不存在；换成的是「148 处改指向 + 锚定公式仍读主窗镜像」的回归风险 —— 由 `--dpi-selftest` 的往返断言兜住。
>
> P1 的线性/单调断言都给了 1px 容差，实测默认皮肤（322x165）在 96/120/144/168/192/240 六档全部通过，没有出现误报。
