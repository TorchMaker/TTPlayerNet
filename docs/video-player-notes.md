# 视频播放（影视仓 / TVBox）问题分析与修复记录

> 2026-11-08。本文档记录视频页与视频播放窗口的已知问题、根因、修复与**踩过的坑**，
> 以及后续仍需补齐的 TVBox 功能。修改相关代码前请先读本文档。

## 一、链路总览

```
CMS 站点 (api.php/provide/vod)
  ├─ ac=list / ac=detail  → video_cms.cpp  FetchHome / FetchCategory / FetchDetail
  │     └─ ParsePlaySources：vod_play_from(线路) $$$ vod_play_url(集)
  ├─ 选集列表：musicfree_browser_window.cpp  kMsgVideoDetailReady → hits → FillResultList
  └─ 播放：ShowVideoPlayer(video_player_window.cpp)
        ├─ m3u8 → HlsWorkerProc（后台拼接 ts → %TEMP%\TTPlayerHLS\<hash>.ts）
        │     └─ AES-128（#EXT-X-KEY）用 BCrypt 逐段解密
        └─ 直链 → Media Foundation (IVideoBackend) 直接 Open
```

## 二、已修复的问题（按发现顺序）

### 1. 选集列表「原名」列显示播放地址（2026-11-08 两轮）
- **现象**：列表里每集显示成两行——一行集名、一行网址；或整行是 `...m3u8#第5期下`。
- **根因 A**：`ParsePlaySources` 用 `$` 切整个 `vod_play_url`，而 CMS 标准格式是
  `集名$地址#集名$地址`——**`#` 才是分集分隔符**，按 `$` 切会把每集拆成两个假选集。
- **根因 B**：部分源无「集名$地址」结构，集名拼在地址后（`...m3u8#第5期下`）。
- **修复**：先按 `#` 分集；**仅当每段都含 `$` 时才认为是标准分集**（否则整串按单集、
  交给 `#` 兼容逻辑：`://` 左地址右集名）；集名仍空兜底「第 N 集」。
- ⚠ 判定「标准分集」必须检查**每段**都有 `$`：`url#集名` 形态按 `#` 切会得到
  「url」「集名」两段（其中一段无 `$`），据此区分两种格式。

### 2. 视频页只有分类没有封面、分类无法点击
- **分类无法点击**：`video_navbar` / `video_iconbar` 是 STATIC，**没有 SS_NOTIFY**。
  无此样式时 `WM_NCHITTEST` 返回 `HTTRANSPARENT`，真实鼠标点击穿透到父窗口，
  `HandleVideoNavClick` 永远收不到。→ 创建样式加 `SS_NOTIFY`。
  （此前 e2e 用合成 `WM_COMMAND` 直送所以没暴露。）
- **封面黑屏（分区视图）**：`DrawDiscoverRowVideo` 里 `if (r.group < 0 || …) return;`
  卡在 `group == -2`（分区卡片行）分支之前，**卡片绘制是死代码**；标题行（-3）
  在它前面处理所以能看到标题。→ 改为 `r.group < -2 ||`。
- **分区数据错位**：`BuildVideoHomeSections` 用「首次出现下标 + 累计总数」当区间，
  而聚合流按站点轮询交错，各分区区间互相重叠（`section[0] start=0 count=14 /
  section[1] start=4`）。→ 重写为「按 key 分桶 → 整桶铺平」，start/count 严格一致。

### 3. 宽窗口右侧大片空白
- `cols_max` 5/6 封顶。→ 统一放宽到 8（1080 宽可得 6 列）。
- 拖动缩放时列数变化不重建行 → 增加 `video_cols_applied / discover_cols_applied`，
  只在跨阈值时 `FillVideoList / FillDiscoverList`（低频）。

### 4. 视频页高度吸附把网格压成 274px
- 音乐页的「1 组标题 + n 行完整卡片」吸附公式套到影视仓多分区滚动内容上，
  只剩第一组第一行，其余黑屏（真机 rows=19、sheet client=647×274）。
- → **视频模式不做吸附**（滚动内容允许底部半截行）；音乐页保持。

### 5. 拖动/加载时闪烁（多轮）
- **拖动闪烁**：navbar/iconbar 自绘直接画屏 → 新增 `PaintBuffered`（模板，内存位图
  + 一次 BitBlt）；`ApplyFrameMetrics` 的 `InvalidateRect(..., TRUE)` 改 FALSE。
- **海报加载闪烁**：每张封面完成/失败都 `InvalidateRect(sheet_list)`，16 并发时
  每秒 10+ 次全网格重绘（逐行 HALFTONE），UI 线程被拖垮 → ①下载回调**只在
  真的拿到图时**才 PostMessage（失败静默，由 3 秒节流补抓轮处理）；②250ms
  合并重绘（`kArtworkRepaintTimerId`，`artwork_dirty`）。
- **选集列表卡/闪**：`WS_EX_COMPOSITED` 与 owner-draw ListView 组合触发全窗软件
  合成，窗口越大越卡 → **去掉 WS_EX_COMPOSITED**。拖动闪烁已由各路径自身双缓冲覆盖。

