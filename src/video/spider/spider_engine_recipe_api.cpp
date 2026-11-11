// 通用「声明式 recipe」引擎（JSON-API 模板：app / live / pan 的接口型 recipe）
//
// 设计目标：把 TVBox 的 spider 还原 recipe（base + headers + request{sign,encoding}
// + endpoints{home,category,detail,search,player} + extract{listing,detail,player}
// + 可选的 response.decrypt）直接解释执行，无需 Java 运行库。cms/html 由各自的专用
// 引擎（RecipeCmsEngine / RecipeHtmlEngine）负责；本引擎只承接「接口型」recipe。
//
// 与专用引擎的关系：RegisterRecipeApiSpiders() 扫描 recipe 目录，仅注册「尚未被专用
// 引擎注册过」的 pan/live/app recipe（skip_if_registered）。因此不会覆盖已实测跑通的
// Alllive/Bili/AppTT/JPJ/S_zps 等专用实现，也不会把「原生未实现」的站点误标成可用——
// 专用引擎对它们的 implemented=false 登记优先保留。一旦某站点从 smali 逆向出 AES
// key/iv，只要在 recipe 里补上 response.decrypt，本引擎即可直接解密并点亮该站点。

#include "ttplayer/video/spider/spider_api.h"
#include "ttplayer/video/spider/spider_http.h"
#include "ttplayer/video/spider/spider_recipe.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/spider/spider_util.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ttplayer::video {
namespace su = spider_util;

namespace {

using json = nlohmann::json;

// ---- 解析一次 recipe + ext，准备请求 ----
struct ApiProfile {
    json recipe;
    std::string base;
    std::map<std::string, std::string> headers;
};

std::string TrimCopy(std::string_view s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

json ParseExt(std::string_view ext) {
    if (ext.empty()) return json::object();
    json j = su::ParseJsonLoose(ext);
    if (j.is_object()) return j;
    return json::object();
}

ApiProfile BuildProfile(const SpiderContext& ctx) {
    ApiProfile p;
    const std::string id = ctx.spider_id.empty() ? ctx.site.api : ctx.spider_id;
    const SpiderRecipeLoad load = LoadSpiderRecipe(id);
    if (!load.ok)
        throw std::runtime_error("RecipeApiEngine：recipe 缺失 " +
                                (load.error.empty() ? id : load.error));
    p.recipe = load.recipe.raw;
    p.base = su::Trim(load.recipe.base);
    if (p.base.empty() && p.recipe.contains("base") && p.recipe["base"].is_string())
        p.base = su::Trim(p.recipe["base"].get<std::string>());

    // headers：recipe.headers 打底，ext.header 覆盖。
    const json ext = ParseExt(ctx.ext.empty() ? ctx.site.ext : ctx.ext);
    if (p.recipe.contains("headers") && p.recipe["headers"].is_object())
        for (auto it = p.recipe["headers"].begin(); it != p.recipe["headers"].end(); ++it)
            if (it.value().is_string())
                p.headers[it.key()] = it.value().get<std::string>();
    if (ext.contains("header")) {
        json h = ext["header"];
        if (h.is_string()) h = su::ParseJsonLoose(h.get<std::string>());
        if (h.is_object())
            for (auto it = h.begin(); it != h.end(); ++it)
                if (it.value().is_string())
                    p.headers[it.key()] = it.value().get<std::string>();
    }
    if (!p.headers.count("User-Agent"))
        p.headers["User-Agent"] =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/122.0.6299.95 Safari/537.36";
    return p;
}

// ---- 变量解析（endpoint.params 的 value / path 的 {x} 占位）----
std::string ResolveSource(std::string_view src, const std::map<std::string, std::string>& vars,
                          const json& ext, std::string_view keyword, int page,
                          std::string_view category_id, const VideoItem* item) {
    const std::string s(TrimCopy(src));
    if (s == "keyword") return std::string(keyword);
    if (s == "page") return std::to_string(page <= 0 ? 1 : page);
    if (s == "type_id" || s == "cid" || s == "category_id" || s == "cateId")
        return std::string(category_id);
    if (s == "vod_id" || s == "id" || s == "album_id" || s == "roomId") {
        if (item) return item->id;
        return {};
    }
    if (s == "index") return item ? std::to_string(0) : std::string("0");
    if (s == "timestamp") return std::to_string(su::UnixSeconds());
    if (s == "uuid" || s == "nonce")
        return s == "uuid" ? su::UuidV4() : su::RandomHex(8);
    if (su::StartsWith(s, "ext.") && ext.is_object()) {
        const std::string key = s.substr(4);
        if (ext.contains(key) && ext[key].is_string()) return ext[key].get<std::string>();
        return {};
    }
    // 未知 → 当作字面量原样返回（如 "21" / "time" / "1" / "version"）。
    return s;
}

std::string Substitute(std::string_view tmpl, const std::map<std::string, std::string>& vars,
                       const json& ext, std::string_view keyword, int page,
                       std::string_view category_id, const VideoItem* item) {
    std::string out;
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            const std::size_t e = tmpl.find('}', i);
            if (e == std::string_view::npos) { out += tmpl.substr(i); break; }
            const std::string name = std::string(tmpl.substr(i + 1, e - i - 1));
            out += ResolveSource(name, vars, ext, keyword, page, category_id, item);
            i = e + 1;
        } else {
            out += tmpl[i++];
        }
    }
    return out;
}

