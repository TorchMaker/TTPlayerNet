#pragma once

// MusicFree 音源兼容层 —— 类型定义
//
// 这些结构是对 MusicFree 插件契约 (src/types/plugin.d.ts -> IPlugin.IPluginDefine)
// 的 C++ 映射。MusicFree 插件是 CommonJS 模块，导出 search / getMediaSource /
// getLyric 等 async 方法；本层用 QuickJS 沙箱运行这些 JS，并把结果转成下面的
// C++ 类型交给 TTPlayer 的播放列表 / 音频引擎 / 歌词子系统使用。
//
// 参考：MusicFree_src/MusicFree-master/src/core/pluginManager/plugin.ts 的
//       _require / mountPlugin 与 IPlugin 接口。

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ttplayer::musicfree {

// 音质键，与 MusicFree IMusic.IQualityKey 一致
enum class QualityKey {
    kStandard,   // standard  标准
    kHigh,       // high      较高
    kSuper,      // super     极高
    kSurpass,    // surpass   无损
    kLossless,   // lossless  无损
    kHires,      // hires      Hi-Res
};

inline std::string_view ToString(QualityKey q) {
    switch (q) {
        case QualityKey::kStandard: return "standard";
        case QualityKey::kHigh:     return "high";
        case QualityKey::kSuper:    return "super";
        case QualityKey::kSurpass:  return "surpass";
        case QualityKey::kLossless: return "lossless";
        case QualityKey::kHires:    return "hires";
    }
    return "standard";
}

inline QualityKey QualityFromView(std::string_view s) {
    if (s == "high")     return QualityKey::kHigh;
    if (s == "super")    return QualityKey::kSuper;
    if (s == "surpass")  return QualityKey::kSurpass;
    if (s == "lossless") return QualityKey::kLossless;
    if (s == "hires")    return QualityKey::kHires;
    return QualityKey::kStandard;
}

// 网络配置：从 TTPlayer 的 settings::NetworkSettings 适配而来，交给沙箱内的
// axios 后端 (WinHTTP) 使用。
struct NetworkConfig {
    std::wstring proxy;          // "host:port" 或空
    std::wstring proxy_user;
    std::wstring proxy_password;
    std::uint32_t timeout_ms = 20000;
    std::wstring user_agent;     // 未指定时沿用插件返回/默认 UA
    bool verify_tls = true;      // 永不在本层禁用 TLS 校验（与歌词子系统一致）
};

// 一首歌曲（MusicFree IMusic.IMusicItem）。保留全部原始字段以便回传给插件。
struct MusicItem {
    std::string id;
    std::string platform;        // 音源名，例如 "网易"
    std::string title;
    std::string artist;
    std::string album;
    std::int64_t duration_ms = 0;
    std::string artwork;         // 封面 URL
    std::string url;             // 直接播放地址（兜底）
    nlohmann::json raw;          // 插件返回的完整对象，原样保存

    // 用插件返回的 JSON 构造，并抽取常用字段
    static MusicItem FromJson(const nlohmann::json& j);
    nlohmann::json ToJson() const { return raw; }
};

// getMediaSource 的返回（MusicFree IMediaSourceResult）
struct MediaSource {
    std::string url;
    std::map<std::string, std::string> headers;
    std::string user_agent;
    QualityKey quality = QualityKey::kStandard;
};

// search 的返回（MusicFree ISearchResult）
struct SearchResult {
    bool is_end = true;
    std::vector<MusicItem> items;
    std::string error;
};

// getLyric 的返回（MusicFree ILyric.ILyricSource）
struct LyricResult {
    std::string raw_lrc;         // 原文
    std::string translation;     // 翻译
};

// 单个已挂载的 MusicFree 插件元信息
struct PluginMeta {
    std::string name;            // platform 名
    std::string version;
    std::string hash;            // 插件内容 sha256（与 MusicFree 一致）
    std::string source_url;      // 插件 JS 的下载地址
    // 插件实际支持的方法
    bool can_search = false;
    bool can_media_source = false;
    bool can_lyric = false;
    bool can_music_info = false;
    bool can_album = false;
    bool can_sheet = false;
};

} // namespace ttplayer::musicfree
