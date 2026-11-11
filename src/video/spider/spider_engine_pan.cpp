#include "ttplayer/video/spider/spider_builtins.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ttplayer/video/spider/spider_http.h"
#include "ttplayer/video/spider/spider_recipe.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/spider/spider_util.h"

// 模板 D（网盘 / 云盘）引擎。
//
// 覆盖 5 个类、6 个站点：
//   * S_zps  —— 网盘搜索聚合（盘搜 / 易搜）：home 无分类，搜索 api/search?kw=
//               返回四盘分享链接。home 为空是该类的已知形态，不算故障。
//   * YpanSo / BpanSo —— 影盘社（melost.cn）：POST /v1/search/disk 返回分享链接；
//               旧版 /film/ 首页已改版为前端渲染，首页不可结构化解析。
//   * MyDrive —— 我的云盘（夸父 / 优汐 / 阿狸 / 嘟嘟）：需要用户在 ext 里配置
//               各盘 token / cookie；无凭据时按原文案「请到用户配置启用该网盘」降级。
//   * Push   —— 手机推送接收器：依赖 TVBox 本机 :9978 代理通道，本程序没有该服务，
//               无法在自测环境验证，注册表标 implemented=false。
//
// 本文件内的网盘抽象负责：分享链接识别（quark / uc / aliyun / baidu / 115 /
// xunlei / magnet / cili / 媒体直链）、分享 id 与提取码解析、直链换取。
// 四盘分享链接换直链都需要「登录 → 转存 → 取直链」三步，本程序不代持账号，
// 因此 ResolvePanPlayUrl 在缺少登录态时返回可读原因，由详情 / 播放回传分享页。
//
// recipe（sources/spiders/csp_S_zps.json 等）提供 base / endpoints / extract；
// 代码只做请求组装与响应解析，不内置任何源站地址。

