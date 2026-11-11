#include "ttplayer/video/spider/spider_builtins.h"

// 模板 B（App 私有签名接口）引擎注册。
//
// 9 个类共用一套 recipe 驱动的签名请求层：
//   - host/base、公共头、端点、参数、字段映射全部来自 sources/spiders/csp_*.json；
//   - request.sign.fields 支持占位符（{ts}/{ms}/{uuid}/{nonce}/{appkey}/{key}/
//     {device}/{version}/{package}），拼接顺序按数组顺序，separator 可选；
//   - 签名类型覆盖 none/md5/sha1/sha256/hmac-sha1/hmac-sha256；
//   - 响应 JSON 按 extract.listing / extract.detail 映射成 VideoListing/VideoDetail。
//
// 只有真实网络跑通的类才在注册表里标 implemented=true；其余保留注册（站点
// 会显示「原生未实现」而不是消失），原因写在 SpiderMeta::note。

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include "ttplayer/video/spider/spider_http.h"
#include "ttplayer/video/spider/spider_recipe.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/spider/spider_util.h"

namespace ttplayer::video {

void RegisterAppSpiderEngines();

namespace {

namespace su = spider_util;
using nlohmann::json;

// ---------- 小工具 ----------

std::string ToLowerCopy(std::string_view text) {
    return su::ToLower(text);
}

bool IsAbsoluteUrl(std::string_view text) {
    const std::string lower = ToLowerCopy(text);
    return su::StartsWith(lower, "http://") || su::StartsWith(lower, "https://");
}

// 拼接 base 与 path：保留 base 里的路径前缀（App99 的 host 带 /app/bn）。
std::string ComposeUrl(std::string base, std::string path) {
    base = su::Trim(base);
    path = su::Trim(path);
    if (path.empty()) return base;
    if (IsAbsoluteUrl(path)) return path;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (path.front() != '/') path.insert(path.begin(), '/');
    return base + path;
}

// 站点常返回 /upload/... 这类站内相对封面，补成绝对地址。
std::string NormalizePic(const std::string& base, std::string pic) {
    if (!pic.empty() && pic.front() == '/' && !base.empty())
        return ComposeUrl(base, pic);
    return pic;
}

std::string ScalarOf(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%g", value.get<double>());
        return buf;
    }
    if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
    return {};
}

// 字段值可以是简单 key，也可以是 "$.a.b[*].c" / "a.b" 形式的路径。
std::string ReadField(const json& obj, std::string_view path) {
    if (path.empty()) return {};
    const std::string p(path);
    if (p.find('.') == std::string::npos && p.find('[') == std::string::npos)
        return su::JsonString(obj, p);
    const json* found = su::JsonFind(obj, p);
    return found ? ScalarOf(*found) : std::string();
}

std::string MappedField(const json& obj, const json& fields, std::string_view key,
                        std::initializer_list<std::string_view> fallbacks = {}) {
    std::string path;
    if (fields.is_object()) path = su::JsonString(fields, key);
    if (!path.empty()) {
        const std::string value = ReadField(obj, path);
        if (!value.empty()) return value;
    }
    if (obj.is_object()) return su::JsonPick(obj, fallbacks);
    return {};
}

// 替换 {name} 占位符；未知占位符原样保留。
std::string ExpandPlaceholders(std::string text,
                               const std::map<std::string, std::string>& vars,
                               const json& ext) {
    std::size_t pos = 0;
    while ((pos = text.find('{', pos)) != std::string::npos) {
        const std::size_t end = text.find('}', pos + 1);
        if (end == std::string::npos) break;
        const std::string name = text.substr(pos + 1, end - pos - 1);
        auto it = vars.find(name);
        std::string value;
        if (it != vars.end())
            value = it->second;
        else if (ext.is_object() && ext.contains(name))
            value = ScalarOf(ext.at(name));
        if (value.empty() && it == vars.end()) {
            pos = end + 1;
            continue;
        }
        text.replace(pos, end - pos + 1, value);
        pos += value.size();
    }
    return text;
}

// ---------- HMAC（WinCrypt） ----------

std::string HmacHex(ALG_ID alg, std::string_view key, std::string_view data) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT))
        return out;
    if (CryptCreateHash(prov, CALG_HMAC, 0, 0, &hash)) {
        HMAC_INFO info{};
        info.HashAlgid = alg;
        if (CryptSetHashParam(hash, HP_HMAC_INFO,
                              reinterpret_cast<BYTE*>(&info), 0) &&
            (key.empty() ||
             CryptHashData(hash, reinterpret_cast<const BYTE*>(key.data()),
                           static_cast<DWORD>(key.size()), 0)) &&
            (data.empty() ||
             CryptHashData(hash, reinterpret_cast<const BYTE*>(data.data()),
                           static_cast<DWORD>(data.size()), 0))) {
            BYTE buf[64]{};
            DWORD len = sizeof(buf);
            if (CryptGetHashParam(hash, HP_HASHVAL, buf, &len, 0))
                out = su::HexEncode(std::string_view(
                    reinterpret_cast<const char*>(buf), len));
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

// ---------- 一次请求的上下文 ----------

struct AppRun {
    const SpiderContext* ctx = nullptr;
    const SpiderRecipe* recipe = nullptr;
    json ext;                  // ext 解析后的对象（纯 URL 也归一成 {"host": ...}）
    std::string base;
    std::string uuid;
    std::string nonce;
    std::string device;
    long long ts = 0;
    long long ms = 0;
    std::map<std::string, std::string> vars;
    std::map<std::string, std::string> headers;
};

// ext 既可能是 JSON，也可能只是 host 字符串。
json ParseExtText(const std::string& text) {
    const std::string trimmed = su::Trim(text);
    if (trimmed.empty()) return json::object();
    const json parsed = su::ParseJsonLoose(trimmed);
    if (parsed.is_object()) return parsed;
    if (parsed.is_string()) {
        const std::string inner = parsed.get<std::string>();
        if (!inner.empty()) return json{{"host", inner}};
        return json::object();
    }
    return json{{"host", trimmed}};
}

std::string ExtString(const json& ext, std::string_view key) {
    if (!ext.is_object()) return {};
    return su::JsonString(ext, key);
}

// ---------- 模板 B 引擎 ----------

class AppEngineBase : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override;
    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override;
    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override;
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override;
    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override;

protected:
    virtual std::string ResolveBase(AppRun& run);
    virtual VideoListing ParseHome(AppRun& run, const json& root);
    virtual VideoListing ParseListing(AppRun& run, const json& root);
    virtual VideoDetail ParseDetail(AppRun& run, const json& root,
                                    const VideoItem& item);

    AppRun Begin(const SpiderContext& ctx);
    json Request(AppRun& run, std::string_view endpoint);

private:
    const SpiderRecipe& Recipe(const SpiderContext& ctx);
    std::string ComputeSign(AppRun& run, const json& request) const;
    std::map<std::string, std::string> BuildParams(const json& params,
                                                   AppRun& run) const;
    void BuildHeaders(AppRun& run);
    void AddEpisodes(const json& value, VideoLine* line) const;
    void AddEpisodeText(const std::string& text, VideoLine* line) const;

    SpiderRecipeLoad load_;
    bool loaded_ = false;
};

const SpiderRecipe& AppEngineBase::Recipe(const SpiderContext& ctx) {
    if (!loaded_) {
        load_ = LoadSpiderRecipe(ctx.spider_id.empty() ? ctx.site.api
                                                       : ctx.spider_id);
        loaded_ = true;
    }
    if (!load_.ok) {
        throw std::runtime_error("模板 B 缺少可用 recipe：" +
                                 (load_.error.empty() ? ctx.spider_id
                                                      : load_.error));
    }
    return load_.recipe;
}

AppRun AppEngineBase::Begin(const SpiderContext& ctx) {
    AppRun run;
    run.ctx = &ctx;
    run.recipe = &Recipe(ctx);
    run.ext = ParseExtText(ctx.ext.empty() ? ctx.site.ext : ctx.ext);
    run.uuid = su::UuidV4();
    run.nonce = su::RandomHex(8);
    run.ts = su::UnixSeconds();
    run.ms = su::UnixMilliseconds();
    run.device = ExtString(run.ext, "device_id");
    if (run.device.empty()) run.device = ExtString(run.ext, "deviceId");
    if (run.device.empty()) run.device = run.uuid;
    run.base = ResolveBase(run);
    BuildHeaders(run);
    return run;
}

std::string AppEngineBase::ResolveBase(AppRun& run) {
    std::string base = run.recipe->base;
    if (base.empty()) base = ExtString(run.ext, "host");
    if (base.empty()) base = ExtString(run.ext, "siteUrl");
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) {
        throw std::runtime_error("模板 B 缺少 host：recipe.base 与 ext.host 均为空");
    }
    return base;
}

