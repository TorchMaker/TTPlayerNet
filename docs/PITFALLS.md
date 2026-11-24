# 踩坑记录（PITFALLS）

按类别记录真机调试 / 构建 / 发布中实际踩过的坑：现象 → 根因 → 解法。
新增条目附日期，跨轮复用，避免重复踩。

---

## 一、GitHub 推送 / 代理 / 凭据（2026-11-24）

### 1. GCM（Git Credential Manager）遇到 socks5 代理直接 fatal

- 现象：`git push github` 报
  `fatal: ServicePointManager 不支持具有 socks5 方案的代理。`（socks5h 同理）
- 根因：credential helper 配置的是 workbuddy 便携版
  `git-credential-manager.exe`（.NET / ServicePointManager），只支持 http 代理。
  **两种配置方式它都看得到**：
  - `git -c http.proxy=socks5://...` → 经 `GIT_CONFIG_PARAMETERS` 环境变量注入所有子进程；
  - `$env:https_proxy='socks5://...'` → GCM 启动时读标准代理环境变量。
- 解法（已验证可用的完整命令）：清空 helper 列表改用原生 `wincred`
  （Windows 凭据库同一条目 `git:https://github.com`，wincred 直接可读），
  代理只交给 libcurl：

  ```powershell
  git -c "credential.helper=" -c "credential.helper=wincred" `
      -c "http.proxy=socks5://127.0.0.1:1080" `
      -c "http.postBuffer=524288000" -c "http.version=HTTP/1.1" `
      push github master:main
  ```

### 2. PowerShell 下 `socks5h` 方案不支持

- 现象：报错 `ServicePointManager 不支持具有 socks5h 方案的代理`。
- 根因：`.NET` 后端只认 `http://` / `https://`；git 的 libcurl 后端两种都认。
- 解法：给 libcurl 用 `socks5://`（不带 h）；给 GCM 干脆换 `wincred`（见上条）。

### 3. 代理未开时的两种失败表现

- `Failed to connect to github.com:443 over proxy 127.0.0.1 after ~2056 ms`：
  socks 端口无监听（或代理上游未就绪）。
- 不带代理直连：`握手失败 / 基础连接已经关闭`（网络环境直连 GitHub 不通）。
- 判定手段（不依赖 git）：手工 SOCKS5 探测——TCP 连 1080 → 发
  `05 01 00` greeting → 收 `05 00` → 发 CONNECT `github.com:443` → 收 `rep=0x00`
  即代理链路正常，问题在 git 配置侧。
- 注意：`Get-NetTCPConnection -State Listen` 可能查不到 1080 监听，但裸
  `TcpClient` 能连通（环境怪象），以实际连接测试为准。

### 4. 残留 push 进程并发挤同一条链路

- 现象：多次超时/失败的 push 残留 `git` / `git-remote-https` /
  `git-credential-manager` 进程，后续 push 与它们并发互相干扰，输出交错难读。
- 解法：重推前先
  `Get-Process git,git-remote-https,git-credential-manager | Stop-Process -Force`。
  **GCM 是独立进程名**，只杀 `git` 杀不掉它。

### 5. 大对象推送 HTTP 408

- 现象：`RPC failed; HTTP 408 ... The requested URL returned error: 408`。
- 根因：安装包 zip（约 27MB）经 socks 代理上传慢，接收端超时。
- 解法：`http.postBuffer=524288000` + `http.version=HTTP/1.1`。

### 6. 排查与验证方法

- `GIT_TRACE=1 git push ...` 可看到完整调用链（哪一步拉起 GCM、GCM 读了哪些
  配置），本次就是靠它定位到 `git credential-manager get` → `git config list`
  之后 fatal。
- 推完必须三方对齐验证：
  `git ls-remote github refs/heads/main` / `git ls-remote origin refs/heads/master`
  / `git rev-parse HEAD` 三者相同才算成功（`Everything up-to-date` 出现在失败
  输出里时有误导，以 ls-remote 为准）。
- `TaskOutput` 工具查不到 Shell 后台命令的 id，Shell 转后台的任务等系统通知即可。

---

## 二、在线音频播放（2026-11-15）

### 1. `MFCreateSourceReaderFromByteStream` 报 0xC00D36C4

- 现象：取链成功（`resolve done ok=1`）但 `audio_play ok=0`，
  `MF_E_UNSUPPORTED_BYTESTREAM_TYPE`（不支持给定的 URL 的字节流类型）。
- 根因：自定义 `IStream` 包成的 `IMFByteStream` 没有内容类型/文件名属性。
- 解法：按直链扩展名 QI `IMFAttributes` 补
  `MF_BYTESTREAM_CONTENT_TYPE`（audio/mpeg 等）与 `MF_BYTESTREAM_ORIGIN_NAME`。
- 附带坑：`IMFByteStream` 接口本身**没有** `SetString`（C2039），必须
  `ComPtr::As(&attributes)` 转到 `IMFAttributes` 再设。

### 2. 首包竞态：同一首歌有时能开有时打不开

- 现象：`first=49 44 33...`（首字节已是合法 MP3）仍报 0xC00D36C4，间歇性。
- 根因：HTTP 响应/首字节未到达就把流交给 MF，MF 探测失败即拒绝。
- 解法：`HttpStreamSource::Open` 里先 `WaitReady(3000ms)`（等首包或网络错误）
  再创建源读取器；超时给 `online stream produced no data within 3 seconds`。