namespace ttplayer::video {

namespace {

namespace su = spider_util;
using nlohmann::json;

// ============================ 通用小工具 ============================

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

bool IsHttpUrl(std::string_view text) {
    const std::string lower = su::ToLower(text);
    return su::StartsWith(lower, "http://") || su::StartsWith(lower, "https://");
}

std::string NormalizeBase(std::string base) {
    base = su::Trim(base);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (su::StartsWith(base, "//")) return "https:" + base;
    if (!base.empty() && !IsHttpUrl(base)) return "https://" + base;
    return base;
}

// 拼接站点根地址与端点路径；path 已是绝对地址时原样返回。
std::string JoinSite(std::string base, std::string path) {
    base = su::Trim(base);
    path = su::Trim(path);
    if (path.empty()) return base;
    if (IsHttpUrl(path)) return path;
    if (su::StartsWith(path, "//")) return "https:" + path;
    if (base.empty()) return path;
    if (base.back() != '/' && path.front() != '/') base += '/';
    if (base.back() == '/' && path.front() == '/') path.erase(0, 1);
    return base + path;
}

std::string ExpandTemplate(std::string text,
                           const std::map<std::string, std::string>& vars) {
    for (const auto& kv : vars) {
        if (kv.first.empty()) continue;
        text = su::ReplaceAll(std::move(text), "{" + kv.first + "}", kv.second);
    }
    return text;
}

// 参数值可以是变量名（"keyword"/"page"），也可以是带 {占位符} 的模板。
std::string ResolveTemplate(const std::string& raw,
                            const std::map<std::string, std::string>& vars) {
    const auto found = vars.find(raw);
    if (found != vars.end()) return found->second;
    return ExpandTemplate(raw, vars);
}

void MergeHeaders(const json& source, std::map<std::string, std::string>* headers) {
    if (!headers || !source.is_object()) return;
    const auto it = source.find("headers");
    if (it == source.end() || !it->is_object()) return;
    for (auto header = it->begin(); header != it->end(); ++header) {
        if (header.value().is_string())
            (*headers)[header.key()] = header.value().get<std::string>();
    }
}

json ParseExtObject(std::string_view text) {
    const std::string trimmed = su::Trim(text);
    if (trimmed.empty()) return json::object();
    const json parsed = su::ParseJsonLoose(trimmed);
    if (parsed.is_object()) return parsed;
    if (parsed.is_string()) {
        const std::string inner = parsed.get<std::string>();
        if (!inner.empty()) return json{{"siteUrl", inner}};
        return json::object();
    }
    if (IsHttpUrl(trimmed)) return json{{"siteUrl", trimmed}};
    return json::object();
}

json ExtractSpec(const json& recipe, const char* section) {
    if (!recipe.is_object()) return json::object();
    const auto extract = recipe.find("extract");
    if (extract == recipe.end() || !extract->is_object()) return json::object();
    const auto spec = extract->find(section);
    if (spec == extract->end() || !spec->is_object()) return json::object();
    return *spec;
}

json ExtractFields(const json& recipe, const char* section) {
    const json spec = ExtractSpec(recipe, section);
    if (!spec.is_object()) return json::object();
    const auto fields = spec.find("fields");
    return (fields != spec.end() && fields->is_object()) ? *fields : json::object();
}

json IconMap(const json& recipe) {
    if (!recipe.is_object()) return json::object();
    const auto it = recipe.find("icons");
    return (it != recipe.end() && it->is_object()) ? *it : json::object();
}

std::string MappedField(const json& node, const json& fields, std::string_view key,
                        std::initializer_list<std::string_view> fallbacks) {
    if (fields.is_object()) {
        const std::string path = su::JsonString(fields, key);
        if (!path.empty()) {
            const json* found = su::JsonFind(node, path);
            if (found) {
                const std::string value = ScalarOf(*found);
                if (!value.empty()) return value;
            }
        }
    }
    return su::JsonPick(node, fallbacks);
}

// ============================ 网盘链接识别 ============================

enum class PanKind {
    kQuark,
    kUc,
    kAliyun,
    kBaidu,
    k115,
    kXunlei,
    kMagnet,
    kCili,
    kMedia,
    kUnknown,
};

const char* PanKindName(PanKind kind) {
    switch (kind) {
        case PanKind::kQuark: return "quark";
        case PanKind::kUc: return "uc";
        case PanKind::kAliyun: return "aliyun";
        case PanKind::kBaidu: return "baidu";
        case PanKind::k115: return "115";
        case PanKind::kXunlei: return "xunlei";
        case PanKind::kMagnet: return "magnet";
        case PanKind::kCili: return "cili";
        case PanKind::kMedia: return "media";
        case PanKind::kUnknown: break;
    }
    return "unknown";
}

// TVBox S_zps$1 的网盘映射：quark/夸父、baidu/嘟嘟、uc/优汐、aliyun/阿狸。
const char* PanKindLabel(PanKind kind) {
    switch (kind) {
        case PanKind::kQuark: return "夸父";
        case PanKind::kUc: return "优汐";
        case PanKind::kAliyun: return "阿狸";
        case PanKind::kBaidu: return "嘟嘟";
        case PanKind::k115: return "115";
        case PanKind::kXunlei: return "迅雷";
        case PanKind::kMagnet: return "磁力";
        case PanKind::kCili: return "磁链";
        case PanKind::kMedia: return "直链";
        case PanKind::kUnknown: break;
    }
    return "网盘";
}

PanKind PanKindFromKey(std::string_view key) {
    const std::string lower = su::ToLower(su::Trim(key));
    if (lower.empty()) return PanKind::kUnknown;
    if (lower == "quark" || lower == "夸父") return PanKind::kQuark;
    if (lower == "uc" || lower == "优汐") return PanKind::kUc;
    if (lower == "aliyun" || lower == "ali" || lower == "alipan" ||
        lower == "阿狸")
        return PanKind::kAliyun;
    if (lower == "baidu" || lower == "嘟嘟") return PanKind::kBaidu;
    if (lower == "115" || lower == "115pan") return PanKind::k115;
    if (lower == "xunlei" || lower == "迅雷") return PanKind::kXunlei;
    if (lower == "magnet" || lower == "磁力") return PanKind::kMagnet;
    if (lower == "cili" || lower == "磁链") return PanKind::kCili;
    if (lower == "media" || lower == "直链") return PanKind::kMedia;
    return PanKind::kUnknown;
}

PanKind ClassifyPanUrl(std::string_view url) {
    const std::string lower = su::ToLower(url);
    if (su::StartsWith(lower, "magnet:")) return PanKind::kMagnet;
    if (!IsHttpUrl(lower) && !su::StartsWith(lower, "//")) return PanKind::kUnknown;

    auto has = [&lower](const char* needle) {
        return lower.find(needle) != std::string::npos;
    };
    if (has("pan.quark.cn") || has("drive-pc.quark.cn") ||
        has("quark.cn/s/"))
        return PanKind::kQuark;
    if (has("drive.uc.cn") || has("pc-api.uc.cn") || has("fast.uc.cn"))
        return PanKind::kUc;
    if (has("aliyundrive.com") || has("alipan.com")) return PanKind::kAliyun;
    if (has("pan.baidu.com")) return PanKind::kBaidu;
    if (has("115.com") || has("115cdn.com")) return PanKind::k115;
    if (has("pan.xunlei.com")) return PanKind::kXunlei;
    if (has("cili")) return PanKind::kCili;
    static const char* kMediaExt[] = {".m3u8", ".mp4", ".mkv", ".flv",
                                      ".ts",   ".mp3", ".m4a", ".avi",
                                      ".mov",  ".webm"};
    for (const char* ext : kMediaExt) {
        if (has(ext)) return PanKind::kMedia;
    }
    return PanKind::kUnknown;
}

// 分享 id：/s/<id>；磁力取 btih；百度兼容 surl 形式。
std::string ShareIdFromUrl(PanKind kind, std::string_view url) {
    if (kind == PanKind::kMagnet) {
        const std::size_t btih = url.find("btih:");
        if (btih == std::string_view::npos) return {};
        const std::size_t start = btih + 5;
        const std::size_t end = url.find_first_of("&#", start);
        return std::string(url.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start));
    }
    const std::size_t slash = url.find("/s/");
    if (slash != std::string_view::npos) {
        const std::size_t start = slash + 3;
        const std::size_t end = url.find_first_of("?#/", start);
        const std::string id = std::string(url.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!id.empty()) return id;
    }
    if (kind == PanKind::kBaidu) {
        const std::string surl = su::QueryParam(url, "surl");
        if (!surl.empty()) return "1" + surl;
    }
    return {};
}

std::string PasswordFromUrl(std::string_view url) {
    for (const char* key : {"pwd", "password", "passcode", "code"}) {
        const std::string value = su::QueryParam(url, key);
        if (!value.empty()) return value;
    }
    return {};
}

// 从一段文本里抓出所有可识别的网盘 / 磁力 / 媒体链接（推送文本、分享文案）。
std::vector<std::string> ExtractPanUrls(std::string_view text) {
    static const std::string_view kStops = " \t\r\n\"'<>，。；;、）)】]}|";
    std::vector<std::string> out;
    const std::string lower = su::ToLower(text);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t start = std::string::npos;
        for (const char* scheme : {"http://", "https://", "magnet:"}) {
            const std::size_t found = lower.find(scheme, pos);
            if (found != std::string::npos && (start == std::string::npos || found < start))
                start = found;
        }
        if (start == std::string::npos) break;
        const std::size_t end = text.find_first_of(kStops, start);
        std::string url = std::string(text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start));
        while (!url.empty() &&
               (url.back() == '.' || url.back() == ',' || url.back() == ';'))
            url.pop_back();
        if (ClassifyPanUrl(url) != PanKind::kUnknown &&
            std::find(out.begin(), out.end(), url) == out.end())
            out.push_back(std::move(url));
        pos = end == std::string_view::npos ? text.size() : end + 1;
    }
    return out;
}

struct PanLink {
    PanKind kind = PanKind::kUnknown;
    std::string url;
    std::string share_id;
    std::string password;
    bool direct = false;
};

