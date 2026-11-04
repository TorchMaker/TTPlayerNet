# 上游合并待移植清单

本文件记录「合入 Sonic853/TTPlayer 至 `f4f2d25`」这次合并中**未自动合并**的文件。

## 背景

我方仓库 `TTPlayerNet` 最初是 2026-09-21 的一次性源码导入，与上游没有共同祖先。
本次合并通过 blob 哈希比对定位到分叉基线 **`296474b`（2026-09-20，tag 2026.09.20）**，
以它为 merge base 做内容级三方合并，并生成带双亲
（`7845a8b` + `f4f2d25`）的合并提交，从而建立上游血缘。

- 无冲突部分：**全量合并**（806 个文件 / 355 个新增 / 511 个变更，零冲突标记）
- 冲突部分：21 个文件，双方都改过且上游大幅重写，文本合并会残留约 96 个冲突块，
  强行落地必然编译失败 → **保留我方版本**，按下表逐个移植
- 目录重写误判：上游重构了 `src/lyrics/`，`git merge-tree` 把我方独有的
  `include/ttplayer/lyrics/lyric_http.h` 与 `src/lyrics/lyric_http.cpp` 误判为删除，
  已从 `backup/pre-merge-20261105` 恢复。二者是 MusicFree 在线歌词层，必须保留。

## 冲突文件清单

`ours / theirs` 表示相对基线 `296474b` 的增删行数。

| 文件 | ours | theirs | 冲突块 | 备注 |
| --- | --- | --- | --- | --- |
| `.github/workflows/manual-build.yml` | 删除 | +216/-176 | 0 | 我方有意移除，保持删除 |
| `CMakeLists.txt` | +53/-1 | +344/-93 | 4 | 构建入口，上游新增模块需在此登记 |
| `README.md` | +402/-190 | +23/-8 | 3 | 我方产品定位，保持我方 |
| `README_chs.md` | 删除 | +28/-17 | 0 | 我方已移除，保持删除 |
| `docs/BUILDING.md` | +7/-0 | +220/-48 | 1 | 构建说明 |
| `include/ttplayer/app/application.h` | +9/-0 | +2/-0 | 1 | 自研入口扩展 |
| `include/ttplayer/settings/settings.h` | +9/-0 | +78/-11 | 1 | 上游新增设置项 |
| `include/ttplayer/ui/player_window.h` | +100/-0 | +361/-44 | 1 | **接口变化大**，优先级高 |
| `src/app/resources.rc` | +31/-0 | +1/-88 | 1 | 图标/版本资源 |
| `src/audio/audio_engine.cpp` | +2/-1 | +1574/-716 | 2 | WASAPI / DSP 断续修复所在 |
| `src/settings/settings.cpp` | +28/-5 | +374/-66 | 3 | 设置持久化 |
| `src/skin/skin.cpp` | +406/-26 | +442/-805 | 2 | 我方高 DPI 适配在此 |
| `src/ui/desktop_lyrics.cpp` | +77/-23 | +126/-115 | 2 | 视觉效果支持 |
| `src/ui/player_window.cpp` | +417/-208 | +2121/-534 | 13 | **主窗口，最复杂** |
| `src/ui/player_window_equalizer.cpp` | +44/-33 | +271/-306 | 9 | 均衡器 |
| `src/ui/player_window_internal.h` | +48/-12 | +93/-7 | 3 | 内部状态结构 |
| `src/ui/player_window_lyrics.cpp` | +80/-66 | +1443/-331 | 12 | 歌词搜索 / ttp_lrcsh |
| `src/ui/player_window_options.cpp` | +48/-7 | +1535/-614 | 1 | 选项属性表 |
| `src/ui/player_window_playlist.cpp` | +194/-99 | +1181/-383 | 36 | **冲突最密集** |
| `src/ui/player_window_playlist_network.cpp` | +437/-1 | +116/-0 | 2 | 我方 MusicFree 网络列表 |
| `tests` | 删除 | 改 gitlink | 0 | 保持删除 |

## 建议移植顺序

1. `include/ttplayer/ui/player_window.h` + `src/ui/player_window_internal.h`
   —— 上游改了成员与消息映射，其它文件都依赖它
2. `CMakeLists.txt` —— 决定是否启用上游新增的 `third_party/ttpcomm-sdk`、
   `third_party/ttpres-sdk`、`src/audio/*` 等模块
3. `src/audio/audio_engine.cpp` —— WASAPI 断续修复、DSP 虚拟注册表
4. `src/ui/player_window_playlist.cpp` / `player_window.cpp` —— 逐块人工合并
5. 其余 UI 文件

## 今后同步方式

```bash
git fetch upstream --tags
git merge upstream/master      # merge-base 现在是 f4f2d25，可正常三方合并
```

上游 remote：`https://github.com/Sonic853/TTPlayer.git`。
合并前的本地快照保存在分支 `backup/pre-merge-20261105`（`7845a8b`）。
