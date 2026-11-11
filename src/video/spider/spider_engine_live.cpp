#include "ttplayer/video/spider/spider_builtins.h"

// 模板 E（直播 / 平台）引擎。
//
// 本文件实现 5 个真实可跑的类（Alllive / Bili / DouDou / Doubao / LiveGz），
// Fy 因逻辑以 34KB 内嵌 Python 形式存在（含 OCR 验证码与会话缓存）只登记元信息、
// 标 implemented=false。
//
// 关键实测结论（2026-11，均来自真实响应）：
//   * Alllive 网关是 {base}/api/{siteId}/...，响应体是 LZString 压缩的 JSON
//     （ASCII，以 "N4Ig" 开头），必须先用 LZString(base64) 解压；
//     首页 getCategories 返回平台分组/子分类，房间由 getCategoryRooms /
//     getRecommendRooms 给出；详情 getRoomDetail 的 info.name + info.code
//     已是虎牙直链所需的签名串（wsSecret/wsTime/fm/ctype/fs/t），
//     拼到 https://tx.flv.huya.com/src/{name}.flv? 后实测可播（AL/HS 主机 403）。
//   * Bili 的匿名路径依赖 init 里内置的 buvid3 cookie，否则搜索返回 412。
//   * DouDou 的 frodo.douban.com 需要 _sig 签名（返回 997），改用
//     m.douban.com/rexxar/api/v2 匿名可用。
//   * Doubao 的直播流在 /api/v1/public/signals/{playId}/play 返回 m3u8。
//   * LiveGz 的 parameter 未加密也能取数；响应 data 为 Base64(AES-128-CBC)，
//     key=KANGEQIU@8868!~. iv=0200010900030207，明文前 16 字节为随机块，
//     JSON 从首个 [ 或 { 开始。

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ttplayer/video/spider/spider_api.h"
#include "ttplayer/video/spider/spider_http.h"
#include "ttplayer/video/spider/spider_recipe.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/spider/spider_util.h"
#include "ttplayer/video/tvbox_crypto.h"

namespace ttplayer::video {

void RegisterLiveSpiderEngines();

namespace {

namespace su = spider_util;
using nlohmann::json;

// ---------- 通用工具 ----------

bool IsAbsoluteUrl(std::string_view text) {
    const std::string lower = su::ToLower(text);
    return su::StartsWith(lower, "http://") || su::StartsWith(lower, "https://");
}

std::string ComposeUrl(std::string base, std::string path) {
    base = su::Trim(base);
    path = su::Trim(path);
    if (path.empty()) return base;
    if (IsAbsoluteUrl(path)) return path;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (path.front() != '/') path.insert(path.begin(), '/');
    return base + path;
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

// 站内相对封面/回放地址补成绝对地址。
std::string NormalizePic(const std::string& base, std::string pic) {
    if (pic.empty()) return pic;
    if (su::StartsWith(pic, "//")) return "https:" + pic;
    if (pic.front() == '/' && !base.empty()) return ComposeUrl(base, pic);
    return pic;
}

// ext 既可能是 JSON，也可能只是 base URL。
json ParseExtText(const std::string& text) {
    const std::string trimmed = su::Trim(text);
    if (trimmed.empty()) return json::object();
    json parsed = su::ParseJsonLoose(trimmed);
    if (parsed.is_object()) return parsed;
    if (parsed.is_string()) {
        const std::string inner = parsed.get<std::string>();
        if (!inner.empty()) return json{{"host", inner}};
        return json::object();
    }
    return json{{"host", trimmed}};
}

bool ValidHeaderName(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '_')) return false;
    }
    return true;
}

// ---------- LZString（base64 变体）解压 ----------
//
// Java 端用 LZString.compressToBase64(String)，输出字符集
// "A-Za-z0-9+/="。这里按 lz-string 的 _decompress 流程还原 UTF-16 码元，
// 再转 UTF-8（含代理对）。

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string Utf16ToUtf8(const std::u16string& text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned cp = text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
            const unsigned low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        AppendUtf8(out, cp);
    }
    return out;
}

std::string LzStringDecompress(std::string_view encoded) {
    const std::string text = su::Trim(encoded);
    if (text.empty()) return {};
    static constexpr char kKeyStr[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    int lookup[256]{};
    for (int i = 0; kKeyStr[i]; ++i)
        lookup[static_cast<unsigned char>(kKeyStr[i])] = i;

    struct State {
        unsigned val = 0;
        int pos = 32;
        std::size_t idx = 1;
    } state;
    const auto char_at = [&text, &lookup](std::size_t i) -> unsigned {
        if (i >= text.size()) return 0;
        return static_cast<unsigned>(lookup[static_cast<unsigned char>(text[i])]);
    };
    state.val = char_at(0);

    const auto read_bits = [&state, &char_at](int count) -> unsigned {
        unsigned bits = 0;
        unsigned power = 1;
        const unsigned max_power = 1u << count;
        while (power != max_power) {
            const unsigned resb = state.val & static_cast<unsigned>(state.pos);
            state.pos >>= 1;
            if (state.pos == 0) {
                state.pos = 32;
                state.val = char_at(state.idx);
                ++state.idx;
            }
            if (resb > 0) bits |= power;
            power <<= 1;
        }
        return bits;
    };

    const unsigned next = read_bits(2);
    std::u16string current;
    if (next == 0) {
        current.push_back(static_cast<char16_t>(read_bits(8)));
    } else if (next == 1) {
        current.push_back(static_cast<char16_t>(read_bits(16)));
    } else {
        return {};
    }

    std::map<unsigned, std::u16string> dictionary;
    unsigned dict_size = 4;
    unsigned enlarge_in = 4;
    int num_bits = 3;
    dictionary[3] = current;
    std::u16string w = current;
    std::u16string result = current;

    for (;;) {
        if (state.idx > text.size()) return {};
        unsigned code = read_bits(num_bits);
        if (code == 0) {
            dictionary[dict_size] =
                std::u16string(1, static_cast<char16_t>(read_bits(8)));
            ++dict_size;
            code = dict_size - 1;
            --enlarge_in;
        } else if (code == 1) {
            dictionary[dict_size] =
                std::u16string(1, static_cast<char16_t>(read_bits(16)));
            ++dict_size;
            code = dict_size - 1;
            --enlarge_in;
        } else if (code == 2) {
            return Utf16ToUtf8(result);
        }
        if (enlarge_in == 0) {
            enlarge_in = 1u << num_bits;
            ++num_bits;
        }
        std::u16string entry;
        const auto found = dictionary.find(code);
        if (found != dictionary.end()) {
            entry = found->second;
        } else if (code == dict_size && !w.empty()) {
            entry = w + w[0];
        } else {
            return {};
        }
        result += entry;
        dictionary[dict_size] = w + entry[0];
        ++dict_size;
        --enlarge_in;
        w = entry;
        if (enlarge_in == 0) {
            enlarge_in = 1u << num_bits;
            ++num_bits;
        }
    }
}

// ---------- 一次调用的上下文 ----------

struct LiveRun {
    const SpiderContext* ctx = nullptr;
    SpiderRecipe recipe;
    bool has_recipe = false;
    json ext;
    std::string base;
    std::map<std::string, std::string> headers;
};

LiveRun BeginLive(const SpiderContext& ctx, const char* fallback_base) {
    LiveRun run;
    run.ctx = &ctx;
    const SpiderRecipeLoad load =
        LoadSpiderRecipe(ctx.spider_id.empty() ? ctx.site.api : ctx.spider_id);
    if (load.ok) {
        run.recipe = load.recipe;
        run.has_recipe = true;
    }
    run.ext = ParseExtText(ctx.ext.empty() ? ctx.site.ext : ctx.ext);

    std::string base;
    if (run.ext.is_object()) {
        for (const char* key : {"host", "siteUrl", "site_url", "base", "url"}) {
            const std::string value = su::JsonString(run.ext, key);
            if (!value.empty()) {
                base = value;
                break;
            }
        }
    }
    if (base.empty() && run.has_recipe) base = run.recipe.base;
    if (base.empty() && fallback_base) base = fallback_base;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty())
        throw std::runtime_error("模板 E 缺少 base：recipe.base 与 ext 均为空");
    run.base = base;

    if (run.has_recipe) {
        const auto headers_it = run.recipe.raw.find("headers");
        if (headers_it != run.recipe.raw.end() && headers_it->is_object()) {
            for (auto it = headers_it->begin(); it != headers_it->end(); ++it) {
                if (!it.value().is_string()) continue;
                const std::string name = it.key();
                const std::string value = it.value().get<std::string>();
                // "User-Agent(douban)" 这类伪头与 "k=v@k=v" 拼接串不是 HTTP 头。
                if (!ValidHeaderName(name) || value.empty() ||
                    value.find('@') != std::string::npos)
                    continue;
                run.headers[name] = value;
            }
        }
    }
    return run;
}