PanLink DetectPanLink(std::string_view text) {
    PanLink link;
    std::string url = su::Trim(text);
    if (url.empty()) return link;
    const bool is_magnet = su::StartsWith(su::ToLower(url), "magnet:");
    if (!IsHttpUrl(url) && !is_magnet) {
        const std::vector<std::string> found = ExtractPanUrls(url);
        if (!found.empty()) url = found.front();
    }
    link.url = url;
    link.kind = ClassifyPanUrl(url);
    if (link.kind == PanKind::kUnknown) return link;
    link.share_id = ShareIdFromUrl(link.kind, url);
    link.password = PasswordFromUrl(url);
    link.direct = link.kind == PanKind::kMedia;
    return link;
}

// ext 里的网盘凭据：支持 {"quark":"cookie"}、{"quark":{"token":"..."}} 与
// {"quark_token":"..."} 三种写法（键名也可用中文盘名）。
std::string ExtCredential(const json& ext, const std::string& key,
                          const std::string& label) {
    if (!ext.is_object()) return {};
    const std::string names[] = {key, label, key + "_token", key + "_cookie",
                                 key + "_authorization"};
    for (const std::string& name : names) {
        const auto it = ext.find(name);
        if (it == ext.end()) continue;
        if (it->is_string()) {
            const std::string value = su::Trim(it->get<std::string>());
            if (!value.empty()) return value;
        } else if (it->is_object()) {
            for (const char* field :
                 {"token", "authorization", "cookie", "access_token", "refresh_token"}) {
                const std::string value = su::JsonString(*it, field);
                if (!value.empty()) return value;
            }
        }
    }
    const auto drives = ext.find("drives");
    if (drives != ext.end() && drives->is_object())
        return ExtCredential(*drives, key, label);
    return {};
}

struct PanResolveResult {
    bool ok = false;
    std::string url;
    std::string reason;
};

// 直链换取接口封装。四盘分享链接都要经历「登录 → 转存 → 取直链」，
// 缺少登录态时返回可读原因（不抛异常），调用方回传分享页地址。
PanResolveResult ResolvePanPlayUrl(std::string_view raw_url, const json& credentials) {
    PanResolveResult result;
    const PanLink link = DetectPanLink(raw_url);
    result.url = link.url;
    const std::string label = PanKindLabel(link.kind);

    if (link.kind == PanKind::kMedia) {
        result.ok = true;
        result.reason = "已是媒体直链";
        return result;
    }
    if (link.kind == PanKind::kMagnet) {
        result.reason = "磁力链接需要本地 BT 客户端，播放器不能直接播放";
        return result;
    }
    if (link.kind == PanKind::kCili) {
        result.reason = "磁链页面需要本地下载工具，播放器不能直接播放";
        return result;
    }
    if (link.kind == PanKind::kUnknown) {
        result.reason = "未识别的链接类型，回传原地址";
        return result;
    }
    if (link.share_id.empty()) {
        result.reason = label + "链接缺少分享 id，回传原地址";
        return result;
    }

    const std::string token =
        ExtCredential(credentials, PanKindName(link.kind), label);
    if (token.empty()) {
        result.reason = label +
                        "分享链接需要登录态（token/cookie）才能换取直链，当前未配置；"
                        "已回传分享页";
    } else {
        result.reason = label +
                        "分享链接需要「转存 + 取直链」两步签名，本实现不做转存；"
                        "已回传分享页";
    }
    return result;
}

// ============================ 请求上下文 ============================

struct PanRun {
    const SpiderContext* ctx = nullptr;
    json recipe = json::object();
    json ext = json::object();
    std::string base;
    std::map<std::string, std::string> headers;
};

PanRun BeginPan(const SpiderContext& ctx) {
    PanRun run;
    run.ctx = &ctx;
    const SpiderRecipeLoad load =
        LoadSpiderRecipe(ctx.spider_id.empty() ? ctx.site.api : ctx.spider_id);
    if (load.ok) {
        run.recipe = load.recipe.raw;
        run.base = su::Trim(load.recipe.base);
    }
    run.ext = ParseExtObject(ctx.ext.empty() ? ctx.site.ext : ctx.ext);
    if (run.ext.is_object()) {
        for (const char* key : {"siteUrl", "site_url", "host", "url", "api"}) {
            const std::string value = su::JsonString(run.ext, key);
            if (!value.empty() && IsHttpUrl(value)) {
                run.base = value;
                break;
            }
        }
    }
    run.base = NormalizeBase(std::move(run.base));
    MergeHeaders(run.recipe, &run.headers);
    MergeHeaders(run.ext, &run.headers);
    return run;
}

const json& PanEndpoint(const PanRun& run, std::string_view name) {
    static const json kEmpty = json::object();
    if (!run.recipe.is_object()) return kEmpty;
    const auto endpoints = run.recipe.find("endpoints");
    if (endpoints == run.recipe.end() || !endpoints->is_object()) return kEmpty;
    const auto endpoint = endpoints->find(std::string(name));
    if (endpoint == endpoints->end() || !endpoint->is_object()) return kEmpty;
    return *endpoint;
}

// 请求体占位符展开：纯变量（"page" 或 "{page}"）且值为纯数字时按数字下发，
// 影盘社对 "page":"1" 返回「参数错误」；数组 / 对象里的占位符递归展开。
json ExpandBodyValue(const json& value,
                     const std::map<std::string, std::string>& vars) {
    if (value.is_string()) {
        const std::string raw = value.get<std::string>();
        std::string resolved;
        bool is_variable = false;
        const auto exact = vars.find(raw);
        if (exact != vars.end()) {
            resolved = exact->second;
            is_variable = true;
        } else if (raw.size() > 2 && raw.front() == '{' && raw.back() == '}') {
            const auto inner = vars.find(raw.substr(1, raw.size() - 2));
            if (inner != vars.end()) {
                resolved = inner->second;
                is_variable = true;
            }
        }
        if (!is_variable) return json(ExpandTemplate(raw, vars));
        const bool numeric =
            !resolved.empty() &&
            std::all_of(resolved.begin(), resolved.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            });
        return numeric ? json(std::stoi(resolved)) : json(resolved);
    }
    if (value.is_array()) {
        json out = json::array();
        for (const auto& element : value) out.push_back(ExpandBodyValue(element, vars));
        return out;
    }
    if (value.is_object()) {
        json out = json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            out[it.key()] = ExpandBodyValue(it.value(), vars);
        return out;
    }
    return value;
}

