#include "ttplayer/video/video_resolve.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/video/video_player_js.h"

namespace ttplayer::video {

namespace {

// 媒体直链的扩展名白名单（与影视仓「能不能直接播」的判定一致）。
bool HasMediaExtension(const std::string& lower) {
    static const char* kExts[] = {".m3u8", ".mp4", ".flv", ".mkv", ".m4v",
                                   ".mov",  ".webm", ".ts", ".mp3", ".aac",
                                   ".m4a",  ".avi", ".wmv", ".mpeg", ".mpg"};
    for (const char* ext : kExts)
        if (lower.find(ext) != std::string::npos) return true;
    return false;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// 把根相对媒体地址（/xxx/index.m3u8?sign=…）按播放页 URL 补成绝对地址。
// 影视仓分享页常见写法：const url = "/20261004/.../index.m3u8?sign=…"，
// 这类地址没有 http(s) 前缀，ExtractMediaUrlFromText 会漏掉。
std::string JoinUrl(const std::string& base, const std::string& ref) {
    if (!ref.empty() && (ref.rfind("http://", 0) == 0 ||
                         ref.rfind("https://", 0) == 0)) return ref;
    if (ref.rfind("//", 0) == 0) {
        const size_t p = base.find("://");
        return p == std::string::npos ? "https:" + ref : base.substr(0, p) + ":" + ref;
    }
    const size_t scheme = base.find("://");
    if (!ref.empty() && ref[0] == '/') {
        const size_t slash = scheme == std::string::npos
                                 ? std::string::npos
                                 : base.find('/', scheme + 3);
        return slash == std::string::npos ? base + ref : base.substr(0, slash) + ref;
    }
    const size_t slash = base.rfind('/');
    return slash == std::string::npos ? ref : base.substr(0, slash + 1) + ref;
}

// 从播放页 JS 里挖出根相对媒体地址（只认带媒体扩展名的根相对路径）。
std::string ExtractRelativeMediaUrl(const std::string& text) {
    static const char* kExts[] = {".m3u8", ".mp4", ".flv", ".ts", ".mkv",
                                  ".m4v", ".mov", ".webm"};
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"' && text[i] != '\'') continue;
        const char quote = text[i];
        const size_t b = i + 1;
        const size_t e = text.find(quote, b);
        if (e == std::string::npos) break;
        i = e;
        if (e - b < 5 || e - b > 2048) continue;
        std::string s = text.substr(b, e - b);
        if (s.rfind("./", 0) == 0) s = s.substr(1);
        if (s.empty() || s[0] != '/') continue;   // 只要根相对路径
        const std::string low = ToLower(s);
        const size_t q = low.find('?');
        const std::string head = q == std::string::npos ? low : low.substr(0, q);
        bool media = false;
        for (const char* ext : kExts)
            if (head.find(ext) != std::string::npos) { media = true; break; }
        if (media) return s;
    }
    return {};
}

// URL 百分号解码（解析接口模板里常有 %u4e2d 之类）。
std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(
                std::stoi(in.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// 按 TVBox 约定拼出解析接口的请求地址。
//   · 模板里含 {url} / %s  → 原地替换
//   · 否则按 type 决定：
//       type 3（直链前缀）→ 直接返回 parse.url（不拼原地址）
//       type 1/2（JSON/HTML）→ 用 ?url= / &url= 追加原地址
std::string BuildParseRequest(const VideoParse& parse, const std::string& raw_url) {
    const std::string tmpl = parse.url;
    if (tmpl.find("{url}") != std::string::npos) {
        std::string out = tmpl;
        const size_t pos = out.find("{url}");
        out.replace(pos, 5, raw_url);
        return out;
    }
    if (tmpl.find("%s") != std::string::npos) {
        std::string out = tmpl;
        const size_t pos = out.find("%s");
        out.replace(pos, 2, raw_url);
        return out;
    }
    if (parse.type == 3) return tmpl;   // 直链：本身就是前缀/模板
    // JSON / HTML 接口：追加原地址。
    char sep = tmpl.find('?') == std::string::npos ? '?' : '&';
    return tmpl + sep + "url=" + raw_url;
}

// 从 JSON 文本里挖出媒体地址（兼容影视仓解析接口的几种常见返回）。
std::string ExtractUrlFromJson(const std::string& body) {
    std::string text = Trim(body);
    if (text.empty()) return {};
    // 1) 整体就是 JSON → 结构化取。
    if (text.front() == '{' || text.front() == '[') {
        try {
            nlohmann::json j = nlohmann::json::parse(text);
            std::vector<nlohmann::json> queue{j};
            while (!queue.empty()) {
                nlohmann::json cur = queue.back();
                queue.pop_back();
                if (cur.is_string()) {
                    const std::string s = cur.get<std::string>();
                    if (s.find("://") != std::string::npos &&
                        HasMediaExtension(ToLower(s)))
                        return s;
                    continue;
                }
                if (cur.is_array()) {
                    // queue 是后进先出：倒序入队，弹出时就恢复 JSON 里的原始
                    // 顺序（6huo 接口里 1080p 在前，旧写法会先取到 480p）。
                    for (auto rit = cur.rbegin(); rit != cur.rend(); ++rit)
                        queue.push_back(*rit);
                    continue;
                }
                if (!cur.is_object()) continue;
                for (auto it = cur.begin(); it != cur.end(); ++it) {
                    const std::string key = ToLower(it.key());
                    // 优先取这些字段名。
                    // movurl：苹果预告片 / 6huo 这类「下载数据」接口的字段，
                    // 返回的直接就是 .mov 直链（此前漏掉 → 整条线路被判失败）。
                    if (key == "url" || key == "playurl" || key == "m3u8" ||
                        key == "mp4" || key == "src" || key == "file" ||
                        key == "link" || key == "movurl" || key == "play_url") {
                        if (it.value().is_string()) {
                            const std::string s = it.value().get<std::string>();
                            if (s.find("://") != std::string::npos) return s;
                        }
                    }
                    queue.push_back(it.value());
                }
            }
        } catch (const std::exception&) {
            // 不是合法 JSON → 走正则兜底。
        }
    }
    // 2) 正则兜底。
    return ExtractMediaUrlFromText(body);
}

} // namespace

bool IsDirectMediaUrl(const std::string& url) {
    if (url.empty()) return false;
    return HasMediaExtension(ToLower(url));
}

bool IsCloudShareUrl(const std::string& url) {
    if (url.empty()) return false;
    static const char* const kHosts[] = {
        "quark.cn",      "pan.quark",   "pan.baidu.com", "alipan.com",
        "aliyundrive",   "115.com",     "115cdn.com",    "123pan.com",
        "123684.com",    "caiyun.139.com", "cloud.189.cn", "drive.uc.cn",
        "uc.h5.cn",      "weiyun.com",  "lanzou",        "pan.xunlei",
        "yunpan.com",    "kuaipan",     "mega.nz",       "feimapan",
        "quark",         "uc.cn",
    };
    std::string lower = ToLower(url);
    for (const char* host : kHosts)
        if (lower.find(host) != std::string::npos) return true;
    return false;
}

std::string ParseInlinePlayHeaders(const std::string& url,
                                   std::map<std::string, std::string>& headers) {
    std::string out = Trim(url);
    if (out.empty()) return out;

    auto put = [&headers](const std::string& key, std::string value) {
        value = Trim(value);
        if (key.empty() || value.empty()) return;
        for (auto it = headers.begin(); it != headers.end();) {
            if (ToLower(it->first) == ToLower(key)) it = headers.erase(it);
            else ++it;
        }
        headers[key] = value;
    };
    auto collect_json = [&](const nlohmann::json& j) {
        if (!j.is_object()) return false;
        bool any = false;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_string()) continue;
            put(it.key(), it.value().get<std::string>());
            any = true;
        }
        return any;
    };