// ---- 签名 ----
void ApplySign(const json& request, std::map<std::string, std::string>& vars,
               const json& ext, std::string_view keyword, int page,
               std::string_view category_id, const VideoItem* item) {
    const json sign = request.value("sign", json::object());
    const std::string type = su::ToLower(sign.value("type", json("none")).get<std::string>());
    if (type == "none" || type.empty()) return;
    std::string material;
    if (sign.contains("fields") && sign["fields"].is_array()) {
        for (const auto& f : sign["fields"]) {
            if (!f.is_string()) continue;
            material += Substitute(f.get<std::string>(), vars, ext, keyword, page,
                                  category_id, item);
        }
    }
    std::string sig;
    if (type == "md5") sig = su::Md5Hex(material);
    else if (type == "sha256") sig = su::Sha256Hex(material);
    if (!sig.empty()) vars["sign"] = sig;
}

// ---- 取 endpoint 配置并构造 URL/body ----
bool BuildRequest(const ApiProfile& p, std::string_view op,
                  const std::map<std::string, std::string>& base_vars, const json& ext,
                  std::string_view keyword, int page, std::string_view category_id,
                  const VideoItem* item, std::string& url, std::string& body,
                  bool& is_post) {
    const json endpoints = p.recipe.value("endpoints", json::object());
    if (!endpoints.contains(op) || !endpoints[op].is_object()) return false;
    const json ep = endpoints[op];

    auto vars = base_vars;
    // 先把 timestamp/uuid/nonce 等可确定的变量填上，便于签名。
    vars["timestamp"] = std::to_string(su::UnixSeconds());
    vars["uuid"] = su::UuidV4();
    vars["nonce"] = su::RandomHex(8);

    // path 占位替换。
    std::string path = ep.value("path", json("")).get<std::string>();
    path = Substitute(path, vars, ext, keyword, page, category_id, item);

    // 绝对 URL（部分 recipe 的 search/player 直接用完整域名）。
    if (su::StartsWith(path, "http://") || su::StartsWith(path, "https://")) {
        url = path;
    } else {
        url = su::JoinUrl(p.base, path);
    }

    // 查询 / 表单参数。
    std::vector<std::string> parts;
    if (ep.contains("params") && ep["params"].is_object()) {
        for (auto it = ep["params"].begin(); it != ep["params"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string val = ResolveSource(it.value().get<std::string>(), vars, ext,
                                                 keyword, page, category_id, item);
            if (val.empty()) continue;
            parts.push_back(su::UrlEncode(it.key()) + "=" + su::UrlEncode(val));
        }
    }
    const std::string method = su::ToLower(p.recipe.value("request", json::object())
                                              .value("method", json("GET")).get<std::string>());
    is_post = (method == "post");
    if (!parts.empty()) {
        if (is_post) body = su::JoinStrings(parts, "&");
        else url += std::string(url.find('?') == std::string::npos ? "?" : "&") +
                    su::JoinStrings(parts, "&");
    }

    // 签名（可能往 vars 里写 sign，再补到参数或 body）。
    ApplySign(p.recipe.value("request", json::object()), vars, ext, keyword, page,
              category_id, item);
    if (vars.count("sign")) {
        if (is_post) {
            body += std::string(body.empty() ? "" : "&") + "sign=" +
                    su::UrlEncode(vars["sign"]);
        } else {
            url += std::string(url.find('?') == std::string::npos ? "?" : "&") +
                   "sign=" + su::UrlEncode(vars["sign"]);
        }
    }
    return true;
}

// ---- 取响应 + 解码（lzstring / AES） + 解析 ----
json FetchJson(SpiderHttp& http, const ApiProfile& p, const std::string& url,
               const std::string& body, bool is_post, const char* stage) {
    http.SetStage(stage);
    SpiderHttpResponse resp;
    if (is_post)
        resp = http.Post(url, body, p.headers, "application/x-www-form-urlencoded");
    else
        resp = http.Get(url);

    std::string text = resp.body;
    if (text.empty() || resp.status >= 400)
        throw std::runtime_error("RecipeApiEngine：" + std::string(stage) +
                                " 请求失败 status=" + std::to_string(resp.status));

    // encoding：lzstring-base64 由各自专用引擎（如 Alllive）处理；通用引擎此处
    // 直接按明文/已解密内容解析。若未来需要，可在 spider_util 增加 LzStringDecompress。
    json root = su::ParseJsonLoose(text);

    // response.decrypt：AES-CBC（key/iv 来自 recipe 或 ext）。
    if (p.recipe.contains("response") && p.recipe["response"].is_object()) {
        const json dec = p.recipe["response"];
        if (dec.value("decrypt", json("")).get<std::string>() == "aes-cbc") {
            const json ext_json = ParseExt(p.recipe.value("response", json::object())
                                               .value("ext", json("")).get<std::string>());
            auto field = [&](const char* name, const json& j) -> std::string {
                if (!j.contains(name)) return {};
                if (j[name].is_string()) return j[name].get<std::string>();
                return {};
            };
            std::string key = field("key", dec);
            std::string iv = field("iv", dec);
            // key/iv 也可能是 hex/base64，这里直接当原始字节；如为 hex 可在此扩展。
            if (!key.empty() && !iv.empty()) {
                std::string plain = su::AesCbcDecrypt(key, iv, text);
                if (!plain.empty()) root = su::ParseJsonLoose(plain);
            }
        }
    }
    return root;
}

// ---- 提取：JSONPath 字段 → VideoItem ----
bool IsJsonPath(std::string_view s) {
    return s.find('$') != std::string_view::npos || s.find('.') != std::string_view::npos ||
           s.find('[') != std::string_view::npos;
}

std::string PickField(const json& node, std::string_view spec) {
    if (IsJsonPath(spec)) {
        if (const json* j = su::JsonFind(node, spec)) {
            if (j->is_string()) return j->get<std::string>();
            if (j->is_number()) return std::to_string(j->get<long long>());
            if (j->is_boolean()) return j->get<bool>() ? "1" : "0";
        }
        // 数组型（如 vod_play_from 是字符串数组）→ 用 JsonFindAll 拼接。
        const auto arr = su::JsonFindAll(node, spec);
        std::string out;
        for (const json* j : arr) {
            std::string v;
            if (j->is_string()) v = j->get<std::string>();
            else if (j->is_number()) v = std::to_string(j->get<long long>());
            if (!v.empty()) { if (!out.empty()) out += "$$$"; out += v; }
        }
        return out;
    }
    return su::JsonString(node, spec);
}

VideoItem MakeItem(const json& node, const json& fields, std::string_view site_key,
                   std::string_view site_name) {
    VideoItem item;
    item.site_key = std::string(site_key);
    item.site_name = std::string(site_name);
    if (fields.is_object()) {
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            const std::string val = PickField(node, it.value().is_string()
                                                  ? it.value().get<std::string>()
                                                  : std::string());
            const std::string out = it.key();
            if (out == "vod_id") item.id = val;
            else if (out == "vod_name") item.title = val;
            else if (out == "vod_pic") item.pic = val;
            else if (out == "vod_remarks") item.remarks = val;
        }
    }
    item.raw = node;
    return item;
}