void AppEngineBase::BuildHeaders(AppRun& run) {
    const auto it = run.recipe->raw.find("headers");
    if (it == run.recipe->raw.end() || !it->is_object()) return;
    std::map<std::string, std::string> vars = run.vars;
    vars["ts"] = std::to_string(run.ts);
    vars["ms"] = std::to_string(run.ms);
    vars["uuid"] = run.uuid;
    vars["nonce"] = run.nonce;
    vars["device"] = run.device;
    for (auto item = it->begin(); item != it->end(); ++item) {
        if (!item.value().is_string()) continue;
        std::string value = item.value().get<std::string>();
        if (value.rfind("ext.", 0) == 0) {
            value = ExtString(run.ext, value.substr(4));
        } else {
            value = ExpandPlaceholders(std::move(value), vars, run.ext);
        }
        if (!value.empty()) run.headers[item.key()] = value;
    }
}

std::string AppEngineBase::ComputeSign(AppRun& run, const json& request) const {
    const auto sign_it = request.find("sign");
    if (sign_it == request.end() || !sign_it->is_object()) return {};
    const json& sign = *sign_it;
    const std::string type = ToLowerCopy(su::JsonString(sign, "type", "none"));
    if (type.empty() || type == "none") return {};

    std::string key_field = su::JsonString(sign, "key_field");
    std::string key_value;
    if (!key_field.empty()) key_value = ExtString(run.ext, key_field);
    if (key_value.empty()) key_value = su::JsonString(sign, "key");

    std::map<std::string, std::string> vars;
    vars["ts"] = std::to_string(run.ts);
    vars["timestamp"] = std::to_string(run.ts);
    vars["time"] = std::to_string(run.ts);
    vars["cur_time"] = std::to_string(run.ts);
    vars["ms"] = std::to_string(run.ms);
    vars["uuid"] = run.uuid;
    vars["nonce"] = run.nonce;
    vars["device"] = run.device;
    vars["deviceid"] = run.device;
    vars["device_id"] = run.device;
    vars["appkey"] = ExtString(run.ext, "appkey");
    vars["key"] = ExtString(run.ext, "key");
    if (!key_value.empty()) vars["sign_key"] = key_value;
    vars["version"] = ExtString(run.ext, "versionName");
    if (vars["version"].empty()) vars["version"] = ExtString(run.ext, "version");
    vars["package"] = ExtString(run.ext, "package");
    for (const auto& kv : run.vars) {
        if (!kv.second.empty()) vars.emplace(kv.first, kv.second);
    }

    std::vector<std::string> parts;
    const auto fields_it = sign.find("fields");
    if (fields_it != sign.end() && fields_it->is_array()) {
        for (const auto& field : *fields_it) {
            if (!field.is_string()) continue;
            std::string token = field.get<std::string>();
            auto found = vars.find(token);
            if (found != vars.end()) {
                parts.push_back(found->second);
            } else if (token.find('{') != std::string::npos) {
                parts.push_back(ExpandPlaceholders(token, vars, run.ext));
            } else if (run.ext.is_object() && run.ext.contains(token)) {
                parts.push_back(ScalarOf(run.ext.at(token)));
            } else {
                parts.push_back(token);
            }
        }
    }
    const std::string separator = su::JsonString(sign, "separator");
    std::string raw;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) raw += separator;
        raw += parts[i];
    }

    std::string digest;
    if (type == "md5") {
        digest = su::Md5Hex(raw);
    } else if (type == "sha1") {
        digest = su::Sha1Hex(raw);
    } else if (type == "sha256") {
        digest = su::Sha256Hex(raw);
    } else if (type == "hmac" || type == "hmac-sha1") {
        digest = HmacHex(CALG_SHA1, key_value, raw);
    } else if (type == "hmac-sha256") {
        digest = HmacHex(CALG_SHA_256, key_value, raw);
    } else {
        throw std::runtime_error("模板 B 不支持的签名类型：" + type);
    }
    if (sign.contains("upper") && sign.at("upper").is_boolean() &&
        sign.at("upper").get<bool>()) {
        for (char& c : digest)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return digest;
}