std::string EndpointPath(const LiveRun& run, const char* name,
                         std::string_view fallback) {
    if (run.has_recipe) {
        const auto endpoints = run.recipe.raw.find("endpoints");
        if (endpoints != run.recipe.raw.end() && endpoints->is_object()) {
            const auto it = endpoints->find(name);
            if (it != endpoints->end() && it->is_object()) {
                const std::string path = su::JsonString(*it, "path");
                if (!path.empty()) return path;
            }
        }
    }
    return std::string(fallback);
}

std::string ExpandSite(std::string path, const std::string& site) {
    return su::ReplaceAll(std::move(path), "{site}", site);
}

json FetchJson(LiveRun& run, const std::string& url, const char* stage) {
    SpiderHttp http(run.ctx->spider_id, run.ctx->net, run.ctx->site);
    http.SetDefaultHeaders(run.headers);
    http.SetStage(stage);
    const SpiderHttpResponse resp = http.Get(url);
    if (resp.body.empty()) {
        throw std::runtime_error("空响应（HTTP " + std::to_string(resp.status) +
                                 "，stage=" + stage + "，url=" +
                                 MaskSpiderUrl(url) + "）");
    }
    json root = su::ParseJsonLoose(resp.body);
    if (root.is_null()) {
        const std::string decoded = LzStringDecompress(resp.body);
        if (!decoded.empty()) root = su::ParseJsonLoose(decoded);
    }
    if (root.is_null()) {
        throw std::runtime_error(
            std::string("响应不是可解析 JSON（stage=") + stage + "，body=" +
            resp.body.substr(0, 120) + "）");
    }
    return root;
}

std::size_t FindJsonStart(const std::string& text) {
    const std::size_t arr = text.find('[');
    const std::size_t obj = text.find('{');
    if (arr == std::string::npos) return obj;
    if (obj == std::string::npos) return arr;
    return std::min(arr, obj);
}

std::string AesCbcDecryptBase64(std::string_view base64, std::string_view key,
                                std::string_view iv) {
    std::string cipher;
    if (!su::Base64Decode(base64, &cipher) || cipher.empty()) return {};
    if ((cipher.size() % 16) != 0 || key.size() != 16 || iv.size() != 16)
        return {};
    std::string plain;
    if (!tvbox_crypto::Aes128CbcDecrypt(
            reinterpret_cast<const std::uint8_t*>(cipher.data()), cipher.size(),
            reinterpret_cast<const std::uint8_t*>(key.data()),
            reinterpret_cast<const std::uint8_t*>(iv.data()), plain))
        return {};
    return plain;
}

long long ToLongLong(std::string_view text, long long fallback = 0) {
    try {
        return std::stoll(std::string(text));
    } catch (...) {
        return fallback;
    }
}

std::string TodayShanghai() {
    const std::time_t now = std::time(nullptr) + 8 * 3600;
    std::tm tm{};
    gmtime_s(&tm, &now);
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string FormatEpoch(long long seconds, const char* format) {
    const std::time_t value = static_cast<std::time_t>(seconds);
    std::tm tm{};
    localtime_s(&tm, &value);
    char buf[64]{};
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

void AppendEpisode(VideoLine* line, std::string name, std::string url) {
    if (!line || url.empty()) return;
    if (name.empty()) name = std::to_string(line->episodes.size() + 1);
    VideoEpisode episode;
    episode.name = std::move(name);
    episode.url = std::move(url);
    line->episodes.push_back(std::move(episode));
}

// ---------- Alllive（一直播，覆盖虎牙/斗鱼/抖音/B站/CC） ----------

std::string SiteLabel(std::string_view site) {
    if (site == "huya") return "虎牙";
    if (site == "douyu") return "斗鱼";
    if (site == "douyin") return "抖音";
    if (site == "bilibili") return "哔哩哔哩";
    if (site == "cc") return "网易CC";
    return std::string(site);
}

VideoListing ParseRoomList(const json& root, const std::string& site) {
    VideoListing listing;
    listing.page = 1;
    const auto data_it = root.find("data");
    if (data_it == root.end() || !data_it->is_object()) return listing;
    const auto list_it = data_it->find("list");
    if (list_it == data_it->end() || !list_it->is_array()) return listing;

    for (const auto& node : *list_it) {
        if (!node.is_object()) continue;
        const std::string room = su::JsonPick(node, {"roomId", "id"});
        if (room.empty()) continue;
        VideoItem item;
        item.id = site + "|" + room;
        item.title = su::JsonPick(node, {"title", "name", "nickname"});
        item.pic = NormalizePic("", su::JsonPick(node, {"cover", "pic"}));
        item.remarks = su::JsonPick(node, {"nickname", "online"});
        item.type_name = su::JsonString(node, "category");
        item.raw = node;
        listing.items.push_back(std::move(item));
    }
    const std::string more = su::JsonString(*data_it, "hasMore");
    listing.is_end = (more == "false" || more == "0");
    return listing;
}

class AllliveEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override;
    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override;
    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override;
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override;
    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        (void)ctx;
        return episode.url;
    }

private:
    static VideoListing FetchRecommend(LiveRun& run, const std::string& site,
                                       int page);
};

VideoListing AllliveEngine::FetchRecommend(LiveRun& run,
                                           const std::string& site, int page) {
    std::string path = EndpointPath(run, "recommend", "/api/{site}/getRecommendRooms");
    path = ExpandSite(std::move(path), site);
    const std::string url = ComposeUrl(
        run.base, path + "?page=" + std::to_string(page <= 0 ? 1 : page));
    return ParseRoomList(FetchJson(run, url, "home"), site);
}

VideoListing AllliveEngine::Home(const SpiderContext& ctx) {
    LiveRun run = BeginLive(ctx, "https://live.muxia.site");
    VideoListing listing;
    listing.page = 1;

    const char* sites[] = {"huya", "douyu", "douyin", "bilibili"};
    for (const char* site : sites) {
        std::string path = EndpointPath(run, "home", "/api/{site}/getCategories");
        path = ExpandSite(std::move(path), site);
        try {
            const json root =
                FetchJson(run, ComposeUrl(run.base, path + "?type=site"), "home");
            const auto data_it = root.find("data");
            if (data_it == root.end() || !data_it->is_array()) continue;
            for (const auto& group : *data_it) {
                if (!group.is_object()) continue;
                const std::string group_name = su::JsonString(group, "name");
                const std::string group_id = su::JsonString(group, "id");
                if (group_name.empty()) continue;
                std::string cid = group_id;
                std::string pid = group_id;
                const auto subs_it = group.find("list");
                if (subs_it != group.end() && subs_it->is_array() &&
                    !subs_it->empty()) {
                    const json& first = (*subs_it)[0];
                    if (first.is_object()) {
                        const std::string sub_cid = su::JsonString(first, "cid");
                        if (!sub_cid.empty()) cid = sub_cid;
                        const std::string sub_pid = su::JsonString(first, "pid");
                        if (!sub_pid.empty()) pid = sub_pid;
                    }
                }
                if (cid.empty()) cid = "0";
                if (pid.empty()) pid = group_id;
                VideoCategory category;
                category.id =
                    std::string(site) + "|" + group_id + "|" + cid + "|" + pid;
                category.name = SiteLabel(site) + "·" + group_name;
                listing.categories.push_back(std::move(category));
            }
        } catch (const std::exception&) {
            // 单个平台失败不影响其它平台。
        }
    }

    for (const char* site : sites) {
        try {
            VideoListing part = FetchRecommend(run, site, 1);
            for (auto& item : part.items)
                listing.items.push_back(std::move(item));
            if (!listing.items.empty()) break;
        } catch (const std::exception&) {
        }
    }
    return listing;
}

