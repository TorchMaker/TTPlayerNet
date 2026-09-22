# 高 DPI 适配规格说明书（基于现有皮肤文件体系）

- 编写日期：2026-09-21（v2 修订：自查纠错 + 补遗，见 §0）
- 源码基线：`69f2c55`（双路播放已合入）
- 前置文档：`docs/HIGH_DPI_ASSESSMENT.md`（2026-09-16 现状评估，结论与本说明书一致）
- 目标读者：实现高 DPI 适配的开发者

本说明书回答一个问题：**在不重写皮肤系统、不丢弃任何既有皮肤 XML 的前提下，如何让 TTPlayer 在 125%~250% 缩放、以及混合 DPI 多显示器下，真正做到界面清晰、点击区域与画面一致、跨屏移动即时重绘。**

---

## 0. v2 修订记录（自查结果）

对 v1 逐条对照源码复核，发现并修正的问题：

| # | 类型 | 问题 | 修正 |
|---|---|---|---|
| 1 | **错误** | v1 §6.11 的 `CreateWindowRegion` 委托草图引用了 `resize_rect`/`tile`，但 `LegacySkin` **没有这两个成员**——主窗口背景是固定尺寸 `SkinImage`（非九宫格 `SkinBitmap`），`resize_rect` 只存在于 `PlaylistSkin`/`LyricSkin`/音乐窗 | §6.11 重写：主窗区域按「空 resize_rect 退化分支」复用 `CreateFrameRegion` 整幅缩放路径；并补齐全部 6 处 `SetWindowRgn` 调用点 |
| 2 | **错误** | v1 §7 @2x 资源方案与「源像素=逻辑像素」原则**直接冲突**：@2x 位图的源像素是 XML 逻辑像素的 2 倍，`resize_rect`（逻辑值）不再等于源像素，九宫格源分割若不乘资源倍率 N，切图必然错位 | §4/§7 引入**资源倍率 N（source_scale）**维度：源矩形 = 逻辑 × N，目标矩形 = 逻辑 × dpi/96，二者彻底解耦 |
| 3 | 错误 | v1 `DpiScale::FontPoints` 命名误导——XML `font_size` 是像素高度（直接赋 `lfHeight`），不是 points | 改名 `FontPx`，语义 = 缩放后的像素字高 |
| 4 | 细节不足 | v1 §6.7 写死 `-scale.Y(13)`，忽略 `font_height` 为**负值**（em 高度）时须保号 | §6.7 给出保号公式 |
| 5 | 细节不足 | v1 对平铺（tile）中心区只说「缩放目标分割」，漏了 `DrawBitmapPatch` 内部每块 tile 的目标尺寸也须按 scale 放大，否则 200% 下中心区仍是 1× 小块平铺 | §6.4 明确 tile 尺寸规则 |
| 6 | 遗漏 | `musicfree_frame.h` 自成一套几何常量（`kResizeBand=6`、natural=位图尺寸、close/backward/content 等 RECT）与 `desktop_lyrics.cpp:1099` 的 `SIZE{231,25}`、`kMinimumWidth`、`font_height*3/2*lines` 行距——v1 只带过一句 | §6.10 扩为具名清单 |
| 7 | 遗漏 | 菜单字体取自系统级 `SPI_GETNONCLIENTMETRICS`（`player_window.cpp:472`），PMv2 下应为**每窗口 DPI** 的 `SystemParametersInfoForDpi`（动态解析），否则自绘菜单字体错档 | §6.7 补 |
| 8 | 遗漏 | `WM_DPICHANGED` 处理流程在 v1 只有一句话（任务描述级别），无落地细节 | 新增 §6.12 完整流程 |
| 9 | 遗漏 | `optional_windows_api` 未落实路径；Win8.1 的 `GetDpiForMonitor` 回退链没写；`gdiScaling` 未明确「不启用」 | §9 补 |
| 10 | 遗漏 | 均衡器十频段按 `模板宽+eq_interval` 步进克隆（`skin.cpp:1219-1224` `OffsetRect`），`eq_interval` 的缩放归属没写 | §6.6 补 |
| 11 | 遗漏 | 图标链路：`player_window.cpp:1471-1507` 三组 RT_TOOLBAR/RT_BITMAP 拼 16×16 ImageList、`DrawIconEx(...,16,16)`、`taskbar_preview.cpp` 缩略图、`RenderSkinPreview` 皮肤预览 | §6.9 审计清单具名 |
| 12 | 遗漏 | 无高 DPI 屏的开发机无法验证 | §6.13 新增 `TTPLAYER_DPI_OVERRIDE` 测试开关 |

---

## 1. 目标与范围

| 目标 | 是否在本说明书范围 |
|---|---|
| 在 2K/4K 桌面启动并使用播放器 | 是（架构已无障碍，见评估文档 §1） |
| 125%~250% 缩放下界面尺寸合适、文字清晰 | **是（核心目标）** |
| 跨不同缩放比显示器移动时即时清晰重绘 | **是（核心目标）** |
| 旧皮肤（原版像素风 BMP/PNG）继续可用 | 是（像素风缩放保留，不强制高清化） |
| 把歌词/列表换成另一套 UI 框架 | 否（评估文档 §5 第四阶段明确不做） |
| 改播放引擎 / 音频管线 | 否 |

保留架构：**Win32 + WTL + x86 + Media Foundation + 现有 LegacySkin XML 解析**。只做坐标与栅格化层的统一改造。

---

## 2. 已核实现状（带证据，避免空泛）

所有行号对应基线 `69f2c55`。

### 2.1 进程处于 DPI-Unaware
- `docs/HIGH_DPI_ASSESSMENT.md` §2.2 实测：`GetProcessDpiAwareness = 0 (PROCESS_DPI_UNAWARE)`，各窗口 `GetDpiForWindow` 返回 96。
- 清单 `src/app/ttplayer.manifest` 只有 Common-Controls v6 + `requestedExecutionLevel`，**无 `dpiAware` / `dpiAwareness` / `gdiScaling`**。
- 入口 `src/app/main.cpp:137`（`wWinMain`）仅调 `SetProcessDEPPolicy`，**无任何 `SetProcessDpiAwareness*`**。
- `CMakeLists.txt` **未定义 `_WTL_DPI_SUPPORT`** → `build2/_deps/.../atlframe.h:2989` 的 `_DPI(n)` 宏、`OnDpiChanged` 编译为空操作。

### 2.2 皮肤像素直接决定窗口与控件（核心矛盾）
- `src/skin/skin.cpp:827`：`skin.window_size_ = { background_info.bmWidth, background_info.bmHeight }` —— 窗口尺寸 = 背景位图尺寸。
- `src/skin/skin.cpp:848`：`element.bounds = ParseRect(Attribute(node, L"position"))` —— 控件矩形直接来自 XML 像素坐标。
- `src/skin/skin.cpp:854`：`element.font_size = ParseInt(Attribute(node, L"font_size"), …)`；`player_window.cpp:887` `CreateSkinFont` 直接 `descriptor.lfHeight = element.font_size`（如 12 即 12 物理像素高，**是像素高度不是 points**）。
- `src/skin/skin.cpp:42` `ParseRect` 是朴素 `%ld , %ld , %ld , %ld` —— 坐标即 96-DPI 逻辑像素，全程无换算。
- 主窗口四处用 `skin_->WindowSize()` 决定 HWND 尺寸：`player_window.cpp:2346 / 3838 / 4279 / 5274`。

### 2.3 九宫格把同一 rect 同时当源与目标（改造关键点）
- `src/skin/skin.cpp:1520` `DrawFrameBackground`：
  ```
  source_x[]      = {0, resize_rect.left,  resize_rect.right, bitmap.size.cx};  // 源像素
  destination_x[] = {0, resize_rect.left,  destination_right,  width};          // 目标像素
  ```
  `resize_rect.left/top` 被**同时**用作源分割与目标分割。因此 `resize_rect` 在数值上等于「逻辑像素 = 源像素」（96 DPI 下、@1x 资源下两者重合）。一旦要缩放，源分割必须保持源像素、目标分割必须按 DPI 放大 —— **不能整体缩放 resize_rect**。