std::map<std::string, std::string> AppEngineBase::BuildParams(
    const json& params, AppRun& run) const {
    std::map<std::string, std::string> out;
    if (!params.is_object()) return out;
    for (auto it = params.begin(); it != params.end(); ++it) {
        const std::string name = it.key();
        if (!it.value().is_string()) {
            out[name] = ScalarOf(it.value());
            continue;
        }
        std::string value = it.value().get<std::string>();
        if (value.rfind("ext.", 0) == 0) {
            value = ExtString(run.ext, value.substr(4));
        } else {
            auto found = run.vars.find(value);
            if (found != run.vars.end()) value = found->second;
            else if (run.ext.is_object() && run.ext.contains(value))
                value = ScalarOf(run.ext.at(value));
            else
                value = ExpandPlaceholders(std::move(value), run.vars, run.ext);
        }
        out[name] = value;
    }
    return out;
}

json AppEngineBase::Request(AppRun& run, std::string_view endpoint) {
    const json& endpoints = run.recipe->raw.value("endpoints", json::object());
    const auto it = endpoints.find(std::string(endpoint));
    if (it == endpoints.end() || !it->is_object()) {
        throw std::runtime_error("recipe 缺少端点：" + std::string(endpoint));
    }
    const json& spec = *it;
    const json request = run.recipe->raw.value("request", json::object());

    run.vars["ts"] = std::to_string(run.ts);
    run.vars["timestamp"] = std::to_string(run.ts);
    run.vars["ms"] = std::to_string(run.ms);
    run.vars["uuid"] = run.uuid;
    run.vars["nonce"] = run.nonce;
    run.vars["device"] = run.device;
    run.vars["deviceid"] = run.device;

    const std::string sign = ComputeSign(run, request);
    if (!sign.empty()) run.vars["sign"] = sign;

    std::string path = su::JsonString(spec, "path");
    if (path.empty()) throw std::runtime_error("端点缺少 path：" + std::string(endpoint));
    path = ExpandPlaceholders(std::move(path), run.vars, run.ext);

    std::map<std::string, std::string> params =
        BuildParams(spec.value("params", json::object()), run);
    if (!sign.empty() && params.find("sign") == params.end()) params["sign"] = sign;

    std::string query;
    for (const auto& kv : params) {
        if (!query.empty()) query += '&';
        query += su::UrlEncode(kv.first) + "=" + su::UrlEncode(kv.second);
    }

    std::string url = ComposeUrl(run.base, path);
    if (!query.empty()) url += (url.find('?') == std::string::npos ? "?" : "&") + query;

    SpiderHttp http(run.ctx->spider_id, run.ctx->net, run.ctx->site);
    http.SetDefaultHeaders(run.headers);
    http.SetStage(std::string(endpoint));

    const std::string method =
        ToLowerCopy(su::JsonString(request, "method", "GET"));
    SpiderHttpResponse response;
    if (method == "post") {
        response = http.Post(url, query, {},
                             "application/x-www-form-urlencoded; charset=utf-8");
    } else {
        response = http.Get(url);
    }

    json root = su::ParseJsonLoose(response.body);
    if (root.is_null()) {
        throw std::runtime_error(
            "模板 B 响应不是 JSON（status=" + std::to_string(response.status) +
            "，body=" + response.body.substr(0, 120) + "）");
    }
    if (root.is_object() && root.contains("code") && root.at("code").is_number()) {
        const int code = root.at("code").get<int>();
        if (code != 1 && code != 200 && !root.contains("data")) {
            const std::string msg = su::JsonString(root, "msg");
            throw std::runtime_error("接口返回 code=" + std::to_string(code) +
                                     (msg.empty() ? "" : " msg=" + msg));
        }
    }
    return root;
}

