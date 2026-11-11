#include "ttplayer/video/video_cms.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/video/spider/spider_api.h"

namespace ttplayer::video {

namespace {

// 把 "k:v,k:v" 或 JSON 字符串解析成请求头。
std::map<std::string, std::string> ParseHeaderSpec(std::string_view spec) {
    std::map<std::string, std::string> out;
    std::string s(spec);
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](unsigned char c) { return c == ' '; }), s.end());
    // 尝试 JSON
    if (!s.empty() && s.front() == '{') {
        try {
            auto j = nlohmann::json::parse(s);
            if (j.is_object())
                for (auto it = j.begin(); it != j.end(); ++it)
                    if (it.value().is_string())
                        out[it.key()] = it.value().get<std::string>();
        } catch (...) {}
        return out;
    }
    // k:v,k:v
    std::stringstream ss(s);
    std::string kv;
    while (std::getline(ss, kv, ',')) {
        const size_t pos = kv.find(':');
        if (pos != std::string::npos && pos > 0)
            out[kv.substr(0, pos)] = kv.substr(pos + 1);
    }
    return out;
}

std::string UrlEncode(std::string_view v) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

} // namespace

std::map<std::string, std::string> BuildSiteHeaders(const VideoSite& site,
                                                   std::wstring_view default_ua) {
    std::map<std::string, std::string> headers;
    std::string ua = site.ua.empty() ? core::WideToUtf8(default_ua) : site.ua;
    if (!ua.empty()) headers["User-Agent"] = ua;
    if (!site.header.empty()) {
        auto extra = ParseHeaderSpec(site.header);
        for (auto& kv : extra) headers[kv.first] = kv.second;
    }
    return headers;
}

// 部分配置里 api 自带 "?ac=list" 或 "?ac=list&source=xxx"。BuildCmsUrl 还要
// 再加一个 ac=videolist/list，重复 ac 会让不少 MacCMS 后端直接返回非 JSON。
// 这里只丢掉已有的 ac 参数，其它参数（source / from / at 等）原样保留。
std::string StripExistingAc(std::string api) {
    const std::size_t q = api.find('?');
    if (q == std::string::npos) return api;
    const std::string base = api.substr(0, q);
    const std::string rest = api.substr(q + 1);
    std::string kept;
    std::size_t pos = 0;
    while (pos < rest.size()) {
        const std::size_t amp = rest.find('&', pos);
        const std::string part =
            rest.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        std::string low = part;
        for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.rfind("ac=", 0) != 0) {
            if (!kept.empty()) kept += "&";
            kept += part;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return base + (kept.empty() ? "" : "?" + kept);
}

std::string BuildCmsUrl(const VideoSite& site,
                        const std::map<std::string, std::string>& params) {
    // 原生 spider 没有 URL（api 是 csp_* 类名），拼出来的地址必然无效。
    if (site.kind == VideoSourceKind::kNativeSpider) return {};
    std::string api = StripExistingAc(site.NormalizedApi());
    std::string sep = api.find('?') == std::string::npos ? "?" : "&";
    std::string out = api + sep;
    bool first = true;
    for (auto& kv : params) {
        if (kv.second.empty()) continue;
        if (!first) out += "&";
        first = false;
        out += UrlEncode(kv.first) + "=" + UrlEncode(kv.second);
    }
    if (!site.ext.empty()) {
        // ext 可能是 "t=1&pg=2" 或 JSON；简单按原样追加（去首尾空白）。
        std::string ext = site.ext;
        size_t s = ext.find_first_not_of(" \t\r\n");
        size_t e = ext.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) {
            ext = ext.substr(s, e - s + 1);
            if (!ext.empty()) out += (out.back() == '?' ? "" : "&") + ext;
        }
    }
    return out;
}

