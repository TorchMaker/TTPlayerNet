#include "ttplayer/video/spider/spider_builtins.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ttplayer/video/spider/spider_api.h"
#include "ttplayer/video/spider/spider_http.h"
#include "ttplayer/video/spider/spider_recipe.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/spider/spider_util.h"
#include "ttplayer/video/video_cms.h"

// 模板 A（CMS JSON）引擎。
//
// T4（光影 / 奶酪）的 ext 是 {"siteUrl": "...", "type": 1, "categories": [...]}，
// 站点本身是标准 MacCMS provide/vod：ac=list 取分类、ac=videolist|list 取列表、
// ac=detail 取详情。因此内容解析直接复用 video_cms 的 Fetch*，本文件只负责：
//   * recipe 与 ext 的 base 优先级、请求头/UA 合并；
//   * 分类表兜底（FetchHome 的 ac=list 偶发超时）与分类顺序整理；
//   * recipe/ext 里 categories 的覆盖（仅在与站点分类名匹配时生效）；
//   * 源站抖动时的有限重试。
// 播放地址由既有 video_resolve 链处理，Player 只回传 episode.url。

namespace ttplayer::video {

namespace {

using nlohmann::json;

constexpr int kMaxAttempts = 3;

std::string JsonToStr(const json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    if (it->is_number_float()) return std::to_string(it->get<double>());
    if (it->is_boolean()) return it->get<bool>() ? "1" : "0";
    return {};
}

bool IsHttpUrl(std::string_view text) {
    const std::string lower = spider_util::ToLower(text);
    return spider_util::StartsWith(lower, "http://") ||
           spider_util::StartsWith(lower, "https://");
}

// ext 可能是 JSON，也可能只是一个 base URL（T4 的 ext 是前者）。
json ParseExtJson(std::string_view ext) {
    json parsed = spider_util::ParseJsonLoose(ext);
    if (parsed.is_object()) return parsed;
    const std::string text = spider_util::Trim(ext);
    if (IsHttpUrl(text)) return json{{"siteUrl", text}};
    return json();
}

struct CmsProfile {
    std::string base;                        // 站点接口根地址
    std::map<std::string, std::string> headers;  // recipe/ext 公共请求头
    json recipe;                             // recipe 原文（缺省为 null）
    json ext;                                // ext 原文（JSON 对象）
};

void MergeHeaders(const json& source, std::map<std::string, std::string>* headers) {
    if (!headers || !source.is_object()) return;
    const auto it = source.find("headers");
    if (it == source.end() || !it->is_object()) return;
    for (auto header = it->begin(); header != it->end(); ++header) {
        if (header.value().is_string())
            (*headers)[header.key()] = header.value().get<std::string>();
    }
}

CmsProfile BuildProfile(const SpiderContext& ctx) {
    CmsProfile profile;
    profile.ext = ParseExtJson(ctx.ext);

    const SpiderRecipeLoad load = LoadSpiderRecipe(ctx.spider_id);
    if (load.ok) {
        profile.recipe = load.recipe.raw;
        profile.base = spider_util::Trim(load.recipe.base);
    }

    // base 优先级：recipe.base → ext.siteUrl/site_url/api/url/host。
    if (profile.base.empty() && profile.ext.is_object()) {
        for (const char* key : {"siteUrl", "site_url", "api", "url", "host"}) {
            const auto it = profile.ext.find(key);
            if (it == profile.ext.end() || !it->is_string()) continue;
            const std::string value = spider_util::Trim(it->get<std::string>());
            if (!value.empty()) {
                profile.base = value;
                break;
            }
        }
    }
    if (profile.base.empty()) {
        throw std::runtime_error("原生 spider " + ctx.spider_id +
                                 " 缺少 CMS 接口（recipe.base 与 ext.siteUrl 均为空）");
    }

    // 容错：省略协议的站点补 http://，//host 补 http:。
    if (spider_util::StartsWith(profile.base, "//")) {
        profile.base = "http:" + profile.base;
    } else if (!IsHttpUrl(profile.base)) {
        profile.base = "http://" + profile.base;
    }

    MergeHeaders(profile.recipe, &profile.headers);
    MergeHeaders(profile.ext, &profile.headers);
    return profile;
}

// 把 CMS 接口还原成普通 VideoSite，交给 video_cms 的 Fetch* 解析。
VideoSite MakeCmsSite(const SpiderContext& ctx, const CmsProfile& profile) {
    VideoSite cms = ctx.site;
    cms.api = profile.base;
    cms.ext.clear();
    cms.kind = VideoSourceKind::kCms;
    cms.spider_id.clear();
    cms.native_ready = false;
    cms.auto_converted = true;
    cms.why_unsupported.clear();
    if (!profile.headers.empty()) {
        cms.header = json(profile.headers).dump();
        for (const auto& kv : profile.headers) {
            if (spider_util::ToLower(kv.first) == "user-agent") {
                cms.ua = kv.second;
                break;
            }
        }
    }
    return cms;
}

// 去掉 URL 里指定的 query 参数（T4 的 siteUrl 自带 ?ac=list，重复 ac 会让
// 部分 MacCMS 后端返回非 JSON）。
std::string StripQueryParam(std::string url, std::string_view key) {
    const std::size_t question = url.find('?');
    if (question == std::string::npos) return url;
    const std::size_t hash = url.find('#', question);
    const std::string head = url.substr(0, question);
    const std::string query =
        url.substr(question + 1, (hash == std::string::npos ? url.size() : hash) -
                                     (question + 1));
    const std::string tail = hash == std::string::npos ? "" : url.substr(hash);

    std::string kept;
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string part =
            query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!part.empty()) {
            const std::size_t eq = part.find('=');
            const std::string name = eq == std::string::npos ? part : part.substr(0, eq);
            if (spider_util::ToLower(name) != spider_util::ToLower(key)) {
                if (!kept.empty()) kept += "&";
                kept += part;
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return head + (kept.empty() ? "" : "?" + kept) + tail;
}

// 按 recipe 的 path/params 拼请求地址；{siteUrl} 替换成 base。
std::string BuildCmsRequestUrl(const std::string& base, std::string_view path,
                               const std::map<std::string, std::string>& params) {
    const std::string raw_path = spider_util::Trim(path);
    std::string url;
    if (raw_path.empty() || raw_path == "{siteUrl}") {
        url = base;
    } else if (raw_path.find("{siteUrl}") != std::string::npos) {
        url = spider_util::ReplaceAll(raw_path, "{siteUrl}", base);
    } else if (IsHttpUrl(raw_path)) {
        url = raw_path;
    } else {
        url = spider_util::JoinUrl(base, raw_path);
    }

    bool overrides_ac = false;
    for (const auto& kv : params) {
        if (spider_util::ToLower(kv.first) == "ac") {
            overrides_ac = true;
            break;
        }
    }
    if (overrides_ac) {
        url = StripQueryParam(url, "ac");
        while (!url.empty() && url.back() == '?') url.pop_back();
    }

    for (const auto& kv : params) {
        if (kv.second.empty()) continue;
        url += (url.find('?') == std::string::npos ? "?" : "&");
        url += spider_util::UrlEncode(kv.first) + "=" + spider_util::UrlEncode(kv.second);
    }
    return url;
}

std::string HomePath(const CmsProfile& profile) {
    if (!profile.recipe.is_object()) return "{siteUrl}";
    const auto endpoints = profile.recipe.find("endpoints");
    if (endpoints == profile.recipe.end() || !endpoints->is_object()) return "{siteUrl}";
    const auto home = endpoints->find("home");
    if (home == endpoints->end() || !home->is_object()) return "{siteUrl}";
    const auto path = home->find("path");
    if (path == home->end() || !path->is_string()) return "{siteUrl}";
    return path->get<std::string>();
}

std::vector<VideoCategory> ParseCategories(const json& root) {
    std::vector<VideoCategory> categories;
    if (!root.is_object()) return categories;
    const auto klass = root.find("class");
    if (klass == root.end() || !klass->is_array()) return categories;
    for (const auto& entry : *klass) {
        if (!entry.is_object()) continue;
        VideoCategory category;
        category.id = JsonToStr(entry, "type_id");
        category.name = JsonToStr(entry, "type_name");
        category.parent_id = JsonToStr(entry, "type_pid");
        if (category.id.empty() && category.name.empty()) continue;
        categories.push_back(std::move(category));
    }
    return categories;
}

// FetchHome 里的 ac=list 只有 6 秒超时，Cloudflare 源偶发超时会丢分类表。
// 这里用 SpiderHttp（recipe 头 + 20 秒超时 + 有限重试）单独补一次分类。
std::vector<VideoCategory> FetchCategoriesWithRetry(const SpiderContext& ctx,
                                                    const CmsProfile& profile) {
    const std::string url = BuildCmsRequestUrl(
        profile.base, HomePath(profile), {{"ac", "list"}, {"pg", "1"}});
    SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
    http.SetStage("home");
    http.SetDefaultHeaders(profile.headers);
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        try {
            const SpiderHttpResponse response = http.Get(url);
            const json root = spider_util::ParseJsonLoose(response.body);
            std::vector<VideoCategory> categories = ParseCategories(root);
            if (!categories.empty()) return categories;
        } catch (const std::exception&) {
            // 单次失败继续重试，全部失败时返回空由上层决定。
        }
        if (attempt + 1 < kMaxAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return {};
}

// 一级分类（电影/电视剧/综艺/动漫）通常没有直属内容，点进去是空列表。
// 把有内容的叶子分类排在前面，首页分类页签的第一项就能直接出卡片。
bool IsParentCategory(const VideoCategory& category) {
    if (!category.parent_id.empty() && category.parent_id != "0") return false;
    if (category.parent_id == "0") return true;
    static const char* kTopNames[] = {"电影", "电视剧", "综艺", "动漫",
                                      "电影片", "连续剧", "综艺片", "动漫片"};
    for (const char* name : kTopNames)
        if (category.name == name) return true;
    return false;
}

void OrderCategories(std::vector<VideoCategory>* categories) {
    if (!categories || categories->empty()) return;
    std::vector<VideoCategory> leaves;
    std::vector<VideoCategory> parents;
    for (const auto& category : *categories) {
        if (IsParentCategory(category))
            parents.push_back(category);
        else
            leaves.push_back(category);
    }
    if (leaves.empty() || parents.empty()) return;
    leaves.insert(leaves.end(), parents.begin(), parents.end());
    *categories = std::move(leaves);
}

// recipe/ext.categories 覆盖：对象数组（含 id/name）直接替换；字符串数组按
// 分类名过滤。逆向资产里的中文分类有乱码，匹配不到时保持站点原分类。
void ApplyRecipeCategories(VideoListing* listing, const CmsProfile& profile) {
    if (!listing) return;
    const json* defs = nullptr;
    if (profile.recipe.is_object() && profile.recipe.contains("categories"))
        defs = &profile.recipe["categories"];
    else if (profile.ext.is_object() && profile.ext.contains("categories"))
        defs = &profile.ext["categories"];
    if (!defs || !defs->is_array()) return;

    std::vector<VideoCategory> explicit_categories;
    std::vector<std::string> names;
    for (const auto& entry : *defs) {
        if (entry.is_object()) {
            VideoCategory category;
            category.id = JsonToStr(entry, "id");
            category.name = JsonToStr(entry, "name");
            if (category.name.empty()) category.name = JsonToStr(entry, "type_name");
            if (category.id.empty()) category.id = JsonToStr(entry, "type_id");
            if (!category.name.empty()) explicit_categories.push_back(std::move(category));
        } else if (entry.is_string()) {
            const std::string name = spider_util::Trim(entry.get<std::string>());
            if (!name.empty()) names.push_back(name);
        }
    }
    if (!explicit_categories.empty()) {
        listing->categories = std::move(explicit_categories);
        return;
    }
    if (names.empty()) return;

    std::vector<VideoCategory> matched;
    for (const auto& category : listing->categories) {
        for (const auto& name : names) {
            if (category.name == name) {
                matched.push_back(category);
                break;
            }
        }
    }
    if (!matched.empty()) listing->categories = std::move(matched);
}

VideoListing RetryListing(const std::function<VideoListing()>& fetch,
                          std::string* last_error) {
    VideoListing best;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        try {
            VideoListing current = fetch();
            if (current.items.size() > best.items.size()) best = std::move(current);
            else if (best.items.empty() && !current.categories.empty() &&
                     best.categories.empty())
                best.categories = std::move(current.categories);
            if (!best.items.empty()) break;
        } catch (const std::exception& e) {
            if (last_error) *last_error = e.what();
        }
        if (attempt + 1 < kMaxAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return best;
}

class RecipeCmsEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        const CmsProfile profile = BuildProfile(ctx);
        const VideoSite cms = MakeCmsSite(ctx, profile);
        std::string last_error;
        VideoListing listing = RetryListing(
            [&] { return FetchHome(cms, ctx.net); }, &last_error);

        if (listing.items.empty() && !last_error.empty())
            throw std::runtime_error("CMS 首页请求失败：" + last_error);

        if (listing.categories.empty())
            listing.categories = FetchCategoriesWithRetry(ctx, profile);
        OrderCategories(&listing.categories);
        ApplyRecipeCategories(&listing, profile);
        return listing;
    }

    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override {
        const CmsProfile profile = BuildProfile(ctx);
        const VideoSite cms = MakeCmsSite(ctx, profile);
        const std::string id(category_id);
        std::string last_error;
        VideoListing listing = RetryListing(
            [&] { return FetchCategory(cms, id, page, ctx.net); }, &last_error);
        if (listing.items.empty() && !last_error.empty())
            throw std::runtime_error("CMS 分类请求失败：" + last_error);
        return listing;
    }

    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override {
        const CmsProfile profile = BuildProfile(ctx);
        const VideoSite cms = MakeCmsSite(ctx, profile);
        const std::string text(keyword);
        std::string last_error;
        VideoListing listing = RetryListing(
            [&] { return FetchSearch(cms, text, page, ctx.net); }, &last_error);
        if (listing.items.empty() && !last_error.empty())
            throw std::runtime_error("CMS 搜索请求失败：" + last_error);
        return listing;
    }

    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        const CmsProfile profile = BuildProfile(ctx);
        const VideoSite cms = MakeCmsSite(ctx, profile);
        VideoDetail detail;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            try {
                detail = FetchDetail(cms, item, ctx.net);
                if (!detail.lines.empty()) break;
            } catch (const std::exception&) {
                // 详情偶发截断时重试；全部失败保持空线路，由上层降级。
            }
            if (attempt + 1 < kMaxAttempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return detail;
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        // CMS 播放地址（m3u8 / 播放页）交给既有 video_resolve 链，不在这里解析。
        (void)ctx;
        return episode.url;
    }
};

void RegisterCmsT4Engine() {
    SpiderMeta t4;
    t4.id = "csp_T4";
    t4.class_name = "T4";
    t4.tmpl = SpiderTemplate::kCms;
    t4.implemented = true;
    t4.note = "标准 MacCMS（ac=list/videolist/detail）：recipe 提供 base/headers/"
              "categories，内容解析复用 video_cms";
    SpiderRegistry::Instance().Register(
        std::move(t4), [] { return std::make_unique<RecipeCmsEngine>(); });
}

// SpiderRegistry 构造函数会调用 RegisterBuiltinSpiderEngines()；此时若再调用
// SpiderRegistry::Instance() 会重入同一个函数内静态变量的初始化而永久阻塞。
// 因此实际登记改由本文件的静态引导对象完成：它第一次调用 Instance() 时先构造
// 注册表（构造内的 RegisterCmsSpiderEngines 只留空），返回后再覆盖 csp_T4。
struct CmsEngineBootstrap {
    CmsEngineBootstrap() {
        RegisterCmsT4Engine();
        // 把 sources/spiders 里所有 cms 模板的社区 spider recipe 自动点亮。
        RegisterRecipeSpiderVariants(SpiderTemplate::kCms,
                                     [] { return std::make_unique<RecipeCmsEngine>(); });
    }
};

const CmsEngineBootstrap kCmsEngineBootstrap;

} // namespace

void RegisterCmsSpiderEngines() {
    // 见上方说明：注册表构造期间不能重入 Instance()，登记由 kCmsEngineBootstrap
    // 在构造完成后执行（重复 Register 会覆盖内置的 BuiltinCmsEngine）。
}

} // namespace ttplayer::video
