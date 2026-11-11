#include "ttplayer/video/spider/spider_api.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

#include "ttplayer/video/spider/spider_registry.h"

namespace ttplayer::video {

std::string_view SpiderTemplateName(SpiderTemplate tmpl) {
    switch (tmpl) {
        case SpiderTemplate::kCms: return "cms";
        case SpiderTemplate::kApp: return "app";
        case SpiderTemplate::kHtml: return "html";
        case SpiderTemplate::kPan: return "pan";
        case SpiderTemplate::kLive: return "live";
    }
    return "cms";
}

bool SpiderTemplateFromName(std::string_view name, SpiderTemplate* out) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "cms") { if (out) *out = SpiderTemplate::kCms; return true; }
    if (lower == "app") { if (out) *out = SpiderTemplate::kApp; return true; }
    if (lower == "html") { if (out) *out = SpiderTemplate::kHtml; return true; }
    if (lower == "pan") { if (out) *out = SpiderTemplate::kPan; return true; }
    if (lower == "live") { if (out) *out = SpiderTemplate::kLive; return true; }
    return false;
}

SpiderContext MakeSpiderContext(const VideoSite& site,
                                const musicfree::NetworkConfig& net) {
    SpiderContext ctx;
    ctx.site = site;
    ctx.ext = site.ext;
    ctx.net = net;
    ctx.spider_id = site.spider_id.empty() ? CanonicalSpiderId(site.api)
                                           : site.spider_id;
    return ctx;
}

bool HasNative(const VideoSite& site) {
    const SpiderMeta* meta = SpiderRegistry::Instance().Find(
        site.spider_id.empty() ? site.api : site.spider_id);
    return meta != nullptr && meta->implemented;
}

namespace {

// 解析站点对应的引擎；未注册 / 未实现都抛异常（与 Fetch* 的错误约定一致）。
std::unique_ptr<SpiderEngine> CreateForSite(const VideoSite& site,
                                            std::string* spider_id) {
    SpiderRegistry& registry = SpiderRegistry::Instance();
    const SpiderMeta* meta = registry.Find(site.spider_id);
    if (!meta) meta = registry.Find(site.api);
    if (!meta) {
        throw std::runtime_error(
            "原生 spider 未注册：" +
            (site.spider_id.empty() ? site.api : site.spider_id));
    }
    if (!meta->implemented) {
        throw std::runtime_error("原生 spider 未实现：" + meta->id);
    }
    std::unique_ptr<SpiderEngine> engine = registry.Create(meta->id);
    if (!engine) {
        throw std::runtime_error("原生 spider 工厂缺失：" + meta->id);
    }
    if (spider_id) *spider_id = meta->id;
    return engine;
}

SpiderContext ContextFor(const VideoSite& site, const musicfree::NetworkConfig& net,
                         const std::string& spider_id) {
    SpiderContext ctx = MakeSpiderContext(site, net);
    ctx.spider_id = spider_id;
    return ctx;
}

} // namespace

VideoListing SpiderHome(const VideoSite& site, const musicfree::NetworkConfig& net) {
    std::string id;
    auto engine = CreateForSite(site, &id);
    return engine->Home(ContextFor(site, net, id));
}

VideoListing SpiderCategory(const VideoSite& site, std::string_view category_id,
                            int page, const musicfree::NetworkConfig& net) {
    std::string id;
    auto engine = CreateForSite(site, &id);
    return engine->Category(ContextFor(site, net, id), category_id, page);
}

VideoListing SpiderSearch(const VideoSite& site, std::string_view keyword,
                          int page, const musicfree::NetworkConfig& net) {
    std::string id;
    auto engine = CreateForSite(site, &id);
    return engine->Search(ContextFor(site, net, id), keyword, page);
}

VideoDetail SpiderDetail(const VideoSite& site, const VideoItem& item,
                         const musicfree::NetworkConfig& net) {
    std::string id;
    auto engine = CreateForSite(site, &id);
    return engine->Detail(ContextFor(site, net, id), item);
}

std::string SpiderPlayer(const VideoSite& site, const VideoEpisode& episode,
                         const musicfree::NetworkConfig& net) {
    std::string id;
    auto engine = CreateForSite(site, &id);
    return engine->Player(ContextFor(site, net, id), episode);
}

} // namespace ttplayer::video
