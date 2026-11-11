# webui —— 音乐窗 WebView2 页面

音乐窗内容区的页面（`index.html` + `style.css` + `app.js`），运行时由 WebView2 承载；
Win32 只负责外壳（皮肤边框 / 拖拽 / 缩放 / 关闭）。`app.js` 分两个模块：
① 桥封装（`Bridge.send/on`）；② 真实模式业务（握手 + 事件驱动渲染 + 控件走桥）。
**只有这三个文件**会被打进 exe（`resources.rc` 的 `IDR_WEBUI_INDEX/STYLE/SCRIPT`
三件套），新增脚本文件不会被嵌入 —— 页面逻辑一律放 `app.js`。

## 1. 页面结构

```
index.html
├── <header class="topbar">
│     ├── #tabbar            顶栏 tab 容器（role=tablist）
│     │     └── button.tab   data-tab="music|video"，当前态 class="active"
│     └── div.topbar-right
│           ├── #search-entry      搜索入口（点击 → 切到音乐 tab 并聚焦搜索框）
│           └── #btn-import-music  导入音源插件（→ cmd music.import）
├── <main id="view">         内容区（flex 纵向容器，不整体滚动）
│     ├── #panel-music       音乐面板（左推荐 / 右歌曲列表 + 搜索条）
│     │     └── div.split    （flex:1;min-height:0；左右两栏各自 overflow-y:auto）
│     │           ├── div.split-left  → #music-charts   推荐榜单卡片（grid）
│     │           └── div.split-right
│     │                 ├── div.search-bar → #search-input / #search-btn
│     │                 ├── h2#track-list-title → #track-list  曲目列表（li.track）
│     │                 └── #search-results  搜索结果（隐藏；有结果时隐藏曲目列表）
│     ├── #panel-video       视频面板（默认 hidden，左推荐 / 右列表）
│     │     ├── #btn-import-src  导入视频源按钮（→ cmd video.sources）
│     │     ├── #video-status   加载/状态行（info / error 两种 class）
│     │     └── div.split       工具行/状态行固定在分栏上方，不随列滚动
│     │           ├── div.split-left
│     │           │     ├── #video-cats     分类 chips（首项「全部」+ 各分区标题）
│     │           │     └── #video-sections 推荐分区容器，每个分区结构：
│     │           │           <section class="vsec">
│     │           │             <h3 class="vsec-title"></h3>
│     │           │             <div class="cards"></div>
│     │           │           </section>
│     │           └── div.split-right → #episode-list   视频列表 / 选集（button.episode）
├── #toast                  轻提示（class="show" 控制显隐）
└── <script> 内联           无桥兜底的假数据分支（见 §3.3）
```

> 音乐窗不承载播放控制条：音乐播放走主窗口原有控件，视频播放由 `video.open`
> 打开独立播放窗口（页面内不嵌播放器）。
> 音乐搜索不再是独立 tab：搜索条在音乐页右栏顶部，结果显示在右栏；
> 视频 tab 暂无视频搜索命令，不放搜索条。
> 宽窗口下 `.view` 不滚动，左右两栏各自滚动；窄窗口（<760px）退回上下布局并恢复整页滚动。

样式文件 `style.css`：深色统一调性，全部颜色/圆角/动效变量集中在 `:root`；
卡片网格 `repeat(auto-fill, minmax(150px, 1fr))`；分区标题（.sec-title / .vsec-title
左侧强调条）；滚动条美化；过渡动画；空态 `.empty`、加载态 `.loading`。
**不引用任何外部网络资源**（无 CDN、无外链字体/图片）；占位图为内联 SVG data URI。

## 2. DOM id 契约（C++ 桥与 app.js 均按此驱动，勿改名）

| 分类 | id |
| --- | --- |
| tab 容器 | `tabbar`（按钮 `data-tab="music|video"`，当前态 `.active`） |
| 内容区 | `view`、`panel-music`、`panel-video`（切换用 `hidden` 属性） |
| 音乐面板 | `music-charts`、`track-list-title`、`track-list`、`search-input`、`search-btn`、`search-results` |
| 视频面板 | `btn-import-src`、`video-cats`、`video-sections`、`episode-list`、`video-status` |
| 提示 | `toast` |
| 补充 | `search-entry`、`btn-import-music` |

约定要点：

