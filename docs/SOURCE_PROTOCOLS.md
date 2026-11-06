# 音源协议兼容说明

千千静听2026 的在线音源层不再只认 MusicFree 插件。导入窗口会自动嗅探内容，
按协议交给对应适配器；上层（搜索、解析、播放列表）只面对统一接口。

## 支持的协议

| 协议 | 识别特征 | 搜索 | 解析直链 | 说明 |
| --- | --- | --- | --- | --- |
| MusicFree | `module.exports` + `search/getMediaSource/getLyric`，或 `{plugins:[...]}` 注册表 JSON | ✓ | ✓ | 原有实现，行为不变 |
| 洛雪 / LX Music 自定义音源 | `globalThis.lx`、`EVENT_NAMES.inited`、`on('request')`；也可靠 `*.lx.js` 后缀 | ✗ | ✓ | 事件式脚本，只把歌曲信息换成直链 |
| 声明式 REST 音源 | JSON 且 `protocol: rest` / 含 `baseUrl` + `search`/`resolve`；或 `*.rest.json`、`*.subsonic.json` | ✓ | ✓ | 对接自建服务，内置 neteaseapi / subsonic 预设 |
| Listen1 provider | `bootstrap_track`、`class MusicProvider` 等回调式结构 | — | — | 当前仅识别并登记元数据，运行时桥接未启用 |

洛雪音源按协议本身**不提供搜索**，所以它在「全部音源搜索」里不出现；搜索仍由
MusicFree 插件或 REST 音源完成。洛雪承担的是播放地址解析的补充通道。

## 导入与管理

导入窗口（音乐窗 → 导入音源）支持粘贴 URL 或选择本地文件，自动嗅探协议：

- 列表每行方括号内是协议标签，后面是音源名、版本与能力（搜 / 播 / 词）；
- 右键「查看音源详情」「删除该音源」对两类音源都生效，删除会同步清理缓存文件
  与持久化记录；
- 非 MusicFree 音源持久化在 `<exe>/musicfree/protocols/index.json`，脚本/配置
  落在同级 `lx/`、`rest/`、`listen1/` 子目录，启动时自动重建。

## 洛雪脚本示例

```js
/**
 * @name 示例音源
 * @version 1.0.0
 * @author me
 */
lx.on('request', async (info) => {
  if (info.action === 'musicUrl') return { url: 'https://cdn.example/a.mp3' };
  throw new Error('unsupported action');
});
lx.send('inited', {
  sources: {
    kw: { name: '酷我', type: 'music', actions: ['musicUrl'] },
    wy: { name: '网易云', type: 'music', actions: ['musicUrl'] }
  }
});
```

宿主提供 `lx.on / lx.send / lx.request / lx.utils`（含 crypto、buffer 垫片），
`lx.request` 走项目既有的 WinHTTP 链路并同步回调，Promise 由
`QuickJsEngine::Await` 泵完，不需要额外事件循环。source key 固定为
`kw / kg / tx / wy / mg / local`，音质为 `128k / 320k / flac / flac24bit`；
MusicFree 的 platform 名会按 `MapToLxSource()` 映射过去。

## REST 音源配置示例

```json
{
  "protocol": "rest",
  "preset": "custom",
  "name": "我的音源",
  "version": "1.0.0",
  "baseUrl": "http://127.0.0.1:3000",
  "headers": { "X-Token": "..." },
  "auth": { "user": "", "password": "", "token": "" },
  "search": {
    "path": "/search",
    "params": { "keywords": "{query}" },
    "result": "result.songs",
    "fields": { "id": "id", "title": "name", "artist": "artists[0].name" }
  },
  "resolve": {
    "path": "/song/url",
    "params": { "id": "{id}" },
    "url": "data[0].url"
  }
}
```

- 模板变量：`{query} {id} {page} {offset} {limit} {user} {password} {token}
  {salt} {client} {version} {format}`，除数字外自动 URL 编码；
- JSON 路径支持点号与数组下标（`result.songs` / `data[0].url`）；
- `preset` 可取 `neteaseapi`（NeteaseCloudMusicApi）、`subsonic`
  （Navidrome / Subsonic）、`listen1api`，未写的字段由内置预设补齐；
- 所有请求沿用 `NetworkConfig`（代理 / 超时 / 跟随重定向），不关闭 TLS 校验。

## 播放地址解析链

```
在线曲目 (platform, id, raw)
  → 存在对应 MusicFree 插件 → GetMediaSource(完整 raw)   [成功即返回]
  → 否则 / 插件失败 → SourceProtocolRegistry::ResolveFallback
        → 洛雪脚本（按歌名、歌手、songmid/hash/copyrightId 等私有字段）
        → REST 音源（按 resolve 模板请求）
```

`OnlineSearchHit::raw` 现在保留音源返回的**完整对象**，入列时序列化进播放列表
元数据袋的 `SourceRaw` 键；从列表重新播放、下载时用 `RestoreSourceRaw()` 取回。
这同时修掉了此前「用 `{id, platform}` 重建对象 → 咪咕插件缺
`contentId/copyrightId` → 插件调用被拒绝」的问题。旧条目没有 `SourceRaw` 时，
会退回用 Title / Artist / Album / Platform / SongId 拼一个最小对象。