VideoListing AppEngineBase::ParseListing(AppRun& run, const json& root) {
    VideoListing listing;
    listing.page = 1;
    const json extract = run.recipe->raw.value("extract", json::object());
    const json spec = extract.value("listing", json::object());
    const json fields = spec.value("fields", json::object());
    const std::string items_path = su::JsonString(spec, "items");
    if (items_path.empty()) return listing;

    for (const json* node : su::JsonFindAll(root, items_path)) {
        if (!node || !node->is_object()) continue;
        VideoItem item;
        item.id = MappedField(*node, fields, "vod_id", {"id", "vod_id"});
        item.title = MappedField(*node, fields, "vod_name",
                                 {"vod_name", "name", "title"});
        if (item.id.empty() || item.title.empty()) continue;
        item.pic = NormalizePic(
            run.base,
            MappedField(*node, fields, "vod_pic", {"vod_pic", "pic", "cover"}));
        item.remarks = MappedField(*node, fields, "vod_remarks",
                                   {"vod_remarks", "remarks", "note", "mask"});
        item.score = MappedField(*node, fields, "vod_scroe", {"vod_score", "score"});
        item.type_name = MappedField(*node, fields, "vod_class", {"vod_class", "type_name"});
        item.raw = *node;
        listing.items.push_back(std::move(item));
    }

    const std::string page_count = su::JsonString(root, "pagecount");
    if (!page_count.empty()) {
        try {
            listing.page_count = std::stoi(page_count);
        } catch (...) {
        }
    }
    return listing;
}