    // ① 末尾内联 JSON 对象： .m3u8${"Referer":"..."} / .m3u8&{"Referer":"..."}
    //    只认最后一个 '}' 收尾的片段，避免误伤 URL query 里的 '{'。
    if (!out.empty() && out.back() == '}') {
        const size_t open = out.rfind('{');
        if (open != std::string::npos && open > 0) {
            try {
                const nlohmann::json j = nlohmann::json::parse(out.substr(open));
                if (collect_json(j)) {
                    std::string head = Trim(out.substr(0, open));
                    // 去掉拼接符 $ / & / #
                    while (!head.empty() &&
                           (head.back() == '$' || head.back() == '&' ||
                            head.back() == '#' || head.back() == ' '))
                        head.pop_back();
                    if (!head.empty()) out = head;
                    return out;
                }
            } catch (...) {}
        }
    }

    // ② @Key=Value / @Key$Value 后缀（catvod 的 @Referer= / @User-Agent=）。
    //    只切「已知请求头键」，避免误伤 http://user:pass@host 里的 @。
    static const char* const kKeys[] = {"Referer", "User-Agent", "Cookie",
                                        "Origin", "Authorization"};
    const size_t first_at = out.find('@');
    if (first_at != std::string::npos && first_at > 0) {
        std::string clean = out.substr(0, first_at);
        bool any = false;
        const std::vector<std::string> segs =
            [&]() {
                std::vector<std::string> v;
                size_t p = first_at;
                while (p < out.size()) {
                    const size_t next = out.find('@', p + 1);
                    const size_t e = next == std::string::npos ? out.size() : next;
                    v.push_back(out.substr(p + 1, e - p - 1));
                    if (next == std::string::npos) break;
                    p = next;
                }
                return v;
            }();
        for (const std::string& seg : segs) {
            bool matched = false;
            for (const char* key : kKeys) {
                for (const char sep : {'=', '$'}) {
                    const std::string prefix = std::string(key) + sep;
                    if (seg.size() <= prefix.size()) continue;
                    if (ToLower(seg.substr(0, prefix.size())) != ToLower(prefix))
                        continue;
                    put(key, seg.substr(prefix.size()));
                    matched = true;
                    any = true;
                    break;
                }
                if (matched) break;
            }
        }
        if (any) {
            if (!clean.empty() &&
                (clean.back() == '$' || clean.back() == '&')) clean.pop_back();
            if (!clean.empty()) return Trim(clean);
        }
    }
    return out;
}

std::string ExtractMediaUrlFromText(const std::string& text) {
    // 影视仓播放页里媒体地址常见形态：
    //   "url":"https://.../xxx.m3u8"      /  var player_aaaa = {...}
    //   <video src="https://.../xxx.mp4">  /  window.playerConfig = {...}
    // 逐条正则按优先级扫（m3u8 优先于 mp4）。
    static const std::regex kPatterns[] = {
        std::regex(R"((https?:)?//[^"'\s<>\\]+?\.m3u8[^"'\s<>\\]*)", std::regex::icase),
        std::regex(R"((https?:)?//[^"'\s<>\\]+?\.mp4[^"'\s<>\\]*)", std::regex::icase),
        // .mov：苹果预告片等；此前不在正则里，导致页面里的 mov 直链被漏掉。
        std::regex(R"((https?:)?//[^"'\s<>\\]+?\.mov[^"'\s<>\\]*)", std::regex::icase),
        std::regex(R"((https?:)?//[^"'\s<>\\]+?\.flv[^"'\s<>\\]*)", std::regex::icase),
        std::regex(R"((https?:)?//[^"'\s<>\\]+?\.ts[^"'\s<>\\]*)", std::regex::icase),
    };
    for (const auto& re : kPatterns) {
        std::smatch m;
        if (std::regex_search(text, m, re)) {
            std::string s = m[1].str();
            if (!s.empty() && s.rfind("//", 0) == 0) s = "https:" + s;
            s = UrlDecode(s);
            if (!s.empty()) return s;
        }
    }
    return {};
}