- 封面 / 卡片图**一律** `<img loading="lazy" src="...">`，由 Chromium 原生解码（webp/avif
  不走 `ttp_i_webp.dll`）。
- 面板切换用 `hidden` 属性（CSS 已保证 `[hidden]{display:none !important}`）。
- 曲目行当前播放态加 class `playing`；选集当前项加 class `active`。
- `#video-status` 的 class：`info`（普通状态）/ `error`（错误），内容由 `evt video.status` 推。

## 3. 桥接协议

### 3.1 页面 → C++

```js
Bridge.send('player.play', { mid: 'xx' });            // 默认 5s 超时
Bridge.send('video.open', {...}, { timeout: 30000 }); // 按命令放宽（仅 video.open 用 30s）
// 内部等价于 chrome.webview.postMessage({ cmd, id, payload })（发对象不发字符串）
```

- `id` 为页面侧**递增整数**，用于把异步响应回挂到请求上。
- 首批 cmd：`app.init`、`ping`、`tab.switch`、`player.play`、`player.pause`、
  `player.next`、`player.prev`、`player.seek`、`player.volume`、`video.home`、
  `video.open`、`video.sources`、`search.query`、`fav.set`、`cfg.get`、`cfg.set`。
- **超时口径**：`send()` 默认 5000ms reject；第三参 `{ timeout: ms }` 按命令覆盖。
  仅 `video.open` 放宽到 30000ms（详情取选集走网络，见 §5）。

### 3.2 C++ → 页面

```js
window.chrome.webview.addEventListener('message', function (e) {
  var msg = JSON.parse(e.data);   // e.data 已是 JSON 字符串
});
```

三种信封：

| 信封 | 含义 | 页面处理 |
| --- | --- | --- |
| `{"evt":"bridge.ready","payload":{...}}` | C++ 就绪 | 随即发 `{"cmd":"app.init","id":N,"payload":{}}` |
| `{"id":N,"ok":true,"result":{...}}` / `{"id":N,"ok":false,"error":"..."}` | 对 id=N 的响应 | 按 id 匹配 pending 请求，成功取 `result`，失败取 `error` |
| `{"evt":"<name>","payload":{...}}` | 状态推送 | 按 `evt` 分发（`player.state`、`video.home`、`video.status`、`search.result`、`source.result`） |

首批 evt：`bridge.ready`、`player.state`、`video.home`、`video.status`、
`search.result`、`source.result`。

推荐 `app.js` 暴露 `send(cmd, payload, opts) -> Promise`（内部自增 id、挂起/超时/匹配）
与 `on(evt, handler)` 两个封装，业务代码不直接碰 `postMessage`。已实现，见 `window.Bridge`。

### 3.3 真实模式 vs 降级模式（判别逻辑）

| 判别 | 条件 | 行为 |
| --- | --- | --- |
| **真实模式** | `window.Bridge.isMock === false`（即 `chrome.webview.postMessage` 存在） | `app.js` 模块 2 启用：等 `bridge.ready` → 发 `app.init` → 拉初始数据 → 全部渲染与控件由桥事件驱动；置 `window.__REAL_RENDERED__ = true` |
| **降级 mock 模式** | `window.Bridge.isMock === true`（普通浏览器直接打开 `index.html`） | `app.js` 模块 2 **整体 return，不注册任何桥监听**；index.html 内联 `__MOCK__` 分支渲染两个 tab（含音乐右栏搜索与视频分类 chips），置 `window.__MOCK_RENDERED__ = true`；只打一条 `console.warn`，不抛未捕获异常 |

降级模式下的行为（保留原样）：

- index.html 内联脚本提供 `window.__MOCK__`（榜单 / 曲目 / 视频分区 / 选集 / 搜索结果
  五组本地假数据，占位图为内联 SVG），并直接渲染两个 tab；
- 控制条与搜索提供纯视觉的演示交互（切换 tab、假搜索、播放/模式按钮、进度点选、
  导入按钮提示），弹 `#toast` 说明当前为演示模式；
- `Bridge.send()` 在 mock 下返回 `window.__MOCK__[cmd]`（缺省 `{}`），`Bridge.ready`
  直接放行，页面初始化不会挂死。

## 4. 外部覆盖（改页面免重编）

构建时 `webui/` 三个文件以 RT_RCDATA 嵌入 exe；运行时按以下优先级加载：