VideoListing ExtractListing(const json& root, const json& sec, std::string_view site_key,
                           std::string_view site_name, int page) {
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    if (!sec.is_object()) return listing;
    const std::string items_path = sec.value("items", json("")).get<std::string>();
    if (!items_path.empty()) {
        for (const json* node : su::JsonFindAll(root, items_path)) {
            if (!node->is_object()) continue;
            listing.items.push_back(
                MakeItem(*node, sec.value("fields", json::object()), site_key, site_name));
        }
    }
    listing.is_end = listing.items.empty();
    return listing;
}

VideoDetail ExtractDetail(const json& root, const json& sec, const std::string& id) {
    VideoDetail detail;
    detail.id = id;
    const auto nodes = sec.is_object() && sec.contains("items")
                          ? su::JsonFindAll(root, sec.value("items", json("")).get<std::string>())
                          : std::vector<const json*>{&root};
    const json* node = nodes.empty() ? &root : nodes.front();
    if (!node || !node->is_object()) return detail;
    const json fields = sec.is_object() ? sec.value("fields", json::object()) : json::object();
    if (fields.is_object()) {
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            const std::string val = PickField(*node, it.value().is_string()
                                                  ? it.value().get<std::string>()
                                                  : std::string());
            const std::string out = it.key();
            if (out == "vod_name") detail.title = val;
            else if (out == "vod_pic") detail.pic = val;
            else if (out == "vod_remarks") detail.remarks = val;
        }
    }
    // 播放线路：play_from / play_url 按 group_sep / ep_sep 拆分（TVBox 约定）。
    std::string froms = PickField(*node, fields.is_object() ? "vod_play_from" : "play_from");
    std::string urls = PickField(*node, fields.is_object() ? "vod_play_url" : "play_url");
    if (!froms.empty() && !urls.empty()) {
        const std::string gsep = "$$$";
        const std::string esep = "#";
        std::vector<std::string> from_vec = su::Split(froms, gsep[0]);
        std::vector<std::string> url_vec = su::Split(urls, gsep[0]);
        for (std::size_t i = 0; i < from_vec.size(); ++i) {
            VideoLine line;
            line.name = from_vec[i];
            const std::string u = i < url_vec.size() ? url_vec[i] : std::string();
            for (const std::string& ep : su::Split(u, esep[0])) {
                VideoEpisode e;
                e.name = std::to_string(line.episodes.size() + 1);
                e.url = ep;
                line.episodes.push_back(e);
            }
            detail.lines.push_back(line);
        }
    }
    detail.raw = *node;
    return detail;
}

