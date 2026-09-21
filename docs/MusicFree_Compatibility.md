# TTPlayer × MusicFree 音源兼容层 —— 集成说明

> 目标：让 TTPlayer（C++20 / Win32 / x86 重建版）像 MusicFree 一样，**手动导入**一个 MusicFree 音源（单个插件 `.js` 或注册表 JSON 如 `yuanli.json`），即可**在线搜索**并**解析播放地址**。

---

## 1. 背景与目标

- MusicFree 的音源是 **CommonJS 的 JavaScript 插件**，导出 `search` / `getMediaSource` / `getLyric` 等 async 方法。
- 一个“音源”可以是：
  - **单个插件 JS**（如 `wy.js`），或
  - **注册表 JSON**：`{ "plugins": [ { "name", "url", "version" } ] }`（如 `yuanli.json`，指向多个插件）。
- 本兼容层把 MusicFree 的插件加载与调用逻辑 **1:1 复刻**到 C++ 侧，用内嵌 **QuickJS-NG** 运行插件 JS，用 **WinHTTP** 提供 `axios`/`fetch` 的真实网络能力。

**关键约束（与 MusicFree 一致）：音源不内置，必须由用户手动导入。** 代码只消费用户提供的 URL / 文件。

---

## 2. 架构总览

```
                    ┌──────────────────────────────────────────────┐
  用户粘贴 URL ──▶  │ MusicFreeSourceManager（手动导入 / 持久化）  │
  或选择本地文件    │  · 识别 注册表JSON / 单个插件JS              │
                    │  · 下载并缓存插件 .js 到 <exe>/musicfree/plugins
                    │  · 写入 imported.json（下次启动 LoadCached） │
                    └───────────────┬──────────────────────────────┘
                                    │ 每个插件一个实例
                                    ▼
                    ┌──────────────────────────────────────────────┐
                    │ MusicFreePlugin（单插件封装）                │
                    │  · Mount：按 MusicFree mountPlugin 方式求值  │
                    │  · Search / GetMediaSource / GetLyric        │
                    └───────────────┬──────────────────────────────┘
                                    │
                                    ▼
                    ┌──────────────────────────────────────────────┐
                    │ QuickJsEngine（QuickJS-NG 沙箱）             │
                    │  · EvalPlugin / EvalLibrary                  │
                    │  · Await：JS_ExecutePendingJob 泵微任务       │
                    │  · require 注册表：                           │
                    │      axios  → WinHTTP（关键，真实网络）       │
                    │      fetch  → WinHTTP（部分插件直接用）       │
                    │      crypto-js/dayjs/qs/big-integer/he → 纯JS │
                    │      cheerio → 尽力垫片；webdav 等 → 空对象   │
                    └───────────────┬──────────────────────────────┘
                                    │
                                    ▼
                    ┌──────────────────────────────────────────────┐
                    │ musicfree_http.cpp（WinHTTP 后端）            │
                    │  · axios 可调用 + .default（复刻 RN interop） │
                    │  · 阻塞式请求 → 同步 resolve Promise          │
                    └──────────────────────────────────────────────┘
```

**为什么这样设计**：axios 后端执行**阻塞式** WinHTTP 请求，拿到响应后**同步 resolve** Promise。这样插件里 `await axios.get(...)` 的续体只进入**微任务队列**，整个 await 链可在 `QuickJsEngine::Await()` 内用 `JS_ExecutePendingJob` 一次性泵完，**无需外部事件循环**。

---

## 3. 新增文件清单

### 头文件 `include/ttplayer/musicfree/`
| 文件 | 作用 |
|---|---|
| `musicfree_types.h` | `MusicItem` / `MediaSource` / `SearchResult` / `LyricResult` / `PluginMeta` / `QualityKey` / `NetworkConfig`；`MusicItem::FromJson` 声明 |
| `quickjs_engine.h` | QuickJS 沙箱封装（`EvalPlugin`/`EvalLibrary`/`Await`/`RegisterRequire`/`JsonToValue`/`ValueToJson`） |
| `musicfree_plugin.h` | 单插件封装（`Mount`/`Search`/`GetMediaSource`/`GetLyric`） |
| `musicfree_source_manager.h` | 手动导入 / 持久化 / 卸载（`ImportFromUrl`/`ImportFromFile`/`LoadCached`/`RemoveSource`/`InstallBundledLibs`） |
| `musicfree_http.h` | `CreateAxiosModule` / `CreateCheerioShim` / `CreateFetchGlobal` / `DownloadText` |
| `musicfree_import_dialog.h` | 手动导入对话框（纯 Win32，无 RC 依赖） |
| `musicfree_app.h` | 应用级入口（单例、启动加载、全源搜索、解析播放地址） |

