// 声明式 REST 音源适配器实现
//
// 这类音源不需要 JS 沙箱：一份 JSON 描述「搜什么 URL、从响应哪一段取列表、
// 每个字段映射到哪、播放地址怎么取」，其余交给项目已有的 WinHTTP 后端。
// 所有请求都带 NetworkConfig 里的代理 / 超时 / TLS 校验设置，与插件音源一致。

#include "ttplayer/musicfree/rest_source_adapter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_http.h"

namespace ttplayer::musicfree {
namespace {

// --------------------------- 小工具 ---------------------------

std::string UrlEncode(const std::string& value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

// RFC 1321 MD5（Subsonic 的 token = md5(password + salt)，不能依赖外部库）。
std::string Md5Hex(std::string_view input) {
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    auto rol = [](std::uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    };

    std::vector<std::uint8_t> msg(input.begin(), input.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));

    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89;
    std::uint32_t c0 = 0x98badcfe, d0 = 0x10325476;
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            M[i] = static_cast<std::uint32_t>(msg[off + i * 4]) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 3]) << 24);
        }
        std::uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rol(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (std::uint32_t word : {a0, b0, c0, d0}) {
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t byte = static_cast<std::uint8_t>(word >> (8 * i));
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 15]);
        }
    }
    return out;
}

using Vars = std::map<std::string, std::string>;

std::string Render(std::string_view text, const Vars& vars) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            const std::size_t end = text.find('}', i + 1);
            if (end != std::string_view::npos) {
                const std::string key(text.substr(i + 1, end - i - 1));
                auto it = vars.find(key);
                if (it != vars.end()) out += it->second;
                i = end + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

// JSON 路径：支持 "result.songs" / "data[0].url" / "subsonic-response.searchResult3.song"。
const nlohmann::json* JsonPath(const nlohmann::json& root,
                               const std::string& path) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '.') { ++i; continue; }
        if (path[i] == '[') {
            const std::size_t e = path.find(']', i);
            if (e == std::string::npos) break;
            tokens.push_back(path.substr(i + 1, e - i - 1));
            i = e + 1;
            continue;
        }
        const std::size_t e = path.find_first_of(".[", i);
        tokens.push_back(path.substr(i, e == std::string::npos ? e : e - i));
        i = e == std::string::npos ? path.size() : e;
    }
    const nlohmann::json* cur = &root;
    for (const auto& token : tokens) {
        if (token.empty()) continue;
        if (cur->is_object()) {
            if (!cur->contains(token)) return nullptr;
            cur = &(*cur)[token];
        } else if (cur->is_array()) {
            try {
                const std::size_t idx = static_cast<std::size_t>(std::stoull(token));
                if (idx >= cur->size()) return nullptr;
                cur = &(*cur)[idx];
            } catch (...) {
                return nullptr;
            }
        } else {
            return nullptr;
        }
    }
    return cur;
}

std::string FieldString(const nlohmann::json& item, const std::string& path) {
    if (path.empty()) return {};
    const nlohmann::json* v = JsonPath(item, path);
    if (!v || v->is_null()) return {};
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer() || v->is_number_unsigned())
        return std::to_string(v->get<std::int64_t>());
    if (v->is_number_float()) return std::to_string(v->get<double>());
    return v->dump();
}

std::string GetString(const nlohmann::json& j, const char* key,
                      const std::string& fallback = {}) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>()
                                                 : fallback;
}

// 预设之上覆盖用户的同名顶层字段；search / resolve 内部逐键覆盖。
void MergeConfig(nlohmann::json& base, const nlohmann::json& override) {
    if (!override.is_object()) return;
    for (auto it = override.begin(); it != override.end(); ++it) {
        const std::string key = it.key();
        if ((key == "search" || key == "resolve" || key == "auth" ||
             key == "headers") &&
            base.contains(key) && base[key].is_object() && it.value().is_object()) {
            for (auto sub = it.value().begin(); sub != it.value().end(); ++sub)
                base[key][sub.key()] = sub.value();
        } else {
            base[key] = it.value();
        }
    }
}

} // namespace

// --------------------------- 内置预设 ---------------------------