- `src/skin/skin.cpp:1558` `CreateFrameRegion` 复用完全相同映射生成透明窗口区域（必须和背景同一套九宫格，否则边框错位）。
- **注意：九宫格只用于可拉伸窗（列表/歌词/音乐窗）。主窗口背景是固定尺寸 `SkinImage`，`LegacySkin` 没有 `resize_rect` 成员**——主窗的 DPI 区域生成走另一条路（§6.11）。

### 2.4 多帧按钮目标尺寸未缩放（隐藏 bug）
- `src/skin/skin.cpp:1590` `DrawFrameButton`：
  ```
  destination_width  = frames > 1 ? frame_width            : bounds.right - bounds.left;
  destination_height = frames > 1 ? element.image_size.cy  : bounds.bottom - bounds.top;
  ```
  多帧按钮（play/pause/prev/next/音量等四态图）目标宽高取自**源像素** `frame_width`/`image_size.cy`，**完全忽略 XML 矩形与缩放**。单帧按钮才用 `bounds`。所以在任何缩放下，多帧按钮都会是 1× 大小 —— 这是必须在 DPI 改造中一并修掉的点。

### 2.5 不规则窗口区域不缩放
- `src/skin/skin.cpp:1404` `CreateWindowRegion`：对主窗/迷你窗背景**逐像素 1:1 扫描**生成区域（与背景位图同分辨率），**无任何缩放**。缩放后窗口变大、区域仍是原尺寸，会导致点击穿透/边缘错误。
- 全部 `SetWindowRgn` 调用点（改造时都要传 scale）：
  - 主窗：`player_window.cpp:2354`（重播/换肤）、`:4231/:4308`（迷你切换）、`:5236/:5282`（换肤动画）、`:2000`（预览）
  - 列表窗：`player_window_playlist.cpp:3000`
  - 歌词窗：`player_window_lyrics.cpp:1503`
  - 均衡器：`player_window_equalizer.cpp:548`
  - 音乐窗：`musicfree_browser_window.cpp:453`

### 2.6 列表行高硬编码
- `player_window.cpp:786` `MakePlaylistGeometry`：`result.row_height = 16;`（硬编码）。
- `:779` `extra_width = width - layout.background.size.cx` 把「逻辑布局尺寸」与「物理窗口尺寸」混用。

### 2.7 栅格化保持像素风
- `src/skin/skin_image.cpp:159` `SkinImage::Draw`：PNG 走 GDI+ **最近邻 + 半像素偏移**（保留原版像素）；BMP 走 `StretchBlt`/`TransparentBlt`。**无按目标 DPI 选 1×/2×/3× 资源的机制**。
- 结论（评估文档 §3.6）：旧皮肤放大只是更大像素；高清皮肤可与旧皮肤共存，不必改所有 XML。

### 2.8 配置里没有 DPI 维度
- `include/ttplayer/settings/settings.h:219`：`PlaylistSettings::font_height{-13}`（**负值 = em 高度**），其余颜色/字体均为固定值，**无 scale/dpi 字段**。保存的窗口矩形、LOGFONT 若再次按新 DPI 相乘会重复累乘（评估文档 §4 风险 4）。

### 2.9 皮肤体系之外的第二套几何（v2 新增核查）
皮肤 XML 并非唯一布局来源，以下常量同样是「96-DPI 逻辑像素」，改造时**必须一并纳入**：

| 位置 | 常量 | 含义 |
|---|---|---|
| `include/ttplayer/musicfree/musicfree_frame.h:122` | `kResizeBand = 6` | 音乐窗拉伸命中带宽 |
| `musicfree_frame.h:34-43` | `natural` / `resize_rect` / `content` / `close`/`backward`/`forward`/`refresh`/`check_box`/`check_label`/`link` | 音乐窗整套几何（源自 `BrowserSkin`） |
| `src/ui/desktop_lyrics.cpp:1099` | `SIZE{231, 25}` | 桌面歌词条默认尺寸（无皮肤时的兜底） |
| `desktop_lyrics.cpp:413-415` | `font_height * 3 / 2 * settings_->lines` | 桌面歌词行距（派生自字高，字高缩放后自动一致） |
| `desktop_lyrics.cpp:290` | `kMinimumWidth` | 桌面歌词最小宽度 |
| `player_window.cpp:3460` | `client.right - playlist_width - 24, 16` | 主窗绘制里的魔法间距 |
| `player_window.cpp:1416/1791/1824` | `DrawIconEx(..., 16, 16)` | 图标绘制尺寸 |
| `player_window.cpp:1471-1507` | 三组 RT_TOOLBAR/RT_BITMAP → 16×16 ImageList | 工具栏图标链路 |
| `player_window.cpp:472-483` | `SPI_GETNONCLIENTMETRICS` → `lfMenuFont` | **系统级**（非每窗口）菜单字体 |
| `player_window.cpp:437-459` | `16*16` PopupMenuImageMask | 菜单图标掩码 |
| `src/ui/player_window_options.cpp:1302` | `InstallButtonBitmap` 16×15 ImageList | 选项对话框按钮图 |
| `wtl_menu.cpp` | 自绘菜单项高度/间距 | 菜单几何 |

### 2.10 平台 API 桥（已有，直接复用）
- `include/ttplayer/platform/optional_windows_api.h`（`src/platform/optional_windows_api.cpp`）：工程既有动态解析机制，已被 12 处使用（audio/ui/platform）。DPI 所需的全部较新 API 都经它接入，**不得引入新的静态导入**。

---

## 3. 设计原则

1. **皮肤数据保持 96-DPI 逻辑坐标，永不就地放大。** 解析结果原样保留；物理像素在「消费边界」由统一换算得出。这样改 DPI 只是换一个换算因子，绝不会把已放大的值再乘一次。
2. **源像素与目标像素严格分离。** 位图裁剪坐标（九宫格源分割、帧偏移）永远是**源像素**（= 逻辑 × 资源倍率 N）；目标矩形永远是**物理像素**（= 逻辑 × dpi/96）。二者不得在同一个值里混用（正是 `resize_rect` 的现状坑）。
3. **单一换算点 + 镜像视图。** 不改动数百处 `element.bounds` 绘制代码：先产出一个「已换算成当前 DPI 物理像素」的皮肤镜像，旧绘制代码读镜像即可；只有真正需要源尺寸的栅格化函数额外收一个 `scale`。
4. **声明感知必须建立在布局已实现之后。** 先让程序自己会按 DPI 重排，再开 `PerMonitorV2`；否则系统一停手放大，界面会过小/错位（评估文档 §4）。
5. **旧系统不丢能力。** 所有新 API 经 `optional_windows_api` 动态解析，回退到系统 DPI 感知；XP/Win7 导入检查继续生效（参考 `docs/LEGACY_WINDOWS.md`）。
6. **（v2）资源倍率与 DPI 倍率解耦。** @Nx 高清资源只影响「源像素」，不影响任何布局/目标换算；无 @Nx 的旧皮肤 N=1，行为与阶段2 完全一致。

---

## 4. 坐标模型（v2：加入资源倍率维度）

```
逻辑坐标（Logical）  : 皮肤 XML 里的像素，基准 96 DPI。皮肤对象里永远是它。
源像素（Source）     : 解码后位图的实际像素 = 逻辑 × N（N = 资源倍率，@1x 时 N=1）。
物理像素（Physical） : 当前显示器上的真实窗口像素 = 逻辑 × dpi / 96。

scale = DpiScale{ dpi };          // 只管 逻辑 → 物理
source_scale = N;                 // 只管 逻辑 → 源（SkinImage::source_scale_，见 §7）

物理 = scale.X/Y(逻辑)          // MulDiv 饱和舍入
源   = 逻辑 × N                  // 整数倍，无损
```

基准 DPI = 96。`dpi` 取自 `GetDpiForWindow`（每窗口），旧系统/Win8.1 回退见 §9。