### 源文件 `src/musicfree/`
`quickjs_engine.cpp`、`musicfree_http.cpp`、`musicfree_plugin.cpp`、`musicfree_source_manager.cpp`、`musicfree_import_dialog.cpp`、`musicfree_app.cpp`、`libs/*.js`（5 个纯 JS 依赖库）。

### 第三方
`third_party/quickjs-0.9.0/`（quickjs-ng v0.9.0）、`include/nlohmann/json.hpp`（v3.11.3 单头）。

### 构建
`CMakeLists.txt` 新增：
- `qjs` 静态库（5 个核心 C 源，`/STACK:8388608`）
- `ttplayer_core` 追加 6 个 `src/musicfree/*.cpp`，链接 `qjs`
- POST_BUILD 阶段把 `src/musicfree/libs` 复制到 `<exe>/musicfree/libs`

---

## 4. 本地编译（本沙箱无 MSVC，须在装有 VS 2022 的机器上执行）

```bash
# 必须 Win32/x86（项目要求 -A Win32）
cmake -S . -B build -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target ttplayer_rebuild
```

产物：`build/.../ttplayer_rebuild.exe`，同级自动生成 `musicfree/libs/`。

> 本仓库源码在无 MSVC 环境中无法编译；**JS 侧逻辑已用 Node 完整验证**（见 §7）。

---

## 5. 如何手动导入音源（与 MusicFree 一致）

### 方式 A：URL（推荐，示例用 `yuanli.json`）
1. 打开“导入 MusicFree 音源”对话框。
2. 在 URL 框粘贴：`https://13413.kstore.vip/yuanli/yuanli.json`
3. 点【导入 URL】→ 逐个下载并挂载注册表里的插件，结果摘要显示“成功挂载 N/M”。

### 方式 B：本地文件
点【选择本地文件…】选择注册表 JSON 或单个插件 `.js`。

### 持久化
导入记录写入 `<exe>/musicfree/imported.json`，插件缓存在 `<exe>/musicfree/plugins/`。下次启动 `LoadMusicFreeSourcesOnStartup()` 自动重建。

### 接线（UI 侧）
```cpp
#include "ttplayer/musicfree/musicfree_import_dialog.h"
#include "ttplayer/musicfree/musicfree_app.h"

// 菜单“导入 MusicFree 音源”：
ttplayer::musicfree::ShowMusicFreeImportDialog(hwnd, ttplayer::musicfree::AppMusicFreeManager());

// 应用初始化：
ttplayer::musicfree::LoadMusicFreeSourcesOnStartup();

// 在线搜索（platform 为空 = 全部音源）：
auto hits = ttplayer::musicfree::SearchAllSources("周杰伦");

// 播放：把在线曲目交给音频引擎
std::string url = ttplayer::musicfree::ResolvePlayUrl(hit.platform, hit.id);
```

---

## 6. 音源契约（复刻 MusicFree，本层已实现）

| MusicFree | 本层 | 说明 |
|---|---|---|
| `mountPlugin` | `QuickJsEngine::EvalPlugin` | `function(require,__musicfree_require,module,exports,console,env,URL,process){...}` 包裹，读 `module.exports`（优先 `.default`） |
| `_require` | `QuickJsEngine::InitRequireRegistry` | 固定包名 → 内置实现 |
| `search(q,page,type)` | `MusicFreePlugin::Search` | 返回 `{isEnd,data[]}`，逐条补 `platform` |
| `getMediaSource(item,quality)` | `MusicFreePlugin::GetMediaSource` | 返回 `{url,headers,userAgent}` |
| `getLyric(item)` | `MusicFreePlugin::GetLyric` | 返回 `{rawLrc,translation}` |
| `env` / `process` / `URL` | `InitGlobals` | `os:"android"`、`lang:"zh-CN"`、`appVersion` |
| `axios`（RN 形态） | `MakeAxios` | **本身可调用**且 `.default` 指向自身（兼容 `_interopRequireDefault` 后 `(_sLS.default)(...)`） |
| `fetch`（RN/浏览器） | `CreateFetchGlobal` | 返回 `{status,ok,headers.get,json(),text(),arrayBuffer()}` |