nlohmann::json RestPresetConfig(std::string_view preset) {
    nlohmann::json cfg = nlohmann::json::object();
    if (preset == "subsonic") {
        cfg["preset"] = "subsonic";
        cfg["name"] = "Subsonic / Navidrome";
        cfg["baseUrl"] = "http://127.0.0.1:4533";
        cfg["auth"] = {{"user", "admin"}, {"password", ""}};
        cfg["search"] = {
            {"path", "/rest/search3.view"},
            {"params", {{"u", "{user}"}, {"t", "{token}"}, {"s", "{salt}"},
                        {"v", "1.16.1"}, {"c", "TTPlayer"}, {"f", "json"},
                        {"query", "{query}"}, {"songCount", "50"},
                        {"songOffset", "{offset}"}}},
            {"result", "subsonic-response.searchResult3.song"},
            {"limit", 50},
            {"fields", {{"id", "id"}, {"title", "title"}, {"artist", "artist"},
                        {"album", "album"}, {"artwork", "coverArt"},
                        {"duration_ms", "duration"}}}};
        cfg["resolve"] = {
            {"path", "/rest/stream.view"},
            {"params", {{"u", "{user}"}, {"t", "{token}"}, {"s", "{salt}"},
                        {"v", "1.16.1"}, {"c", "TTPlayer"}, {"f", "json"},
                        {"id", "{id}"}}},
            {"direct", true}};
    } else if (preset == "listen1api") {
        // 第三方把 listen1-api 包装成 HTTP 服务时的常见形态；路径按实际部署调整。
        cfg["preset"] = "listen1api";
        cfg["name"] = "listen1-api HTTP 服务";
        cfg["baseUrl"] = "http://127.0.0.1:3000";
        cfg["search"] = {
            {"path", "/search"},
            {"params", {{"keywords", "{query}"}, {"source", "netease"},
                        {"offset", "{offset}"}, {"limit", "30"}}},
            {"result", "result.songs"},
            {"limit", 30},
            {"fields", {{"id", "id"}, {"title", "name"},
                        {"artist", "artists[0].name"}, {"album", "album.name"},
                        {"artwork", "album.picUrl"}, {"duration_ms", "duration"}}}};
        cfg["resolve"] = {{"path", "/song/url"},
                          {"params", {{"id", "{id}"}, {"source", "netease"}}},
                          {"url", "data[0].url"}, {"direct", false}};
    } else if (preset == "neteaseapi") {
        cfg["preset"] = "neteaseapi";
        cfg["name"] = "NeteaseCloudMusicApi";
        cfg["baseUrl"] = "http://127.0.0.1:3000";
        cfg["search"] = {
            {"path", "/search"},
            {"params", {{"keywords", "{query}"}, {"offset", "{offset}"},
                        {"limit", "30"}}},
            {"result", "result.songs"},
            {"limit", 30},
            {"fields", {{"id", "id"}, {"title", "name"},
                        {"artist", "artists[0].name"}, {"album", "album.name"},
                        {"artwork", "album.picUrl"}, {"duration_ms", "duration"}}}};
        cfg["resolve"] = {{"path", "/song/url"},
                          {"params", {{"id", "{id}"}}},
                          {"url", "data[0].url"}, {"direct", false}};
    } else {
        cfg["preset"] = "custom";
        cfg["name"] = "自定义 REST 音源";
        cfg["baseUrl"] = "";
        cfg["search"] = {{"path", ""}, {"params", nlohmann::json::object()},
                         {"result", ""}, {"limit", 30},
                         {"fields", nlohmann::json::object()}};
        cfg["resolve"] = {{"path", ""}, {"params", nlohmann::json::object()},
                          {"url", ""}, {"direct", false}};
    }
    return cfg;
}

// --------------------------- 适配器主体 ---------------------------

struct RestSourceAdapter::Impl {
    nlohmann::json cfg;
    NetworkConfig network;
    ProtocolSourceInfo info;
    bool mounted = false;

    Vars MakeVars(const std::string& query, int page,
                  const ProtocolTrack* track) const {
        const int limit = cfg.contains("search") &&
                                  cfg["search"].contains("limit") &&
                                  cfg["search"]["limit"].is_number()
                              ? cfg["search"]["limit"].get<int>()
                              : 30;
        const nlohmann::json auth = cfg.contains("auth") && cfg["auth"].is_object()
                                        ? cfg["auth"]
                                        : nlohmann::json::object();
        const std::string user = GetString(auth, "user");
        const std::string password = GetString(auth, "password");
        const std::string salt =
            std::to_string(std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count());
        Vars vars;
        vars["query"] = UrlEncode(query);
        vars["id"] = UrlEncode(track ? track->id : std::string{});
        vars["page"] = std::to_string(page);
        vars["offset"] = std::to_string((page - 1) * limit);
        vars["limit"] = std::to_string(limit);
        vars["user"] = UrlEncode(user);
        vars["password"] = UrlEncode(password);
        vars["salt"] = UrlEncode(salt);
        vars["token"] = GetString(auth, "token");
        if (vars["token"].empty()) vars["token"] = Md5Hex(password + salt);
        vars["client"] = "TTPlayer";
        vars["version"] = "1.16.1";
        vars["format"] = "json";
        return vars;
    }