**不变式（实现与评审共同遵守）：**
- 任何传给 `SkinImage::Draw` 的 `source_*` 参数都是源像素；任何 `x/y/width/height`（目标）都是物理像素。
- 任何从镜像读出的 `bounds`/`position`/`window_size` 都是物理像素。
- `resize_rect` 永远以**逻辑坐标**存储；在栅格化函数内部一次性换算成源分割（×N）与目标分割（×scale）。

---

## 5. 总体架构：DpiScale + ScaledSkin 镜像

### 5.1 DpiScale（新增 `include/ttplayer/ui/dpi_scale.h`）

```cpp
struct DpiScale {
    int dpi = 96;
    static constexpr int kBaseDpi = 96;
    double factor() const { return static_cast<double>(dpi) / kBaseDpi; }
    int X(int logical) const { return ::MulDiv(logical, dpi, kBaseDpi); }
    int Y(int logical) const { return ::MulDiv(logical, dpi, kBaseDpi); }
    SIZE Size(SIZE s) const { return {X(s.cx), Y(s.cy)}; }
    RECT Rect(RECT r) const {
        return {X(r.left), Y(r.top), X(r.right), Y(r.bottom)};
    }
    // XML font_size 是像素高度（直接赋 lfHeight），不是 points —— 命名 FontPx。
    int FontPx(int px) const { return ::MulDiv(px, dpi, kBaseDpi); }
    // LOGFONT lfHeight 可为负（em 高度）：保号缩放。
    int LfHeight(int lf) const {
        return lf < 0 ? -MulDiv(-lf, dpi, kBaseDpi) : MulDiv(lf, dpi, kBaseDpi);
    }
};
```

`MulDiv` 自带饱和舍入：`(logical*dpi + 48) / 96`，单调且无半像素累积。

### 5.2 ScaledSkin 镜像（核心策略）

皮肤对象（`LegacySkin`）**保持逻辑坐标不变**（解析逻辑 `skin.cpp` 一行不改）。新增一个产出「当前 DPI 物理像素视图」的方法：

```cpp
// include/ttplayer/skin/skin.h
class LegacySkin {
public:
    // 返回几何已换算为 physical 像素的镜像；位图与 resize_rect 仍保留源/逻辑像素。
    [[nodiscard]] LegacySkin AtDpi(const ui::DpiScale& scale) const;
    // 现有访问器不变：WindowSize()/MiniWindowSize()/Elements()/Playlist()/Lyric()/...
};
```

**实现注意（v2 补）：`LegacySkin` 拷贝构造已 delete（move-only）**，`AtDpi` 需要私有人工拷贝：`SkinImage` 本身可拷贝（`shared_ptr<Storage>` 共享位图，不重新解码）、`bitmaps_` 缓存 map 同样共享拷贝；几何字段逐一 `scale.Rect/Size/LfHeight` 重算。镜像成本低（无解码、无大分配）。

**镜像里「缩放」的字段（逻辑→物理）：**
- `window_size_`、`mini_.window_size`
- 所有 `SkinElement::bounds`、`font_size`（`FontPx` 化）
- `PlaylistSkin`：`list_bounds`、`toolbar_bounds`、`close.bounds`、`title.bounds`、`toolbar_items` 各 RECT、`position`
- `LyricSkin`：`position`、`lyric_bounds`、`mini_padding`、`font.lfHeight`（`LfHeight` 保号）、`mini_font.lfHeight`
- `EqualizerSkin`：`position`、`title/close/enabled/profile/reset/balance/surround/preamp.bounds`、**十频段 `bands[i].bounds`**（解析期已按 `模板宽+eq_interval` 步进 `OffsetRect` 过，镜像直接缩放结果即可，`eq_interval` 不必单独换算——但若后续布局代码直接消费 `eq_interval`，需 `scale.X`）
- `BrowserSkin`：`position`、`content_bounds`、`close/backward/forward/refresh/check/link.bounds`（音乐窗 `musicfree_frame.h` 的几何全由它派生）
- `DesktopLyricBarSkin`：`icon`、各命令按钮 `bounds`
- `VisualSkin::font`（可选 LOGFONT）：`LfHeight`

**镜像里「保持源/逻辑、不缩放」的字段（关键）：**
- 所有位图尺寸：`SkinBitmap::size`、`SkinElement::image_size`、`bar_size/fill_size/thumb_size/flash_size`（供栅格化函数当源坐标基准）
- `PlaylistSkin::resize_rect`、`LyricSkin::resize_rect`、`BrowserSkin` 的 `resize_rect`（九宫格源分割点，**逻辑坐标**，由 `DrawFrameBackground` 内部 ×N 与 ×scale 分别换算）
- 帧偏移（`state * frame_width`）——源像素，由 `DrawFrameButton` 内部处理
- `scrollbar_thumb_resize_center`（源片段高度，逻辑/源语义）

### 5.3 为什么是「镜像」而不是「在 Load 里缩放」

`resize_rect` 和 `background.size` 在两个相互冲突的语义里被复用（`DrawFrameBackground` 当源、`MakePlaylistGeometry`/`ResolveAlignedRect` 当逻辑布局）。若整体缩放，源坐标会错；若整体不缩放，布局会错。镜像只缩放显式逻辑几何，把源尺寸与 `resize_rect` 留给「接 scale 的栅格化函数」单独处理，两套语义互不污染。代价仅是每次换 DPI 重新产一个轻量镜像（位图 `shared_ptr` 共享，不重新解码）。

---

## 6. 改造点详述（按文件/行号）

### 6.1 DPI 感知声明（阶段0）
- `src/app/ttplayer.manifest` 增加（在既有 `<asmv1:assembly>` 内加 `<asmv3:application><asmv3:windowsSettings>` 节；旧系统忽略不认识的 `dpiAwareness`）：
  ```xml
  <asmv3:application xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
    <asmv3:windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2, PerMonitor</dpiAwareness>
    </asmv3:windowsSettings>
  </asmv3:application>
  ```