VideoListing AllliveEngine::Category(const SpiderContext& ctx,
                                     std::string_view category_id, int page) {
    LiveRun run = BeginLive(ctx, "https://live.muxia.site");
    const int current = page <= 0 ? 1 : page;
    const std::vector<std::string> parts = su::Split(category_id, '|');
    std::string site = parts.size() >= 1 ? parts[0] : "huya";
    const std::string cid = parts.size() >= 3 ? parts[2] : "0";
    const std::string pid = parts.size() >= 4 ? parts[3] : "1";

    if (site != "bilibili") {
        std::string path =
            EndpointPath(run, "category", "/api/{site}/getCategoryRooms");
        path = ExpandSite(std::move(path), site);
        const std::string url = ComposeUrl(
            run.base, path + "?id=" + su::UrlEncode(cid) + "&pid=" +
                          su::UrlEncode(pid) + "&page=" +
                          std::to_string(current));
        try {
            const json root = FetchJson(run, url, "category");
            if (su::JsonString(root, "code") == "200") {
                VideoListing listing = ParseRoomList(root, site);
                listing.page = current;
                if (!listing.items.empty()) return listing;
            }
        } catch (const std::exception&) {
        }
    }
    // B 站分类接口固定 -352（风控），统一回落到推荐房间。
    VideoListing listing = FetchRecommend(run, site, current);
    listing.page = current;
    return listing;
}

VideoListing AllliveEngine::Search(const SpiderContext& ctx,
                                   std::string_view keyword, int page) {
    LiveRun run = BeginLive(ctx, "https://live.muxia.site");
    const int current = page <= 0 ? 1 : page;
    VideoListing listing;
    listing.page = current;

    const char* sites[] = {"huya", "douyu", "bilibili", "douyin"};
    for (const char* site : sites) {
        std::string path = EndpointPath(run, "search", "/api/{site}/searchRooms");
        path = ExpandSite(std::move(path), site);
        const std::string url = ComposeUrl(
            run.base, path + "?page=" + std::to_string(current) + "&kw=" +
                          su::UrlEncode(keyword));
        try {
            VideoListing part =
                ParseRoomList(FetchJson(run, url, "search"), site);
            for (auto& item : part.items)
                listing.items.push_back(std::move(item));
        } catch (const std::exception&) {
        }
        if (listing.items.size() >= 60) break;
    }
    listing.is_end = listing.items.empty();
    return listing;
}

VideoDetail AllliveEngine::Detail(const SpiderContext& ctx,
                                  const VideoItem& item) {
    LiveRun run = BeginLive(ctx, "https://live.muxia.site");
    std::string site = "huya";
    std::string room = su::DigitsOnly(item.id);
    const std::size_t bar = item.id.find('|');
    if (bar != std::string::npos) {
        site = item.id.substr(0, bar);
        room = item.id.substr(bar + 1);
    }
    if (room.empty())
        throw std::runtime_error("Alllive 详情缺少 roomId：" + item.id);

    std::string path = EndpointPath(run, "detail", "/api/{site}/getRoomDetail");
    path = ExpandSite(std::move(path), site);
    const json root = FetchJson(
        run, ComposeUrl(run.base, path + "?id=" + su::UrlEncode(room)),
        "detail");
    const std::string code = su::JsonString(root, "code");
    if (!code.empty() && code != "200") {
        throw std::runtime_error("Alllive 详情 code=" + code + " msg=" +
                                 su::JsonString(root, "msg"));
    }
    const auto data_it = root.find("data");
    if (data_it == root.end() || !data_it->is_object())
        throw std::runtime_error("Alllive 详情缺少 data");
    const json& data = *data_it;

    VideoDetail detail;
    detail.id = item.id;
    detail.title = su::JsonPick(data, {"title", "name"}, item.title);
    detail.pic = NormalizePic(
        run.base, su::JsonPick(data, {"cover", "avatar"}, item.pic));
    detail.remarks = su::JsonPick(data, {"online", "nickname"}, item.remarks);
    detail.raw = data;

    // 虎牙：网关返回的 info.code 就是 wsSecret/wsTime/... 签名串，
    // 与 info.name 拼成 tx CDN 直链（实测 200 video/x-flv）。
    std::string huya_direct;
    if (site == "huya") {
        const auto info_it = data.find("info");
        if (info_it != data.end() && info_it->is_object()) {
            const std::string name = su::JsonString(*info_it, "name");
            const std::string sign = su::JsonString(*info_it, "code");
            if (!name.empty() && !sign.empty()) {
                huya_direct = "https://tx.flv.huya.com/src/" + name + ".flv?" +
                              sign;
            }
        }
    }

    const auto stream_it = data.find("stream");
    const bool has_stream =
        stream_it != data.end() && stream_it->is_array() && !stream_it->empty();

    if (!huya_direct.empty() && has_stream) {
        // 每个清晰度一路，全部使用同一签名直链。
        for (const auto& group : *stream_it) {
            if (!group.is_object()) continue;
            VideoLine line;
            line.name = su::JsonString(group, "name");
            if (line.name.empty()) line.name = "虎牙";
            AppendEpisode(&line, line.name, huya_direct);
            detail.lines.push_back(std::move(line));
        }
    } else if (has_stream) {
        for (const auto& group : *stream_it) {
            if (!group.is_object()) continue;
            VideoLine line;
            line.name = su::JsonString(group, "name");
            if (line.name.empty())
                line.name = "线路" + std::to_string(detail.lines.size() + 1);
            const auto lines_it = group.find("lines");
            if (lines_it != group.end() && lines_it->is_array()) {
                for (const auto& node : *lines_it) {
                    if (!node.is_object()) continue;
                    AppendEpisode(&line, su::JsonPick(node, {"name", "cdn"}),
                                  su::JsonString(node, "url"));
                }
            }
            if (!line.episodes.empty()) detail.lines.push_back(std::move(line));
        }
    }
    if (detail.lines.empty()) {
        const std::string page = su::JsonString(data, "url");
        if (!page.empty()) {
            VideoLine line;
            line.name = "直播页";
            AppendEpisode(&line, SiteLabel(site), page);
            detail.lines.push_back(std::move(line));
        }
    }
    return detail;
}

// ---------- Bili（一套实现覆盖 7 个 csp_BiliGuard 站点） ----------

// Java 类 init 内置的游客 cookie：没有它搜索接口返回 412。
constexpr const char* kBiliCookie =
    "buvid3=7238A82F-D821-D23A-FAA5-ADC19C9796B458050infoc;"
    "bsource=search_baidu;"
    "_uuid=628F6D84-DF8D-ED5A-E4BF-66219979424B58395infoc;"
    "buvid_fp=f7761c3c9bde36415f0299493c60b971;"
    "bp_t_offset_55423440=1203047221809905664;"
    "b_lsid=C1568F05_19E3B3AD0DE";

constexpr const char* kBiliUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/117.0.0.0 Safari/537.36";

// 旧 cookie 仅作兜底；B 站对 playurl 的风控按 IP+设备指纹，实测旧的固定
// buvid3 会被 412 拒绝，重新向 /x/frontend/finger/spi 申请 b_3/b_4 即可恢复。
std::string FetchBiliGuestCookie(const SpiderContext& ctx) {
    SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
    http.SetStage("init");
    http.SetDefaultHeaders({{"User-Agent", kBiliUserAgent},
                            {"Referer", "https://www.bilibili.com/"}});
    try {
        const SpiderHttpResponse resp =
            http.Get("https://api.bilibili.com/x/frontend/finger/spi");
        const json root = su::ParseJsonLoose(resp.body);
        if (root.is_object()) {
            const auto data_it = root.find("data");
            if (data_it != root.end() && data_it->is_object()) {
                const std::string b3 = su::JsonString(*data_it, "b_3");
                const std::string b4 = su::JsonString(*data_it, "b_4");
                if (!b3.empty()) {
                    std::string cookie = "buvid3=" + b3 + ";";
                    if (!b4.empty()) cookie += " buvid4=" + b4 + ";";
                    cookie += " b_nut=" + std::to_string(su::UnixSeconds());
                    return cookie;
                }
            }
        }
    } catch (const std::exception&) {
    }
    return kBiliCookie;
}