VideoListing AppEngineBase::ParseHome(AppRun& run, const json& root) {
    VideoListing listing;
    listing.page = 1;
    const json extract = run.recipe->raw.value("extract", json::object());
    const json spec = extract.value("listing", json::object());
    const json fields = spec.value("fields", json::object());
    const std::string items_path = su::JsonString(spec, "items");
    if (items_path.empty()) return listing;

    for (const json* node : su::JsonFindAll(root, items_path)) {
        if (!node || !node->is_object()) continue;
        const std::string type_name = su::JsonString(*node, "type_name");
        const std::string vod_name = su::JsonPick(*node, {"vod_name", "name", "title"});
        if (!type_name.empty() && vod_name.empty()) {
            VideoCategory category;
            category.id = su::JsonPick(*node, {"type_id", "id"});
            category.name = type_name;
            if (!category.id.empty() && !category.name.empty())
                listing.categories.push_back(std::move(category));
            continue;
        }
        VideoItem item;
        item.id = MappedField(*node, fields, "vod_id", {"id", "vod_id"});
        item.title = MappedField(*node, fields, "vod_name",
                                 {"vod_name", "name", "title"});
        if (item.id.empty() || item.title.empty()) continue;
        item.pic = NormalizePic(
            run.base,
            MappedField(*node, fields, "vod_pic", {"vod_pic", "pic", "cover"}));
        item.remarks = MappedField(*node, fields, "vod_remarks",
                                   {"vod_remarks", "remarks", "note", "mask"});
        item.raw = *node;
        listing.items.push_back(std::move(item));
    }
    return listing;
}

void AppEngineBase::AddEpisodeText(const std::string& text, VideoLine* line) const {
    if (!line || text.empty()) return;
    std::string name;
    std::string url = text;
    const std::size_t dollar = text.find('$');
    if (dollar != std::string::npos) {
        name = text.substr(0, dollar);
        url = text.substr(dollar + 1);
    }
    if (url.empty()) return;
    if (name.empty()) name = std::to_string(line->episodes.size() + 1);
    line->episodes.push_back(VideoEpisode{std::move(name), std::move(url)});
}

void AppEngineBase::AddEpisodes(const json& value, VideoLine* line) const {
    if (!line || value.is_null()) return;
    if (value.is_string()) {
        AddEpisodeText(value.get<std::string>(), line);
        return;
    }
    if (value.is_object()) {
        const std::string url = su::JsonPick(value, {"url", "playurl", "play_url",
                                                     "source_url"});
        if (url.empty()) return;
        VideoEpisode episode;
        episode.name = su::JsonPick(value, {"name", "title", "source_name"},
                                    std::to_string(line->episodes.size() + 1));
        episode.url = url;
        line->episodes.push_back(std::move(episode));
        return;
    }
    if (value.is_array()) {
        for (const auto& element : value) AddEpisodes(element, line);
    }
}