    std::map<std::string, std::string> Headers() const {
        std::map<std::string, std::string> headers;
        if (cfg.contains("headers") && cfg["headers"].is_object()) {
            for (auto it = cfg["headers"].begin(); it != cfg["headers"].end(); ++it)
                if (it.value().is_string())
                    headers[it.key()] = it.value().get<std::string>();
        }
        if (cfg.contains("auth") && cfg["auth"].is_object() &&
            cfg["auth"].contains("token") && cfg["auth"]["token"].is_string() &&
            !cfg["auth"]["token"].get<std::string>().empty() &&
            headers.find("Authorization") == headers.end()) {
            headers["Authorization"] =
                "Bearer " + cfg["auth"]["token"].get<std::string>();
        }
        return headers;
    }

    std::string BuildUrl(const nlohmann::json& endpoint, const Vars& vars) const {
        std::string path = Render(GetString(endpoint, "path"), vars);
        std::string url = GetString(cfg, "baseUrl") + path;
        if (endpoint.contains("params") && endpoint["params"].is_object()) {
            std::string query;
            for (auto it = endpoint["params"].begin();
                 it != endpoint["params"].end(); ++it) {
                const std::string value = it.value().is_string()
                                              ? Render(it.value().get<std::string>(), vars)
                                              : it.value().dump();
                if (!query.empty()) query += '&';
                query += UrlEncode(it.key()) + "=" + value;
            }
            if (!query.empty())
                url += (url.find('?') == std::string::npos ? "?" : "&") + query;
        }
        return url;
    }

    nlohmann::json Fetch(const nlohmann::json& endpoint, const Vars& vars) const {
        const std::string url = BuildUrl(endpoint, vars);
        HttpExchange r = PerformHttpRequest(url, "GET", Headers(), {}, network);
        if (r.status < 200 || r.status >= 300)
            throw std::runtime_error("HTTP " + std::to_string(r.status) +
                                    " — " + url);
        const std::string text(r.body.begin(), r.body.end());
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded())
            throw std::runtime_error("响应不是有效 JSON — " + url);
        return j;
    }
};

RestSourceAdapter::RestSourceAdapter() : impl_(std::make_unique<Impl>()) {}
RestSourceAdapter::~RestSourceAdapter() = default;

const ProtocolSourceInfo& RestSourceAdapter::info() const { return impl_->info; }
bool RestSourceAdapter::IsMounted() const { return impl_->mounted; }

bool RestSourceAdapter::Mount(const nlohmann::json& config,
                              const NetworkConfig& network,
                              std::string_view source_url, std::string* error) {
    try {
        if (!config.is_object())
            throw std::runtime_error("REST 音源配置必须是 JSON 对象");
        const std::string preset = GetString(config, "preset", "neteaseapi");
        nlohmann::json cfg = RestPresetConfig(preset);
        MergeConfig(cfg, config);
        cfg["protocol"] = "rest";

        if (GetString(cfg, "baseUrl").empty())
            throw std::runtime_error("缺少 baseUrl（自建服务地址）");
        if (!cfg.contains("search") || !cfg["search"].is_object() ||
            GetString(cfg["search"], "path").empty())
            throw std::runtime_error("缺少 search.path");
        if (!cfg.contains("resolve") || !cfg["resolve"].is_object() ||
            GetString(cfg["resolve"], "path").empty())
            throw std::runtime_error("缺少 resolve.path");

        impl_->cfg = std::move(cfg);
        impl_->network = network;
        auto& info = impl_->info;
        info.kind = SourceProtocol::kRest;
        info.name = GetString(impl_->cfg, "name", preset);
        info.version = GetString(impl_->cfg, "version");
        info.author = GetString(impl_->cfg, "author");
        info.description = GetString(impl_->cfg, "description");
        info.homepage = GetString(impl_->cfg, "homepage");
        info.source_url = std::string(source_url);
        info.sources = {preset};
        info.can_search = !GetString(impl_->cfg["search"], "result").empty();
        info.can_resolve = true;
        info.can_lyric = false;
        impl_->mounted = true;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        impl_->mounted = false;
        return false;
    }
}