// 按 recipe 端点发请求：path 支持 {占位符} 与 S_zps 原串里的 '@'（关键词位置）；
// params 逐项拼 query；body 为对象时以 JSON POST 发出（影盘社 / 阿里云盘）。
SpiderHttpResponse PanCall(
    const PanRun& run, const json& endpoint,
    const std::map<std::string, std::string>& vars, std::string_view stage,
    const std::map<std::string, std::string>& extra_headers = {}) {
    if (!endpoint.is_object())
        throw std::runtime_error("recipe 缺少端点：" + std::string(stage));
    std::string path = su::JsonString(endpoint, "path");
    if (path.empty())
        throw std::runtime_error("recipe 端点缺少 path：" + std::string(stage));
    path = ExpandTemplate(std::move(path), vars);

    const auto keyword = vars.find("keyword");
    if (keyword != vars.end() && path.find('@') != std::string::npos)
        path = su::ReplaceAll(std::move(path), "@", su::UrlEncode(keyword->second));

    std::string url = JoinSite(run.base, std::move(path));
    if (endpoint.contains("params") && endpoint["params"].is_object()) {
        for (auto it = endpoint["params"].begin(); it != endpoint["params"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string value =
                ResolveTemplate(it.value().get<std::string>(), vars);
            if (value.empty()) continue;
            url += (url.find('?') == std::string::npos ? "?" : "&") +
                   su::UrlEncode(it.key()) + "=" + su::UrlEncode(value);
        }
    }

    bool has_body = false;
    std::string body_text;
    if (endpoint.contains("body") && endpoint["body"].is_object()) {
        json body = json::object();
        for (auto it = endpoint["body"].begin(); it != endpoint["body"].end(); ++it)
            body[it.key()] = ExpandBodyValue(it.value(), vars);
        body_text = body.dump();
        has_body = true;
    }

    const std::string method = su::ToLower(
        su::JsonString(endpoint, "method", has_body ? "post" : "get"));

    SpiderHttp http(run.ctx->spider_id, run.ctx->net, run.ctx->site);
    http.SetStage(std::string(stage));
    http.SetDefaultHeaders(run.headers);
    if (has_body || method == "post") {
        return http.Post(url, body_text, extra_headers,
                         "application/json; charset=utf-8");
    }
    return http.Get(url, extra_headers);
}

// 部分网盘站对 WinHTTP 时好时坏（偶发超时，或被中间层当成明文 HTTP 返回 400）。
// 搜索是网盘源的主路径，这里做有限重试；404/417 属正常业务码，不重试。
SpiderHttpResponse PanCallRetry(
    const PanRun& run, const json& endpoint,
    const std::map<std::string, std::string>& vars, std::string_view stage,
    const std::map<std::string, std::string>& extra_headers = {},
    int attempts = 3) {
    std::string last_error;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        try {
            const SpiderHttpResponse response =
                PanCall(run, endpoint, vars, stage, extra_headers);
            if (response.status == 0) {
                last_error = "网络错误（无响应）";
            } else if (response.status == 400 &&
                       response.body.find("plain HTTP request") != std::string::npos) {
                last_error = "中间层把 HTTPS 请求当成了明文 HTTP";
            } else {
                return response;
            }
        } catch (const std::exception& e) {
            last_error = e.what();
        }
        if (attempt + 1 < attempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    throw std::runtime_error(std::string(stage) + " 请求失败（已重试 " +
                             std::to_string(attempts) + " 次）：" + last_error);
}

int JsonCode(const json& root, const char* key, bool* present) {
    *present = false;
    if (!root.is_object()) return 0;
    const auto it = root.find(key);
    if (it == root.end()) return 0;
    if (it->is_number_integer()) {
        *present = true;
        return it->get<int>();
    }
    if (it->is_string()) {
        try {
            const int value = std::stoi(it->get<std::string>());
            *present = true;
            return value;
        } catch (...) {
        }
    }
    return 0;
}

std::string JsonMessage(const json& root) {
    std::string message = su::JsonPick(root, {"message", "msg", "errmsg"});
    if (message.empty()) {
        const json* data = su::JsonFind(root, "$.data");
        if (data && data->is_object())
            message = su::JsonPick(*data, {"message", "msg"});
    }
    return message;
}

// ============================ 列表解析 ============================

void AddPanItem(const json& node, const json& fields, const json& recipe,
                const std::string& type_key, PanKind kind,
                const std::string& label, VideoListing* listing) {
    if (!listing || !node.is_object()) return;
    std::string url = MappedField(node, fields, "vod_id", {"url", "link"});
    if (url.empty()) url = su::JsonPick(node, {"url", "link"});
    if (url.empty()) return;

    std::string title = MappedField(node, fields, "vod_name",
                                    {"note", "disk_name", "name"});
    if (title.empty()) title = url;

    VideoItem item;
    item.id = url;
    item.title = su::StripTags(title);
    std::string remarks = MappedField(node, fields, "vod_remarks",
                                      {"datetime", "shared_time", "disk_type"});
    item.remarks = remarks.empty() ? label : label + " · " + remarks;
    item.pic = MappedField(node, fields, "vod_pic", {"thumbnail", "poster"});
    if (item.pic.empty()) item.pic = su::JsonString(IconMap(recipe), type_key);
    item.type_name = label;
    item.raw = node;
    item.raw["_pan"] = json{{"kind", PanKindName(kind)},
                            {"label", label},
                            {"url", url},
                            {"password", su::JsonPick(node, {"password", "disk_pass", "pwd"})}};
    listing->items.push_back(std::move(item));
}

// S_zps：data.merged_by_type 是「盘类型 → 条目数组」的对象，逐类型展开。
VideoListing ParseSzpsListing(const json& root, const json& recipe) {
    VideoListing listing;
    listing.page = 1;
    const json fields = ExtractFields(recipe, "listing");
    const json* merged = su::JsonFind(root, "$.data.merged_by_type");
    if (!merged || !merged->is_object()) return listing;

    for (auto it = merged->begin(); it != merged->end(); ++it) {
        const PanKind kind = PanKindFromKey(it.key());
        const std::string label =
            kind == PanKind::kUnknown ? it.key() : PanKindLabel(kind);
        if (it.value().is_array()) {
            for (const auto& node : it.value())
                AddPanItem(node, fields, recipe, su::ToLower(it.key()), kind, label,
                           &listing);
        } else if (it.value().is_object()) {
            AddPanItem(it.value(), fields, recipe, su::ToLower(it.key()), kind, label,
                       &listing);
        }
    }
    return listing;
}

// 影盘社 / 云盘：扁平 data.list[]（disk_type 区分盘）。
VideoListing ParsePanList(const json& root, const json& recipe,
                          std::string_view default_items_path) {
    VideoListing listing;
    listing.page = 1;
    const json spec = ExtractSpec(recipe, "listing");
    const json fields = spec.is_object() ? spec.value("fields", json::object())
                                         : json::object();
    std::string path = su::JsonString(spec, "items");
    if (path.empty()) path = std::string(default_items_path);

    for (const json* node : su::JsonFindAll(root, path)) {
        if (!node || !node->is_object()) continue;
        const std::string disk_type =
            su::JsonPick(*node, {"disk_type", "pan", "type"});
        const PanKind kind = PanKindFromKey(disk_type);
        const std::string label =
            kind == PanKind::kUnknown
                ? (disk_type.empty() ? std::string("网盘") : disk_type)
                : PanKindLabel(kind);
        AddPanItem(*node, fields, recipe, su::ToLower(disk_type), kind, label,
                   &listing);
    }

    bool has_total = false;
    bool has_per_size = false;
    int total = 0;
    int per_size = 0;
    if (const json* node = su::JsonFind(root, "$.data.total")) {
        if (node->is_number_integer()) {
            total = node->get<int>();
            has_total = true;
        }
    }
    if (const json* node = su::JsonFind(root, "$.data.per_size")) {
        if (node->is_number_integer()) {
            per_size = node->get<int>();
            has_per_size = true;
        }
    }
    if (has_total && has_per_size && per_size > 0)
        listing.page_count = (total + per_size - 1) / per_size;
    listing.is_end = listing.items.empty() ||
                     (has_per_size && per_size > 0 &&
                      static_cast<int>(listing.items.size()) < per_size);
    return listing;
}

VideoDetail BuildPanDetail(const SpiderContext& ctx, const VideoItem& item) {
    VideoDetail detail;
    detail.id = item.id;
    detail.title = item.title;
    detail.pic = item.pic;

    std::string url;
    std::string label;
    std::string password;
    if (item.raw.is_object() && item.raw.contains("_pan") &&
        item.raw["_pan"].is_object()) {
        const json& pan = item.raw["_pan"];
        url = su::JsonString(pan, "url");
        label = su::JsonString(pan, "label");
        password = su::JsonString(pan, "password");
    }
    if (url.empty()) url = item.id;

    const PanLink link = DetectPanLink(url);
    if (label.empty() && link.kind != PanKind::kUnknown)
        label = PanKindLabel(link.kind);
    if (label.empty()) label = "网盘";
    if (password.empty()) password = link.password;

    std::string play_url = link.url.empty() ? url : link.url;
    // 提取码：115 用 password，百度/迅雷用 pwd；缺省不覆盖链接里已有的参数。
    if (!password.empty() &&
        (link.kind == PanKind::kBaidu || link.kind == PanKind::k115 ||
         link.kind == PanKind::kXunlei)) {
        const bool has_password =
            !su::QueryParam(play_url, "pwd").empty() ||
            !su::QueryParam(play_url, "password").empty();
        if (!has_password) {
            const char* key = link.kind == PanKind::k115 ? "password" : "pwd";
            play_url += (play_url.find('?') == std::string::npos ? "?" : "&") +
                        std::string(key) + "=" + su::UrlEncode(password);
        }
    }

    VideoLine line;
    line.name = label;
    VideoEpisode episode;
    episode.name = "打开分享页";
    episode.url = play_url;
    line.episodes.push_back(std::move(episode));
    detail.lines.push_back(std::move(line));

    const PanResolveResult resolved =
        ResolvePanPlayUrl(play_url, ParseExtObject(ctx.ext));
    detail.remarks = resolved.reason;
    return detail;
}

std::string ResolveEpisode(const SpiderContext& ctx,
                           const VideoEpisode& episode) {
    const PanResolveResult resolved =
        ResolvePanPlayUrl(episode.url, ParseExtObject(ctx.ext));
    return resolved.url.empty() ? episode.url : resolved.url;
}

// ============================ S_zps（盘搜 / 易搜） ============================

// 登录：ext 可选 {"username","password"}，POST api/auth/login 换 token，
// 之后搜索带 Authorization: Bearer。匿名搜索本身可用，登录失败不阻断。
std::string SzpsLoginToken(const PanRun& run) {
    std::string token = su::JsonPick(run.ext, {"token", "authorization", "auth"});
    if (!token.empty()) return token;

    const std::string username = su::JsonPick(run.ext, {"username", "user", "account"});
    const std::string password = su::JsonPick(run.ext, {"password", "pass", "pwd"});
    if (username.empty() || password.empty()) return {};

    static std::mutex mutex;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find(run.base);
        if (found != cache.end()) return found->second;
    }

    const json& login = PanEndpoint(run, "login");
    if (login.empty()) return {};
    std::map<std::string, std::string> vars;
    vars["username"] = username;
    vars["password"] = password;
    try {
        const SpiderHttpResponse response = PanCall(run, login, vars, "login");
        const json root = su::ParseJsonLoose(response.body);
        std::string value = su::JsonString(root, "token");
        if (value.empty()) {
            const json* data = su::JsonFind(root, "$.data");
            if (data && data->is_object()) value = su::JsonString(*data, "token");
        }
        if (value.empty()) return {};
        std::lock_guard<std::mutex> lock(mutex);
        cache[run.base] = value;
        return value;
    } catch (const std::exception&) {
        return {};
    }
}

VideoListing SzpsSearch(const SpiderContext& ctx, std::string_view keyword,
                        int page) {
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    const std::string text = su::Trim(keyword);
    if (text.empty()) return listing;

    PanRun run = BeginPan(ctx);
    std::map<std::string, std::string> vars;
    vars["keyword"] = text;
    vars["page"] = std::to_string(listing.page);

    std::map<std::string, std::string> headers;
    const std::string token = SzpsLoginToken(run);
    if (!token.empty()) {
        headers["authorization"] = su::StartsWith(su::ToLower(token), "bearer ")
                                       ? token
                                       : "Bearer " + token;
    }

    const SpiderHttpResponse response =
        PanCallRetry(run, PanEndpoint(run, "search"), vars, "search", headers);
    const json root = su::ParseJsonLoose(response.body);
    if (!root.is_object()) {
        throw std::runtime_error("S_zps 搜索响应不是 JSON（status=" +
                                 std::to_string(response.status) + "）");
    }
    bool has_code = false;
    const int code = JsonCode(root, "code", &has_code);
    if (has_code && code != 0 && code != 200) {
        const std::string message = JsonMessage(root);
        throw std::runtime_error("S_zps 搜索失败 code=" + std::to_string(code) +
                                 (message.empty() ? "" : " msg=" + message));
    }
    listing = ParseSzpsListing(root, run.recipe);
    listing.page = page <= 0 ? 1 : page;
    return listing;
}

class SzpsEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        (void)ctx;
        VideoListing listing;
        listing.page = 1;
        // 原类 homeContent 只返回「本源是网盘搜索源，仅支持搜索」提示，无分类。
        listing.error = "本源是网盘搜索源，仅支持搜索（home 无分类）";
        return listing;
    }

    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override {
        if (su::Trim(category_id).empty()) {
            VideoListing listing;
            listing.page = page <= 0 ? 1 : page;
            return listing;
        }
        return SzpsSearch(ctx, category_id, page);
    }

    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override {
        return SzpsSearch(ctx, keyword, page);
    }

    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        return BuildPanDetail(ctx, item);
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        return ResolveEpisode(ctx, episode);
    }
};