namespace {

std::vector<std::string> SplitBy(std::string_view s, std::string_view sep);

// CMS 字段容错读取：type_id / vod_id / vod_year 在不同源里时而是字符串
// 时而是数字，get<std::string>() 碰到数字会抛异常 —— 之前就是这个把
// ac=list 的分类表整个弄丢的（异常被上层 catch 吞掉，页签一直是空的）。
std::string JsonToStr(const nlohmann::json& j, const char* k) {
    const auto it = j.find(k);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    if (it->is_number_float()) return std::to_string(it->get<double>());
    if (it->is_boolean()) return it->get<bool>() ? "1" : "0";
    return {};
}

VideoItem ParseVodItem(const nlohmann::json& v, const VideoSite& site) {
    VideoItem item;
    item.id = JsonToStr(v, "vod_id");
    item.site_key = site.key.empty() ? site.name : site.key;
    item.site_name = site.name;
    item.title = JsonToStr(v, "vod_name");
    item.pic = JsonToStr(v, "vod_pic");
    item.remarks = JsonToStr(v, "vod_remarks");
    item.score = JsonToStr(v, "vod_score");
    if (item.score.empty()) item.score = JsonToStr(v, "vod_douban_score");
    if (item.score.empty()) item.score = JsonToStr(v, "score");
    item.type_name = JsonToStr(v, "type_name");
    item.year = JsonToStr(v, "vod_year");
    item.area = JsonToStr(v, "vod_area");
    item.actor = JsonToStr(v, "vod_actor");
    item.description = JsonToStr(v, "vod_content");
    item.raw = v;
    return item;
}

VideoListing ParseListJson(const nlohmann::json& root, const VideoSite& site) {
    VideoListing listing;
    if (root.contains("list") && root["list"].is_array()) {
        for (const auto& v : root["list"]) {
            if (!v.is_object()) continue;
            listing.items.push_back(ParseVodItem(v, site));
        }
    }
    if (root.contains("class") && root["class"].is_array()) {
        for (const auto& c : root["class"]) {
            if (!c.is_object()) continue;
            VideoCategory cat;
            cat.id = JsonToStr(c, "type_id");
            cat.name = JsonToStr(c, "type_name");
            cat.parent_id = JsonToStr(c, "type_pid");
            if (cat.id.empty() && cat.name.empty()) continue;
            listing.categories.push_back(std::move(cat));
        }
    }
    if (root.contains("page") && root["page"].is_number())
        listing.page = root["page"].get<int>();
    if (root.contains("pagecount") && root["pagecount"].is_number())
        listing.page_count = root["pagecount"].get<int>();
    listing.is_end = listing.page >= listing.page_count;
    return listing;
}

// 把 vod_play_from / vod_play_url 拆成多路线路 + 选集。
std::vector<VideoLine> ParsePlaySources(const nlohmann::json& vod) {
    std::vector<VideoLine> lines;
    std::string from = vod.contains("vod_play_from") && vod["vod_play_from"].is_string()
                           ? vod["vod_play_from"].get<std::string>() : "";
    std::string url = vod.contains("vod_play_url") && vod["vod_play_url"].is_string()
                          ? vod["vod_play_url"].get<std::string>() : "";
    std::vector<std::string> froms = SplitBy(from, "$$$");
    std::vector<std::string> urls = SplitBy(url, "$$$");
    const size_t n = std::max(froms.size(), urls.size());
    for (size_t i = 0; i < n; ++i) {
        VideoLine line;
        line.name = i < froms.size() ? froms[i] : ("线路" + std::to_string(i + 1));
        std::string seg = i < urls.size() ? urls[i] : "";
        // ⚠ CMS 的 vod_play_url 标准格式是「集名$地址#集名$地址」——# 才是分集
        // 分隔符。以前直接按 $ 切整串，`第01集$url` 会被切成「第01集」「url」
        // 两个假选集（列表里每集显示成两行、一行名字一行网址）。
        // 兼容两种变体：
        //   · 标准分集：每段都含 '$' → 按 # 切分集，段内再按 $ 拆名/址；
        //   · url#集名（部分源把集名拼在地址后）：整串当单集，交给下面的
        //     # 兼容逻辑拆。
        std::vector<std::string> eps = SplitBy(seg, "#");
        bool standard = eps.size() > 1;
        if (standard) {
            for (const auto& h : eps) {
                if (h.find('$') == std::string::npos) { standard = false; break; }
            }
        }
        if (!standard) eps = {seg};
        for (auto& e : eps) {
            if (e.empty()) continue;
            VideoEpisode ep;
            const size_t cut = e.find('$');  // 部分源 "名称$地址"
            if (cut != std::string::npos) {
                ep.name = e.substr(0, cut);
                ep.url = e.substr(cut + 1);
            } else {
                // 部分源把集名拼在地址后面且没有「名称$地址」结构：
                // "https://.../index.m3u8#第5期下" —— # 前是地址、# 后是集名。
                // 以前直接 name=url=整串，选集列表的「原名」列显示的就是一串
                // 播放地址（用户反馈「应该显示的是标题」）。
                const size_t hash = e.find('#');
                if (hash != std::string::npos && hash > 0 &&
                    e.find("://") != std::string::npos) {
                    ep.url = e.substr(0, hash);
                    ep.name = e.substr(hash + 1);
                } else {
                    ep.name = e;
                    ep.url = e;
                }
            }
            if (ep.name.empty())
                ep.name = "第" + std::to_string(line.episodes.size() + 1) + "集";
            // 保住 player flag：TVBox 的 playerContent(flag, id, vipFlags)
            // 第一个参数就是 vod_play_from，丢了就无法按线路派发播放器。
            ep.flag = line.name;
            line.episodes.push_back(std::move(ep));
        }
        if (!line.episodes.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> SplitBy(std::string_view s, std::string_view sep) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) { out.emplace_back(s.substr(start)); break; }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + sep.size();
    }
    return out;
}

// ── JSON 容错解析 ─────────────────────────────────────────────
//
// 部分 CMS 响应会被掐断或夹带噪声（图3 的 parse_error.101「unexpected end of
// input」就是响应在 755 字节处截断）。直接 parse 会把异常原样抛给上层，页面
// 只剩「加载选集失败」的英文报错。这里分三步抢救：
//   1) 去 BOM 后严格解析；
//   2) 丢掉 JSON 之前的噪声文本再试；
//   3) 按截断处之前的最后一个完整值补齐右括号后重试；
// 全部失败才抛中文错误（不再把 nlohmann 的英文异常文案透给用户）。

void StripBom(std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

bool ParseJsonNoThrow(const std::string& text, nlohmann::json* out) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return false;
    if (out) *out = std::move(j);
    return true;
}

// 找到第一个 '{' / '['，丢掉前面可能混入的空白、BOM 残留或提示文本。
std::string JsonFromFirstBrace(const std::string& s) {
    const size_t pos = s.find_first_of("{[");
    return pos == std::string::npos ? std::string() : s.substr(pos);
}

// 截断修复用的候选点：扫描时记录「逗号」与「右括号」处的容器栈快照 ——
// 这些位置之前的文本一定停在完整值之后。从最靠后的候选点往前试，按栈补齐
// 右括号，总能在数据被截断时抢救出前若干条记录。
struct JsonCutPoint {
    size_t pos = 0;              // 前缀长度（右括号含自身，逗号不含）
    std::vector<char> open;      // 该点仍需补齐的容器（'{' / '['）
};

std::string RepairTruncatedJson(const std::string& text) {
    std::vector<JsonCutPoint> cuts;
    std::vector<char> open;
    bool in_str = false;
    bool esc = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{' || c == '[') { open.push_back(c); continue; }
        if (c == '}' || c == ']') {
            if (!open.empty()) open.pop_back();
            cuts.push_back(JsonCutPoint{i + 1, open});
            continue;
        }
        if (c == ',') cuts.push_back(JsonCutPoint{i, open});
    }
    // 候选过多时只看尾部，避免大响应上的 O(n²) 开销失控。
    constexpr size_t kMaxTries = 2000;
    const size_t begin = cuts.size() > kMaxTries ? cuts.size() - kMaxTries : 0;
    for (size_t k = cuts.size(); k > begin; --k) {
        const JsonCutPoint& cp = cuts[k - 1];
        std::string fixed = text.substr(0, cp.pos);
        for (size_t j = cp.open.size(); j > 0; --j)
            fixed.push_back(cp.open[j - 1] == '{' ? '}' : ']');
        nlohmann::json j;
        if (ParseJsonNoThrow(fixed, &j)) return fixed;
    }
    return {};
}

nlohmann::json GetJson(const VideoSite& site, const std::string& url,
                       const NetworkConfig& cfg) {
    // 视频首页会并发请求几十个站点，个别失效源用默认 20s 超时会把整页
    // 拖成「一直在准备」。单请求 6 秒兜底，快的源照常先出卡片。
    musicfree::ScopedHttpTimeout timeout(6000);
    auto headers = BuildSiteHeaders(site, cfg.user_agent);
    musicfree::HttpExchange ex = musicfree::PerformHttpRequest(
        url, "GET", headers, {}, cfg);
    if (ex.status == 0)
        throw std::runtime_error("网络错误（无响应）");
    std::string body(ex.body.begin(), ex.body.end());
    StripBom(body);

    nlohmann::json parsed;
    if (ParseJsonNoThrow(body, &parsed)) return parsed;

    const std::string trimmed = JsonFromFirstBrace(body);
    if (!trimmed.empty() && trimmed != body &&
        ParseJsonNoThrow(trimmed, &parsed)) {
        return parsed;
    }

    const std::string repaired =
        RepairTruncatedJson(trimmed.empty() ? body : trimmed);
    if (!repaired.empty() && ParseJsonNoThrow(repaired, &parsed)) return parsed;

    throw std::runtime_error("源返回数据格式错误（可能已失效）");
}

} // namespace

namespace {

// spider 引擎返回的条目不带 site_key/site_name（那是 CMS 的 ParseVodItem 才填的）。
// 之前直接把 SpiderHome 的列表返回出去，导致首页聚合后 383/403 张卡片「有 id、
// 没有 site」——点开就报「卡片缺少站点/编号标识」。这里统一回填。
void FillSiteIdentity(VideoListing& listing, const VideoSite& site) {
    const std::string key = site.key.empty() ? site.name : site.key;
    for (auto& it : listing.items) {
        if (it.site_key.empty()) it.site_key = key;
        if (it.site_name.empty()) it.site_name = site.name;
    }
}

} // namespace

VideoListing FetchHome(const VideoSite& site, const NetworkConfig& cfg) {
    // 原生 spider：交给注册表里的引擎（模板 A/B/C/D/E 各自实现）。
    if (site.kind == VideoSourceKind::kNativeSpider) {
        VideoListing listing = SpiderHome(site, cfg);
        FillSiteIdentity(listing, site);
        return listing;
    }
    std::map<std::string, std::string> p{{"ac", "list"}};
    // 首页一般先用 ac=list 拿到分类，再用 ac=detail 拿带封面的卡片；
    // 这里用 ac=videolist（部分源支持）直接返回带封面的列表，失败回退 ac=list。
    VideoListing listing;
    try {
        auto root = GetJson(site, BuildCmsUrl(site, {{"ac", "videolist"}, {"pg", "1"}}), cfg);
        listing = ParseListJson(root, site);
    } catch (...) {
        auto root = GetJson(site, BuildCmsUrl(site, p), cfg);
        listing = ParseListJson(root, site);
    }
    // 部分源的 videolist 响应不带 class（分类表）→ 影视仓式分类页签会空。
    // 补一发 ac=list 只为拿分类，失败不影响已有卡片。
    if (listing.categories.empty()) {
        try {
            auto root = GetJson(site, BuildCmsUrl(site, {{"ac", "list"}, {"pg", "1"}}), cfg);
            if (root.contains("class") && root["class"].is_array()) {
                VideoListing cats = ParseListJson(root, site);
                listing.categories = std::move(cats.categories);
            }
        } catch (...) {}
    }
    // 各站「最新」列表往往整页都是剧集 / 动漫，首页因此没有「热播电影」。
    // 分类表里有电影类（子类常叫「动作片 / 喜剧片 …」，父类叫「电影」）时，
    // 再补抓一页该分类；优先子类（父类通常只是分组、本身没有内容）。
    if (!listing.categories.empty()) {
        auto ends_with_pian = [](const std::string& s) {
            return s.size() >= 3 && s.compare(s.size() - 3, 3, "片") == 0;
        };
        std::string movie_id;
        std::string parent_id;
        for (const auto& cat : listing.categories) {
            if (cat.id.empty() || cat.name.empty()) continue;
            if (cat.name.find("纪录片") != std::string::npos ||
                cat.name.find("专题片") != std::string::npos)
                continue;
            if (ends_with_pian(cat.name)) {
                movie_id = cat.id;
                break;
            }
            if (parent_id.empty() &&
                cat.name.find("电影") != std::string::npos)
                parent_id = cat.id;
        }
        if (movie_id.empty()) movie_id = parent_id;
        if (!movie_id.empty()) {
            try {
                auto root = GetJson(
                    site,
                    BuildCmsUrl(site, {{"ac", "videolist"},
                                       {"t", movie_id},
                                       {"pg", "1"}}),
                    cfg);
                VideoListing movies = ParseListJson(root, site);
                std::set<std::string> known;
                for (const auto& it : listing.items) known.insert(it.id);
                for (auto& it : movies.items) {
                    if (!it.id.empty() && !known.insert(it.id).second) continue;
                    listing.items.push_back(std::move(it));
                    if (listing.items.size() >= 80) break;
                }
            } catch (...) {
                // 补抓电影失败不影响首页其它内容。
            }
        }
    }
    return listing;
}

VideoListing FetchCategory(const VideoSite& site, std::string_view category_id,
                           int page, const NetworkConfig& cfg) {
    if (site.kind == VideoSourceKind::kNativeSpider) {
        VideoListing listing = SpiderCategory(site, category_id, page, cfg);
        FillSiteIdentity(listing, site);
        return listing;
    }
    // 影视仓对 JSON 源用 ac=detail&t= 拿列表（带封面）。苹果 CMS 系的等价
    // 参数是 ac=videolist；ac=list 的列表通常没有 vod_pic，海报墙会变灰块。
    // 先试 videolist，失败回退 list。
    try {
        auto root = GetJson(site, BuildCmsUrl(site, {{"ac", "videolist"},
                                                     {"t", std::string(category_id)},
                                                     {"pg", std::to_string(page)}}), cfg);
        VideoListing listing = ParseListJson(root, site);
        bool has_pic = false;
        for (const auto& it : listing.items)
            if (!it.pic.empty()) { has_pic = true; break; }
        if (!listing.items.empty() && has_pic) return listing;
    } catch (...) {}
    auto root = GetJson(site, BuildCmsUrl(site, {{"ac", "list"},
                                                 {"t", std::string(category_id)},
                                                 {"pg", std::to_string(page)}}), cfg);
    return ParseListJson(root, site);
}

VideoListing FetchSearch(const VideoSite& site, std::string_view keyword,
                         int page, const NetworkConfig& cfg) {
    if (site.kind == VideoSourceKind::kNativeSpider) {
        VideoListing listing = SpiderSearch(site, keyword, page, cfg);
        FillSiteIdentity(listing, site);
        return listing;
    }
    auto root = GetJson(site, BuildCmsUrl(site, {{"ac", "list"}, {"wd", std::string(keyword)}, {"pg", std::to_string(page)}}), cfg);
    return ParseListJson(root, site);
}

VideoDetail FetchDetail(const VideoSite& site, const VideoItem& item,
                        const NetworkConfig& cfg) {
    if (site.kind == VideoSourceKind::kNativeSpider)
        return SpiderDetail(site, item, cfg);
    VideoDetail detail;
    detail.id = item.id;
    detail.title = item.title;
    detail.pic = item.pic;
    detail.remarks = item.remarks;
    detail.raw = item.raw;
    auto root = GetJson(site, BuildCmsUrl(site, {{"ac", "detail"}, {"ids", item.id}}), cfg);
    if (root.contains("list") && root["list"].is_array() && !root["list"].empty()) {
        const auto& vod = root["list"][0];
        detail.lines = ParsePlaySources(vod);
    }
    return detail;
}

LiveListing FetchLive(const VideoLive& live, const NetworkConfig& cfg) {
    LiveListing out;
    try {
        std::string body = musicfree::DownloadText(live.url, cfg);
        if (body.empty()) { out.error = "直播内容为空"; return out; }
        std::string lower = live.url;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find(".m3u") != std::string::npos || body.find("#EXTM3U") != std::string::npos) {
            std::string group = live.group.empty() ? "直播" : live.group;
            std::string name;
            std::stringstream ss(body);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                if (line.rfind("#EXTINF", 0) == 0) {
                    size_t comma = line.find(',');
                    name = comma != std::string::npos ? line.substr(comma + 1) : line;
                } else if (line.front() != '#' && line.find("://") != std::string::npos) {
                    LiveChannel ch;
                    ch.name = name.empty() ? line : name;
                    ch.group = group;
                    ch.url = line;
                    out.channels.push_back(std::move(ch));
                    name.clear();
                }
            }
        } else {
            // TXT：每行 "名称,url" 或 "名称$url"
            std::stringstream ss(body);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.empty() || line.front() == '#') continue;
                std::string name, url;
                size_t pos = line.find(',');
                if (pos == std::string::npos) pos = line.find('$');
                if (pos != std::string::npos) {
                    name = line.substr(0, pos);
                    url = line.substr(pos + 1);
                } else {
                    url = line;
                    name = line;
                }
                if (url.find("://") != std::string::npos) {
                    LiveChannel ch;
                    ch.name = name;
                    ch.group = live.group.empty() ? "直播" : live.group;
                    ch.url = url;
                    out.channels.push_back(std::move(ch));
                }
            }
        }
    } catch (const std::exception& e) {
        out.error = std::string("直播解析失败: ") + e.what();
    }
    return out;
}

} // namespace ttplayer::video