### 6. 封面下载失败永驻彩色骨架
- `ArtworkCache::EnsureDecoded` 失败也 `emplace(url, nullptr)`，`Peek` 永远空且
  `Request` 见 memory 有 key 不再重发。
- → 失败**不占位**；`CountMissingVideoArtwork / RetryMissingVideoArtwork` 3 秒节流
  补抓（`video_art_retries` 每 url 限 2 次）；`kMaxParallel` 8 → 16。

### 7. 音乐窗标题栏左上角异常
- 默认皮肤位图左上角有青色「柒」logo 残迹（(56,162,194)，x=0..9）。
- → 盖除，但**必须先检测**：`GetPixel(2,5)/(5,5)/(8,5)` 三点满足青色特征才盖。
  用户换肤后左上角是浅色边框，无条件 BitBlt 反而制造白块。

### 8. 导航按钮贴图与背景色对不上
- 皮肤设计位：backward 106 / forward 137 / refresh 169..194（位图 474 宽）；
  1:1 标题重绘宽度硬编码 160 → refresh 落在拉伸区，贴图自带背景与渐变不符。
- → 重绘宽度 = `min(位图宽, max(160, 按钮簇右边界+6))`。

### 9. 视频播放窗口（本轮）
- **标题栏「音乐窗」文本 + 青绿花字残影**：皮肤位图标题文字被九宫格横向拉伸到
  整段。→ 画完皮肤后 `StretchBlt` 用覆盖区右端**一列渐变横向铺满 0..title_rect.right**
  （单列拉伸不会越界取源，浅/深色皮肤都平滑），再画 `title_text`。
- **窗口闪烁**：父窗无 `WS_CLIPCHILDREN`，播放中每次重绘先把 MF 渲染面子窗口区域
  刷黑再恢复。→ 加 `WS_CLIPCHILDREN`。
- **鼠标从下方进入一直是缩放图标**：`WM_NCHITTEST` 用 `st->content.bottom`
  （= 控制栏顶）判断下边，鼠标一进控制栏就是 HTBOTTOM。→ 用**客户区**四边
  （`GetClientRect`）。
- **全屏按钮位置**：从右上角移到**控制栏最右**（`fs_btn` 在 footer，音量滑条让位）；
  标题栏只留关闭（皮肤位图）+「切换源」。
- **圆形锯齿**：GDI `Ellipse/Polygon` 无抗锯齿。→ GDI+ `FillEllipseAA / FillPolygonAA`
  （`SetSmoothingMode(AntiAlias)`；`EnsureGdiplus()` 进程内一次 Startup，与封面解码同模式）。
  播放圆钮 / 进度手柄 / 音量手柄 / 播放三角形全部改 AA。
- **切换源**：`ShowVideoPlayer` 增 `sources`（`VideoPlayerSource{url,name}`）；
  detail handler 摊平时记 `line_idx/ep_idx`，把**同一集在其它线路**的地址打包进
  `hit.raw["sources"]`；播放窗「切换源」药丸（id=7）循环切换重新 Open。

### 10. m3u8 无法播放（核心）
- **根因**：Media Foundation 在 **Windows 10 1803+ 已移除内置 HLS 源**，m3u8 直链
  无法直接 Open；且视频源大量使用 **AES-128 加密**（`#EXT-X-KEY:METHOD=AES-128`），
  把加密 ts 直接拼接出来的文件 MF 也解不了。
- **修复**：`HlsWorkerProc`
  1. 下载 m3u8 → 主列表（`#EXT-X-STREAM-INF`）取第一个子列表再解析一层；
  2. 媒体列表收集段地址，同时解析 `#EXT-X-KEY` 的 `URI` 与 `IV`（hex 16 字节）；
  3. 下载 key（必须 16 字节）→ 每段 `DownloadBinary` 后
     `AesCbcDecrypt128`（BCrypt AES-128-CBC，去 PKCS7 padding）；
  4. **校验明文首字节 0x47（TS sync）**，不是则明确报「不是可拼接的 TS 格式」；
  5. 顺序追加写入 `%TEMP%\TTPlayerHLS\<fnv1a62>.ts`，**同地址已有成品直接复用**；
  6. 按段数 PostMessage `WM_USER+5` 进度 → 窗口显示「正在缓冲 n%」；
     完成 `WM_USER+6` 交给 MF `Open(本地 ts)`；`WM_DESTROY` 置 cancel。
- **验证**：openssl 独立验证该源 `key(16B) + IV=0 + AES-128-CBC` 解密后首字节
  `0x47`、偏移 188 也是 `0x47` → 明文 TS 成立，方案正确。

### 11. 重复的第二条分类
- 分组标题行里又画了一排分类 chips（与顶部导航行重复）。→ **不再画**，分类入口
  只保留顶部导航行。

### 12. 第一次切到视频页左上角异常
- 只刷 sheet_list，tab 左缘/标题栏下沿残留上一页像素。→ `OnTabChanged` 末尾
  `InvalidateRect(hwnd, nullptr, TRUE)` 整窗重绘一次。