// ============================ 影盘社（YpanSo / BpanSo） ============================

class MelostPanEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        PanRun run = BeginPan(ctx);
        VideoListing listing;
        listing.page = 1;
        const json& endpoint = PanEndpoint(run, "home");
        if (endpoint.empty()) {
            listing.error = "recipe 缺少 home 端点";
            return listing;
        }
        try {
            const SpiderHttpResponse response = PanCall(run, endpoint, {}, "home");
            if (response.status < 200 || response.status >= 300) {
                listing.error = "首页不可用（status=" +
                                std::to_string(response.status) +
                                "）：站点已改版为前端渲染，请直接使用搜索";
                return listing;
            }
            listing.error = "首页为前端渲染页面，无可结构化条目，请直接使用搜索";
        } catch (const std::exception& e) {
            listing.error = std::string("首页不可用：") + e.what();
        }
        return listing;
    }

    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override {
        if (su::Trim(category_id).empty()) {
            VideoListing listing;
            listing.page = page <= 0 ? 1 : page;
            return listing;
        }
        return Search(ctx, category_id, page);
    }

    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override {
        VideoListing listing;
        listing.page = page <= 0 ? 1 : page;
        const std::string text = su::Trim(keyword);
        if (text.empty()) return listing;

        PanRun run = BeginPan(ctx);
        std::map<std::string, std::string> vars;
        vars["keyword"] = text;
        vars["page"] = std::to_string(listing.page);

        const SpiderHttpResponse response =
            PanCallRetry(run, PanEndpoint(run, "search"), vars, "search");
        const json root = su::ParseJsonLoose(response.body);
        if (!root.is_object()) {
            throw std::runtime_error("影盘社搜索响应不是 JSON（status=" +
                                     std::to_string(response.status) + "）");
        }
        bool has_code = false;
        const int code = JsonCode(root, "code", &has_code);
        if (has_code && code != 0 && code != 200) {
            // 417 = 没有搜索结果，按空列表返回而不是抛错。
            if (code == 417 || code == 404) return listing;
            const std::string message = JsonMessage(root);
            throw std::runtime_error("影盘社搜索失败 code=" + std::to_string(code) +
                                     (message.empty() ? "" : " msg=" + message));
        }
        listing = ParsePanList(root, run.recipe, "$.data.list[*]");
        listing.page = page <= 0 ? 1 : page;
        return listing;
    }

    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        return BuildPanDetail(ctx, item);
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        return ResolveEpisode(ctx, episode);
    }
};

