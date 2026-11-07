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
    // 内容浏览能力（复刻 MusicFree IPlugin 的内容方法，洛雪音源协议不提供这些）
    bool can_top_lists = false;     // getTopLists
    bool can_sheet_detail = false;  // getTopListDetail / getMusicSheetInfo
    bool can_recommend = false;     // getRecommendSheetTags / getRecommendSheetsByTag
};

// 歌单 / 排行榜条目（MusicFree IMusicSheetItem）。封面字段在 MusicFree 里叫
// coverImg，原样保留 raw，详情调用（getTopListDetail / getMusicSheetInfo）要把
// raw 原样回传，和搜索结果的 raw 同样不能只重建 id/platform。
struct MusicSheet {
    std::string id;
    std::string platform;
    std::string title;
    std::string description;
    std::string artwork;       // 封面 URL（MusicFree 用 coverImg）
    std::string work_id;       // 部分音源 getTopListDetail 需要的标识
    std::int64_t play_count = 0;
    std::int64_t create_at = 0;
    std::string group;         // 分组名（排行榜常见），无则空
    nlohmann::json raw;        // 插件返回完整对象（详情调用要原样回传）

    static MusicSheet FromJson(const nlohmann::json& j);
};

// 推荐歌单标签（MusicFree getRecommendSheetTags 返回 {id,title} 或字符串）。
struct SheetTag {
    std::string id;
    std::string title;
    nlohmann::json raw;

    static SheetTag FromJson(const nlohmann::json& j);
};

// 排行榜 / 推荐歌单 列表（getTopLists / getRecommendSheetsByTag 的返回）。
struct TopListsResult {
    std::vector<MusicSheet> items;
    std::string error;
};

// 歌单 / 排行榜详情里的曲目（getTopListDetail / getMusicSheetInfo 的返回）。
struct SheetTracksResult {
    std::vector<MusicItem> items;
    bool is_end = true;
    std::string error;
};

// 推荐标签列表（getRecommendSheetTags 的返回）。
struct SheetTagsResult {
    std::vector<SheetTag> tags;
    std::string error;
};

// ---- 音源更新检查（复刻 MusicFree 的 updatePlugin / installPluginFromUrl）----
//
// 原版语义（src/core/pluginManager/index.ts）：
//  * 每个插件记住下载地址 srcUrl，更新 = 重新下载（Cache-Control: no-cache）；
//  * 内容哈希与本地相同 → "插件已安装"（视为已是最新）；
//  * 本地版本 > 远端版本 → 拒绝安装（防降级，compare-versions 比较）；
//  * 否则替换旧文件并重新挂载。

// 复刻 compare-versions 的比较：按 '.'/'-'/'+' 分段，纯数字段按数值比较
// （缺段按 0，前导 v/V 忽略），非数字段按字典序。返回 <0 / 0 / >0。
inline int CompareSourceVersions(std::string_view lhs, std::string_view rhs) {
    if (!lhs.empty() && (lhs.front() == 'v' || lhs.front() == 'V'))
        lhs.remove_prefix(1);
    if (!rhs.empty() && (rhs.front() == 'v' || rhs.front() == 'V'))
        rhs.remove_prefix(1);
    auto next = [](std::string_view s, std::size_t& pos) {
        if (pos >= s.size()) return std::string_view();
        const std::size_t start = pos;
        while (pos < s.size() && s[pos] != '.' && s[pos] != '-' && s[pos] != '+')
            ++pos;
        const std::string_view part = s.substr(start, pos - start);
        if (pos < s.size()) ++pos;  // 跳过分隔符
        return part;
    };
    std::size_t pa = 0, pb = 0;
    for (;;) {
        if (pa >= lhs.size() && pb >= rhs.size()) return 0;
        const std::string_view a = next(lhs, pa);
        const std::string_view b = next(rhs, pb);
        // 空段视为数字 0（"1.0" vs "1.0.1" 的缺段）
        const bool numa = a.empty() ||
            a.find_first_not_of("0123456789") == std::string_view::npos;
        const bool numb = b.empty() ||
            b.find_first_not_of("0123456789") == std::string_view::npos;
        int cmp = 0;
        if (numa && numb) {
            // 段长可能超过 long long：去前导零后先比长度再比字典序
            const std::size_t za = a.find_first_not_of('0');
            const std::size_t zb = b.find_first_not_of('0');
            const std::string_view ca =
                za == std::string_view::npos ? std::string_view() : a.substr(za);
            const std::string_view cb =
                zb == std::string_view::npos ? std::string_view() : b.substr(zb);
            if (ca.size() != cb.size()) cmp = ca.size() < cb.size() ? -1 : 1;
            else if (const int r = ca.compare(cb)) cmp = r < 0 ? -1 : 1;
        } else if (numa != numb) {
            cmp = numa ? -1 : 1;  // 数字段 < 非数字段
        } else {
            const int r = a.compare(b);
            cmp = r < 0 ? -1 : (r > 0 ? 1 : 0);
        }
        if (cmp != 0) return cmp;
    }
}

// 单个音源的更新检查结果。
struct SourceUpdateInfo {
    std::string protocol;      // "MusicFree" / "洛雪音源" / "REST 音源" / "Listen1"
    std::string name;          // 插件/音源名
    std::string url;           // 更新源 URL（本地导入没有 URL）
    std::string old_version;
    std::string new_version;
    std::string status;        // updated / latest / downgrade / no_url / failed
    std::string detail;        // 失败原因 / 提示
};

struct SourceUpdateReport {
    int checked = 0;
    int updated = 0;
    int latest = 0;
    int failed = 0;
    std::vector<SourceUpdateInfo> items;
    std::vector<std::string> errors;

    // UTF-8 多行摘要，导入窗的结果框直接显示。
    std::string Summary() const {
        std::string out = "检查更新完成：可更新音源 " + std::to_string(checked) +
                          " 个，已更新 " + std::to_string(updated) +
                          "，已是最新 " + std::to_string(latest) +
                          "，失败 " + std::to_string(failed);
        for (const auto& it : items) {
            out += "\r\n- [" + it.protocol + "] " + it.name;
            if (it.status == "updated") {
                out += "：已更新 " + it.old_version + " -> " + it.new_version;
            } else if (it.status == "latest") {
                out += "：已是最新";
            } else if (it.status == "downgrade") {
                out += "：远端版本(" + it.new_version + ")较旧，已拒绝更新";
            } else if (it.status == "no_url") {
                out += "：本地导入，没有更新源";
            } else {
                out += "：失败";
                if (!it.detail.empty()) out += "：" + it.detail;
            }
        }
        for (const auto& e : errors) out += "\r\n- " + e;
        return out;
    }
};

} // namespace ttplayer::musicfree