std::string BiliGuestCookie(const SpiderContext& ctx) {
    static std::mutex mutex;
    static std::string cached;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!cached.empty()) return cached;
    }
    std::string cookie = FetchBiliGuestCookie(ctx);
    {
        std::lock_guard<std::mutex> lock(mutex);
        cached = cookie;
    }
    return cookie;
}

void ApplyBiliHeaders(LiveRun* run) {
    run->headers["User-Agent"] = kBiliUserAgent;
    run->headers["Referer"] = "https://www.bilibili.com/";
    run->headers["Origin"] = "https://www.bilibili.com";
    run->headers["cookie"] = BiliGuestCookie(*run->ctx);
}

VideoItem MakeBiliItem(const json& node) {
    VideoItem item;
    item.id = su::JsonPick(node, {"bvid", "bv_id", "aid", "id"});
    item.title = su::StripTags(su::JsonPick(node, {"title", "name"}));
    item.pic = NormalizePic(
        "https://api.bilibili.com",
        su::JsonPick(node, {"pic", "cover", "cover_url"}));
    item.remarks = su::JsonPick(node, {"author", "up_name"});
    if (item.remarks.empty()) {
        const auto owner = node.find("owner");
        if (owner != node.end() && owner->is_object())
            item.remarks = su::JsonString(*owner, "name");
    }
    item.score = su::JsonPick(node, {"score"});
    item.raw = node;
    return item;
}

class BiliEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override;
    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override;
    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override;
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override;
    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override;

private:
    static VideoListing SearchVideos(LiveRun& run, const std::string& keyword,
                                     int page);
};

VideoListing BiliEngine::SearchVideos(LiveRun& run, const std::string& keyword,
                                      int page) {
    std::string path = EndpointPath(run, "search",
                                    "/x/web-interface/search/type");
    const std::string url = ComposeUrl(
        run.base, path + "?search_type=video&keyword=" + su::UrlEncode(keyword) +
                      "&order=totalrank&duration=0&page=" +
                      std::to_string(page <= 0 ? 1 : page));
    const json root = FetchJson(run, url, "search");
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    const auto data_it = root.find("data");
    if (data_it == root.end() || !data_it->is_object()) return listing;
    const auto result_it = data_it->find("result");
    if (result_it == data_it->end() || !result_it->is_array()) return listing;
    for (const auto& node : *result_it) {
        if (!node.is_object()) continue;
        VideoItem item = MakeBiliItem(node);
        if (item.id.empty() || item.title.empty()) continue;
        listing.items.push_back(std::move(item));
    }
    const std::string pages = su::JsonString(*data_it, "numPages");
    if (!pages.empty()) {
        try {
            listing.page_count = std::stoi(pages);
        } catch (...) {
        }
    }
    return listing;
}

VideoListing BiliEngine::Home(const SpiderContext& ctx) {
    LiveRun run = BeginLive(ctx, "https://api.bilibili.com");
    ApplyBiliHeaders(&run);

    VideoListing listing;
    listing.page = 1;
    struct CategorySpec {
        const char* id;
        const char* name;
    };
    static const CategorySpec kCategories[] = {
        {"音乐", "音乐"},       {"MV", "MV"},
        {"演唱会", "演唱会"},   {"少儿", "少儿"},
        {"小学课堂", "小学课堂"}, {"初中课堂", "初中课堂"},
        {"高中课堂", "高中课堂"},
    };
    for (const auto& spec : kCategories) {
        VideoCategory category;
        category.id = spec.id;
        category.name = spec.name;
        listing.categories.push_back(std::move(category));
    }

    try {
        std::string path = EndpointPath(run, "home", "/x/web-interface/popular");
        const json root =
            FetchJson(run, ComposeUrl(run.base, path + "?pn=1&ps=20"), "home");
        const auto data_it = root.find("data");
        if (data_it != root.end() && data_it->is_object()) {
            const auto list_it = data_it->find("list");
            if (list_it != data_it->end() && list_it->is_array()) {
                for (const auto& node : *list_it) {
                    if (!node.is_object()) continue;
                    VideoItem item = MakeBiliItem(node);
                    if (item.id.empty() || item.title.empty()) continue;
                    listing.items.push_back(std::move(item));
                }
            }
        }
    } catch (const std::exception&) {
    }
    return listing;
}

VideoListing BiliEngine::Category(const SpiderContext& ctx,
                                  std::string_view category_id, int page) {
    LiveRun run = BeginLive(ctx, "https://api.bilibili.com");
    ApplyBiliHeaders(&run);
    return SearchVideos(run, std::string(category_id), page);
}

VideoListing BiliEngine::Search(const SpiderContext& ctx,
                                std::string_view keyword, int page) {
    LiveRun run = BeginLive(ctx, "https://api.bilibili.com");
    ApplyBiliHeaders(&run);
    return SearchVideos(run, std::string(keyword), page);
}

VideoDetail BiliEngine::Detail(const SpiderContext& ctx,
                               const VideoItem& item) {
    LiveRun run = BeginLive(ctx, "https://api.bilibili.com");
    ApplyBiliHeaders(&run);

    std::string path = EndpointPath(run, "detail", "/x/web-interface/view");
    const std::string id = item.id;
    const bool is_bvid =
        id.size() >= 2 && (id[0] == 'B' || id[0] == 'b') &&
        (id[1] == 'V' || id[1] == 'v');
    const std::string url =
        ComposeUrl(run.base, path + (is_bvid ? "?bvid=" : "?aid=") +
                                 su::UrlEncode(id));
    const json root = FetchJson(run, url, "detail");
    const std::string code = su::JsonString(root, "code");
    if (!code.empty() && code != "0") {
        throw std::runtime_error("Bili 详情 code=" + code + " msg=" +
                                 su::JsonString(root, "message"));
    }
    const auto data_it = root.find("data");
    if (data_it == root.end() || !data_it->is_object())
        throw std::runtime_error("Bili 详情缺少 data");
    const json& data = *data_it;

    VideoDetail detail;
    detail.id = item.id;
    detail.title = su::JsonString(data, "title", item.title);
    detail.pic = NormalizePic(
        run.base, su::JsonString(data, "pic", item.pic));
    detail.remarks = su::JsonString(data, "desc").substr(0, 120);
    detail.raw = data;

    const std::string aid = su::JsonString(data, "aid");
    VideoLine line;
    line.name = "B站";
    const auto pages_it = data.find("pages");
    if (pages_it != data.end() && pages_it->is_array() && !pages_it->empty()) {
        int index = 0;
        for (const auto& page_node : *pages_it) {
            if (!page_node.is_object()) continue;
            ++index;
            const std::string cid = su::JsonString(page_node, "cid");
            if (cid.empty()) continue;
            std::string name = "P" + std::to_string(index);
            const std::string part = su::JsonString(page_node, "part");
            if (!part.empty()) name += " " + part;
            const std::string playurl =
                "https://api.bilibili.com/x/player/playurl?avid=" + aid +
                "&cid=" + cid + "&qn=32&fnval=1&fourk=1";
            AppendEpisode(&line, std::move(name), playurl);
        }
    } else {
        const std::string cid = su::JsonString(data, "cid");
        if (!aid.empty() && !cid.empty()) {
            AppendEpisode(&line, "正片",
                          "https://api.bilibili.com/x/player/playurl?avid=" +
                              aid + "&cid=" + cid +
                              "&qn=32&fnval=1&fourk=1");
        }
    }
    if (!line.episodes.empty()) detail.lines.push_back(std::move(line));
    return detail;
}