// ---- 引擎本体 ----
class RecipeApiEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        const ApiProfile p = BuildProfile(ctx);
        SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
        http.SetDefaultHeaders(p.headers);
        std::map<std::string, std::string> vars;
        std::string url, body;
        bool post = false;
        if (!BuildRequest(p, "home", vars, ParseExt(ctx.ext.empty() ? ctx.site.ext : ctx.ext),
                          "", 1, "", nullptr, url, body, post))
            return VideoListing{};
        const json root = FetchJson(http, p, url, body, post, "home");
        const json extract = p.recipe.value("extract", json::object());
        VideoListing listing = ExtractListing(root, extract.value("listing", json::object()),
                                             ctx.site.key, ctx.site.name, 1);
        // 首页分类（如有）。
        const json cats = extract.value("categories", json::object());
        if (cats.is_object() && cats.contains("items")) {
            for (const json* node : su::JsonFindAll(root, cats.value("items", json("")).get<std::string>())) {
                VideoCategory c;
                if (node->is_object()) {
                    c.id = su::JsonString(*node, cats.value("group_fields", json::object())
                                                  .value("id", json("id")).get<std::string>());
                    c.name = su::JsonString(*node, cats.value("group_fields", json::object())
                                                  .value("name", json("name")).get<std::string>());
                }
                if (!c.name.empty()) listing.categories.push_back(c);
            }
        }
        return listing;
    }

    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                         int page) override {
        const ApiProfile p = BuildProfile(ctx);
        SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
        http.SetDefaultHeaders(p.headers);
        std::map<std::string, std::string> vars;
        std::string url, body;
        bool post = false;
        if (!BuildRequest(p, "category", vars,
                          ParseExt(ctx.ext.empty() ? ctx.site.ext : ctx.ext), "", page,
                          category_id, nullptr, url, body, post))
            return VideoListing{};
        const json root = FetchJson(http, p, url, body, post, "category");
        return ExtractListing(root, p.recipe.value("extract", json::object())
                                          .value("listing", json::object()),
                              ctx.site.key, ctx.site.name, page);
    }

    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                       int page) override {
        const ApiProfile p = BuildProfile(ctx);
        SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
        http.SetDefaultHeaders(p.headers);
        std::map<std::string, std::string> vars;
        std::string url, body;
        bool post = false;
        if (!BuildRequest(p, "search", vars, ParseExt(ctx.ext.empty() ? ctx.site.ext : ctx.ext),
                          keyword, page, "", nullptr, url, body, post))
            return VideoListing{};
        const json root = FetchJson(http, p, url, body, post, "search");
        return ExtractListing(root, p.recipe.value("extract", json::object())
                                          .value("listing", json::object()),
                              ctx.site.key, ctx.site.name, page);
    }

    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        const ApiProfile p = BuildProfile(ctx);
        SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
        http.SetDefaultHeaders(p.headers);
        std::map<std::string, std::string> vars;
        std::string url, body;
        bool post = false;
        if (!BuildRequest(p, "detail", vars, ParseExt(ctx.ext.empty() ? ctx.site.ext : ctx.ext),
                          "", 1, "", &item, url, body, post))
            return VideoDetail{};
        const json root = FetchJson(http, p, url, body, post, "detail");
        return ExtractDetail(root, p.recipe.value("extract", json::object())
                                         .value("detail", json::object()),
                             item.id);
    }

    std::string Player(const SpiderContext& ctx, const VideoEpisode& episode) override {
        (void)ctx;
        // 通用 recipe 引擎不重写 Player：播放地址通常已是直链或需上层解析链处理。
        return episode.url;
    }
};

