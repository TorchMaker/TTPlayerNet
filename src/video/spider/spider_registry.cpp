#include "ttplayer/video/spider/spider_registry.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "ttplayer/video/video_cms.h"
#include "ttplayer/video/video_config.h"
#include "ttplayer/video/spider/spider_builtins.h"

namespace ttplayer::video {

namespace {

std::string TrimCopy(std::string_view raw) {
    std::size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string_view::npos) return {};
    std::size_t e = raw.find_last_not_of(" \t\r\n");
    return std::string(raw.substr(s, e - s + 1));
}

std::string ToLower(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// 从 ext 文本推 CMS 接口：优先走带解密的标准提取；否则读 siteUrl/host 等字段
// 并补上 MacCMS 的接口后缀（T4 的 ext 两种写法都存在）。
std::string CmsApiFromExtText(const std::string& ext) {
    if (ext.empty()) return {};
    std::string api;
    try {
        api = FindCmsApiFromExt(nlohmann::json(ext));
    } catch (...) {}
    if (!api.empty()) return api;
    try {
        const nlohmann::json root = nlohmann::json::parse(ext);
        if (root.is_object()) {
            for (const char* key : {"siteUrl", "site_url", "api", "url", "host"}) {
                const auto it = root.find(key);
                if (it == root.end() || !it->is_string()) continue;
                std::string value = it->get<std::string>();
                if (value.rfind("http", 0) != 0) continue;
                if (value.find("provide/vod") == std::string::npos &&
                    value.find("/api.php") == std::string::npos) {
                    while (!value.empty() && value.back() == '/') value.pop_back();
                    value += "/api.php/provide/vod";
                }
                return value;
            }
        }
    } catch (...) {}
    return {};
}

// 模板 A 的内置实现：把站点还原成标准 CMS 站点后直接复用 video_cms 的抓取
// 逻辑（T4 / 光影 / 奶酪都是这一类）。后续 Task 3 会用 recipe 细化参数映射。
class BuiltinCmsEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        return FetchHome(CmsSite(ctx), ctx.net);
    }
    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override {
        return FetchCategory(CmsSite(ctx), category_id, page, ctx.net);
    }
    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override {
        return FetchSearch(CmsSite(ctx), keyword, page, ctx.net);
    }
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        return FetchDetail(CmsSite(ctx), item, ctx.net);
    }
    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        // CMS 站点的播放地址由 video_resolve 的解析链处理，这里只回传原地址。
        (void)ctx;
        return episode.url;
    }

private:
    static VideoSite CmsSite(const SpiderContext& ctx) {
        VideoSite cms = ctx.site;
        const std::string ext = ctx.ext.empty() ? ctx.site.ext : ctx.ext;
        const std::string api = CmsApiFromExtText(ext);
        if (api.empty()) {
            throw std::runtime_error("原生 spider " + ctx.spider_id +
                                     " 缺少 CMS 接口（ext 中未找到 provide/vod 地址）");
        }
        cms.api = api;
        cms.ext.clear();
        cms.kind = VideoSourceKind::kCms;
        cms.spider_id.clear();
        cms.native_ready = false;
        cms.auto_converted = true;
        cms.why_unsupported.clear();
        return cms;
    }
};

} // namespace

std::string NormalizeSpiderId(std::string_view raw) {
    std::string s = TrimCopy(raw);
    // 全限定类名只取最后一段（com.github.tvbox.osc.spider.csp.T4Guard）。
    const std::size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(dot + 1);
    // TVBox 里的内部类写法 csp_Xxx$1。
    const std::size_t dollar = s.find('$');
    if (dollar != std::string::npos) s.resize(dollar);
    if (s.rfind("csp_", 0) == 0) s = s.substr(4);
    if (s.size() > 5 && ToLower(s.substr(s.size() - 5)) == "guard")
        s.resize(s.size() - 5);
    return s;
}

std::string CanonicalSpiderId(std::string_view raw) {
    const std::string bare = NormalizeSpiderId(raw);
    return bare.empty() ? std::string() : "csp_" + bare;
}

SpiderRegistry& SpiderRegistry::Instance() {
    static SpiderRegistry registry;
    return registry;
}

SpiderRegistry::SpiderRegistry() {
    SpiderMeta t4;
    t4.id = "csp_T4";
    t4.class_name = "T4Guard";
    t4.tmpl = SpiderTemplate::kCms;
    t4.implemented = true;
    t4.note = "标准 MacCMS：复用 video_cms，接口来自 ext(siteUrl)";
    Register(std::move(t4), [] { return std::make_unique<BuiltinCmsEngine>(); });
    // 各模板引擎在独立文件里登记，避免并行开发时互改本文件。
    RegisterBuiltinSpiderEngines();
}

void SpiderRegistry::Register(SpiderMeta meta, SpiderFactory factory) {
    if (meta.id.rfind("csp_", 0) != 0) meta.id = CanonicalSpiderId(meta.id);
    const std::string key = ToLower(NormalizeSpiderId(meta.id));
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = SpiderEntry{std::move(meta), std::move(factory)};
}

const SpiderMeta* SpiderRegistry::Find(std::string_view id) const {
    const std::string key = ToLower(NormalizeSpiderId(id));
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second.meta;
}

bool SpiderRegistry::Has(std::string_view id) const {
    return Find(id) != nullptr;
}

std::vector<SpiderMeta> SpiderRegistry::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SpiderMeta> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.second.meta);
    std::sort(out.begin(), out.end(),
              [](const SpiderMeta& a, const SpiderMeta& b) { return a.id < b.id; });
    return out;
}

int SpiderRegistry::ImplementedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& kv : entries_)
        if (kv.second.meta.implemented) ++count;
    return count;
}

std::unique_ptr<SpiderEngine> SpiderRegistry::Create(std::string_view id) const {
    const std::string key = ToLower(NormalizeSpiderId(id));
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.factory) return nullptr;
    return it->second.factory();
}

} // namespace ttplayer::video