std::string BiliEngine::Player(const SpiderContext& ctx,
                               const VideoEpisode& episode) {
    if (episode.url.find("/x/player/playurl") == std::string::npos)
        return episode.url;
    LiveRun run = BeginLive(ctx, "https://api.bilibili.com");
    ApplyBiliHeaders(&run);

    const auto pick_media = [](const json& root) -> std::string {
        const auto data_it = root.find("data");
        if (data_it == root.end() || !data_it->is_object()) return {};
        const auto durl_it = data_it->find("durl");
        if (durl_it != data_it->end() && durl_it->is_array() &&
            !durl_it->empty()) {
            const json& first = (*durl_it)[0];
            if (first.is_object()) {
                const std::string url = su::JsonString(first, "url");
                if (!url.empty()) return url;
            }
        }
        const auto dash_it = data_it->find("dash");
        if (dash_it != data_it->end() && dash_it->is_object()) {
            for (const char* key : {"video", "audio"}) {
                const auto list_it = dash_it->find(key);
                if (list_it == dash_it->end() || !list_it->is_array() ||
                    list_it->empty())
                    continue;
                const json& first = (*list_it)[0];
                if (!first.is_object()) continue;
                const std::string base_url = su::JsonString(first, "baseUrl");
                if (!base_url.empty()) return base_url;
            }
        }
        return {};
    };

    std::string media = pick_media(FetchJson(run, episode.url, "player"));
    if (!media.empty()) return media;

    // durl 取不到（登录/清晰度限制）时退回 DASH（fnval=4048）。
    const std::string avid = su::QueryParam(episode.url, "avid");
    const std::string cid = su::QueryParam(episode.url, "cid");
    if (!avid.empty() && !cid.empty()) {
        const std::string dash_url =
            "https://api.bilibili.com/x/player/playurl?avid=" + avid +
            "&cid=" + cid + "&qn=127&fnval=4048&fourk=1";
        media = pick_media(FetchJson(run, dash_url, "player"));
        if (!media.empty()) return media;
    }
    return episode.url;
}

// ---------- DouDou（豆瓣片单） ----------

void ApplyDoubanHeaders(LiveRun* run) {
    run->headers["User-Agent"] =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/110.0.0.0 Safari/537.36";
    run->headers["Referer"] = "https://m.douban.com/";
}

VideoItem MakeDoubanItem(const json& node) {
    VideoItem item;
    if (!node.is_object()) return item;
    const json* target = &node;
    const auto target_it = node.find("target");
    if (target_it != node.end() && target_it->is_object()) target = &*target_it;

    item.id = su::JsonPick(*target, {"id", "target_id"});
    item.title = su::JsonString(*target, "title");
    item.pic = su::JsonString(*target, "cover_url");
    if (item.pic.empty()) {
        const auto cover_it = target->find("cover");
        if (cover_it != target->end()) {
            if (cover_it->is_object())
                item.pic = su::JsonString(*cover_it, "url");
            else if (cover_it->is_string())
                item.pic = cover_it->get<std::string>();
        }
    }
    const auto rating_it = target->find("rating");
    if (rating_it != target->end() && rating_it->is_object()) {
        const std::string value = su::JsonString(*rating_it, "value");
        if (!value.empty()) item.remarks = "评分：" + value;
    }
    if (item.remarks.empty()) item.remarks = su::JsonString(*target, "info");
    item.raw = *target;
    return item;
}

class DouDouEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override;
    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override;
    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override;
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override;
    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        (void)ctx;
        return episode.url;
    }
};

VideoListing DouDouEngine::Home(const SpiderContext& ctx) {
    LiveRun run = BeginLive(ctx, "https://m.douban.com/rexxar");
    ApplyDoubanHeaders(&run);

    VideoListing listing;
    listing.page = 1;
    struct CategorySpec {
        const char* id;
        const char* name;
    };
    static const CategorySpec kCategories[] = {
        {"movie_hot_gaia", "热播电影"},
        {"tv_hot", "热播剧集"},
        {"show_hot", "热播综艺"},
        {"movie_real_time_hotest", "实时热门电影"},
        {"tv_real_time_hotest", "实时热门剧集"},
    };
    for (const auto& spec : kCategories) {
        VideoCategory category;
        category.id = spec.id;
        category.name = spec.name;
        listing.categories.push_back(std::move(category));
    }

    try {
        std::string path = EndpointPath(
            run, "home",
            "/api/v2/subject_collection/subject_real_time_hotest/items");
        const json root =
            FetchJson(run, ComposeUrl(run.base, path + "?start=0&count=20"),
                      "home");
        const auto items_it = root.find("subject_collection_items");
        if (items_it != root.end() && items_it->is_array()) {
            for (const auto& node : *items_it) {
                VideoItem item = MakeDoubanItem(node);
                if (item.id.empty() || item.title.empty()) continue;
                listing.items.push_back(std::move(item));
            }
        }
    } catch (const std::exception&) {
    }
    return listing;
}

VideoListing DouDouEngine::Category(const SpiderContext& ctx,
                                    std::string_view category_id, int page) {
    LiveRun run = BeginLive(ctx, "https://m.douban.com/rexxar");
    ApplyDoubanHeaders(&run);
    const int current = page <= 0 ? 1 : page;
    std::string path = EndpointPath(
        run, "category", "/api/v2/subject_collection/{collection}/items");
    path = su::ReplaceAll(std::move(path), "{collection}",
                          std::string(category_id));
    const std::string url = ComposeUrl(
        run.base, path + "?start=" + std::to_string((current - 1) * 20) +
                      "&count=20");
    const json root = FetchJson(run, url, "category");

    VideoListing listing;
    listing.page = current;
    const auto items_it = root.find("subject_collection_items");
    if (items_it != root.end() && items_it->is_array()) {
        for (const auto& node : *items_it) {
            VideoItem item = MakeDoubanItem(node);
            if (item.id.empty() || item.title.empty()) continue;
            listing.items.push_back(std::move(item));
        }
    }
    const std::string total = su::JsonString(root, "total");
    if (!total.empty()) {
        try {
            listing.page_count = (std::stoi(total) + 19) / 20;
        } catch (...) {
        }
    }
    return listing;
}

VideoListing DouDouEngine::Search(const SpiderContext& ctx,
                                  std::string_view keyword, int page) {
    LiveRun run = BeginLive(ctx, "https://m.douban.com/rexxar");
    ApplyDoubanHeaders(&run);
    const int current = page <= 0 ? 1 : page;
    std::string path = EndpointPath(run, "search", "/api/v2/search");
    const std::string url = ComposeUrl(
        run.base, path + "?q=" + su::UrlEncode(keyword) +
                      "&type=movie&start=" + std::to_string((current - 1) * 20) +
                      "&count=20");
    const json root = FetchJson(run, url, "search");

    VideoListing listing;
    listing.page = current;
    const auto subjects_it = root.find("subjects");
    if (subjects_it == root.end() || !subjects_it->is_object()) return listing;
    const auto items_it = subjects_it->find("items");
    if (items_it == subjects_it->end() || !items_it->is_array()) return listing;
    for (const auto& node : *items_it) {
        VideoItem item = MakeDoubanItem(node);
        if (item.id.empty() || item.title.empty()) continue;
        listing.items.push_back(std::move(item));
    }
    return listing;
}