std::vector<ProtocolTrack> RestSourceAdapter::Search(const std::string& query,
                                                      int page) {
    std::vector<ProtocolTrack> out;
    if (!impl_->mounted || query.empty()) return out;
    try {
        const auto& search = impl_->cfg["search"];
        Vars vars = impl_->MakeVars(query, page, nullptr);
        nlohmann::json root = impl_->Fetch(search, vars);
        const std::string result_path = GetString(search, "result");
        const nlohmann::json* list = JsonPath(root, result_path);
        if (!list) return out;
        const nlohmann::json* array = list;
        nlohmann::json wrapped;
        if (!list->is_array()) {
            wrapped = nlohmann::json::array();
            wrapped.push_back(*list);
            array = &wrapped;
        }
        const nlohmann::json fields = search.contains("fields") &&
                                              search["fields"].is_object()
                                          ? search["fields"]
                                          : nlohmann::json::object();
        const std::string source_name = impl_->info.name;
        for (const auto& item : *array) {
            if (!item.is_object()) continue;
            ProtocolTrack track;
            track.platform = source_name;
            track.id = FieldString(item, GetString(fields, "id"));
            track.title = FieldString(item, GetString(fields, "title"));
            track.artist = FieldString(item, GetString(fields, "artist"));
            track.album = FieldString(item, GetString(fields, "album"));
            track.artwork = FieldString(item, GetString(fields, "artwork"));
            const std::string dur =
                FieldString(item, GetString(fields, "duration_ms"));
            if (!dur.empty()) {
                try {
                    const double seconds = std::stod(dur);
                    // Subsonic 的 duration 是秒，NeteaseCloudMusicApi 是毫秒。
                    track.duration_ms = seconds < 10000
                                            ? static_cast<std::int64_t>(seconds * 1000)
                                            : static_cast<std::int64_t>(seconds);
                } catch (...) {
                }
            }
            track.raw = item;
            track.raw["platform"] = source_name;
            if (!track.id.empty()) out.push_back(std::move(track));
        }
    } catch (...) {
        // 单个音源失败不应打断其它音源；诊断信息由 registry 汇总。
    }
    return out;
}

ProtocolResolveResult RestSourceAdapter::ResolveUrl(const ProtocolTrack& track) {
    ProtocolResolveResult out;
    out.via = "REST: " + impl_->info.name;
    if (!impl_->mounted) {
        out.error = "REST 音源未挂载";
        return out;
    }
    try {
        const auto& resolve = impl_->cfg["resolve"];
        Vars vars = impl_->MakeVars({}, 1, &track);
        const bool direct = resolve.contains("direct") &&
                            resolve["direct"].is_boolean() &&
                            resolve["direct"].get<bool>();
        if (direct) {
            out.url = impl_->BuildUrl(resolve, vars);
            out.headers = impl_->Headers();
            // 播放器走自定义字节流时可以带上这些头（签名直链常需要 Referer/UA）。
            if (!impl_->network.user_agent.empty()) {
                out.user_agent = core::WideToUtf8(impl_->network.user_agent);
                if (out.headers.find("user-agent") == out.headers.end())
                    out.headers["user-agent"] = out.user_agent;
            }
            return out;
        }
        nlohmann::json root = impl_->Fetch(resolve, vars);
        const std::string url_path = GetString(resolve, "url");
        const nlohmann::json* value = JsonPath(root, url_path);
        if (!value || value->is_null())
            throw std::runtime_error("响应里找不到播放地址（" + url_path + "）");
        out.url = value->is_string() ? value->get<std::string>() : value->dump();
        if (out.url.empty() || out.url == "null")
            throw std::runtime_error("播放地址为空");
    } catch (const std::exception& e) {
        out.url.clear();
        out.error = e.what();
    }
    return out;
}

std::string RestSourceAdapter::Describe() const {
    const auto& i = impl_->info;
    std::string text = "协议：声明式 REST 音源\n";
    text += "名称：" + i.name + "\n";
    const std::string preset = i.sources.empty() ? std::string{} : i.sources.front();
    text += "预设：" + preset + "\n";
    text += "服务地址：" + GetString(impl_->cfg, "baseUrl") + "\n";
    if (!impl_->cfg.contains("search")) return text;
    text += "搜索路径：" + GetString(impl_->cfg["search"], "path") +
            "　结果：" + GetString(impl_->cfg["search"], "result") + "\n";
    if (impl_->cfg.contains("resolve"))
        text += "解析路径：" + GetString(impl_->cfg["resolve"], "path") + "\n";
    if (!i.source_url.empty()) text += "来源：" + i.source_url + "\n";
    text += "能力：搜索" + std::string(i.can_search ? " ✓" : " ✗") +
            "　播放直链 ✓　歌词 ✗";
    return text;
}

} // namespace ttplayer::musicfree