### 3. `IStream::Read` 空缓冲跳读

- 现象：MF 跳过 ID3v2/探测帧同步时以 `pb=NULL` 调 Read，旧实现返回
  `STG_E_INVALIDPOINTER` 导致打开失败。
- 解法：`pv == nullptr` 时按 IStream 约定只前移读指针，`*pcbRead = 实际跳过字节数`。

### 4. 后台缓存写半成品文件被当本地副本打开

- 现象：`local_copy hit → audio_play ok=0 error=reader slot 3 Open(IStream,
  flags) failed (0x8BDA000A)`，时好时坏；下载完成后恢复正常。
- 根因：「边播边落盘」直接写最终文件，扫描线程把写了一半的同名文件当成本地副本。
- 解法：先写 `<name>.part`，下载完成再 `rename` 原子改名；失败删半成品。
- 附带坑：`DefaultDownloadName` 已含扩展名时再 `+ GuessExtension` 产生
  `xxx.mp3.mp3` 双扩展名——追加前先判断是否已有扩展名。

### 5. 本地副本被 reader 拒绝时的兜底

- 现象：本地文件也报 `0x8BDA000A`（上一遍播放未释放/文件刚被重写），旧逻辑直接
  `ShowAudioError`「无法打开网络媒体」并触发每 2~3 秒一次的失败重试循环。
- 解法：在线曲目本地副本打开失败 → 自动回退直链流式（`local_fail_stream`），
  不弹错误、不进重试循环。

### 6. 解析风暴

- 现象：失败后 `serial=8,9,10...15` 每秒一次重新取链刷日志。
- 解法：同曲 10 秒窗口内最多 2 次后台取链，超出记 `PLAYLIST give_up`。

### 7. SDK 常量缺失 / 取链丢 headers

- `ERROR_HTTP_INVALID_STATUS_CODE` 部分 Windows SDK 的 `winhttp.h` 未导出
  （C2065）→ 用字面值 `12159`。
- 播放列表取链用 `ResolvePlayUrl`（只返回 url）会丢插件给的 Referer/UA →
  改用 `ResolvePlayUrlEx`（带 headers/user_agent），CDN 校验头的源才能播。

---

## 三、视频源解析（2026-11-24）

### 1. 豆瓣资料页被当成播放线路弹出来

- 现象：点影片直接弹出豆瓣网页；`play_fail.log` 显示线路是
  `https://www.douban.com/doubanapp/dispatch/movie/...`，5 个 parses 接口全失败。
- 根因：豆瓣片单 spider（`DouDouEngine::Detail`）只造一条「豆瓣页面」线路
  （sharing_url / subject 页），它是资料页不是播放页；解析失败后 WebView2
  兜底把它整个打开。
- 解法（三层）：
  1. 桥层 `video.open` 把「非直链 + douban 域名」线路整条剔除（同网盘线路机制），
     剔光后 `lines.empty()` 自动触发跨源详情回退去其它源找可播线路；
  2. WebView2 兜底 `TryStartWebFallback` 拒绝豆瓣域名（历史记录里的旧线路兜底）；
  3. 跨源详情抢答 `FindCrossSourceDetail` 同样过滤豆瓣线路，防止抢答选中它自己。
- 注意：豆瓣线路剔光**不能**复用 `cloud_only` 的「网盘站学习」逻辑
  （`MarkCloudSite`），否则豆瓣片单会被误标成网盘站而在跨源扫描中被跳过。

### 2. 跨源直链扫描串行 25s 只够扫 8/26 站

- 现象：`cross_source.log` 对比——11-14 扫 10/26 命中 14 条直链，
  11-24 扫 8/26 超时未命中（站点普遍变慢，单站 6s 超时吃光预算）。
- 解法：`FindCrossSourceDirectLines` 从串行改 6 线程并行抢答（与
  `FindCrossSourceDetail` 同构），同预算扫完全部候选；URL 去重必须在锁内做。
- 日志：新增逐站 `SCAN 《片名》 site=xx 耗时:xxms result=...`
  （no-match / detail-no-direct / hit / dup-only / error），后续分析看这里。

---

## 四、构建 / 仓库杂项

- 仓库根存在名为 `nul` 的杂项文件，导致 ripgrep 全仓库搜索报
  `rg: ...nul: 函数不正确` —— 搜索时按具体子目录指定 path 绕开。
- `git status --porcelain --cached` 是无效选项（129），看暂存区用
  `git diff --cached --name-only`。
- `tools/` 整目录在 .gitignore 第 1 行（历史约定），新增探针源码不会进版本库，
  但 CMakeLists 引用 `tools/*.cpp` 是既有模式（HEAD 已如此），照旧即可。
- 打包约定（2026-11-07 用户更正）：**编译后只部署 exe；推送时才做 ISCC 打包 +
  哈希清单 + 静默安装校验 → 复制进仓库 `dist/` → 与代码同批提交 → 双端推送**。
  流程与命令详见 `.workbuddy/memory/2026-11-07.md` 与 `2026-11-05.md`。
