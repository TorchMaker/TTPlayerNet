#pragma once

// 原生 spider 公共接口
//
// TVBox 的 csp_* 站点是 Java 类（api 写的是类名而不是 URL）。本层把已被复刻成
// C++ 的类抽象成 SpiderEngine：注册表按规范化 id（csp_T4）找到工厂，统一入口
// 负责构造 SpiderContext 并调用引擎。模板引擎（CMS / App 签名 / HTML / 网盘 /
// 直播）都实现同一组纯虚函数，recipe 驱动其行为。
//
// 约定与 video_cms 一致：失败抛 std::runtime_error，由上层按「跳过异常源」处理。

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/video/video_export.h"
#include "ttplayer/video/video_types.h"

namespace ttplayer::video {

// 站点形态（对应 spec 的模板 A~E）。
enum class SpiderTemplate {
    kCms,   // A 标准 JSON CMS（MacCMS provide/vod）
    kApp,   // B 私有 App 签名接口
    kHtml,  // C HTML 抓取
    kPan,   // D 网盘/云盘
    kLive,  // E 直播/平台
};

// 模板名（recipe 里用小写："cms" / "app" / "html" / "pan" / "live"）。
std::string_view SpiderTemplateName(SpiderTemplate tmpl);
// 解析模板名（大小写不敏感）；不认识返回 false。
bool SpiderTemplateFromName(std::string_view name, SpiderTemplate* out);

// 注册表元信息。
struct SpiderMeta {
    std::string id;          // 规范化 id，如 "csp_T4"
    std::string class_name;  // TVBox 原始类名，如 "T4Guard"
    SpiderTemplate tmpl = SpiderTemplate::kCms;
    bool implemented = false;  // 是否已有可用的 C++ 实现
    std::string note;          // 备注（数据来源 / 已知限制）
};

// 一次调用的上下文：站点原样 + ext + 网络配置 + 规范化 id。
struct SpiderContext {
    VideoSite site;
    std::string ext;                // 站点 ext 原文（字符串或 JSON 文本）
    musicfree::NetworkConfig net;   // UA / 代理 / 超时
    std::string spider_id;          // 规范化 id，如 "csp_T4"
};

// 模板引擎抽象基类。失败抛 std::runtime_error。
class SpiderEngine {
public:
    virtual ~SpiderEngine() = default;

    virtual VideoListing Home(const SpiderContext& ctx) = 0;
    virtual VideoListing Category(const SpiderContext& ctx,
                                  std::string_view category_id, int page) = 0;
    virtual VideoListing Search(const SpiderContext& ctx,
                                std::string_view keyword, int page) = 0;
    virtual VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) = 0;
    // 把播放页地址解析成可直接播放的媒体地址；解析不了返回原地址或抛异常。
    virtual std::string Player(const SpiderContext& ctx,
                               const VideoEpisode& episode) = 0;
};

// 由站点构造上下文（spider_id 为空时按 api 归一）。
SpiderContext MakeSpiderContext(const VideoSite& site,
                                const musicfree::NetworkConfig& net);

// 站点是否有原生实现（kind 无关，只看注册表）。
bool HasNative(const VideoSite& site);

// 统一入口：video_cms 在 Fetch* 开头按 kind==kNativeSpider 转调这里。
// 未注册 / 未实现 / 工厂缺失均抛 std::runtime_error。
VideoListing SpiderHome(const VideoSite& site, const musicfree::NetworkConfig& net);
VideoListing SpiderCategory(const VideoSite& site, std::string_view category_id,
                            int page, const musicfree::NetworkConfig& net);
VideoListing SpiderSearch(const VideoSite& site, std::string_view keyword,
                          int page, const musicfree::NetworkConfig& net);
VideoDetail SpiderDetail(const VideoSite& site, const VideoItem& item,
                         const musicfree::NetworkConfig& net);
// 宿主（主 exe 的 video.open 详情线程）需要跨 DLL 调用 Player 层，故单独导出。
TTPLAYER_VIDEO_API std::string SpiderPlayer(
    const VideoSite& site, const VideoEpisode& episode,
    const musicfree::NetworkConfig& net);

} // namespace ttplayer::video
