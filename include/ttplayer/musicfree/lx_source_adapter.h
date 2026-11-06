#pragma once

// 洛雪 / LX Music 自定义音源适配器
//
// 协议（v2.0.0，桌面端与移动端通用）：
//   globalThis.lx = { version, env, currentScriptInfo, EVENT_NAMES,
//                     on, send, request, utils }
//   脚本用 on('request', handler) 注册处理器，末尾 send('inited', { sources })；
//   宿主按 {source, action, info} 调 handler（必须返回 Promise）：
//     musicUrl → 音频直链；lyric → {lyric, tlyric, rlyric, lxlyric}；pic → 图片 URL
//   sources 的键固定为 kw/kg/tx/wy/mg/local，音质为
//   128k / 320k / flac / flac24bit。
//
// 关键差异：洛雪音源**不提供搜索**。它只把「歌曲信息」换成直链，因此在本项目里
// 承担两条职责：① 播放地址解析的补充通道（MusicFree 插件解析失败时兜底）；
// ② 为 local 源提供歌词/封面。搜索仍由 MusicFree 插件或 REST 音源完成。
//
// 实现要点：lx.request 落到项目既有的 WinHTTP 后端并**同步**回调，Promise 的
// 续体全部留在微任务队列，由 QuickJsEngine::Await 泵完 —— 与 axios 后端同一
// 套机制，不需要额外事件循环。

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/musicfree/source_protocol.h"

namespace ttplayer::musicfree {

namespace detail {
struct LxBridge;  // 实现细节（持有 QuickJS 上下文与事件状态）
}

class LxSourceAdapter {
public:
    LxSourceAdapter();
    ~LxSourceAdapter();
    LxSourceAdapter(const LxSourceAdapter&) = delete;
    LxSourceAdapter& operator=(const LxSourceAdapter&) = delete;

    // 挂载一份洛雪脚本。失败返回 false，error 写入原因。
    bool Mount(std::string_view code, const NetworkConfig& network,
               const std::filesystem::path& lib_dir,
               std::string_view source_url, std::string* error);

    const ProtocolSourceInfo& info() const;
    bool IsMounted() const;

    // 用音源声明的 capabilities 尝试解析播放直链。
    ProtocolResolveResult ResolveUrl(const ProtocolTrack& track);

    // lyric / pic 仅 local 源声明时可用。
    std::optional<LyricResult> ResolveLyric(const ProtocolTrack& track);
    std::string ResolvePic(const ProtocolTrack& track);

    // 多行详情（导入窗口「查看」用）。
    std::string Describe() const;

private:
    std::unique_ptr<detail::LxBridge> bridge_;
};

} // namespace ttplayer::musicfree