1. **外部文件优先**：`<exe>\webui\index.html` 存在 → 直接加载外部目录
   （相对引用的 `style.css`、`app.js` 同样取外部文件）。
2. 否则读 exe 内嵌资源。

因此联调/改版只需把本目录文件拷到 exe 同级的 `webui\` 下覆盖，**不必重新编译**；
改完刷新（或重开音乐窗）即生效。回退基准：删掉 `<exe>\webui\` 目录即回到嵌资源版本。

浏览器直接双击打开本目录 `index.html` 也能预览两个 tab（走 §3.3 降级 mock 分支）。

## 5. 真实模式：握手与 cmd/evt → 渲染目标清单

### 5.1 握手时序

```
C++ NavigationCompleted → evt bridge.ready
  → 页面立即 cmd app.init { ua, version:"webui 1.0" }
  → 回执 { bridge:"1.0", cmds:[...], events:[...] }（进入真实数据模式）
  → 依次拉初始数据：
      cmd tab.switch { tab:"music" }   同步初始页签
      cmd video.home {}                视频首页（回执 {pending:true}，数据走 evt video.home）
```

握手前/事件未到时页面**已有占位**：`#video-status` info 行、`#video-sections` 用
`.loading`、`#music-charts`/`#track-list`/`#episode-list` 用 `.empty` —— 不白屏。
12s 未收到 `bridge.ready` → `#video-status` error 行提示。

### 5.2 cmd（页面 → C++）

| cmd | 触发 | payload | 回执处理 |
| --- | --- | --- | --- |
| `app.init` | 收到 `bridge.ready` 立即 | `{ua, version}` | 成功 → 初始拉取链 |
| `tab.switch` | 点 tab / 搜索入口 | `{tab}` | 失败仅 console.warn |
| `player.play` | 点 `.result-row` / 音乐曲目 | 曲目字段或 `{tracks,index}` | 失败 `#toast`（播放控制本身在主窗口） |
| `video.home` | 握手后 / 切到视频 tab 且无数据 / 导入源后 | `{}` | 结果走 evt `video.home`；90s 事件看门狗 → `#video-status` error + `.empty` |
| `video.open` | 点卡片（取详情）/ 点选集（开播），**30s 超时** | 卡片：`{url?,id,site,title,sources?,line?}`；选集：全字段回发 | `{mode:"detail",lines:[{name,episodes:[{name,url,sources}]}]}` → `#episode-list`；`{mode:"play"}` → `#toast` |
| `video.sources` | `#btn-import-src` | `{}` | 回执超 5s 超时属预期（模态对话框），静默等 evt |
| `search.query` | `#search-btn` / Enter | `{q}` | 立即 `{accepted:true}` → 显示 `.loading`，结果走 evt；30s 看门狗 → `.empty` |
| `player.play`（播放条目） | 点 `.result-row` | 条目字段 `{id,mid,title,artist,album,duration,cover,extra}` | 失败 `#toast` |

### 5.3 evt（C++ → 页面）→ 渲染目标

| evt | payload 概形 | 渲染目标 |
| --- | --- | --- |
| `bridge.ready` | `{}` | 触发握手（`app.js` 用 `Bridge.on` + `Bridge.ready` 双保险，幂等） |
| `video.home` | `{sections:[{title,cards:[{id,title,cover,rate,from,remarks,type,year,site}]}],error?,errors?,sites?}` | `#video-sections`（严格 `section.vsec > h3.vsec-title + div.cards`）；空/错 → `#video-status` + `.empty` |
| `video.status` | `{text|message, error?|level?}` | `#video-status`（`info`/`error` class） |
| `search.result` | `{q, groups:[{name,items:[{title,artist,album,duration,id,cover,extra}]}]}` | `#search-results`（`.result-group > h3 + .result-row`）；迟到旧结果按 `q` 过滤 |
| `source.result` | `{text|message, sources,cms,spider,lives}` | `#toast`；若首页仍空则顺手重拉 `video.home` |

### 5.4 封面规则

一律 `<img loading="lazy" src="URL">`，URL 来自数据字段（`cover`/`art`）；字段缺失
则**不渲染 img**，由 `.card-cover` 的 CSS 底色占位（不发任何网络占位图请求）；
URL 加载失败时页面用捕获阶段 `error` 监听隐藏 broken 图，同样露出 CSS 占位。
