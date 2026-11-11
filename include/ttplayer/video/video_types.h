#pragma once

// 视频源兼容层 —— 类型定义（影视仓 / TVBox / MarcoMovie 协议）
//
// 把 TVBox 系（single-repo / multi-repo JSON、sites / parses / lives、m3u_vod 直播、
// CMS provide/vod 接口）映射到 C++ 结构。视频站点=分组、视频条目=卡片、选集=曲目，
// 全部复用音乐窗已有的「分组卡片网格 + 曲目列表」渲染管线，避免重写绘制逻辑。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ttplayer::video {

// 视频源种类：决定用哪条解析链路。
// 新增枚举值只能追加在末尾：视频源缓存 index.json 里存的是这个 int，改动
// 既有顺序会让老缓存被识别成别的类型。
enum class VideoSourceKind {
    kUnknown,       // 暂不支持（如需要 JS 扩展的 spider）
    kCms,           // CMS provide/vod（ac=list/detail/videolist）
    kLive,          // m3u_vod 直播（api 以 m3u_vod:// 开头）
    kSpider,        // csp_/drpy/js 扩展（需 QuickJS；best-effort）
    kNativeSpider,  // csp_* 已有 C++ 原生实现（走 spider 引擎，没有 URL）
};

// 单个视频站点（TVBox site / CmsSite）。
struct VideoSite {
    std::string key;        // 站点唯一键（可能为空，用 name 兜底）
    std::string name;       // 显示名
    std::string api;        // 接口地址（CMS api.php/provide/vod 或 m3u_vod:// 或 spider 路径）
    std::string play_url;   // 官方解析前缀（拼到播放地址前）
    std::string ext;        // 附加参数（CMS ext）
    std::string group;      // 分组名
    std::string ua;         // 自定义 UA
    std::string header;     // 自定义请求头（JSON 字符串或 "k:v,k:v"）
    VideoSourceKind kind = VideoSourceKind::kUnknown;
    std::string why_unsupported;  // kind==kUnknown 时说明原因
    // 由 csp_* SPIDER 站点的加密 ext 自动还原出标准 CMS 接口（诊断/报告用）。
    bool auto_converted = false;
    // 原生 spider 标识：csp_XxxGuard 去掉 Guard 后的规范化 id，如 "csp_T4"。
    // 仅 kind==kSpider / kNativeSpider 时有值。
    std::string spider_id;
    // 该 spider 是否已有 C++ 原生实现（kind==kNativeSpider 时为 true）。
    bool native_ready = false;
    std::string player;    // 可选：站点级 player（TVBox player 字段）：自定义播放器标识/地址

    // 从配置 JSON 的一个 site 对象解析；校验 api 是否可识别。
    static VideoSite FromJson(const nlohmann::json& j);

    // 规范化后的有效接口地址（去掉首尾空白、补全相对路径）。
    // 原生 spider 没有网络 URL，返回 spider_id 供界面展示。
    std::string NormalizedApi() const;
    // 解析器认识这一项（CMS / 直播 / SPIDER 都算认识）。
    // ⚠ 认识 ≠ 能取到内容：SPIDER 需要 TVBox 的 Java 运行库（csp_XxxGuard 这类
    // api 是类名，不是 URL），本程序取不了。要判断「能不能真的拉内容」用 CanFetch()。
    bool IsSupported() const { return kind != VideoSourceKind::kUnknown; }
    // 本程序真能拉取内容的站点类型（CMS 接口 / 直播 / 原生 spider）。
    bool CanFetch() const {
        return kind == VideoSourceKind::kCms || kind == VideoSourceKind::kLive ||
               kind == VideoSourceKind::kNativeSpider;
    }
};

// 解析线路（parses）。
struct VideoParse {
    std::string name;
    int type = 0;       // 0=嗅探 1/2/3=官方/直链
    std::string url;    // 解析地址或前缀
    std::string ext;
};

// 直播分组（lives）。
struct VideoLive {
    std::string name;
    std::string url;    // m3u/txt 地址
    std::string group;
};

// 一整份影视仓 / TVBox 配置（单仓；多仓会拆成多份）。
struct VideoConfig {
    std::string spider;                 // 顶层 spider 字段（透传，供 spider 站点用）
    std::string player_url;              // 顶层 playerUrl（TVBox playerUrl 字段）：自定义播放器 JS
    std::vector<VideoSite> sites;
    std::vector<VideoParse> parses;
    std::vector<VideoLive> lives;
};

// 分类（CMS class）。
struct VideoCategory {
    std::string id;
    std::string name;
    std::string parent_id;
};

// 一个视频条目（卡片）。
struct VideoItem {
    std::string id;
    std::string site_key;     // 所属站点 key（点击详情时回传）
    std::string site_name;
    std::string title;
    std::string pic;          // 封面 URL
    std::string remarks;      // 更新状态 / 备注（如「更新至12集」）
    std::string score;        // 评分（影视仓式卡片角标，CMS 多数源不带，仅字段存在时画）
    std::string type_name;    // 分类名
    std::string year;
    std::string area;
    std::string actor;
    std::string description;
    nlohmann::json raw;       // 原样保留，详情调用要回传
};

// 一条选集（剧集）。
struct VideoEpisode {
    std::string name;         // 选集名（如「第1集」）
    std::string url;          // 原始播放地址（可能需 play_url 前缀 / 解析）
    // TVBox 的 vod_play_from（线路标识 / player flag）。播放串要不要先过
    // 自定义播放器，由它决定；以前被当成「线路名」留在 VideoLine 上，
    // 传到播放层就没了。
    std::string flag;
};

// 一路播放线路（如「线路1」「线路2」）。
struct VideoLine {
    std::string name;
    std::vector<VideoEpisode> episodes;
};

// 视频详情（含多路线路）。
struct VideoDetail {
    std::string id;
    std::string title;
    std::string pic;
    std::string remarks;
    std::vector<VideoLine> lines;
    nlohmann::json raw;
};

// 一页列表结果（首页/分类/搜索）。
struct VideoListing {
    std::vector<VideoItem> items;
    std::vector<VideoCategory> categories;  // 仅首页带分类
    bool is_end = true;
    int page = 1;
    int page_count = 1;
    std::string error;
};

// 一份「已导入并持久化」的视频源（与音乐源的 ImportedSource 对应）。
struct ImportedVideoSource {
    std::string name;                // 配置名 / 单仓名
    std::string source_url;          // 来源 URL（本地文件为空）
    std::filesystem::path local_path; // 本地缓存的 JSON
    std::vector<VideoSite> sites;    // 解析出的站点
    std::vector<VideoLive> lives;
    // 解析接口（parses）：把播放页地址变成媒体直链用。⚠ 以前这里没存，
    // 导致「配置里明明写了 parses，播放时却一个都没用」—— 播放链路拿不到。
    std::vector<VideoParse> parses;
    std::string player_url;  // 订阅级 playerUrl（TVBox playerUrl 字段）：自定义播放器 JS
    std::string version;
};

} // namespace ttplayer::video
