# 原生 Spider（recipe 驱动，禁用 Java）解决方案

## 1. 背景与目标

TVBox 的生态里有大量社区 spider（`csp_*.json`），原本只能在 TVBox 的 **Java 运行库**里执行。
本项目目标是把这些 spider 还原成 **纯 C++ 原生实现**，从而：

- 彻底摆脱 Java / JS 运行库依赖（`kind` 从 `kSpider` 翻转为 `kNativeSpider`，可播）；
- 复用从 TVBox `smali` 反编译还原的 recipe 配置（`sources/spiders/csp_*.json`），
  recipe 已包含 `base / headers / request{ sign, encoding } / endpoints{ home, category, detail, search, player } / extract{ listing, detail, player }` 等全部行为信息。

> 一句话：**recipe 就是 TVBox 的"JS spider"等价物，本项目用 C++ 直接解释执行它。**

## 2. 方案总览

| 模板 | 引擎 | 来源 | 状态 |
| --- | --- | --- | --- |
| `cms` | `RecipeCmsEngine` | 项目既有 | 自动注册 |
| `html` | `RecipeHtmlEngine`（CSS 选择器 / 正则） | 项目既有 | 自动注册 |
| `pan` / `live` / `app` | `RecipeApiEngine`（通用声明式引擎） | 本次新增 | 自动注册（仅承接未被专用引擎覆盖者） |
| `pan` / `live` / `app`（已实测跑通） | `SzpsEngine` / `MyDriveEngine` / `PushEngine` / `AllliveEngine` / `BiliEngine` / `AppEngineBase`(`AppTT`/`JPJ`) | 项目既有 | 已注册 |

核心机制：扫描 `SpiderRecipeDirs()` 下的所有 `csp_*.json`，按模板把每个 recipe 注册进
`SpiderRegistry`，引擎在调用时靠 `ctx.spider_id` 各自 `LoadSpiderRecipe` 加载自己的配置。

### 2.3 内嵌 recipe（解决「部署版找不到文件」——这是「还提示需 Java」的真正根因）

`SpiderRecipeDirs()` 只探测 `<exe>\spiders` 与向上 6 层找 `sources/spiders`。**部署版 exe 周围
既没有 `spiders` 目录、也不在源码树里**，于是运行时一个 recipe 都没注册到——这正是用户在导
入里看到一堆「原生未实现（csp_Xxx），需要 TVBox 的 Java 运行库」的根因（其实它们不需要 Java，
只是 recipe 没被加载）。

修复：`tools/gen_embedded_spider_recipes.ps1` 把 `sources/spiders` + `TTPlayer/spiders` 下全部
37 个 `csp_*.json` 以 **Base64** 内嵌进 `src/video/spider/embedded_spider_recipes.inc`，再由
`spider_recipe.cpp` 在编译期 `#include` 进二进制。两套加载路径都加上了内嵌回退：

- `LoadSpiderRecipe`：目录未命中 → 从内嵌表按 id 解码；
- `RegisterRecipeSpiderVariants`：目录扫描之后，再扫内嵌表注册。

因此 **recipe 随 exe 一起分发，任何部署形态都可用**，无需任何外部文件。新增/修改 recipe 后重跑
脚本重新生成 `.inc` 即可（CMake 也可加一步 `add_custom_command` 自动生成）。

### 2.1 归一化键（为什么 `csp_MusicGuard` 能命中 `csp_Music`）

站点配置里的 `api` 写的是 `csp_MusicGuard`，而 recipe 文件的 `id` 是 `csp_Music`。
`NormalizeSpiderId`（`spider_registry.cpp`）会同时剥掉 `csp_` 前缀与尾部 `Guard` → 都归一成裸名
`music`；注册与查询都以该裸名（小写）为键，因此二者精确命中。

### 2.2 kind 翻转（`video_config.cpp:565-588`）

当站点 `api` 含 `csp_`：查注册表 → 命中且 `meta->implemented` 为真 →
`site.kind = VideoSourceKind::kNativeSpider`、`native_ready = true`、清空 `why_unsupported`。
否则保持 `kSpider` 并提示"需要 TVBox 的 Java 运行库"。

## 3. 已解决的站点（cms / html）

`RegisterRecipeSpiderVariants(SpiderTemplate, factory)`（`spider_recipe.cpp`）遍历 recipe 目录，
把指定模板的 `csp_*.json` 全部注册为原生 spider。已在两个静态引导对象里调用：

- `CmsEngineBootstrap` → 注册所有 `cms` 模板（含 `csp_T4`）；
- `HtmlEngineRegistrar` → 注册所有 `html` 模板（共 16 个：`Music / Seedhub / NewCz / Libvio /
  WoGG / YGP / Bttwoo / Nmyswv / SixV / Auete / Dm84 / Kanqiu / FirstAid / KkSs / UuSs / XPath`）。

共 **17 个站点**由"需 Java"变为原生可播（零 Java 运行库）。

## 4. 通用声明式引擎 `RecipeApiEngine`（本次新增）

文件：`src/video/spider/spider_engine_recipe_api.cpp`。

针对 `pan / live / app` 这类**接口型** recipe（`endpoints` + `extract` + `request`），用一个引擎
统一解释执行，支持：

- `request.method`：`GET` / `POST`（`application/x-www-form-urlencoded`）；
- `request.sign`：`none` / `md5` / `sha256`（按 `fields` 模板拼接后哈希，支持 `{uuid}`/`{timestamp}`/`{nonce}`/`{appkey}` 等占位）；
- 路径 `{x}` 占位与查询/表单参数解析（来源支持 `keyword / page / type_id / vod_id / timestamp / uuid / nonce / ext.* / 字面量`）；
- 响应解析：`su::ParseJsonLoose` + `su::JsonFindAll/JsonFind` JSONPath-lite 提取为
  `VideoListing / VideoDetail / VideoLine / VideoEpisode`；