- **明确不启用 `gdiScaling`**（`<gdiScaling>true</gdiScaling>`）：本程序大量使用 DIB 段、离屏位图、GDI+ 与分层窗口，微软对该兼容开关明确列出的 DIB/离屏限制会命中（评估文档 §6）；我们的路线是自己消费 DPI，不是靠系统兼容拉伸。
- `src/app/main.cpp` 在 `wWinMain` 最早期（`SetProcessDEPPolicy` 之后、建窗之前）经 `include/ttplayer/platform/optional_windows_api.h` 解析并调用：
  - 优先 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`
  - 回退 `SetProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE)`
  - 再回退 `SetProcessDPIAware()`（XP/Win7）
- `CMakeLists.txt` 给 `ttplayer_rebuild` 目标加 `target_compile_definitions(... PRIVATE _WTL_DPI_SUPPORT)`，启用 WTL 自带 `_DPI()` 与 `OnDpiChanged` 骨架（本工程 WTL 10.01 的 `atlframe.h:2989` 即此开关的 no-op 分支）。
- 注意：**manifest 与运行时调用二选一即可生效**（运行时调用优先）。两者都写是为了「源码自文档 + 防某条路径早于 wWinMain」。

### 6.2 DpiScale 工具（阶段1）
- 新增 `include/ttplayer/ui/dpi_scale.h`，内容见 §5.1。
- 每顶层窗口（`PlayerWindow`、歌词窗、列表窗、均衡器、音乐窗、桌面歌词条、桌面歌词窗）保存 `ui::DpiScale current_scale_`，初始取值优先级：
  1. `GetDpiForWindow(hwnd)`（Win10 1607+，动态解析）
  2. `GetDpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI)`（Win8.1，动态解析）
  3. `GetDeviceCaps(GetDC(nullptr), LOGPIXELSX)`（XP/Win7 系统档）

### 6.3 ScaledSkin 镜像（阶段1）
- `src/skin/skin.cpp` 实现 `LegacySkin::AtDpi(scale)`，对 §5.2 列出的「逻辑字段」逐一 `scale.Rect/Size/FontPx/LfHeight`；位图与 `resize_rect` 原样拷贝（人工拷贝，绕过 delete 的拷贝构造）。
- `PlayerWindow` 持有 `std::unique_ptr<LegacySkin> skin_`（逻辑）+ 布局重算时 `scaled_skin_ = skin_->AtDpi(current_scale_)`，**绘制 / Hit-test / 布局 / 字体创建全部读 `scaled_skin_`**。
- 换 DPI 时：重新 `AtDpi(new_scale)` 替换 `scaled_skin_`，再触发整窗重排（§6.12）。
- **消费面切换清单**（把「读 `skin_->`」改为「读 `scaled_skin_->`」的文件）：`player_window.cpp`（绘制+命中+尺寸）、`player_window_playlist.cpp`、`player_window_lyrics.cpp`、`player_window_equalizer.cpp`、`player_window_visual.cpp`、`player_window_skin_animation.cpp`、`player_window_options.cpp`（`RenderSkinPreview`）、`desktop_lyrics.cpp`、`musicfree_browser_window.cpp`。这是阶段2 的主体工作量，必须逐文件 grep `skin_->` 核对。

### 6.4 九宫格与区域（阶段2，关键）
- `DrawFrameBackground`（`skin.cpp:1520`）与 `CreateFrameRegion`（`skin.cpp:1558`）签名增加 `const ui::DpiScale& scale`：
  ```cpp
  void DrawFrameBackground(HDC, const SkinBitmap&, RECT resize_rect,
                           int width, int height, bool tile, const ui::DpiScale& scale);
  ```
  内部：源分割 = `resize_rect × N`（N 来自 `SkinImage::source_scale()`，§7；@1x 时恒 1），目标分割 = `scale.X/Y(resize_rect)`：
  ```cpp
  const int n = bitmap.image.SourceScale();           // 资源倍率，@1x=1
  const int src_left  = resize_rect.left  * n;         // 源像素
  const int src_right = resize_rect.right * n;
  const int dst_left  = scale.X(resize_rect.left);     // 物理像素
  const int dst_right = scale.X(resize_rect.right);
  // source_x[] = {0, src_left,  src_right,  bitmap.size.cx}
  // destination_x[] = {0, dst_left, dst_right, width}   （y 同理）
  ```
  角块（row/col = 0 或 2）的目标尺寸随之放大；退化分支（空 `resize_rect` 或目标 < 源）退化为整幅 `Draw(0,0,width,height, 0,0, size…)` —— 本身就是「缩放到目标」，天然正确，无需改。
- **平铺规则（v2 补）**：`DrawBitmapPatch` 的 `tile` 分支（中心区平铺）里，**每块 tile 的目标尺寸** = 源 patch × scale（`scale.X(source_width)`），用该尺寸步进循环；不能沿用 1× 源尺寸平铺，否则 200% 下中心区是一堆 1× 小块。
- `skin_frame.h` 声明、`player_window_internal.h:474` 的 `DrawResizableSkinBitmap` / `CreateSkinWindowRegion` 内联转发同步加 `scale` 形参。
- 所有调用点（`player_window_playlist.cpp:4117`、`player_window_lyrics.cpp:1532/1618`、音乐窗）传入 `current_scale_`。

### 6.5 多帧按钮（阶段2，修 §2.4 的 bug）
- `DrawFrameButton`（`skin.cpp:1590`）增 `const ui::DpiScale& scale`：
  ```cpp
  const int destination_width  = frames > 1 ? scale.X(frame_width)
                                            : bounds.right - bounds.left;
  const int destination_height = frames > 1 ? scale.Y(element.image_size.cy)
                                            : bounds.bottom - bounds.top;
  ```
  源偏移 `state * frame_width` 仍用源像素（不变，@Nx 时 `frame_width = image_size.cx / frames` 自动是源像素）。单帧路径用已缩放的 `bounds`（来自镜像），不变。

### 6.6 列表几何（阶段2）
- `MakePlaylistGeometry`（`player_window.cpp:774`）增 `const ui::DpiScale& scale`：
  - `result.row_height = scale.Y(16);`
  - `extra_width  = width - scale.X(layout.background.size.cx);`
  - `extra_height = height - scale.Y(layout.background.size.cy);`
  - 滚动条宽：`scale.X(layout.scrollbar_buttons.size.cx) / 3`
  - 分隔条宽：`scale.X(layout.splitter_bar.size.cx)`（无图兜底 `scale.X(5)`）
  - `list/toolbar/close/title` 来自镜像（已缩放 bounds），无需再乘。
  - **注意**：`row_height` 变为 DPI 相关后，`visible_rows`/`page_rows`/滚动计算自动随高度换算，但**所有把 16 当魔法数的滚动/命中处**（评估文档 §3.2 点名的分组列表多处）必须改为引用同一 `row_height` 来源——阶段2 子任务，grep ` 16\b` 在 playlist 相关文件人工核对。
- `ResolveAlignedRect`（`player_window_internal.h:488`）增 `scale`，传入的 `native`（= `background.size`）按 scale 换算后再做对齐计算；调用点传 `scale.Size(layout.background.size)`。
- 均衡器（v2 补）：十频段位置来自镜像 `bands[i].bounds`（已缩放）；若 `player_window_equalizer.cpp` 中另有直接消费 `eq_interval` 的布局代码，用 `scale.X(eq_interval)`。

### 6.7 字体（阶段2）
- `CreateSkinFont`（`player_window.cpp:879`）改读镜像里已 `FontPx` 化的 `element.font_size`（镜像预乘，函数体不变）；`ANTIALIASED_QUALITY` 可顺带升级 `CLEARTYPE_QUALITY`（高 DPI 下 ClearType 更清晰，属可选优化）。
- **菜单字体（v2 补）**：`player_window.cpp:472` 的 `SPI_GETNONCLIENTMETRICS` 是**系统级**查询；PMv2 下自绘菜单/标题字体须改为动态解析的 `SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, 0, &metrics, 0, dpi)`，`dpi` = 所在窗口 DPI。无此 API 的旧系统维持现状（系统档）。
- 列表 `PlaylistSettings::font_height`（`settings.h:219`，默认 -13）：**保号缩放** `lfHeight = scale.LfHeight(font_height)`（见 §5.1 `LfHeight`）。
- 富文本/滚动信息（`DrawScrollingSkinInfo` 等）用镜像 `bounds`（已缩放），无需改。
- 歌词字号：普通/全屏/桌面歌词的 LOGFONT 在渲染前用 `scale.LfHeight` 重算（**渲染时换算，落盘保存原值**——防 §8 累乘）；全屏 AutoFont 的「按可用宽度缩字」逻辑在物理像素空间进行（现成，仅输入换成物理宽）。
- 桌面歌词行距 `font_height * 3 / 2 * lines`（`desktop_lyrics.cpp:415`）：字高换算后自动一致，无需单独改。

### 6.8 主窗口尺寸（阶段2）
- 四处 `skin_->WindowSize()`（`player_window.cpp:2346 / 3838 / 4279 / 5274`）改为 `scale.Size(skin_->WindowSize())`。
- 迷你窗 `MiniWindowSize()`（`player_window.cpp:3838/4279`、`player_window_lyrics.cpp:1032/1040`）同样 `scale.Size(...)`。

### 6.9 命中测试、硬编码间距与图标审计（阶段2）
- 所有用 `element.bounds` / `scaled_skin_->Elements()` 做命中判断的代码（如 `player_window.cpp:3802`）读镜像（已缩放），自动一致。
- **硬编码魔法数审计（具名清单，阶段2 子任务）**：
  - `player_window.cpp:3460`（`-24`、`16` 间距）
  - `player_window.cpp:437-459`（`16*16` 菜单掩码）→ `scale.X/Y(16)`
  - `player_window.cpp:1416/1791/1824`（`DrawIconEx 16,16`）→ 图标见下条
  - `player_window.cpp:1471-1507`（16×16 ImageList）→ 按 DPI 建 2× ImageList 或改用 `LoadIconWithScaleDown`（动态解析）从 32×32 资源取
  - `player_window_options.cpp:1302`（16×15 按钮图 ImageList）
  - `musicfree_frame.h:122`（`kResizeBand=6`）→ `scale.X(kResizeBand)`
  - `desktop_lyrics.cpp:1099`（`SIZE{231,25}` 兜底）与 `:290`（`kMinimumWidth`）→ `scale.Size(...)` / `scale.X(...)`
  - 拖拽/吸附/鼠标：`window_fade_policy.h`、`WINDOW_DRAG.md` 对应实现、`playlist_marquee.h`（跑马灯几何）、`MOUSE_CURSOR_PARITY.md` 对应命中逻辑中的像素阈值
  - `RenderSkinPreview`（`player_window_internal.h:486`，选项对话框皮肤预览图）→ 预览也应按 DPI 缩放
  - `taskbar_preview.cpp`（任务栏缩略图位图尺寸）
  - 审计方法：对上述文件 grep `\b(3|5|6|8|9|10|12|13|15|16|18|20|23|24|25|31)\b`，人工区分「布局魔法数（须包 scale）」与「算法常量/迭代次数（不动）」。
- **图标资源升级（可选，推荐随阶段4）**：现有图标以 16×16 位图为主。高 DPI 下文字清晰后图标会显得毛糙；提供 32×32 资源并 `LoadIconWithScaleDown`，或接受像素风放大（与旧皮肤一致的取舍）。

### 6.10 歌词 / 桌面歌词 / 均衡器 / 音乐窗（阶段2）
- 歌词窗、桌面歌词条、桌面歌词窗、均衡器、音乐窗统一：各自持 `current_scale_`，用 `skin_->AtDpi(scale)` 镜像，`DrawResizableSkinBitmap` / `CreateSkinWindowRegion` 传 scale，`WindowSize()`/`MiniWindowSize()` 包 scale。
- 音乐窗几何全部经 `musicfree_frame.h` 的 `BrowserSkin` 派生（`natural`/`resize_rect`/`content`/各按钮 RECT、`kResizeBand`）——镜像已缩放 `BrowserSkin` 的 bounds；`kResizeBand` 单独包 `scale.X`。`musicfree_frame.h:65` 的 `grow_x = width - natural_width` 中 `natural_width` 须取 `scale.X(natural)`。
- 透明边缘（桌面歌词点击穿透、全屏透明歌词灰度字形）在物理像素表面绘制，`CoverageMask` 的采样规则（`skin_image.cpp:149`）已要求「与图像同最近/半像素采样」，DPI 下仍成立，无需改。
- 桌面歌词窗（`desktop_lyrics.cpp`）：窗口尺寸由字高×行数派生（§2.9），字高 `scale.LfHeight` 后整体自动缩放；条形工具栏 `SIZE{231,25}` 与 `kMinimumWidth` 包 scale。

### 6.11 不规则窗口区域（阶段2，修 §2.5；v2 重写）
主窗/迷你窗背景是**固定尺寸 `SkinImage`，没有 `resize_rect`**（九宫格属于列表/歌词/音乐窗）。正确的缩放区域方案：

```cpp
// skin.h / skin.cpp
[[nodiscard]] HRGN CreateWindowRegion(bool mini, const ui::DpiScale& scale) const;
```

实现：复用 `CreateFrameRegion` 的「非 1:1 尺寸」分支——把主窗背景包成 `SkinBitmap`，传**空 `resize_rect`**（`{0,0,0,0}` → 触发整幅缩放退化分支）与 `scale.Size(WindowSize())`：

```cpp
HRGN LegacySkin::CreateWindowRegion(bool mini, const ui::DpiScale& scale) const {
    const auto& image = mini ? mini_.background : background_;
    SkinBitmap frame{image, image.Size()};          // 包一层，复用公共路径
    const SIZE physical = scale.Size(mini ? mini_.window_size : window_size_);
    return CreateFrameRegion(frame, RECT{}, physical.cx, physical.cy,
                             /*tile=*/false, TransparentColor(), scale);
}
```

`CreateFrameRegion` 的非 1:1 分支会先把背景**渲染到物理尺寸的内存位图**再扫描颜色键/alpha 掩码——与 `DrawFrameBackground` 缩放规则严格同源（同 §2.3 的映射一致性要求）。PNG 主窗走 `CoverageMask()`（该路径 `CreateFrameRegion` 已内置）。

调用点全部传 scale（§2.5 已列 6 处）：`player_window.cpp:2354/4231/5236/2000`、`player_window_equalizer.cpp:548`（均衡器背景同理）、`player_window_playlist.cpp:3000` 与 `player_window_lyrics.cpp:1503`（这两处本就走 `CreateFrameRegion` 带九宫格，补 scale 形参即可）。

### 6.12 WM_DPICHANGED 处理流程（阶段3；v2 新增详述）

每个顶层窗口（含 owned popups——它们各自独立收到该消息）：

```
WM_DPICHANGED:
  new_dpi = LOWORD(wParam)                       // 新 DPI
  suggested = (RECT*)lParam                      // 系统建议的新窗口矩形（已按比例换算）
  1. SetWindowPos(hwnd, 0, suggested->left/top, width+Δ, height+Δ,
                  SWP_NOZORDER|SWP_NOACTIVATE)   // 先落尺寸，系统按此重绘
  2. current_scale_ = DpiScale{new_dpi}
  3. scaled_skin_ = skin_->AtDpi(current_scale_) // 重产镜像
  4. 重建字体（CreateSkinFont 读新镜像；菜单字体重查 SystemParametersInfoForDpi）
  5. 重建窗口区域（SetWindowRgn(CreateWindowRegion/CreateFrameRegion(..., scale))）
  6. 重建离屏缓存/预览/工具提示；InvalidateRect(hwnd, nullptr, TRUE)
  7. 从属窗重排：主窗 DPI 变化时，吸附在主窗边的列表/歌词/均衡器按新物理尺寸
     重新对齐（复用现有吸附重排逻辑，仅输入换算为物理像素）