VideoDetail DouDouEngine::Detail(const SpiderContext& ctx,
                                 const VideoItem& item) {
    LiveRun run = BeginLive(ctx, "https://m.douban.com/rexxar");
    ApplyDoubanHeaders(&run);

    std::string path = EndpointPath(run, "detail", "/api/v2/movie/{vod_id}");
    path = su::ReplaceAll(std::move(path), "{vod_id}", item.id);
    json root;
    try {
        root = FetchJson(run, ComposeUrl(run.base, path), "detail");
    } catch (const std::exception&) {
        // 剧集在 /tv/{id}，电影在 /movie/{id}。
        const std::string tv_path = su::ReplaceAll(
            std::string("/api/v2/tv/{vod_id}"), "{vod_id}", item.id);
        root = FetchJson(run, ComposeUrl(run.base, tv_path), "detail");
    }

    VideoDetail detail;
    detail.id = item.id;
    detail.title = su::JsonString(root, "title", item.title);
    std::string pic = su::JsonString(root, "cover_url");
    if (pic.empty()) pic = su::JsonString(root, "pic");
    if (pic.empty()) {
        const auto pic_it = root.find("pic");
        if (pic_it != root.end() && pic_it->is_object())
            pic = su::JsonString(*pic_it, "large");
    }
    detail.pic = NormalizePic(run.base, pic.empty() ? item.pic : pic);
    detail.remarks = su::JsonString(root, "intro").substr(0, 120);
    detail.raw = root;

    std::string page = su::JsonString(root, "sharing_url");
    if (page.empty()) page = su::JsonString(root, "url");
    if (page.empty()) page = "https://movie.douban.com/subject/" + item.id + "/";
    VideoLine line;
    line.name = "豆瓣";
    AppendEpisode(&line, "豆瓣页面", page);
    detail.lines.push_back(std::move(line));
    return detail;
}

// ---------- Doubao（体育直播 / 回放） ----------

VideoItem MakeDoubaoMatchItem(const json& element) {
    VideoItem item;
    if (!element.is_object()) return item;
    const auto match_it = element.find("match");
    if (match_it == element.end() || !match_it->is_object()) return item;
    const json& match = *match_it;

    std::string room_id;
    std::string room_title;
    std::string room_cover;
    const auto rooms_it = element.find("rooms");
    if (rooms_it != element.end() && rooms_it->is_array() && !rooms_it->empty()) {
        const json& first = (*rooms_it)[0];
        if (first.is_object()) {
            room_id = su::JsonString(first, "id");
            room_title = su::JsonString(first, "title");
            room_cover = su::JsonString(first, "cover");
        }
    }
    const std::string match_id = su::JsonString(match, "id");
    if (!room_id.empty())
        item.id = "room:" + room_id;
    else if (!match_id.empty())
        item.id = "match:" + match_id;
    else
        return item;

    const std::string home = su::JsonString(match, "home");
    const std::string away = su::JsonString(match, "away");
    item.title = (!home.empty() || !away.empty()) ? home + " vs " + away
                                                  : room_title;
    item.pic = su::JsonString(match, "homeLogo");
    if (item.pic.empty()) item.pic = room_cover;
    const std::string league = su::JsonString(match, "league");
    std::string when = su::JsonString(match, "matchTime");
    if (when.size() >= 16) {
        when = when.substr(5, 11);
        std::replace(when.begin(), when.end(), 'T', ' ');
    }
    item.remarks = league;
    if (!when.empty()) item.remarks += (item.remarks.empty() ? "" : " ") + when;
    item.raw = element;
    return item;
}

class DoubaoEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override;
    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override;
    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override;
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override;
    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override;

private:
    static VideoListing FetchSchedules(LiveRun& run, const std::string& category,
                                       int page);
    static VideoListing FetchPlaybacks(LiveRun& run, int page);
};

VideoListing DoubaoEngine::FetchSchedules(LiveRun& run,
                                          const std::string& category,
                                          int page) {
    std::string path = EndpointPath(run, "home", "/api/v1/home/schedules");
    const std::string url = ComposeUrl(
        run.base, path + "?date=" + TodayShanghai() +
                      "&days=5&category=" + su::UrlEncode(category) +
                      "&continuous=true&pageSize=30&q=");
    const json root = FetchJson(run, url, "home");
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    const auto data_it = root.find("data");
    if (data_it == root.end() || !data_it->is_object()) return listing;
    const auto items_it = data_it->find("items");
    if (items_it == data_it->end() || !items_it->is_array()) return listing;
    for (const auto& node : *items_it) {
        VideoItem item = MakeDoubaoMatchItem(node);
        if (item.id.empty() || item.title.empty()) continue;
        listing.items.push_back(std::move(item));
    }
    // 有直播间的排前面，保证详情/播放能拿到地址。
    std::stable_sort(listing.items.begin(), listing.items.end(),
                     [](const VideoItem& a, const VideoItem& b) {
                         const bool a_live = su::StartsWith(a.id, "room:");
                         const bool b_live = su::StartsWith(b.id, "room:");
                         return a_live && !b_live;
                     });
    return listing;
}

VideoListing DoubaoEngine::FetchPlaybacks(LiveRun& run, int page) {
    std::string path = EndpointPath(run, "playbacks", "/api/v1/playbacks");
    const std::string url = ComposeUrl(
        run.base, path + "?category=&q=&page=" +
                      std::to_string(page <= 0 ? 1 : page) + "&pageSize=20");
    const json root = FetchJson(run, url, "category");
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    const auto data_it = root.find("data");
    if (data_it == root.end() || !data_it->is_object()) return listing;
    const auto items_it = data_it->find("items");
    if (items_it == data_it->end() || !items_it->is_array()) return listing;
    for (const auto& node : *items_it) {
        if (!node.is_object()) continue;
        const std::string id = su::JsonString(node, "scheduleId");
        if (id.empty()) continue;
        VideoItem item;
        item.id = "pb:" + id;
        item.title = su::JsonString(node, "title");
        item.pic = NormalizePic(run.base, su::JsonString(node, "cover"));
        const auto schedule_it = node.find("schedule");
        if (schedule_it != node.end() && schedule_it->is_object())
            item.remarks = su::JsonString(*schedule_it, "league");
        item.raw = node;
        listing.items.push_back(std::move(item));
    }
    return listing;
}

VideoListing DoubaoEngine::Home(const SpiderContext& ctx) {
    LiveRun run = BeginLive(ctx, "https://www.doubaozhibo.com");
    VideoListing listing = FetchSchedules(run, "all", 1);
    struct CategorySpec {
        const char* id;
        const char* name;
    };
    static const CategorySpec kCategories[] = {
        {"all", "今日赛程"}, {"basketball", "篮球"},
        {"football", "足球"}, {"playbacks", "赛事回放"},
    };
    for (const auto& spec : kCategories) {
        VideoCategory category;
        category.id = spec.id;
        category.name = spec.name;
        listing.categories.push_back(std::move(category));
    }
    return listing;
}

VideoListing DoubaoEngine::Category(const SpiderContext& ctx,
                                    std::string_view category_id, int page) {
    LiveRun run = BeginLive(ctx, "https://www.doubaozhibo.com");
    if (category_id == "playbacks") return FetchPlaybacks(run, page);
    return FetchSchedules(run, std::string(category_id), page);
}

VideoListing DoubaoEngine::Search(const SpiderContext& ctx,
                                  std::string_view keyword, int page) {
    LiveRun run = BeginLive(ctx, "https://www.doubaozhibo.com");
    std::string path = EndpointPath(run, "search", "/api/v1/home/schedules");
    const std::string url = ComposeUrl(
        run.base, path + "?date=" + TodayShanghai() + "&days=5&category=all" +
                      "&continuous=true&pageSize=30&q=" +
                      su::UrlEncode(keyword));
    const json root = FetchJson(run, url, "search");
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    const auto data_it = root.find("data");
    if (data_it == root.end() || !data_it->is_object()) return listing;
    const auto items_it = data_it->find("items");
    if (items_it == data_it->end() || !items_it->is_array()) return listing;
    for (const auto& node : *items_it) {
        VideoItem item = MakeDoubaoMatchItem(node);
        if (item.id.empty() || item.title.empty()) continue;
        listing.items.push_back(std::move(item));
    }
    return listing;
}