VideoDetail AppEngineBase::ParseDetail(AppRun& run, const json& root,
                                       const VideoItem& item) {
    VideoDetail detail;
    detail.id = item.id;
    const json extract = run.recipe->raw.value("extract", json::object());
    const json spec = extract.value("detail", json::object());
    const json fields = spec.value("fields", json::object());

    const json* node = nullptr;
    const std::string items_path = su::JsonString(spec, "items");
    if (!items_path.empty()) {
        const std::vector<const json*> found = su::JsonFindAll(root, items_path);
        if (!found.empty()) node = found.front();
    }
    if (!node) node = &root;
    const json& obj = *node;

    detail.title = MappedField(obj, fields, "vod_name",
                               {"vod_name", "name", "title"});
    if (detail.title.empty()) detail.title = item.title;
    detail.pic = MappedField(obj, fields, "vod_pic", {"vod_pic", "pic", "cover"});
    if (detail.pic.empty()) detail.pic = item.pic;
    detail.remarks = MappedField(obj, fields, "vod_remarks",
                                 {"vod_remarks", "remarks", "note"});
    detail.raw = obj;

    const std::string from_path = su::JsonString(fields, "vod_play_from");
    const std::string url_path = su::JsonString(fields, "vod_play_url");
    if (!from_path.empty() && !url_path.empty()) {
        const std::vector<const json*> froms = su::JsonFindAll(obj, from_path);
        const std::vector<const json*> urls = su::JsonFindAll(obj, url_path);
        if (froms.size() == urls.size() && !froms.empty()) {
            for (std::size_t i = 0; i < froms.size(); ++i) {
                VideoLine line;
                line.name = ScalarOf(*froms[i]);
                if (line.name.empty()) line.name = "线路" + std::to_string(i + 1);
                AddEpisodes(*urls[i], &line);
                if (!line.episodes.empty()) detail.lines.push_back(std::move(line));
            }
        }
    }
    return detail;
}

VideoListing AppEngineBase::Home(const SpiderContext& ctx) {
    AppRun run = Begin(ctx);
    return ParseHome(run, Request(run, "home"));
}

VideoListing AppEngineBase::Category(const SpiderContext& ctx,
                                     std::string_view category_id, int page) {
    AppRun run = Begin(ctx);
    run.vars["type_id"] = std::string(category_id);
    run.vars["category_id"] = std::string(category_id);
    run.vars["cateId"] = std::string(category_id);
    run.vars["page"] = std::to_string(page <= 0 ? 1 : page);
    VideoListing listing = ParseListing(run, Request(run, "category"));
    listing.page = page <= 0 ? 1 : page;
    return listing;
}

VideoListing AppEngineBase::Search(const SpiderContext& ctx,
                                   std::string_view keyword, int page) {
    AppRun run = Begin(ctx);
    run.vars["keyword"] = std::string(keyword);
    run.vars["page"] = std::to_string(page <= 0 ? 1 : page);
    VideoListing listing = ParseListing(run, Request(run, "search"));
    listing.page = page <= 0 ? 1 : page;
    return listing;
}

VideoDetail AppEngineBase::Detail(const SpiderContext& ctx, const VideoItem& item) {
    AppRun run = Begin(ctx);
    run.vars["vod_id"] = item.id;
    run.vars["id"] = item.id;
    return ParseDetail(run, Request(run, "detail"), item);
}

std::string AppEngineBase::Player(const SpiderContext& ctx,
                                  const VideoEpisode& episode) {
    (void)ctx;
    // 模板 B 的播放地址多数是 m3u8 直链；需要二次解析的（如 AppTT 的 NBY 线路）
    // 回传详情里的原始地址，由上层解析链按 notes 说明处理。
    return episode.url;
}

// ---------- AppTT：POST + md5(key+timestamp) ----------

class AppTTEngine final : public AppEngineBase {
protected:
    VideoDetail ParseDetail(AppRun& run, const json& root,
                            const VideoItem& item) override {
        VideoDetail detail = AppEngineBase::ParseDetail(run, root, item);
        // lz/ff 线路给出 m3u8 直链，nby 线路是待解析密文；把可直接播放的线路排前，
        // 默认播放时不会落到需要二次解析的地址。
        std::stable_sort(detail.lines.begin(), detail.lines.end(),
                         [](const VideoLine& a, const VideoLine& b) {
                             const bool a_direct =
                                 !a.episodes.empty() &&
                                 IsAbsoluteUrl(a.episodes.front().url);
                             const bool b_direct =
                                 !b.episodes.empty() &&
                                 IsAbsoluteUrl(b.episodes.front().url);
                             return a_direct && !b_direct;
                         });
        return detail;
    }
};

// ---------- JPJ：动态域名 + 专题首页 + source_list_source ----------

class JpjEngine final : public AppEngineBase {
protected:
    std::string ResolveBase(AppRun& run) override;
    VideoListing ParseHome(AppRun& run, const json& root) override;
    VideoDetail ParseDetail(AppRun& run, const json& root,
                            const VideoItem& item) override;
};