// 注册「尚未被专用引擎覆盖」的 pan/live/app recipe 为原生（通用）实现。
void RegisterRecipeApiSpiders() {
    auto factory = [] { return std::make_unique<RecipeApiEngine>(); };
    // skip_if_registered=true：专用引擎已注册的（含诚实标为未实现的 MyDrive/Push/
    // Fy/App* 等需要外部密钥/凭据/代理）一律跳过，零回归；仅填补未被专用引擎覆盖的缺口。
    // 该函数会同时扫描目录与内嵌副本，部署版无需外部文件也能点亮。
    RegisterRecipeSpiderVariants(SpiderTemplate::kPan, factory, true);
    RegisterRecipeSpiderVariants(SpiderTemplate::kLive, factory, true);
    RegisterRecipeSpiderVariants(SpiderTemplate::kApp, factory, true);
}

// 静态引导：注册表构造完成后扫描并注册未被专用引擎覆盖的 pan/live/app recipe。
struct RecipeApiBootstrap {
    RecipeApiBootstrap() {
        // 触发注册表构造（必要时），再注册；自身不重入。
        RegisterRecipeApiSpiders();
    }
};

} // namespace

namespace {
RecipeApiBootstrap g_recipe_api_bootstrap;
} // namespace

} // namespace ttplayer::video