## 导入去重与批量管理

「导入音源」窗口的列表是 **多选列表**（`LBS_EXTENDEDSEL`）：可按住 `Ctrl` 逐项
复选、`Shift` 连选、拖拽框选，或用「全选」一次选中全部，再点「删除所选」
一次清理。右键落在已复选的行上时会保留整组选择，菜单显示
「删除所选 N 个音源」；落在未选中的行上则只选中该行，避免误删。

批量删除把选中行按类型拆成两组，分别交给
`MusicFreeSourceManager::RemovePluginsAt()` 与
`SourceProtocolRegistry::RemoveMany()`：两者都先固化目标、倒序移除实例、
最后只写一次持久化文件。共用同一缓存 JS 的重复插件实例会一起清掉，
不会因为下标前移而错位或漏删。

导入去重分三层，全部以「音源文件内容哈希（FNV-1a）+ 来源 URL + 缓存路径」
为判据：

| 场景 | 行为 |
| --- | --- |
| 同一 URL 再次导入、内容相同 | 直接跳过，结果框显示「已跳过重复导入」，列表不新增 |
| 同一份脚本换 URL / 换本地文件名导入 | 内容哈希命中，同样跳过 |
| 同一 URL 但内容变了 | 视为更新：先摘掉旧条目与其缓存文件，再挂载新版本，列表仍只留一条 |
| 重复导入整个 MusicFree 注册表 JSON | 已存在的插件连下载都省掉，只报告「跳过重复插件 N 个」 |
| 旧版 `imported.json` / `protocols/index.json` 已有重复记录 | 启动时按内容哈希去重并回写索引，不再挂出多份同名音源 |

`LoadCached()` 会把 `plugin_urls` 与 `plugin_files` 按同一索引配对恢复，
去重时同步丢弃对应 URL，保证 `DropPluginFile()` 的平行关系不被破坏。

端到端验证脚本：`.test/e2e_batch.py`（预置重复 REST 音源 → 真机启动 →
复选删两行 → 同一 URL 导入两次 → 校验第二次被跳过），运行结束后自动恢复
索引与缓存文件。

## 音源自动更新

复刻原版 MusicFree 的插件更新机制（`PluginManager.updatePlugin` /
`installPluginFromUrl`）：

1. **来源记忆**——导入时每个音源都记住下载地址：MusicFree 插件记在
   `imported.json` 的 `plugin_urls`，洛雪 / REST / Listen1 记在
   `protocols/index.json` 的 `source_url`（对应原版插件实例的 `srcUrl`）。
2. **重新下载**——更新检查对每个来源带 `Cache-Control: no-cache` 重新下载，
   与原版一致，防止中间缓存把旧脚本再吐回来。
3. **哈希比对**——内容哈希与本地相同 → 视为「已是最新」，不动本地文件。
4. **版本比较拒绝降级**——内容变化时先探针挂载读新版本号，按
   `compare-versions` 语义（`CompareSourceVersions`，纯数字段按数值、缺段按
   0、忽略前导 v）比较：本地版本较新 → 拒绝覆盖。
5. **原地替换**——缓存文件名由 URL 派生（路径不变），覆写后重新挂载，
   播放列表里的既有曲目不受影响。

触发方式两种：

- **自动**——启动后台线程在音源重建完成后检查一次；开关持久化在
  `<exe>/musicfree/update_state.json`（`auto_check` / `last_check`），间隔
  24 小时。导入窗的「启动时自动检查更新」复选框控制开关。
- **手动**——导入窗「检查更新」按钮：后台线程执行（UI 不卡顿），完成后
  结果框显示逐音源报告（已更新 / 已是最新 / 拒绝降级 / 失败原因），列表
  自动刷新。

本地导入（无 URL）的音源记为「没有更新源」跳过，不会被误删。
进程退出时检查在插件之间可被中断，不会拖住关闭。

## 诊断

设 `TTPLAYER_MF_TRACE=1` 后，日志写到 `%TEMP%\ttplayer_mf_http.log`：

- `PROTOCOL_FALLBACK`：命中兜底音源时的 platform / id / via / url；
- `MF_SELFTEST_SEARCH`、`MF_SELFTEST_RESOLVE`、`ONLINE_TRACK_STORED`：
  把搜索结果、解析地址、入列后的字段串成一条可逐行核对的链路。

## 当前限制

- Listen1 provider 依赖浏览器宿主对象（get/post/ajax/angular、crypto-js），
  本版本只做识别与元数据登记，不参与搜索与解析；
- 洛雪音源不提供搜索，因此单独导入洛雪脚本时，音乐窗仍需要至少一个
  MusicFree 插件或 REST 音源才能搜到歌；
- 本仓库不内置任何音源脚本、插件、音乐文件与账号凭据，配置里的服务地址
  由使用者自行提供。