std::string JpjEngine::ResolveBase(AppRun& run) {
    std::string host = ExtString(run.ext, "host");
    if (!host.empty()) {
        while (!host.empty() && host.back() == '/') host.pop_back();
        return host;
    }
    static std::mutex mutex;
    static std::string cached;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!cached.empty()) return cached;
    }

    SpiderHttp http(run.ctx->spider_id, run.ctx->net, run.ctx->site);
    http.SetStage("init");
    const SpiderHttpResponse dns = http.Get(
        "https://dns.alidns.com/resolve?name=swrdsfeiujo25sw.cc&type=TXT");
    const json root = su::ParseJsonLoose(dns.body);
    std::string text;
    if (root.is_object()) {
        const std::vector<const json*> answers =
            su::JsonFindAll(root, "$.Answer[*].data");
        if (!answers.empty()) text = ScalarOf(*answers.front());
    }
    std::vector<std::string> domains = su::Split(text, ',');
    for (std::string& domain : domains) {
        domain = su::Trim(domain);
        while (!domain.empty() &&
               (domain.front() == '"' || domain.front() == '\''))
            domain.erase(domain.begin());
        while (!domain.empty() && (domain.back() == '"' || domain.back() == '\''))
            domain.pop_back();
        if (domain.empty()) continue;
        const std::string candidate = "https://fan123." + domain;
        try {
            const SpiderHttpResponse probe = http.Get(
                candidate + "/api/v2/settings/resourceDomainConfig");
            if (probe.status == 200 &&
                probe.body.find("\"code\":1") != std::string::npos) {
                std::lock_guard<std::mutex> lock(mutex);
                cached = candidate;
                return candidate;
            }
        } catch (...) {
        }
    }
    throw std::runtime_error("JPJ 动态域名解析失败（DNS TXT 无可用 fan123 主机）");
}

VideoListing JpjEngine::ParseHome(AppRun& run, const json& root) {
    VideoListing listing;
    listing.page = 1;
    for (const json* tag : su::JsonFindAll(root, "$.data[*]")) {
        if (!tag || !tag->is_object()) continue;
        VideoCategory category;
        category.id = su::JsonPick(*tag, {"id", "category_id"});
        category.name = su::JsonPick(*tag, {"name", "title"});
        if (!category.id.empty() && !category.name.empty())
            listing.categories.push_back(std::move(category));

        for (const json* node : su::JsonFindAll(*tag, "$.dataList[*]")) {
            if (!node || !node->is_object()) continue;
            VideoItem item;
            item.id = su::JsonPick(*node, {"id", "vod_id"});
            item.title = su::JsonPick(*node, {"title", "vod_name", "name"});
            if (item.id.empty() || item.title.empty()) continue;
            item.pic = NormalizePic(
                run.base,
                su::JsonPick(*node, {"path", "tvimg", "vod_pic", "thumbnail"}));
            item.remarks = su::JsonPick(*node, {"mask", "vod_remarks"});
            item.score = su::JsonPick(*node, {"score"});
            item.raw = *node;
            listing.items.push_back(std::move(item));
        }
    }
    return listing;
}

VideoDetail JpjEngine::ParseDetail(AppRun& run, const json& root,
                                   const VideoItem& item) {
    VideoDetail detail;
    detail.id = item.id;
    const json extract = run.recipe->raw.value("extract", json::object());
    const json spec = extract.value("detail", json::object());
    const json fields = spec.value("fields", json::object());

    const json* node = nullptr;
    const std::string items_path = su::JsonString(spec, "items");
    if (!items_path.empty()) {
        const std::vector<const json*> found = su::JsonFindAll(root, items_path);
        if (!found.empty()) node = found.front();
    }
    if (!node) node = &root;
    const json& obj = *node;

    detail.title = su::JsonPick(obj, {"title", "vod_name", "name"}, item.title);
    detail.pic = NormalizePic(
        run.base,
        su::JsonPick(obj, {"tvimg", "thumbnail", "path", "vod_pic"}, item.pic));
    detail.remarks = su::JsonPick(obj, {"mask", "vod_remarks", "remark"});
    detail.raw = obj;

    for (const char* key : {"source_list_source", "vip_source_list_source"}) {
        const auto it = obj.find(key);
        if (it == obj.end() || !it->is_array() || it->empty()) continue;
        for (const auto& group : *it) {
            if (!group.is_object()) continue;
            VideoLine line;
            line.name = su::JsonPick(group, {"name", "title", "flag"});
            if (line.name.empty()) line.name = "线路";
            const auto eps = group.find("source_list");
            if (eps != group.end() && eps->is_array()) {
                for (const auto& ep : *eps) {
                    if (!ep.is_object()) continue;
                    VideoEpisode episode;
                    episode.name = su::JsonPick(ep, {"source_name", "name", "title"});
                    episode.url = su::JsonPick(ep, {"url", "playurl", "play_url"});
                    if (episode.url.empty()) continue;
                    if (episode.name.empty())
                        episode.name = std::to_string(line.episodes.size() + 1);
                    line.episodes.push_back(std::move(episode));
                }
            }
            if (!line.episodes.empty()) detail.lines.push_back(std::move(line));
        }
        if (!detail.lines.empty()) break;
    }
    return detail;
}