VideoDetail DoubaoEngine::Detail(const SpiderContext& ctx,
                                 const VideoItem& item) {
    LiveRun run = BeginLive(ctx, "https://www.doubaozhibo.com");
    VideoDetail detail;
    detail.id = item.id;
    detail.title = item.title;
    detail.pic = item.pic;

    if (su::StartsWith(item.id, "pb:")) {
        const std::string schedule_id = item.id.substr(3);
        for (int page = 1; page <= 3; ++page) {
            const VideoListing listing = FetchPlaybacks(run, page);
            const VideoItem* found = nullptr;
            for (const auto& candidate : listing.items) {
                if (candidate.id == item.id) {
                    found = &candidate;
                    break;
                }
            }
            if (!found) continue;
            detail.raw = found->raw;
            VideoLine line;
            line.name = "回放";
            const auto lines_it = found->raw.find("lines");
            if (lines_it != found->raw.end() && lines_it->is_array()) {
                for (const auto& node : *lines_it) {
                    if (!node.is_object()) continue;
                    AppendEpisode(
                        &line, su::JsonString(node, "title"),
                        ComposeUrl(run.base, su::JsonString(node, "proxyUrl")));
                }
            }
            if (!line.episodes.empty()) detail.lines.push_back(std::move(line));
            break;
        }
        (void)schedule_id;
        return detail;
    }

    json data;
    try {
        if (su::StartsWith(item.id, "room:")) {
            const std::string room_id = item.id.substr(5);
            std::string path =
                EndpointPath(run, "detail", "/api/v1/public/signals/{roomId}");
            path = su::ReplaceAll(std::move(path), "{roomId}", room_id);
            const json root =
                FetchJson(run, ComposeUrl(run.base, path), "detail");
            const auto data_it = root.find("data");
            if (data_it != root.end() && data_it->is_object()) data = *data_it;
        } else if (su::StartsWith(item.id, "match:")) {
            const std::string match_id = item.id.substr(6);
            const std::string path =
                "/api/v1/public/matches/" + match_id + "/signals";
            const json root =
                FetchJson(run, ComposeUrl(run.base, path), "detail");
            const auto data_it = root.find("data");
            if (data_it != root.end() && data_it->is_object()) data = *data_it;
        }
    } catch (const std::exception&) {
    }
    if (!data.is_null()) detail.raw = data;

    VideoLine line;
    line.name = "直播";
    if (data.is_object()) {
        const char* arrays[] = {"signals", "items"};
        for (const char* key : arrays) {
            const auto list_it = data.find(key);
            if (list_it == data.end() || !list_it->is_array()) continue;
            for (const auto& node : *list_it) {
                if (!node.is_object()) continue;
                const std::string play_id = su::JsonString(node, "playId");
                if (play_id.empty()) continue;
                AppendEpisode(&line, su::JsonString(node, "name"),
                              run.base + "/api/v1/public/signals/" + play_id +
                                  "/play");
            }
            if (!line.episodes.empty()) break;
        }
    }
    if (line.episodes.empty()) {
        const std::string page = su::JsonString(item.raw, "playUrl");
        if (!page.empty())
            AppendEpisode(&line, "直播页", NormalizePic(run.base, page));
    }
    if (!line.episodes.empty()) detail.lines.push_back(std::move(line));
    return detail;
}

std::string DoubaoEngine::Player(const SpiderContext& ctx,
                                 const VideoEpisode& episode) {
    const std::string& url = episode.url;
    if (url.find("/api/v1/public/signals/") != std::string::npos &&
        su::EndsWith(url, "/play")) {
        LiveRun run = BeginLive(ctx, "https://www.doubaozhibo.com");
        const json root = FetchJson(run, url, "player");
        const auto data_it = root.find("data");
        if (data_it != root.end() && data_it->is_object()) {
            const std::string media = su::JsonString(*data_it, "url");
            if (!media.empty()) return NormalizePic(run.base, media);
        }
    }
    return url;
}

// ---------- LiveGz（赛事，AES-128-CBC 响应） ----------

constexpr const char* kLiveGzKey = "KANGEQIU@8868!~.";
constexpr const char* kLiveGzIv = "0200010900030207";

void ApplyLiveGzHeaders(LiveRun* run) {
    run->headers["User-Agent"] = "okhttp/3.12.0";
    run->headers["client-version"] = "3.0.4.0";
}

json FetchLiveGz(LiveRun& run, const std::string& path, const char* stage) {
    SpiderHttp http(run.ctx->spider_id, run.ctx->net, run.ctx->site);
    http.SetDefaultHeaders(run.headers);
    http.SetStage(stage);
    const SpiderHttpResponse resp =
        http.Get(ComposeUrl(run.base, path + "?parameter=key"));
    json envelope = su::ParseJsonLoose(resp.body);
    if (envelope.is_null())
        throw std::runtime_error("LiveGz 响应不是 JSON（body=" +
                                 resp.body.substr(0, 100) + "）");
    const std::string code = su::JsonString(envelope, "code");
    if (!code.empty() && code != "200") {
        throw std::runtime_error("LiveGz code=" + code + " msg=" +
                                 su::JsonString(envelope, "message"));
    }
    const std::string data = su::JsonString(envelope, "data");
    if (data.empty()) throw std::runtime_error("LiveGz 响应缺少 data");
    const std::string plain =
        AesCbcDecryptBase64(data, kLiveGzKey, kLiveGzIv);
    if (plain.empty()) throw std::runtime_error("LiveGz AES 解密失败");
    const std::size_t start = FindJsonStart(plain);
    if (start == std::string::npos)
        throw std::runtime_error("LiveGz 明文不是 JSON");
    json root = su::ParseJsonLoose(plain.substr(start));
    if (root.is_null())
        throw std::runtime_error("LiveGz 明文 JSON 解析失败");
    return root;
}

int LiveGzBallType(const json& node) {
    const std::string text = su::JsonString(node, "ball_type");
    try {
        return std::stoi(text);
    } catch (...) {
        return 0;
    }
}

VideoItem MakeLiveGzItem(const json& node) {
    VideoItem item;
    if (!node.is_object()) return item;
    item.id = su::JsonString(node, "mid");
    if (item.id.empty()) return item;
    const auto home_it = node.find("home");
    const auto away_it = node.find("visiting");
    std::string home;
    std::string away;
    if (home_it != node.end() && home_it->is_object()) {
        home = su::JsonString(*home_it, "name");
        item.pic = su::JsonString(*home_it, "logo");
    }
    if (away_it != node.end() && away_it->is_object())
        away = su::JsonString(*away_it, "name");
    item.title = home.empty() && away.empty() ? su::JsonString(node, "event_name")
                                              : home + " vs " + away;
    const std::string event = su::JsonString(node, "event_name");
    const std::string when =
        FormatEpoch(ToLongLong(su::JsonString(node, "match_time")), "%m-%d %H:%M");
    item.remarks = event;
    if (!when.empty())
        item.remarks += (item.remarks.empty() ? "" : " ") + when;
    item.raw = node;
    return item;
}

bool HasLiveGzLines(const json& node) {
    const auto lines_it = node.find("line_list");
    return lines_it != node.end() && lines_it->is_array() && !lines_it->empty();
}

class LiveGzEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override;
    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override;
    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override;
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override;
    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        (void)ctx;
        return episode.url;
    }

private:
    static json FetchSports(LiveRun& run);
};

json LiveGzEngine::FetchSports(LiveRun& run) {
    std::string path = EndpointPath(run, "home", "/gz/live/sports");
    return FetchLiveGz(run, path, "home");
}

VideoListing LiveGzEngine::Home(const SpiderContext& ctx) {
    LiveRun run = BeginLive(ctx, "https://api.5h5hbfhh.com");
    ApplyLiveGzHeaders(&run);
    const json root = FetchSports(run);
    if (!root.is_array()) throw std::runtime_error("LiveGz sports 不是数组");

    VideoListing listing;
    listing.page = 1;
    struct CategorySpec {
        const char* id;
        const char* name;
    };
    static const CategorySpec kCategories[] = {
        {"1", "足球"}, {"2", "篮球"}, {"37", "NBA"},
    };
    for (const auto& spec : kCategories) {
        VideoCategory category;
        category.id = spec.id;
        category.name = spec.name;
        listing.categories.push_back(std::move(category));
    }
    for (const auto& node : root) {
        VideoItem item = MakeLiveGzItem(node);
        if (item.id.empty() || item.title.empty()) continue;
        listing.items.push_back(std::move(item));
    }
    std::stable_sort(listing.items.begin(), listing.items.end(),
                     [](const VideoItem& a, const VideoItem& b) {
                         return HasLiveGzLines(a.raw) && !HasLiveGzLines(b.raw);
                     });
    return listing;
}