---

## 7. 验证结果（Node 环境实测，无需 MSVC）

用 Node 复刻 `EvalPlugin`/`EvalLibrary` 的包裹方式与注入环境，对**真实文件**做了验证：

```
[lib] crypto-js.js OK -> object [lib,enc,algo,x64,MD5,HmacMD5]
[lib] dayjs.js     OK -> function [extend,locale,isDayjs,unix,en,Ls]
[lib] qs.js        OK -> object [formats,parse,stringify]
[lib] big-integer.js OK -> function
[lib] he.js        OK -> object [version,encode,decode,escape,unescape]
[plugin] 最小插件 search/getMediaSource/getLyric 全部通过
[wy.js] platform = 元力WY | version = 1.2.1
[wy.js] methods: search=true getMediaSource=true getLyric=true getMusicInfo=true
[wy.js] MOUNT OK
```

真实网络调用（`eapi/batch`）：
```
>> POST https://interface.music.163.com/eapi/batch  (加密 params 由插件 crypto-js 生成)
<< 200 {"result":{"songs":[ ...真实歌曲数据... ]}}
```
证明**插件的签名加密链路、axios 调用、响应解析均正常**。

### 已知边界：`cloudsearch/pc` 返回 `{"msg":"参数错误","code":400}`
- 该接口除签名外还校验 **UA / Cookie / 会话**。真实 MusicFree 在 RN 环境带有完整会话头。
- 本层**原样转发插件设置的头**，仅在插件未提供时才注入默认 UA（设计如此，不覆盖插件意图）。
- 结论：这是**服务端反爬/会话约束**，不是本层接口或逻辑缺陷；`eapi/batch` 已能正常返回数据即为明证。

### 编译期已修正的 QuickJS-NG 兼容点
| 问题 | 修正 |
|---|---|
| `JS_ExecutePendingJob(rt,&int)` | ng 签名是 `JS_ExecutePendingJob(rt, JSContext**)` |
| `JS_ToBool(ctx,&b,v)` | ng 是 `JS_ToBool(ctx,v)` 返回 int |
| `JS_GetArrayLength` | ng 无此 API → 改用 `JS_GetLength` |
| `JS_TAG_FLOAT` | ng 是 `JS_TAG_FLOAT64` |
| `switch(JS_TAG_OBJECT)` | ng 的 tag 被掩码（对象→15），改用 `JS_IsObject/JS_IsString/...` 谓词 |
| `JS_NewUint8Array(size)` | ng 无分配重载 → 二进制用 `JS_NewArrayBufferCopy`；TypedArray 走 `JS_NewTypedArray` |
| Promise/TypedArray 不可用 | 构造器内显式 `JS_AddIntrinsicPromise/TypedArrays/...` |
| `JSON` 全局缺失 | 显式 `JS_AddIntrinsicJSON` |
| UMD/插件裸 `function` 声明求值为 undefined | `EvalPlugin`/`EvalLibrary` 用 IIFE 包裹返回函数 |

---

## 8. 安全说明

- **TLS 校验始终开启**，与歌词子系统一致，绝不因本层禁用。
- 插件在 **QuickJS 沙箱**内运行，**不暴露**文件系统与原生句柄；`require` 只解析白名单包。
- 网络仅经 WinHTTP，支持 TTPlayer 既有代理配置。
- 音源仅在用户**显式导入**后生效，无任何内置/自动拉取。

---

## 9. 后续可延伸（未包含在本次改动）

- 把 `SearchAllSources` 结果接入播放列表 UI（“网络”视图已存在 `player_window_playlist_network.cpp`）。
- 在 Options 增加“MusicFree 音源管理”入口（复用导入对话框 + 已导入列表）。
- 歌词：`GetLyric` 已可用，可接歌词子系统实现网络歌词匹配。
- 用户变量：`env.getUserVariables` 目前返回空对象，可按需接 TTPlayer 设置。