- **`response.decrypt: aes-cbc`**：新增的 `su::AesCbcDecrypt`（`spider_util.cpp`，基于 BCrypt）
  用来解密"响应体 AES 加密"的 app 站点（如 `App99` / `YueTing`），一旦在 recipe 里补上
  `key/iv` 即可直接点亮。

`RegisterRecipeApiSpiders()` 只注册 **pan/live/app** 中**尚未被专用引擎注册**的 recipe
（`SpiderRegistry::Has(id)` 跳过），因此：

- 不会覆盖已实测跑通的 `Alllive / Bili / AppTT / JPJ / S_zps / YpanSo / BpanSo / MyDrive / Push`；
- 也不会把"原生未实现"的站点误标成可用（专用引擎对它们的 `implemented=false` 优先保留）；
- 新加的 recipe 若没有专用引擎，通用引擎即可接管——完成 5 种模板统一的"recipe → 原生"变异。

## 5. 剩余阻断站点（需外部密钥 / 服务 / 源码，非代码缺口）

以下站点已被专用引擎正确地标为 `implemented=false`。它们**无法仅靠代码解决**，需要外部依赖。

> 提示文案已修正（`video_config.cpp`）：这些站点属于「**已复刻成 C++ 原生，但当前被外部依赖阻断**」，
> 因此导入时显示的是各自的真实原因（来自 `meta.note`，如"需配置各盘 token/cookie""依赖本机
> :9978 代理""响应体 AES 加密 key 未知"），**不再笼统写成"需要 TVBox 的 Java 运行库"**——它们根本
> 不需要 Java。只有**完全未被注册**的纯 `csp_*` 类名才提示"需要 Java"。

| 站点 | 模板 | 阻断原因 | 启用所需 |
| --- | --- | --- | --- |
| `csp_MyDrive` | pan | 需各网盘（夸克/UC/阿里/百度）的 token / cookie，来自 `ext` | 在站点 `ext` 配置好网盘凭据后，专用引擎即可工作 |
| `csp_Push` | pan | 依赖本机 `:9978` 代理通道（TVBox 本地服务） | 部署对应代理服务，无可移植性 |
| `csp_Fy` | live | 34KB 内嵌 drpy Python + OCR 验证码，recipe 无法描述 | 需要该 Python 源码做 C++ 等价移植 |
| `csp_App99` | app | 响应体 AES 加密，`key/iv` 未从 smali 静态确认 | 逆向出 `key/iv` 后写入 recipe 的 `response.decrypt` |
| `csp_YueTing` | app | `payload` 经 AES 加密，`key/iv` 来自 h5api 引导响应 | 同上（逆向 key/iv） |
| `csp_Appgz / Hmys / Jpys / AiDj / AppSx` | app | 未知 AES key / 设备注册 / 签名公式 | 逆向对应密钥或注册流程 |

> 注：`su::AesCbcDecrypt` 已就位，上述 AES 站点只要补齐 `key/iv` 即可被 `RecipeApiEngine`
> 直接解密；无需再改引擎代码。

## 6. 如何扩展 / 启用新站点

1. 在 `sources/spiders/`（及运行时 `<exe>\spiders\`）放一份 `csp_Xxx.json`，含 `id / template /
   base / endpoints / extract`（可选 `request.sign`、`response.decrypt`）。
2. 若该模板已有专用引擎覆盖，直接被其注册；否则 `RecipeApiEngine` 自动接管（默认 `implemented=true`）。
3. 对 AES 加密站点，在 recipe 加：
   ```json
   "response": { "decrypt": "aes-cbc", "key": "<原始字节或hex>", "iv": "<16字节>" }
   ```
4. 重新构建即可；站点 `api` 写 `csp_XxxGuard` 或 `csp_Xxx` 都能命中。

## 7. 编译说明

- 工具链：Visual Studio 2022/2026 生成器，**必须 Win32 / x86**（`TTPlayerRebuild` 强制 x86）。
  ```
  cmake -G "Visual Studio 18 2026" -A Win32 ..
  ```
- 依赖：CMake 配置期会用 `FetchContent` 从 SourceForge 拉取 **WTL 10.01**——需要联网；
  离线环境无法完成首次配置（与本项目代码无关）。
- 本项目代码本身已通过 clangd 校验（0 诊断），并用项目真实工具链（MSVC 19.51 / C++20 / Win32）
  对 5 个 spider 翻译单元做了 `cl /c` 独立编译验证（0 错误，含内嵌 `.inc`）。

### 7.1 内嵌 recipe 的构建集成（可选）

`embedded_spider_recipes.inc` 当前由脚本手动生成并提交。若希望 recipe 改动后自动重新生成，可在
`CMakeLists.txt` 加：

```cmake
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/video/spider/embedded_spider_recipes.inc
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
          -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_embedded_spider_recipes.ps1
          -RepoRoot ${CMAKE_CURRENT_SOURCE_DIR}/..
          -OutFile ${CMAKE_CURRENT_SOURCE_DIR}/src/video/spider/embedded_spider_recipes.inc
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_embedded_spider_recipes.ps1
          ${CMAKE_CURRENT_SOURCE_DIR}/../sources/spiders
  COMMENT "生成内嵌 spider recipe")
# 并把该 .inc 加入 ttplayer_core 的依赖/源列表。
```