// ============================ MyDrive（四盘） ============================

struct DriveCfg {
    std::string key;
    std::string label;
    std::string token;
    json spec;
};

std::vector<DriveCfg> LoadDriveConfigs(const PanRun& run) {
    std::vector<DriveCfg> drives;
    if (!run.recipe.is_object()) return drives;
    const auto found = run.recipe.find("drives");
    if (found == run.recipe.end() || !found->is_object()) return drives;

    for (auto it = found->begin(); it != found->end(); ++it) {
        if (!it.value().is_object()) continue;
        DriveCfg drive;
        drive.key = it.key();
        drive.label = su::JsonString(it.value(), "label");
        if (drive.label.empty()) drive.label = PanKindLabel(PanKindFromKey(drive.key));
        drive.spec = it.value();
        drive.token = ExtCredential(run.ext, drive.key, drive.label);
        drives.push_back(std::move(drive));
    }
    return drives;
}

const DriveCfg* FindDrive(const std::vector<DriveCfg>& drives,
                          std::string_view key) {
    for (const auto& drive : drives) {
        if (drive.key == key) return &drive;
    }
    return nullptr;
}

VideoListing FetchDriveListing(const PanRun& run, const DriveCfg& drive,
                               bool search, std::string_view keyword, int page) {
    const std::string path_field = search ? "search" : "list";
    const std::string body_field = search ? "search_body" : "list_body";
    const std::string raw = su::JsonString(drive.spec, path_field);
    if (raw.empty())
        throw std::runtime_error(drive.label + " 缺少 recipe 端点：" + path_field);
    if (drive.token.empty())
        throw std::runtime_error(drive.label +
                                 " 未配置 token/cookie：请到用户配置启用该网盘-并重载或刷新");

    std::map<std::string, std::string> vars;
    vars["keyword"] = std::string(keyword);
    vars["page"] = std::to_string(page <= 0 ? 1 : page);
    vars["fid"] = su::JsonString(drive.spec, "root_fid", "0");
    vars["file_id"] = vars["fid"];
    vars["dir"] = su::JsonString(drive.spec, "root_dir", "/");
    std::string drive_id = su::JsonString(run.ext, drive.key + "_drive_id");
    if (drive_id.empty()) drive_id = su::JsonString(run.ext, "drive_id");
    vars["drive_id"] = drive_id;

    json endpoint = json::object();
    endpoint["path"] = raw;
    if (drive.spec.contains(body_field) && drive.spec[body_field].is_object()) {
        endpoint["method"] = "post";
        endpoint["body"] = drive.spec[body_field];
    }

    std::map<std::string, std::string> headers;
    const std::string auth = su::JsonString(drive.spec, "auth", "cookie");
    headers[auth] = drive.token;

    const SpiderHttpResponse response = PanCall(
        run, endpoint, vars, search ? "search" : "category", headers);
    const json root = su::ParseJsonLoose(response.body);
    if (!root.is_object()) {
        throw std::runtime_error(drive.label + " 接口响应不是 JSON（status=" +
                                 std::to_string(response.status) + "）");
    }
    bool has_code = false;
    const int code = JsonCode(root, "code", &has_code);
    if (has_code && code != 0 && code != 200) {
        const std::string message = JsonMessage(root);
        throw std::runtime_error(drive.label + " 接口失败 code=" +
                                 std::to_string(code) +
                                 (message.empty() ? "" : " msg=" + message));
    }
    bool has_errno = false;
    const int errno_value = JsonCode(root, "errno", &has_errno);
    if (has_errno && errno_value != 0) {
        const std::string message = JsonMessage(root);
        throw std::runtime_error(drive.label + " 接口失败 errno=" +
                                 std::to_string(errno_value) +
                                 (message.empty() ? "" : " msg=" + message));
    }

    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    for (const char* path : {"$.data.list[*]", "$.items[*]", "$.list[*]",
                             "$.data[*]"}) {
        for (const json* node : su::JsonFindAll(root, path)) {
            if (!node || !node->is_object()) continue;
            VideoItem item;
            item.id = su::JsonPick(*node, {"fid", "file_id", "fs_id", "id"});
            item.title =
                su::JsonPick(*node, {"file_name", "name", "server_filename"});
            if (item.id.empty() || item.title.empty()) continue;
            item.pic = su::JsonPick(*node, {"thumbnail", "thumb", "poster"});
            item.remarks = su::JsonPick(
                *node, {"updated_at", "server_mtime", "modified_time", "size"});
            item.type_name = drive.label;
            item.raw = *node;
            item.raw["_pan"] = json{{"kind", drive.key},
                                    {"label", drive.label},
                                    {"url", ""},
                                    {"password", ""}};
            listing.items.push_back(std::move(item));
        }
        if (!listing.items.empty()) break;
    }
    return listing;
}

class MyDriveEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        PanRun run = BeginPan(ctx);
        const std::vector<DriveCfg> drives = LoadDriveConfigs(run);
        if (drives.empty()) {
            throw std::runtime_error(
                "我的云盘未配置：请到用户配置启用该网盘-并重载或刷新");
        }

        VideoListing listing;
        listing.page = 1;
        int enabled = 0;
        for (const auto& drive : drives) {
            VideoCategory category;
            category.id = drive.key;
            category.name = drive.label;
            listing.categories.push_back(std::move(category));
            if (!drive.token.empty()) ++enabled;
        }
        if (enabled == 0) {
            throw std::runtime_error(
                "请到用户配置启用该网盘-并重载或刷新（夸父/优汐/阿狸/嘟嘟 均未配置 token）");
        }

        for (const auto& drive : drives) {
            if (drive.token.empty()) continue;
            try {
                VideoListing sub = FetchDriveListing(run, drive, false, {}, 1);
                for (auto& item : sub.items) {
                    item.remarks = item.remarks.empty()
                                       ? drive.label
                                       : drive.label + " · " + item.remarks;
                    listing.items.push_back(std::move(item));
                }
                if (!listing.items.empty()) break;
            } catch (const std::exception& e) {
                if (listing.error.empty()) listing.error = e.what();
            }
        }
        return listing;
    }

    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override {
        PanRun run = BeginPan(ctx);
        const std::vector<DriveCfg> drives = LoadDriveConfigs(run);
        const DriveCfg* drive = FindDrive(drives, category_id);
        if (!drive) {
            throw std::runtime_error("未找到网盘配置：" + std::string(category_id) +
                                     "（请到用户配置启用该网盘-并重载或刷新）");
        }
        return FetchDriveListing(run, *drive, false, {}, page);
    }

    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override {
        VideoListing listing;
        listing.page = page <= 0 ? 1 : page;
        const std::string text = su::Trim(keyword);
        if (text.empty()) return listing;

        PanRun run = BeginPan(ctx);
        const std::vector<DriveCfg> drives = LoadDriveConfigs(run);
        int enabled = 0;
        for (const auto& drive : drives) {
            if (drive.token.empty()) continue;
            ++enabled;
            try {
                VideoListing sub =
                    FetchDriveListing(run, drive, true, text, listing.page);
                for (auto& item : sub.items) {
                    item.remarks = item.remarks.empty()
                                       ? drive.label
                                       : drive.label + " · " + item.remarks;
                    listing.items.push_back(std::move(item));
                }
            } catch (const std::exception& e) {
                if (listing.error.empty()) listing.error = e.what();
            }
        }
        if (enabled == 0) {
            throw std::runtime_error(
                "请到用户配置启用该网盘-并重载或刷新（未配置任何网盘 token）");
        }
        return listing;
    }

    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        PanRun run = BeginPan(ctx);
        VideoDetail detail;
        detail.id = item.id;
        detail.title = item.title;
        detail.pic = item.pic;

        std::string key;
        std::string label;
        if (item.raw.is_object() && item.raw.contains("_pan") &&
            item.raw["_pan"].is_object()) {
            key = su::JsonString(item.raw["_pan"], "kind");
            label = su::JsonString(item.raw["_pan"], "label");
        }
        const std::vector<DriveCfg> drives = LoadDriveConfigs(run);
        const DriveCfg* drive = FindDrive(drives, key);
        if (!drive) return detail;
        if (label.empty()) label = drive->label;

        std::map<std::string, std::string> vars;
        vars["fid"] = item.id;
        vars["file_id"] = item.id;
        vars["page"] = "1";
        std::string drive_id = su::JsonString(run.ext, drive->key + "_drive_id");
        if (drive_id.empty()) drive_id = su::JsonString(run.ext, "drive_id");
        vars["drive_id"] = drive_id;

        std::string url = ExpandTemplate(
            su::JsonString(drive->spec, "download"), vars);
        if (url.empty() && item.raw.is_object())
            url = su::JsonPick(item.raw, {"download_url", "url"});
        if (!url.empty()) {
            VideoLine line;
            line.name = label;
            VideoEpisode episode;
            episode.name = "下载 / 播放";
            episode.url = url;
            line.episodes.push_back(std::move(episode));
            detail.lines.push_back(std::move(line));
            detail.remarks =
                label + "：直链请求需要携带登录 Cookie/token，本程序播放时未注入";
        } else {
            detail.remarks = label + "：该文件无法直接生成直链，请在网盘客户端打开";
        }
        return detail;
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        (void)ctx;
        return episode.url;
    }
};