bool LooksLikeJsonApiUrl(const std::string& url) {
    if (url.empty()) return false;
    const std::string lower = ToLower(url);
    // 影视仓系的「数据接口」特征（返回 JSON 而不是 HTML 播放页）。
    static const char* const kMarks[] = {
        "view=api",        "mode=download-data", "ac=detail",
        "ac=videolist",    "format=json",        "type=json",
        "/api.php/provide/vod",
    };
    for (const char* mark : kMarks)
        if (lower.find(mark) != std::string::npos) return true;
    // 路径本身以 .json 结尾（去掉查询串后判断）。
    const std::size_t q = lower.find('?');
    const std::string path = q == std::string::npos ? lower : lower.substr(0, q);
    return path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0;
}

std::string ResolveJsonApiMedia(const std::string& url, const NetworkConfig& cfg,
                                const std::map<std::string, std::string>& headers) {
    if (url.empty()) return {};
    try {
        const std::string body = musicfree::DownloadText(url, cfg, headers);
        std::string media = ExtractUrlFromJson(body);
        if (media.empty()) media = ExtractMediaUrlFromText(body);
        return media == url ? std::string{} : media;
    } catch (const std::exception&) {
        return {};
    } catch (...) {
        return {};
    }
}

VideoResolveResult ResolveVideoUrl(
    const std::string& raw_url,
    const std::vector<VideoParse>& parses,
    const NetworkConfig& cfg,
    const std::map<std::string, std::string>& base_headers,
    const std::string& flag,
    const std::string& player_url,
    const std::string& base_url) {
    VideoResolveResult result;
    // catvod 的 playerContent 常把请求头内联在播放串尾部
    // （.m3u8${"Referer":"..."} / @Referer=...）。不剥掉就会把整串当 URL 请求。
    std::map<std::string, std::string> headers = base_headers;
    std::string trimmed = ParseInlinePlayHeaders(Trim(raw_url), headers);
    result.headers = headers;
    if (trimmed.empty()) {
        result.detail = "空地址";
        return result;
    }

    // TVBox 的 flag 决定「播放串要不要先过播放器」。自定义 flag 即使地址看起来
    // 是直链（加密串常见 .m3u8 尾巴），也必须交给 player 解一次；普通 flag
    // （空 / m3u8 / direct…）直接播，零额外开销。
    const bool custom_flag = !flag.empty() && !IsBuiltinPlayerFlag(flag);

    // ---- ①.0 按 flag 指定的解析器（parses 里同名项优先）----
    if (custom_flag) {
        for (const auto& parse : parses) {
            if (parse.url.empty() || parse.type == 0) continue;
            if (parse.name != flag) continue;
            const std::string request = BuildParseRequest(parse, trimmed);
            try {
                const std::string body = musicfree::DownloadText(request, cfg, headers);
                std::string media = parse.type == 1 ? ExtractUrlFromJson(body)
                                                    : ExtractMediaUrlFromText(body);
                if (media.empty()) media = ExtractUrlFromJson(body);
                if (!media.empty() && media != trimmed) {
                    result.url = media;
                    result.method = "flagparse";
                    result.detail = parse.name;
                    return result;
                }
            } catch (const std::exception& e) {
                result.detail += std::string("flag解析[") + parse.name + "]失败:" +
                                e.what() + "; ";
            }
        }
    }

    // ---- ①.1 自定义播放器（playerUrl / 站点 player 的 JS 执行器）----
    if (custom_flag && !player_url.empty()) {
        const auto pr = RunVideoPlayerJs(player_url, base_url, flag, trimmed, cfg, headers);
        if (pr.ok()) {
            for (const auto& kv : pr.headers)
                result.headers[kv.first] = kv.second;
            result.detail = pr.detail;
            if (pr.parse == 0) {
                result.url = pr.url;
                result.method = "player";
                return result;
            }
            // player 返回 parse=1：它给的是中间地址，继续走后面 parses / 页面解析。
            trimmed = pr.url;
        } else {
            result.detail += " | player: " + pr.detail;
        }
    }

    // 已经是媒体直链 → 不必解析（内置 flag 的绝大多数线路在这里秒返回）。
    if (IsDirectMediaUrl(trimmed)) {
        result.url = trimmed;
        result.method = "direct";
        return result;
    }

    // ---- ① 依次尝试配置里的解析接口（影视仓 parses）----
    for (const auto& parse : parses) {
        if (parse.url.empty()) continue;
        // type 0 = 嗅探：需要 WebView 抓包，本程序没有，跳过。
        if (parse.type == 0) continue;
        const std::string request = BuildParseRequest(parse, trimmed);
        try {
            const std::string body = musicfree::DownloadText(request, cfg, headers);
            std::string media;
            if (parse.type == 1) {
                media = ExtractUrlFromJson(body);
            } else {
                media = ExtractMediaUrlFromText(body);
                if (media.empty()) media = ExtractUrlFromJson(body);
            }
            if (!media.empty() && media != trimmed) {
                result.url = media;
                result.method = "parse" + std::to_string(parse.type);
                result.detail = parse.name.empty() ? parse.url : parse.name;
                return result;
            }
        } catch (const std::exception& e) {
            result.detail += std::string("接口[") + parse.name + "]失败:" +
                            e.what() + "; ";
        }
    }

    // ---- ①.5 数据接口（返回 JSON 的选集地址）→ 直接解析 ----
    // 6huo 这类「预告片」源的选集地址是 ?view=api&mode=download-data，
    // 返回的是 {"movurl":"http://…/xxx.mov"} 而不是网页。按网页去爬会
    // 走完所有 parses 再全页面正则，十几秒后才失败。
    if (LooksLikeJsonApiUrl(trimmed)) {
        try {
            const std::string body = musicfree::DownloadText(trimmed, cfg, headers);
            std::string media = ExtractUrlFromJson(body);
            if (media.empty()) media = ExtractMediaUrlFromText(body);
            if (!media.empty() && media != trimmed) {
                result.url = media;
                result.method = "jsonapi";
                return result;
            }
        } catch (const std::exception& e) {
            result.detail += std::string("数据接口抓取失败:") + e.what() + "; ";
        }
    }

    // ---- ② 兜底爬虫：直接抓播放页 HTML 提取媒体直链 ----
    // 影视仓的 spider 对播放页做的就是这件事；这里用通用正则覆盖多数 CMS 源。
    try {
        const std::string page = musicfree::DownloadText(trimmed, cfg, headers);
        std::string media = ExtractMediaUrlFromText(page);
        // 分享页根相对媒体地址兜底（/20261004/.../index.m3u8?sign=…）。
        if (media.empty()) {
            std::string rel = ExtractRelativeMediaUrl(page);
            if (!rel.empty()) media = JoinUrl(trimmed, rel);
        }
        if (!media.empty()) {
            result.url = media;
            result.method = "page";
            return result;
        }
        result.detail += "播放页里没找到媒体直链";
    } catch (const std::exception& e) {
        result.detail += std::string("播放页抓取失败:") + e.what();
    }
    return result;
}

} // namespace ttplayer::video