struct AppMetaSpec {
    const char* id;
    const char* class_name;
    bool implemented;
    const char* note;
    bool jpj;
};

// RegisterAppSpiderEngines() 由 SpiderRegistry 构造函数调用；此时再调用
// SpiderRegistry::Instance() 会重入函数局部静态的初始化（MSVC 下死锁），
// 因此这里只置位，真正的注册推迟到静态对象构造完成之后执行。
std::atomic<bool> g_app_pending{false};

void RegisterAppSpiderMetas(SpiderRegistry& registry) {
    // 只有 AppTT / JPJ 在 2026-11 实测跑通；其余按未实现登记，避免站点从列表消失。
    static const AppMetaSpec specs[] = {
        {"csp_App99", "App99", false,
         "签名 SHA256(uuid:ts:nonce:appkey:version) 已确认，但响应体 AES 加密"
         "（x-data-encrypted: true）且 key 未知", false},
        {"csp_AppSx", "AppSx", false,
         "ext 已知，但 /api.php/getappapi.* 当前返回 404/超时，RSA 握手无法验证", false},
        {"csp_AppTT", "AppTT", true,
         "已跑通：POST + sign=md5(key+timestamp)；直链线路 lz/ff 为 m3u8，nby 线路需二次解析",
         false},
        {"csp_Appgz", "Appgz", false,
         "需设备注册换 token_id/token 且业务体 AES/CBC，动态 key 未静态确认", false},
        {"csp_Hmys", "Hmys", false,
         "缺 ext(appid/channel/package/version)，且播放需 ck+sign 经 :9978 代理", false},
        {"csp_Jpys", "Jpys", false,
         "接口存活但 sign 头公式（SHA-1 输入顺序）未确认，返回 122001 应用签名失败", false},
        {"csp_JPJ", "JPJ", true,
         "已跑通：DNS TXT 解析 fan123.<domain> 动态 host；搜索/详情/播放 m3u8 直链",
         true},
        {"csp_AiDj", "AiDj", false,
         "需先向 log.snssdk.com 注册设备并派生 AES key，接口路径当前 404", false},
        {"csp_YueTing", "YueTing", false,
         "分类接口返回 payload 密文（AES key/iv 在 h5api.dat 响应内），无法解密", false},
    };

    for (const AppMetaSpec& spec : specs) {
        SpiderMeta meta;
        meta.id = spec.id;
        meta.class_name = spec.class_name;
        meta.tmpl = SpiderTemplate::kApp;
        meta.implemented = spec.implemented;
        meta.note = spec.note;
        SpiderFactory factory;
        if (spec.jpj) {
            factory = [] { return std::make_unique<JpjEngine>(); };
        } else if (std::string(spec.class_name) == "AppTT") {
            factory = [] { return std::make_unique<AppTTEngine>(); };
        } else {
            factory = [] { return std::make_unique<AppEngineBase>(); };
        }
        registry.Register(std::move(meta), std::move(factory));
    }
}

struct AppEngineRegistrar {
    AppEngineRegistrar() {
        // 触发（或等待）注册表单例构造完成后再注册，避免构造期间重入。
        SpiderRegistry& registry = SpiderRegistry::Instance();
        if (g_app_pending.exchange(false)) RegisterAppSpiderMetas(registry);
    }
};

AppEngineRegistrar g_app_registrar;

} // namespace

void RegisterAppSpiderEngines() {
    g_app_pending.store(true);
}

} // namespace ttplayer::video