// ============================ Push（手机推送） ============================

// 原类不请求外部站点，而是嗅探推送文本里的夸克/阿里/UC/百度/磁力/媒体链接，
// 经 TVBox 本机 :9978 代理（/proxy?do=input&csp=Push）投递后播放。本程序没有
// 该代理通道，Home/Detail 一律给出可读原因；Player 保留链接嗅探实现。
class PushEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        (void)ctx;
        throw std::runtime_error(
            "Push 是手机推送接收器，依赖 TVBox 本机 :9978 推送通道，本程序没有该服务");
    }

    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override {
        (void)ctx;
        (void)category_id;
        (void)page;
        throw std::runtime_error(
            "Push 是手机推送接收器，依赖 TVBox 本机 :9978 推送通道，本程序没有该服务");
    }

    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override {
        (void)ctx;
        (void)keyword;
        (void)page;
        throw std::runtime_error(
            "Push 是手机推送接收器，依赖 TVBox 本机 :9978 推送通道，本程序没有该服务");
    }

    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        (void)ctx;
        (void)item;
        throw std::runtime_error(
            "Push 是手机推送接收器，依赖 TVBox 本机 :9978 推送通道，本程序没有该服务");
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        (void)ctx;
        const std::vector<std::string> urls = ExtractPanUrls(episode.url);
        for (const auto& url : urls) {
            if (ClassifyPanUrl(url) == PanKind::kMedia) return url;
        }
        return urls.empty() ? episode.url : urls.front();
    }
};

// ============================ 注册 ============================

struct PanMetaSpec {
    const char* id;
    const char* class_name;
    bool implemented;
    const char* note;
    int engine;  // 0=S_zps 1=影盘社 2=MyDrive 3=Push
};

void RegisterPanSpiderMetas(SpiderRegistry& registry) {
    // 只有 2026-11 实测跑通的才标 implemented=true；其余保留注册，站点不会消失。
    static const PanMetaSpec kSpecs[] = {
        {"csp_S_zps", "S_zps", true,
         "网盘搜索聚合（so.252035.xyz）：home 无分类是已知形态，搜索 api/search?kw= "
         "实测可用；结果为夸父/优汐/阿狸/嘟嘟分享链接，播放回传分享页", 0},
        {"csp_YpanSo", "YpanSo", true,
         "影盘社 melost.cn：POST /v1/search/disk 实测可用（data.list 含 link/"
         "disk_type）；旧首页 /film/ 已改版 404，播放回传分享页", 1},
        {"csp_BpanSo", "BpanSo", true,
         "与 YpanSo 同实现（melost.cn POST /v1/search/disk）；首页 404，"
         "播放回传分享页", 1},
        {"csp_MyDrive", "MyDrive", false,
         "四盘（夸父/优汐/阿狸/嘟嘟）文件浏览/搜索：需在 ext 配置各盘 token 或 "
         "cookie；本机无凭据，接口未实测，标未实现", 2},
        {"csp_Push", "Push", false,
         "手机推送接收器：依赖 TVBox 本机 :9978 代理通道（/proxy?do=input&csp="
         "Push），本程序无该服务，无法在自测环境验证", 3},
    };

    for (const PanMetaSpec& spec : kSpecs) {
        SpiderMeta meta;
        meta.id = spec.id;
        meta.class_name = spec.class_name;
        meta.tmpl = SpiderTemplate::kPan;
        meta.implemented = spec.implemented;
        meta.note = spec.note;

        SpiderFactory factory;
        switch (spec.engine) {
            case 0:
                factory = [] { return std::make_unique<SzpsEngine>(); };
                break;
            case 1:
                factory = [] { return std::make_unique<MelostPanEngine>(); };
                break;
            case 2:
                factory = [] { return std::make_unique<MyDriveEngine>(); };
                break;
            default:
                factory = [] { return std::make_unique<PushEngine>(); };
                break;
        }
        registry.Register(std::move(meta), std::move(factory));
    }
}

// RegisterPanSpiderEngines() 由 SpiderRegistry 构造函数调用；此时再调用
// SpiderRegistry::Instance() 会重入函数局部静态的初始化（MSVC 下死锁），
// 因此这里只置位，真正的注册推迟到静态对象构造完成之后执行。
std::atomic<bool> g_pan_pending{false};

struct PanEngineRegistrar {
    PanEngineRegistrar() {
        SpiderRegistry& registry = SpiderRegistry::Instance();
        if (g_pan_pending.exchange(false)) RegisterPanSpiderMetas(registry);
    }
};

PanEngineRegistrar g_pan_registrar;

} // namespace

void RegisterPanSpiderEngines() {
    g_pan_pending.store(true);
}

} // namespace ttplayer::video