VideoListing LiveGzEngine::Category(const SpiderContext& ctx,
                                    std::string_view category_id, int page) {
    LiveRun run = BeginLive(ctx, "https://api.5h5hbfhh.com");
    ApplyLiveGzHeaders(&run);
    const json root = FetchSports(run);
    if (!root.is_array()) throw std::runtime_error("LiveGz sports 不是数组");

    int ball_type = 0;
    try {
        ball_type = std::stoi(std::string(category_id));
    } catch (...) {
    }
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    for (const auto& node : root) {
        VideoItem item = MakeLiveGzItem(node);
        if (item.id.empty() || item.title.empty()) continue;
        if (ball_type == 37) {
            // "NBA 0-37-0" 的 37 是赛事 tag，不是 ball_type。
            if (su::ToLower(su::JsonString(node, "event_name")).find("nba") ==
                std::string::npos)
                continue;
        } else if (ball_type != 0 && LiveGzBallType(node) != ball_type) {
            continue;
        }
        listing.items.push_back(std::move(item));
    }
    std::stable_sort(listing.items.begin(), listing.items.end(),
                     [](const VideoItem& a, const VideoItem& b) {
                         return HasLiveGzLines(a.raw) && !HasLiveGzLines(b.raw);
                     });
    return listing;
}

VideoListing LiveGzEngine::Search(const SpiderContext& ctx,
                                  std::string_view keyword, int page) {
    LiveRun run = BeginLive(ctx, "https://api.5h5hbfhh.com");
    ApplyLiveGzHeaders(&run);
    const json root = FetchSports(run);
    if (!root.is_array()) throw std::runtime_error("LiveGz sports 不是数组");

    const std::string needle = su::ToLower(keyword);
    VideoListing listing;
    listing.page = page <= 0 ? 1 : page;
    for (const auto& node : root) {
        VideoItem item = MakeLiveGzItem(node);
        if (item.id.empty() || item.title.empty()) continue;
        const std::string haystack = su::ToLower(
            item.title + " " + item.remarks + " " + su::JsonString(node, "stage"));
        if (haystack.find(needle) == std::string::npos) continue;
        listing.items.push_back(std::move(item));
    }
    return listing;
}

VideoDetail LiveGzEngine::Detail(const SpiderContext& ctx,
                                 const VideoItem& item) {
    LiveRun run = BeginLive(ctx, "https://api.5h5hbfhh.com");
    ApplyLiveGzHeaders(&run);
    const json root = FetchSports(run);
    if (!root.is_array()) throw std::runtime_error("LiveGz sports 不是数组");

    const json* found = nullptr;
    for (const auto& node : root) {
        if (su::JsonString(node, "mid") == item.id) {
            found = &node;
            break;
        }
    }
    if (!found)
        throw std::runtime_error("LiveGz 未找到赛事：" + item.id);

    VideoDetail detail;
    detail.id = item.id;
    detail.title = item.title;
    detail.pic = item.pic;
    detail.remarks = item.remarks;
    detail.raw = *found;

    VideoLine line;
    line.name = "直播";
    const auto lines_it = found->find("line_list");
    if (lines_it != found->end() && lines_it->is_array()) {
        for (const auto& node : *lines_it) {
            if (!node.is_object()) continue;
            AppendEpisode(&line, su::JsonString(node, "name"),
                          su::JsonString(node, "url"));
        }
    }
    if (line.episodes.empty()) {
        const std::string live_url = su::JsonString(*found, "live_url");
        if (!live_url.empty()) AppendEpisode(&line, "直播", live_url);
    }
    if (!line.episodes.empty()) detail.lines.push_back(std::move(line));
    return detail;
}

// ---------- Fy（内置 34KB Python，明确降级） ----------

class FyEngine final : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext&) override { Throw(); }
    VideoListing Category(const SpiderContext&, std::string_view, int) override {
        Throw();
    }
    VideoListing Search(const SpiderContext&, std::string_view, int) override {
        Throw();
    }
    VideoDetail Detail(const SpiderContext&, const VideoItem&) override { Throw(); }
    std::string Player(const SpiderContext&, const VideoEpisode&) override {
        Throw();
    }

private:
    [[noreturn]] static void Throw() {
        throw std::runtime_error(
            "Fy 的内嵌 Python spider（含 OCR 验证码与会话缓存）未实现");
    }
};

// ---------- 注册 ----------

struct LiveMetaSpec {
    const char* id;
    const char* class_name;
    bool implemented;
    const char* note;
    int engine;  // 0=Alllive 1=Bili 2=DouDou 3=Doubao 4=LiveGz 5=Fy
};

// RegisterLiveSpiderEngines() 由 SpiderRegistry 构造函数调用；此时再调
// SpiderRegistry::Instance() 会重入函数局部静态初始化（MSVC 下死锁），
// 因此与 app/cms 引擎一样把真正的注册推迟到静态对象构造之后。
std::atomic<bool> g_live_pending{false};

void RegisterLiveSpiderMetas(SpiderRegistry& registry) {
    static const LiveMetaSpec specs[] = {
        {"csp_Alllive", "Alllive", true,
         "已跑通：网关 {base}/api/{siteId}/...，响应 LZString；虎牙用 info.code "
         "拼 tx.flv.huya.com 签名直链可播；斗鱼/B站直链透传；抖音详情受网关"
         "解析限制",
         0},
        {"csp_Bili", "Bili", true,
         "已跑通：一套实现覆盖 7 个 csp_BiliGuard 站点（MV/合集/演唱会/少儿/"
         "小学/初中/高中）；匿名路径用内置 buvid3 cookie；播放 durl mp4 优先、"
         "DASH baseUrl 兜底",
         1},
        {"csp_DouDou", "DouDou", true,
         "已跑通：frodo 需 _sig（997），改用 m.douban.com/rexxar 匿名；仅片单/"
         "搜索，播放地址为豆瓣网页（需外部解析）",
         2},
        {"csp_Doubao", "Doubao", true,
         "已跑通：赛程/回放/信号接口匿名可用；/signals/{playId}/play 返回 m3u8，"
         "回放 /vodplay/*.m3u8 实测 200",
         3},
        {"csp_LiveGz", "LiveGz", true,
         "已跑通：parameter 未加密也取数；响应 Base64+AES-128-CBC"
         "（key=KANGEQIU@8868!~. iv=0200010900030207），line_list m3u8 实测 200",
         4},
        {"csp_Fy", "Fy", false,
         "逻辑是 34KB 内嵌 Python（drpy），含 OCR 验证码与 requests 会话缓存，"
         "无法用 recipe 描述；按 spec 明确降级",
         5},
    };

    for (const LiveMetaSpec& spec : specs) {
        SpiderMeta meta;
        meta.id = spec.id;
        meta.class_name = spec.class_name;
        meta.tmpl = SpiderTemplate::kLive;
        meta.implemented = spec.implemented;
        meta.note = spec.note;
        SpiderFactory factory;
        switch (spec.engine) {
            case 0:
                factory = [] { return std::make_unique<AllliveEngine>(); };
                break;
            case 1:
                factory = [] { return std::make_unique<BiliEngine>(); };
                break;
            case 2:
                factory = [] { return std::make_unique<DouDouEngine>(); };
                break;
            case 3:
                factory = [] { return std::make_unique<DoubaoEngine>(); };
                break;
            case 4:
                factory = [] { return std::make_unique<LiveGzEngine>(); };
                break;
            default:
                factory = [] { return std::make_unique<FyEngine>(); };
                break;
        }
        registry.Register(std::move(meta), std::move(factory));
    }
}

struct LiveEngineRegistrar {
    LiveEngineRegistrar() {
        SpiderRegistry& registry = SpiderRegistry::Instance();
        if (g_live_pending.exchange(false)) RegisterLiveSpiderMetas(registry);
    }
};

LiveEngineRegistrar g_live_registrar;

} // namespace

void RegisterLiveSpiderEngines() {
    g_live_pending.store(true);
}

} // namespace ttplayer::video