WM_GETDPISCALEDSIZE:
  允许自定义换算尺寸（如希望 200% 下窗口比 2× 略大以容纳整数字体）：
  *width = scale.X(logical_w); *height = scale.Y(logical_h); return TRUE
```

- 顺序约束：**先 SetWindowPos 后重建资源**——否则区域/字体按旧尺寸生成又被覆盖。
- XP/Win7：无此消息，窗口生命周期内 DPI 不变（系统档），流程退化为「启动时一次」。
- 运行时改缩放（设置→显示）：每个受影响显示器上的窗口各收一次 `WM_DPICHANGED`，流程同上；需防抖（连续两次变化时以最后一次为准，中间态可跳过重镜像）。
- 位置恢复（启动时）：保存的是逻辑矩形+保存时 dpi（§8），恢复到当前显示器后按该屏 DPI 换算物理矩形，再走上面 2~7 步。

### 6.13 测试开关（v2 新增，阶段1 就要做）
- 环境变量 `TTPLAYER_DPI_OVERRIDE`（如 `=144` 强制 150%）：`DpiScale` 初始化处读取并覆盖探测结果（仅 Debug 构建或带 `TTPLAYER_DPI_UNSAFE` 双开关更稳妥）。**没有高 DPI 屏的开发机也能全流程验证缩放逻辑**；`--dpi-selftest=N` 无界面模式可输出 `window_size_` 换算结果做冒烟断言（仿 `--mf-selftest` 模式）。

---

## 7. 皮肤资源升级（阶段4，可选但推荐；v2 修正源坐标冲突）

目标：旧皮肤「放大只是更大像素」，高清皮肤才能真清晰。

1. **资源倍率（source_scale）机制——必须先于 @Nx 落地**：`SkinImage` 增加 `int source_scale_{1}`。`LegacySkin::LoadBitmap`（`skin.cpp:277`）加载 `foo.png` 时探测同目录 `foo@2x.png`/`foo@3x.png`，按 `scale.factor()` 就近选档（`≥2.25` 选 @3x，`≥1.5` 选 @2x，否则 @1x），并把 `source_scale_` 记为 N。
   - **一致性规则**：@Nx 资源的源像素 = 逻辑 × N。九宫格源分割、帧偏移、`CoverageMask` 全部自动正确——因为它们都从 `image_size`（源像素）推导；唯一需要显式乘 N 的是 `resize_rect`（逻辑存储）→ 源分割（§6.4 的 `× n`）。XML 一个字都不用改。
   - 皮肤包内只需补充 @Nx 文件；`SkinEntry`/打包格式不变（同名带 `@Nx` 后缀的普通条目）。
2. **高清档走平滑插值**：`SkinImage::Draw`（`skin_image.cpp:159`）在 `N > 1`（即已选高清档）时，PNG 路径改用 `InterpolationModeHighQualityBicubic`（而非最近邻），保留边缘抗锯齿；@1x 旧皮肤仍走最近邻以保像素风。判据用 `source_scale_` 而非目标/源尺寸比——语义更稳（§修订记录 #2 的教训：资源倍率与显示倍率解耦）。
3. **向后兼容**：无 @Nx 文件的皮肤 N=1，退回现有像素风缩放，行为等同阶段2 结果，不破坏既有 XML。
4. 文档 `docs/BAIDU_SKIN_PNG_UPGRADE.md` 已有 PNG 升级经验，可复用其加载路径。
5. **皮肤包级声明（可选扩展）**：`<skin>` 根节点允许 `resource_scale="2"` 属性，声明「本包位图整体为 2× 资源」（不逐文件加 @2x 后缀）；解析进 `SkinMetadata`，`LoadBitmap` 将其作为默认 N。两种机制可共存，文件级后缀优先。

---

## 8. 配置迁移（阶段3，防重复累乘）

问题：保存的窗口矩形、LOGFONT 若在每次启动按新 DPI 再乘，会越乘越大（评估文档 §4 风险4）。

规则：
- **保存逻辑值，或记录对应 DPI。** 窗口矩形以「逻辑坐标 + 保存时 dpi」写入配置；读取时若 `saved_dpi == current_dpi` 直接用，否则 `physical = scale.Rect(logical)` 重算。绝不保存已乘过的值再乘。
- `PlayList/*.ttbl` 的 `Platform/SongId` 元数据袋可扩展一个 `dpi` 字段（评估文档 §遗留已提及此袋）；旧配置缺该字段时按 96 处理。
- 字体：保存 LOGFONT 原值（与 DPI 无关，§6.7「渲染时换算」），**不把放大后的 `lfHeight` 落盘**。
- 窗口位置恢复：屏幕坐标按目标显示器 DPI 换算（§6.12 末条）。
- `PlaylistSettings::font_height` 等既有字段语义不变（逻辑值）；新增字段仅 `saved_dpi`（每窗口或全局一处）。

---

## 9. 旧系统兼容（XP / Win7）与 API 解析链（v2 落实路径）

- 动态解析统一走 **`include/ttplayer/platform/optional_windows_api.h`**（工程既有，12 处在用）；本说明书涉及的动态 API 清单：
  - `SetProcessDpiAwarenessContext`、`SetProcessDpiAwareness`（Win10 1607 / Win8.1）
  - `GetDpiForWindow`（Win10 1607+）
  - `GetDpiForMonitor`（Win8.1，`MDT_EFFECTIVE_DPI`）
  - `SystemParametersInfoForDpi`（Win10 1607+）
  - `LoadIconWithScaleDown`（Win8.1+，图标升级时）
  - `GetDpiForSystem`（Win10 1607+，启动兜底）
- 探测优先级（`DpiScale` 初始化）：`GetDpiForWindow` → `GetDpiForMonitor(MonitorFromWindow)` → `GetDeviceCaps(LOGPIXELSX)`。
- 解析失败（XP/Win7）→ 系统档 `current_scale_ = DpiScale{GetDeviceCaps(...)}`，仍走完整缩放链路；即旧系统获得「系统 DPI 一档」的清晰界面，而非无感知虚拟化。
- 继续通过现有 XP/Win7 导入检查；**不得把上述任何 API 变成静态强制依赖**（评估文档 §5 第三阶段；`GetDpiForWindow` 最低客户端 Win10 1607）。
- `WM_DPICHANGED`/`WM_GETDPISCALEDSIZE` 常量在旧 SDK 下用 `#define`/`constexpr` 本地定义（值固定，不依赖新 SDK）。
- 清单同时声明 `dpiAware`（旧系统识别）与 `dpiAwareness`（新系统识别）；旧系统忽略 `dpiAwareness`。**不声明 `gdiScaling`**（§6.1）。

---

## 10. 验收矩阵（来自评估文档 §7，细化）

| 维度 | 覆盖场景 |
|---|---|
| 缩放比 | 100% / 125% / 150% / 175% / 200% / 250%（重点非整数比：125%、175%）；`TTPLAYER_DPI_OVERRIDE` 覆盖法在 100% 屏先行冒烟 |
| 显示器 | 单高 DPI；100% 与 150%/200% 混用；副屏在左/上 |
| 生命周期 | 各屏启动、跨屏拖动、运行中改缩放、断/重连屏、重启恢复位置 |
| 皮肤 | 默认、迷你、BMP 色键、PNG Alpha、可拉伸列表/歌词窗、@2x 高清档（若做 §7） |
| 操作 | 播放/音量拖动、歌词拖拽定位、列表命中与滚动、窗口吸附、菜单/提示位置 |
| 全屏与透明 | 不同屏全屏、透明歌词边缘、桌面歌词点击穿透、退出恢复 |
| 一致性 | 画面像素 vs 点击区域逐控件比对（复用评估文档的 `out/dpi-audit/probe.py` PMv2 探针）；九宫格边框与区域无错位 |
| 旧系统 | Win10 PMv2 功能验证；XP/Win7 导入检查 + 对应系统实测 |
| 回归 | 100% 时绘制/交互与改造前一致（基线 `69f2c55`；scale=1 时 `AtDpi`/`MulDiv` 恒等、`DrawFrameBackground` 退化回原逻辑）——建议截图逐窗口 diff |

---

## 11. 实施阶段与风险

| 阶段 | 内容 | 主要风险 | 缓解 |
|---|---|---|---|
| 0 | 感知声明 + 入口 + `_WTL_DPI_SUPPORT` | 过早开 PMv2 致界面过小 | 阶段2 布局完成后再开 V2；先以 `dpiAware` 系统档验证；`TTPLAYER_DPI_OVERRIDE` 冒烟 |
| 1 | DpiScale + `AtDpi` 镜像 + 测试开关 | 镜像漏缩放字段；move-only 拷贝实现错 | 对照 §5.2 字段清单逐结构审核；单测 `AtDpi(96)==原值` |
| 2 | 栅格化/几何/字体接入 + §6.9 审计 | 硬编码间距漏包 scale；消费面切换漏文件 | §6.3/§6.9 具名清单 + 探针比对 + 截图 diff |
| 3 | `WM_DPICHANGED` + 迁移 | 重复累乘；重排顺序错（先资源后尺寸） | §8 保存逻辑值/记 dpi；§6.12 固定顺序 |
| 4 | @2x（含 source_scale 机制）+ 性能 | @Nx 与源坐标混淆（v1 已踩）；4K 多表面内存 | §7 一致性规则：源=逻辑×N；按尺寸/DPI 缓存、控制重建（4K 32位全屏≈31.6 MiB，评估文档 §5 第四阶段） |

**必须守住的回归线**：100% DPI 下，所有绘制与交互与基线 `69f2c55` 一致（scale=1 时 `AtDpi`/`MulDiv` 恒等，`DrawFrameBackground` 退化回原逻辑，`source_scale=1` 时源=逻辑）。

---

## 12. 不在范围

- 不用 WPF/WebView/另一套 UI 重写歌词或列表（评估文档 §5 第四阶段明确）。
- 不为每个旧皮肤手工绘制矢量版；旧皮肤保持像素风缩放，高清靠 @Nx 资源可选升级。
- 不启用 `gdiScaling` 等 GDI 兼容拉伸开关（§6.1）。
- 不改音频/解码管线、不改网络/插件链路。

---

## 附录 A：关键文件速查

| 文件 | 角色 | 本说明书涉及 |
|---|---|---|
| `src/app/ttplayer.manifest` | 清单 | §6.1 加 dpiAware/dpiAwareness（asmv3 节） |
| `src/app/main.cpp` | 入口 | §6.1 SetProcessDpiAwarenessContext |
| `CMakeLists.txt` | 构建 | §6.1 `_WTL_DPI_SUPPORT` |
| `include/ttplayer/ui/dpi_scale.h` | **新增** | §5.1 |
| `include/ttplayer/skin/skin.h` | 皮肤模型 | §5.2/§6.3 `AtDpi`；§6.11 `CreateWindowRegion` 新签名 |
| `src/skin/skin.cpp` | 解析/框体/区域 | §6.3/§6.4/§6.5/§6.11/§7（LoadBitmap 选档） |
| `src/skin/skin_image.cpp` | 位图绘制 | §7.2 平滑插值；`source_scale_` 成员 |
| `src/ui/player_window.cpp` | 主窗/字体/尺寸 | §6.6/§6.7/§6.8/§6.9/§6.11 |
| `src/ui/player_window_internal.h` | 内联绘制 | §6.4/§6.6 签名；`RenderSkinPreview` |
| `src/ui/player_window_playlist.cpp` | 列表绘制 | §6.4/§6.11 调用点；row_height 消费核对 |
| `src/ui/player_window_lyrics.cpp` | 歌词窗 | §6.8/§6.4/§6.11 调用点 |
| `src/ui/player_window_equalizer.cpp` | 均衡器 | §6.6/§6.11 |
| `src/ui/desktop_lyrics.cpp` | 桌面歌词窗/条 | §6.10（231×25、kMinimumWidth、行距派生） |
| `src/musicfree/musicfree_browser_window.cpp` | 音乐窗 | §6.10 |
| `include/ttplayer/musicfree/musicfree_frame.h` | 音乐窗几何 | §6.9/§6.10（kResizeBand、natural） |
| `src/ui/player_window_options.cpp` | 选项对话框 | §6.9（InstallButtonBitmap 16×15）；皮肤预览 |
| `src/ui/taskbar_preview.cpp` | 任务栏缩略图 | §6.9 |
| `include/ttplayer/platform/optional_windows_api.h` | API 动态解析 | §6.1/§6.2/§6.7/§9 |
| `include/ttplayer/settings/settings.h` | 配置 | §6.7/§8（saved_dpi 字段） |

> 本说明书与 `docs/HIGH_DPI_ASSESSMENT.md` 互为补充：评估文档记录「现状与为什么不能只开开关」，本说明书记录「怎么改、改哪、按什么原则」。实施时两文档对照阅读。

---

## §11 实施状态（2026-09-21，基线 69f2c55 之后）

本章记录规格书落地到代码的真实进度，便于次日接着做。**构建基线**：`ninja -C build2 ttplayer_rebuild` → BUILD_EXIT=0（零错误零警告）；
**自检基线**：`TTPLAYER_MF_TRACE=1 ./TTPlayer.exe --mf-selftest="告白气球"` 在 `TTPLAYER_DPI_OVERRIDE=96 / 144 / 192` 三档下均 EXIT=0。

### 11.1 已落地

| 阶段 | 内容 | 落地位置 |
|---|---|---|
| 0 | 进程级 Per-Monitor V2 感知 | `src/app/main.cpp`（`wWinMain` 最早期：`SetProcessDpiAwarenessContext((void*)-4)` → `SetProcessDpiAwareness(2)` → `SetProcessDPIAware()`） |
| 0 | 清单声明 | `src/app/ttplayer.manifest`（asmv3 `dpiAwareness`/`dpiAware`，未启用 `gdiScaling`） |
| 0 | 新 API 动态解析 | `include/ttplayer/platform/optional_windows_api.h` / `src/platform/optional_windows_api.cpp`（`SetProcessDpiAwarenessContext`、`SetProcessDpiAwareness`、`GetDpiForWindow`、`GetDpiForMonitor`） |
| 0 | WTL 缩放宏 | `CMakeLists.txt`：`_WTL_DPI_SUPPORT` |
| 1 | `DpiScale` 工具 | `include/ttplayer/ui/dpi_scale.h` + `src/ui/dpi_scale.cpp`（`MulDiv` 换算、`FontPx`/`LfHeight` 保号、`TTPLAYER_DPI_OVERRIDE` 覆盖、`ResolveScaleForWindow` 回退链） |
| 1 | `LegacySkin::AtDpi` 镜像 | `src/skin/skin.cpp`（缩放 `window_size`/`mini_.window_size`/各 `bounds`/`position`/`font_size`/LOGFONT；**位图 `size`、`image_size`、`resize_rect` 保持源像素**；位图 `shared_ptr` 共享不重解码） |
| 1 | 资源倍率 N | `SkinImage::source_scale_` + `SourceScale()`（`include/ttplayer/skin/skin_image.h`）；`DrawFrameBackground` 中源分割 ×N、目标分割 ×dpi/96 解耦 |
| 2 | 九宫格/区域/按钮 scale | `skin_frame.h`：`DrawFrameBackground`/`CreateFrameRegion`/`DrawFrameButton` 均增 `scale` 默认参；`DrawBitmapPatch` 平铺块按 scale 放大 |
| 2 | 主窗区域 | `LegacySkin::CreateWindowRegion(mini, scale)` 复用空 `resize_rect` 的整幅缩放分支；**注意**：`window_size_` 取自镜像（已是物理像素），此处不得再乘 scale |
| 2 | 播放列表几何 | `MakePlaylistGeometry` 增 `scale`（`row_height=16`、默认分隔条 5、24px 余量、scrollbar/ splitter 位图宽比较）；`ResolveAlignedRect` 增 `scale`（`native`/`image_size` 为源像素，须换算后与物理 width/height 比较） |
| 2 | 音乐窗/导入窗 | `musicfree_frame.h` 的 `PaintBackground`/`ApplyWindowRegion` 增 scale 参数，调用点传 `ui::ResolveScaleForWindow(hwnd)` |
| 3 | `WM_DPICHANGED` | `PlayerWindow::HandleMessage`：`WM_DPICHANGED` → 先 `SetWindowPos` 用系统建议矩形，再 `ApplyDpiScale` |
| 3 | 换 DPI 重建 | `PlayerWindow::ApplyDpiScale`（核心：`logical_skin_` 逻辑源 + `skin_` 物理镜像；换 DPI 只换换算因子，不二次累乘）；换肤失败回滚同步回滚 `logical_skin_` |
| — | 皮肤加载双份持有 | `player_window.h`：`logical_skin_` + `current_scale_`；`LoadSkin` 中 `logical_skin_.emplace(std::move(next))` → `skin_.emplace(logical_skin_->AtDpi(current_scale_))`（`LegacySkin` 拷贝构造已删除，必须 move）；`WM_CREATE` 里按真实窗口 DPI 复核一次 |

**调用点改造量**：`MakePlaylistGeometry` ×20、`DrawElementFrame` ×11、`DrawResizableSkinBitmap` ×3、`CreateSkinWindowRegion` ×3（`player_window_playlist/_lyrics/_equalizer/_skin_animation.cpp`），统一在尾部追加 `current_scale_`。

### 11.2 第二轮实施状态（2026-09-21 夜间）

| # | 遗留项 | 状态 | 落地位置 |
|---|---|---|---|
| 1 | 配置迁移（§8） | **已完成** | `settings.h` 新增 `PlayerSettings::window_dpi`（0 = 旧版配置，不做迁移）；`settings.cpp` 两条读路径（`IntAttr(L"WindowDpi")`）与两条写路径（`SetAttribute(L"WindowDpi")`)均已挂上；`PlayerWindow::MigrateSavedWindowGeometry()` 在窗口创建前 / `ApplySkinProfileWindowState()` 里按 `current/saved` 换算 8 个矩形（含位置与尺寸）；`CaptureWindowState()` 与 `ApplyDpiScale()` 各自写回 `window_dpi`。**兼容策略**：老配置 `window_dpi==0` 时只刷新字段、不缩放矩形，行为与高 DPI 改造前完全一致，不会拿猜测的系数去缩放。 |
| 2 | 每窗口独立 DPI（§6.12） | **部分完成 / 其余见 11.4** | 桌面歌词窗已独立：`DesktopLyricsWindow::Impl` 增 `ui::DpiScale scale_`，`RefreshScale()` 在 `Create()` 后与每次 `SetSkin()` 时用 `ResolveScaleForWindow(control_)` 重读。歌词窗/列表窗/均衡器窗仍共用主窗 `current_scale_`（理由见 11.4）。 |
| 3 | `WM_GETDPISCALEDSIZE` | **已完成** | `player_window_internal.h` 的 `kMsgGetDpiScaledSize`；`HandleMessage` 新增分支：从 `logical_skin_->WindowSize()/MiniWindowSize()` 按 `DpiScale{dpi}` 直接算出目标尺寸返回，绕开系统线性投影对九宫格各块的独立取整。无 `logical_skin_` 时 `break` 走默认行为。 |
| 4 | 硬编码间距（§6.9） | **已完成** | 桌面歌词：`SIZE{231,25}`、`icon_bounds{5,4,21,20}`、`DefaultBarRect()`、`kMinimumWidth/kMaximumWidth`、`kMinimum/MaximumLineHeight`、默认 `720x166` 全部走 `scale_`（**皮肤自带矩形不缩放**——它们已是物理像素）。音乐窗：`ScaledResizeBorder(hwnd)` 按窗口 DPI 放大 `kResizeBand`；`frame::Map` 增 scale，复选框标签的 `+6`（间距）与 `76`（宽度）按 DPI 换算（`check_box`/`link` 来自皮肤，不缩放）。自绘菜单：20px 图标槽扩为 `popup_menu_icon_slot_`，16px 位图保持原生并在槽里居中。 |
| 5 | 自绘菜单字体（§6.10） | **已完成** | `optional_windows_api` 增 `GetSystemMetricsForDpi`；`dpi_scale` 增 `NonClientMetricsForDpi()` 与 `SystemMetricForDpi()`；`BeginPopupMenuStyle()` 用窗口 DPI 取 `lfMenuFont`，`MeasurePopupMenuItem()` 的 `SM_CYMENU`/`SM_CYMENUCHECK` 改走 `SystemMetricForDpi`。 |
| 6 | @Nx 资源（§7） | **约定已定 + 选档改用会话 DPI** | 文件名后缀式：`背景.png` → `背景@2.png` / `背景@3.png`（扩展名之前插入 `@N`）。选档规则：`dpi/96 >= 2.25 → @3`，`>= 1.5 → @2`，否则 `@1`；缺失则逐级回退到原图。倍率写进 `SkinImage::source_scale_`，九宫格源分割 ×N 与目标分割 ×dpi/96 解耦。选档依据改为 `ui::SessionDpi()`（初始取系统 DPI，`PlayerWindow::ApplyDpiScale` 每次换 DPI 刷新），不再写死 `GetDpiForSystem()`。**仍未经实测**（现网皮肤包均无 @Nx 资源，实际按 N=1 运行）。 |
| 7 | 视觉窗与皮肤预览 | **核查结论：视觉窗无需改动；预览已按 DPI 放大填充** | 视觉窗（`player_window_visual.cpp`）只消费 `skin_->Visual()` 的颜色与字体，**没有几何**；而 `AtDpi` 已 `ScaleLogfont(out.visual_.font)`，故自动跟随。皮肤预览：绘制端的「永不放大」上限改为 `min(dpi 系数, 适配比例)` —— **96 DPI 下与原行为逐字一致**，高 DPI 下缩略图会填满已放大的框而不是只占一半。 |
| 8 | 真机目视验收 | **未做** | 依赖 150%/200% 物理屏，自检脚本无法替代。 |

> **本节已于 2026-09-22 重新定级，且计划已执行完毕**，最新分析与结果见 **`docs/HIGH_DPI_REMAINING_PLAN.md`**。
> 结果摘要：第 1 项**已完成**（策略 C：`SkinForDpi` 缓存 + 三窗 148 处 `skin_->` 改指向各自的 `LyricSkin()/PlaylistSkin()/EqualizerSkin()` + 三窗各自处理 `WM_DPICHANGED`）；
> 第 2 项 won't-fix；第 3 项阻塞待生态；第 4 项**大部分已自动化** —— 新增 `--dpi-selftest` 无头几何回归（96/120/144/168/192/240 六档，失败进程退出码 1），三档跑均 `ok=1 EXIT=0`。
> N1/N2/N3 三个原清单外的问题也全部修完。**现在只剩 3 项**：皮肤预览位图级高清（不做）、@Nx 重解码（等生态）、真机目视验收的人工部分。
> 以下为定级前的原始清单，保留作历史记录。

### 11.3 仍未完成（按优先级）

1. **每窗口独立 DPI 的剩余部分**：歌词窗 / 播放列表窗 / 均衡器窗共 148 处 `skin_->` 引用（歌词 66、列表 56、均衡器 26），要真正独立就必须各自持有一份 `LegacySkin::AtDpi(自己的 DPI)` 镜像并全部改指向它。而这三个窗是**锚定在主窗皮肤几何上的附属弹窗**（位置由 `skin_` 的 `position` 推算、迷你模式下隐藏），若它们单独换 DPI 反而会与主窗边框脱节。因此先保持共用主窗 `current_scale_`，仅在 `WM_DPICHANGED` 随主窗整体重建。要彻底解决需引入 `const LegacySkin& SkinForDpi(int)` 缓存并按窗口分发。
2. **`RenderSkinPreview` 真正的高清渲染**（现只做到「按 DPI 放大填充」，位图本体仍是 96-DPI 上采样）：镜像里位图**刻意保持源像素**（`AtDpi` 不改位图 `size`），所以 `AtDpi` 无法让预览变清晰。要真正 2x 必须给 `RenderLegacySkinPreview` 加 scale 参数并**区分源矩形与目标矩形** —— 难点在于 `DrawPreviewSlider` / `DrawSkinLed` 等 5 个辅助函数目前把 `bar_size`/`fill_size`/`thumb_size`/`image_size` **同时当作源尺寸和目标尺寸**用，逐个拆开才能放大；另外 `SkinImage::Draw` 的 GDI+ 分支无视 DC 映射模式，所以「设个 `MM_ANISOTROPIC` 变换」这条路走不通。收益只是一个选项页缩略图的锐度，故排在最后。
3. **@Nx 换 DPI 后不重新解码**：会话 DPI 更新了，但已加载的位图不会重挑 @Nx 档（皮肤重新加载才生效）。
4. **真机 150% / 200% 目视验收**。

### 11.3 实施中踩到的坑（已修）

1. `skin_frame.h` 的三个函数在 `musicfree_frame.h`（header-only、inline）里也被调用 → 必须给 `scale` **默认实参**，否则 `musicfree` 两个 TU 直接编译失败。
2. `LegacySkin::CreateWindowRegion` 曾被同时定义在 `skin.cpp` 两处（原实现 + 新镜像实现）→ C2084 重复主体，删除原实现。
3. `LegacySkin` **拷贝构造已删除** → `logical_skin_.emplace(next)` 编译失败，须 `std::move(next)`。
4. `internal.h` 的 `ResolveAlignedRect`/`DrawElementFrame`/`MakePlaylistGeometry` 声明与定义必须同步给 `image_size`/`scale` 默认实参，否则调用点实参个数不匹配。
5. `reinterpret_cast` 不是常量表达式 → `static constexpr void* kPerMonitorV2 = reinterpret_cast<void*>(-4)` 报 C2131，改普通局部变量。
6. `optional_windows_api` 里的 DPI 项是**函数**不是函数指针 → `if (platform::GetDpiForWindow)` 触发 C4551，直接调用并由返回值判空。
7. **`window_size_` 二重缩放陷阱**：镜像里 `window_size_` 已是物理像素，`CreateWindowRegion` 内若再 `scale.Size(window_size_)` 会放大两次（200% 下窗口变成 4 倍）。凡是「镜像成员」都不得再乘 scale；只有「源像素成员」（位图 `size`、`image_size`、`resize_rect`）才需要乘。