## 三、已知限制 / 未完成（TVBox 功能差距）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| 网页分享页源（如 `lzv34.com/share/...`） | **不支持** | 需要 TVBox 的「解析接口 / spider」拿到真实直链。已给出明确错误提示 + 引导切源。 |
| fMP4 / CMAF 分片 m3u8 | 不支持 | 拼接器只认 TS 段（sync 0x47 校验会拒绝并提示）。 |
| 直播 m3u8（无限滚动列表） | 不支持 | 拼接是点播语义；直播需要持续拉流。 |
| 清晰度选择 | 未做 | 部分 CMS 的 m3u8 主列表有多个 `#EXT-X-STREAM-INF`，可在解析时让用户选；当前取第一个。 |
| 换源（跨站点同一影片） | 部分 | 「切换源」按**同一集的多线路**切换；跨站点换源需在详情层做影片名匹配。 |
| 解析接口（parse/spider） | 未做 | 这是网页源能播的前提，工程量大（需 JS 运行库或内置解析）。 |
| 缓存视频/收藏/下载 | 未做 | — |

## 四、踩坑清单（务必记住）

1. **STATIC 控件必须 `SS_NOTIFY`** 才能接收鼠标；否则 `HTTRANSPARENT` 穿透。
2. **`WS_EX_COMPOSITED` 会拖垮 owner-draw ListView**（全窗软件合成，越大越卡越闪）。
3. **父窗有子窗口渲染面（MF/EVR）必须 `WS_CLIPCHILDREN`**，否则重绘刷黑子窗口区 → 闪。
4. **`WM_NCHITTEST` 的缩放带用客户区边界**，不能用 content/footer 等内部矩形。
5. **所有逐行自绘控件要走双缓冲**（`PaintBuffered`），否则拖动/滚动闪烁。
6. **高频回调（图片到位/下载失败）必须合并重绘 + 失败不触发**，否则重绘风暴。
7. **跨进程 UI 测试**：`GetDlgItem` 必须设 `restype=HWND`（否则 64 位句柄被截断成 0）；
   `LVM_GETITEMRECT`/`LB_GETITEMRECT` 等**指针型消息**跨进程不可用，改用坐标/固定值；
   `mouse_event` 在 Python 3.13 不可用，用 `SendInput`；合成 `WM_COMMAND` 打不开复杂
   对话框路径（真实点击更可靠）；截图前需置顶窗口（否则被其它应用遮挡）。
8. **`SendInput` 双击时序**不稳，测「播放」优先用「点击按钮 / 未选中自动播第一集」。
9. **`ParsePlaySources` 的分隔符**：`$$$` 分线路、`#` 分集、`$` 分「名/址」，
   顺序不能混。
10. **MSVC 两个匿名命名空间**：函数声明与定义必须同处一个（否则 LNK2001 未解析，
    且 mangled 名带 `?A0x…` 匿名空间后缀，容易误判）。检查 `grep -n "namespace {"`。
11. **`grep "Linking CXX executable"` 过滤会吞掉 LNK 错误**——编译后**必须 `ls -la`
    确认 exe 存在且时间戳更新**，不能只看日志里出现 Linking 行。
12. **GDI+ API**：是 `SetSmoothingMode`（不是 `SetSmoothing`）；`GdiplusStartup` 可
    进程内一次、不 Shutdown（与 `skin_image.cpp` / `cover_image.cpp` 同模式）。
13. **HLS**：MF 无内置 HLS 源；AES-128 段必须逐段解密；**判「标准分集」要逐段查 `$`**。

## 五、测试

| 脚本（`.test/`） | 用途 |
| --- | --- |
| `e2e_video_play.py` | 视频页 → 点卡片 → 选集 → 点播放所选 → 等视频窗 → 采样画面亮度/方差 |
| `e2e_video_home.py` | 视频页聚合、分类真实点击、宽窗列数 |
| `probe_rows.py` | 跨进程读 sheet_list 行数/行高/行矩形（定位布局/黑屏） |
| `probe_seal.py` | 三宽度标题栏左上角青色像素统计（柒字回归） |
| `e2e_drag.py` | 真实拖拽：底边吸附（diff 应为 0）、缩放后列数重排 |
| `repro_abort.py` | 退出崩溃回归（clean exit） |

**画面判定**：`e2e_video_play.py` 采样视频窗中心 200×120 的平均色与方差——
浅灰 (240,240,240) 且方差 0 = 无画面；方差明显 > 0 = 有画面内容。

**近期实测结论**：
- 分集解析修复后，选集列表回归正常行数与标题（截图确认「第 01 集」单行）。
- 「量子资源(幼)」源（`lzv34.com/share/...`）是网页分享页 → 已提示需解析接口。
- AES-128 HLS 解密方案经 openssl 独立验证正确；应用内端到端播放在 UI 自动化下
  未稳定复现（合成鼠标时序限制），需真机点击验证。
