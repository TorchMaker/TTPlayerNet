#include "ttplayer/video/video_config.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "ttplayer/core/text.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/tvbox_crypto.h"
#include "ttplayer/video/tvbox_ext_crypto.h"

namespace ttplayer::video {

namespace {

// 内联 Base64 解码（URL-safe 无关，影视仓配置用标准 Base64）。
int DecodeBase64(std::string_view in, std::string& out) {
    static const int kPad = -1;
    // 标准 Base64 解码表，恰好 256 项（索引 0..255）。
    static const signed char kTable[256] = {
        // 0..15
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        // 16..31
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        // 32..47  (+ = 43 -> 62, / = 47 -> 63)
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        // 48..63  (0..9 = 52..61)
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        // 64..79  (A..O = 0..14)
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        // 80..95  (P..Z = 15..25)
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        // 96..111 (a..o = 26..40)
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        // 112..127 (p..z = 41..51)
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        // 128..255
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    out.clear();
    int buffer = 0, bits = 0;
    for (char c : in) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = static_cast<unsigned char>(c) < 256 ? kTable[static_cast<unsigned char>(c)] : -1;
        if (v < 0) {
            if (v == kPad) break;
            return -1;  // 非法字符
        }
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return 0;
}

// 去掉首尾空白。
std::string_view Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// 判断文本是否像影视仓配置（JSON 对象/数组）或直链播放列表。
bool LooksLikeJson(std::string_view s) {
    s = Trim(s);
    return !s.empty() && (s.front() == '{' || s.front() == '[');
}
bool LooksLikePlaylist(std::string_view s) {
    s = Trim(s);
    if (s.size() > 7 && (s.substr(0, 7) == "#EXTM3U" || s.substr(0, 7) == "#extm3u"))
        return true;
    // 多行 URL 也视为直链播放列表
    size_t nl = s.find('\n');
    if (nl != std::string_view::npos) {
        std::string_view first = Trim(s.substr(0, nl));
        return first.find("://") != std::string_view::npos;
    }
    return false;
}

// 影视仓「图片伪装配置」：找 8 位标记 + "**"，把标记之后整段当作 Base64。
//
// TVBox 原版正则是 "[A-Za-z0]{8}\\*\\*"（只认字母和数字 0，是他们的笔误），
// 这里放宽到 0-9 —— 是原规则的超集，真实标记不会漏。
//
// 与 TVBox 唯一的差别：它只取第一个匹配就去解码，这里会遍历所有候选，
// 只接受"解码后确实像配置（{ 或 [ 开头）"的那个 —— 图片压缩数据里偶尔
// 也会出现 "**"，只取第一个会误判成"解出来是垃圾"。
bool TryUnwrapMarkedConfig(std::string_view raw, std::string& out) {
    constexpr std::size_t kMarkLen = 8;
    for (std::size_t i = kMarkLen; i + 1 < raw.size(); ++i) {
        if (raw[i] != '*' || raw[i + 1] != '*') continue;
        // 回头校验标记的 8 位是字母或数字。
        bool ok = true;
        for (std::size_t k = 1; k <= kMarkLen && ok; ++k) {
            const unsigned char c = static_cast<unsigned char>(raw[i - k]);
            ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9');
        }
        if (!ok) continue;
        // 标记之后是纯 Base64：只在字母表内取字符，'=' 是结束符，遇到别的
        // 字节（图片残留）就停 —— 否则会把后面的二进制拼进来污染解码。
        std::string payload;
        payload.reserve(raw.size() - (i + 2));
        for (std::size_t p = i + 2; p < raw.size(); ++p) {
            const unsigned char c = static_cast<unsigned char>(raw[p]);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '+' || c == '/')
                payload.push_back(static_cast<char>(c));
            else if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
                continue;
            else
                break;   // '=' 填充或图片残留
        }
        if (payload.empty()) continue;
        std::string decoded;
        if (DecodeBase64(payload, decoded) != 0 || decoded.empty()) continue;
        if (!LooksLikeJson(decoded)) continue;   // 不是配置就换下一个候选
        out = std::move(decoded);
        return true;
    }
    return false;
}

// TVBox 配置普遍不是严格 JSON：UTF-8 BOM、整行 "//" 注释、字符串里嵌裸换行
// （"优\n酷" 这种直接写在引号里的换行很常见）、对象末尾多余逗号、以及尾部
// 追加的 HTML 广告或 AES 填充字节。nlohmann 的 ignore_comments 只解决第一类，
// 这里做一次状态机扫描，把它规整成严格 JSON。
//
// 关键设计：只有首个有效字符是 { 或 [ 时才进入清洗，否则原样返回 —— 裸 CMS
// 接口列表 / M3U 播放列表仍然交给后面的分支处理。括号配平到 0 立即截断，
// 既丢掉尾部 HTML，也丢掉 AES 解密后残留的 PKCS#7 / 随机填充字节。
std::string CleanConfigText(std::string_view in) {
    std::string_view s = in;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.remove_prefix(3);

    // 预扫：跳过空白与行注释，判断是不是 JSON 文档。
    {
        std::size_t i = 0;
        bool jsonish = false;
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                i += 2;
                while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i;
                continue;
            }
            jsonish = (c == '{' || c == '[');
            break;
        }
        if (!jsonish) return std::string(s);   // 播放列表 / 裸接口列表
    }

    std::string out;
    out.reserve(s.size());
    bool in_str = false, esc = false, line_comment = false, block_comment = false;
    int depth = 0;
    bool started = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        const unsigned char c = static_cast<unsigned char>(ch);
        if (line_comment) {
            if (ch == '\n' || ch == '\r') { line_comment = false; out.push_back(ch); }
            continue;
        }
        if (block_comment) {
            if (ch == '*' && i + 1 < s.size() && s[i + 1] == '/') {
                block_comment = false; ++i;
            }
            continue;
        }
        if (in_str) {
            if (esc) { out.push_back(ch); esc = false; continue; }
            if (ch == '\\') { out.push_back(ch); esc = true; continue; }
            if (ch == '"') { in_str = false; out.push_back(ch); continue; }
            if (c < 0x20) {
                switch (ch) {
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default: {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                }
                continue;
            }
            out.push_back(ch);
            continue;
        }
        if (ch == '"') { in_str = true; out.push_back(ch); continue; }
        if (ch == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            line_comment = true; ++i; continue;
        }
        if (ch == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            block_comment = true; ++i; continue;
        }
        if (ch == '{' || ch == '[') {
            ++depth; started = true; out.push_back(ch); continue;
        }
        if (ch == '}' || ch == ']') {
            std::size_t k = out.size();
            while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\t' ||
                             out[k - 1] == '\r' || out[k - 1] == '\n')) --k;
            if (k > 0 && out[k - 1] == ',') out.resize(k - 1);
            out.push_back(ch);
            --depth;
            if (depth <= 0 && started) return out;   // 丢弃尾部一切噪声
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

// 统一解码链：按内容特征（而不是按网址或源名称）判断该用哪种解密方式。
// 顺序不能随意调换 —— 图片伪装体里也可能出现类似 hex 的片段，必须先剥壳。
bool DecodeTvboxPayloadImpl(std::string_view raw, std::string& out,
                            std::string* kind) {
    if (raw.empty()) return false;
    const std::string_view t = Trim(raw);
    std::string stage;
    std::string hit;

    if (TryUnwrapMarkedConfig(raw, stage)) {
        hit = "image_marker_base64";
    } else if (tvbox_crypto::TryDecryptAesCbcSelf(t, stage)) {
        hit = "aes_cbc_self_2423";
    } else {
        // 旧的 "$#…#$" 文本壳：内部一般是 Base64（少数是 AES 密文，格式不同）
        if (t.size() > 4 && t.substr(0, 2) == "$#" &&
            t.substr(t.size() - 2) == "#$") {
            std::string inner(t.substr(2, t.size() - 4));
            std::string dec;
            if (DecodeBase64(inner, dec) == 0 && LooksLikeJson(dec)) {
                stage = std::move(dec);
                hit = "dollar_shell_base64";
            }
        }
        if (stage.empty() && t.size() > 64) {
            std::string dec;
            if (DecodeBase64(t, dec) == 0 && LooksLikeJson(dec)) {
                stage = std::move(dec);
                hit = "base64_plain";
            }
        }
        if (stage.empty()) stage.assign(t);
    }

    std::string cleaned = CleanConfigText(stage);
    if (!LooksLikeJson(cleaned)) return false;   // 裸列表 / 播放列表，交回原逻辑
    if (kind) *kind = hit.empty() ? "json_plain" : hit;
    out = std::move(cleaned);
    return true;
}

// ------------------------------------------------------------------- SPIDER ext
// 绝大多数 TVBox 站点的 api 是 csp_Xxx（Java 类名），程序跑不了；但其中一部分
// 把真实的标准 MacCMS 接口藏在 site.ext 里（明文 JSON / 对象 / RC4 密文）。
// 这里把 ext 还原成可扫描文本，找出 CMS 地址，由 VideoSite::FromJson 把站点
// 改写成 kCms —— 首页就能真正拉到内容，而不是显示「0/N 可播放」。
std::string ExtToPlainText(const nlohmann::json& ext) {
    if (ext.is_string()) {
        std::string s = ext.get<std::string>();
        std::string dec;
        // 用完整 TVBox 解密链（AES-CBC"2423"/图片伪装/Base64）优先解密 ext：
        // 大量 csp_* 站点的真实 CMS 接口就藏在加密 ext 里，只有先解密才能提取
        // 出 provide/vod 直链。原 tvbox_ext::TryDecryptExt 仅覆盖部分形态，会
        // 漏掉 "2423" 自解密密文（csp_App99/AppSx/Fy 这类与已可播 csp_T4 同构）。
        if (DecodeTvboxPayloadImpl(s, dec, nullptr) && !dec.empty()) return dec;
        if (tvbox_ext::TryDecryptExt(s, dec)) return dec;
        return s;
    }
    if (ext.is_object() || ext.is_array()) return ext.dump();
    return {};
}

bool LooksLikeCmsUrl(std::string_view u) {
    std::string low(u);
    for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("api.php/provide/vod") != std::string::npos ||
           low.find("provide/vod") != std::string::npos ||
           low.find("ac=list") != std::string::npos ||
           low.find("at=m3u8") != std::string::npos ||
           low.find("/api.php") != std::string::npos;
}

std::string FindCmsApiFromExtImpl(const nlohmann::json& ext) {
    const std::string text = ExtToPlainText(ext);
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t p = text.find("http", i);
        if (p == std::string::npos) break;
        if (text.compare(p, 7, "http://") != 0 &&
            text.compare(p, 8, "https://") != 0) {
            i = p + 4;
            continue;
        }
        std::size_t e = p;
        while (e < text.size()) {
            const char c = text[e];
            if (c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' ||
                c == ',' || c == ';' || c == ')' || c == '}' || c == ']' ||
                c == ' ' || c == '\r' || c == '\n' || c == '\t') break;
            ++e;
        }
        std::string url = text.substr(p, e - p);
        // 剥掉已有 query：BuildCmsUrl 会自行追加 ac/pg 等参数，
        // 保留 "?ac=list" 会产生重复参数。
        const std::size_t q = url.find('?');
        if (q != std::string::npos) url.resize(q);
        if (LooksLikeCmsUrl(url)) return url;
        i = (e > p) ? e : p + 4;
    }
    return {};
}

// 把解析出来的 site JSON 收进 VideoConfig。
void ParseSitesInto(const nlohmann::json& root, VideoConfig& cfg) {
    if (!root.contains("sites") || !root["sites"].is_array()) return;
    for (const auto& s : root["sites"]) {
        if (!s.is_object()) continue;
        VideoSite site = VideoSite::FromJson(s);
        cfg.sites.push_back(std::move(site));
    }
}
void ParseParsesInto(const nlohmann::json& root, VideoConfig& cfg) {
    if (!root.contains("parses") || !root["parses"].is_array()) return;
    for (const auto& p : root["parses"]) {
        if (!p.is_object()) continue;
        VideoParse vp;
        if (p.contains("name")) vp.name = p["name"].get<std::string>();
        if (p.contains("type")) vp.type = p["type"].get<int>();
        if (p.contains("url")) vp.url = p["url"].get<std::string>();
        if (p.contains("ext")) vp.ext = p["ext"].get<std::string>();
        cfg.parses.push_back(std::move(vp));
    }
}
void ParseLivesInto(const nlohmann::json& root, VideoConfig& cfg) {
    if (!root.contains("lives") || !root["lives"].is_array()) return;
    for (const auto& l : root["lives"]) {
        if (!l.is_object()) continue;
        VideoLive vl;
        if (l.contains("name")) vl.name = l["name"].get<std::string>();
        if (l.contains("url")) vl.url = l["url"].get<std::string>();
        if (l.contains("group")) vl.group = l["group"].get<std::string>();
        cfg.lives.push_back(std::move(vl));
    }
}

// 单仓 JSON 对象 → VideoConfig。
VideoConfig ParseSingleRepo(const nlohmann::json& root, std::string_view origin) {
    VideoConfig cfg;
    if (root.contains("spider") && root["spider"].is_string())
        cfg.spider = root["spider"].get<std::string>();
    if (root.contains("playerUrl") && root["playerUrl"].is_string())
        cfg.player_url = root["playerUrl"].get<std::string>();
    ParseSitesInto(root, cfg);
    ParseParsesInto(root, cfg);
    ParseLivesInto(root, cfg);
    (void)origin;
    return cfg;
}

// 递归解析一份已下载的文本：可能是多仓（含 urls）或单仓。
void ParseTextRecursive(std::string_view text, std::string_view origin,
                        const NetworkConfig& cfg, ParsedConfigs& out,
                        int depth) {
    std::string trimmed = std::string(Trim(text));
    // 常见包装：去掉首尾空白后若被反引号/引号包裹
    if (depth > 6) { out.error = "多仓层级过深，停止解析"; return; }

    // 统一解码链：图片伪装 / "2423" AES-CBC 自解密 / Base64 / JSONC 清洗。
    // 必须排在所有「不是 JSON 就当裸列表」的判断之前，否则加密源会被直接判死。
    // 返回 false 说明确实不是 JSON 文档（播放列表 / 裸接口列表），保留原文。
    {
        std::string decoded;
        std::string kind;
        if (DecodeTvboxPayloadImpl(trimmed, decoded, &kind))
            trimmed = std::move(decoded);
    }

    // 直链播放列表 → 作为一组直播源
    if (LooksLikePlaylist(trimmed) && !LooksLikeJson(trimmed)) {
        VideoConfig cfg2;
        VideoLive vl;
        vl.name = "直播直链";
        vl.url = std::string(origin);
        vl.group = "直播";
        cfg2.lives.push_back(std::move(vl));
        out.configs.push_back(std::move(cfg2));
        return;
    }

    // 裸 CMS 接口列表（非 JSON）：每行一条 http(s) 接口地址。教程里常见的
    // 「把接口地址粘进来」就是这种 —— 以前会报「配置不是合法 JSON」，现在
    // 逐行包成单仓站点。api 类型交给 VideoSite::FromJson 判（provide/vod /
    // .php / 裸 http 都是 CMS；m3u_vod:// 会判成直播）。
    if (!LooksLikeJson(trimmed)) {
        VideoConfig list_cfg;
        // ⚠ 别写成 istringstream lines(std::string(trimmed)); —— 那是「最令人头疼
        // 的解析」：编译器会把整句当成函数声明（参数 std::string trimmed），
        // std::getline 就再也找不到匹配的重载了。用花括号初始化绕开。
        std::istringstream lines{std::string(trimmed)};
        std::string line;
        int index = 0;
        while (std::getline(lines, line)) {
            const std::string item = std::string(Trim(line));
            if (item.empty() || item.front() == '#') continue;
            // 允许「名称$url」/「名称,url」写法（TVBox 直链习惯），取前半做接口
            std::string url = item;
            const std::size_t sep = url.find_first_of("$,|");
            std::string label;
            if (sep != std::string::npos) {
                label = url.substr(0, sep);
                url = url.substr(sep + 1);
            }
            if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
                continue;
            // 站点名：优先用户给的 label，否则取 host，别让列表行全叫「未命名」
            std::string host;
            const std::size_t scheme = url.find("://");
            const std::size_t host_begin = scheme + 3;
            const std::size_t host_end = url.find('/', host_begin);
            host = url.substr(host_begin,
                              host_end == std::string::npos
                                  ? std::string::npos
                                  : host_end - host_begin);
            nlohmann::json j{
                {"key", "cms" + std::to_string(index)},
                {"name", label.empty() ? host : label},
                {"api", url},
            };
            list_cfg.sites.push_back(VideoSite::FromJson(j));
            ++index;
        }
        if (!list_cfg.sites.empty()) {
            out.configs.push_back(std::move(list_cfg));
            return;
        }
    }

    nlohmann::json root;
    try {
        // ignore_comments = true：影视仓配置里经常用 "//" 整行注释掉某个站点，
        // TVBox 走的是 Gson 宽松模式可以直接跳过；标准 JSON 解析会在这里报错，
        // 结果就是"配置能下到但解析失败"。
        root = nlohmann::json::parse(trimmed, nullptr, /*allow_exceptions=*/true,
                                     /*ignore_comments=*/true);
    } catch (...) {
        out.error = "配置不是合法 JSON";
        return;
    }

    // 多仓：顶层 urls 数组
    if (root.is_object() && root.contains("urls") && root["urls"].is_array()) {
        for (const auto& u : root["urls"]) {
            // 多仓有两种写法：纯字符串 URL，或 {"name":"…","url":"…"}。
            // 后者在影视仓里更常见（要显示线路名），以前只认字符串会静默
            // 得到 0 个源，用户看到的是「导入成功但首页空白」。
            std::string sub;
            if (u.is_string()) sub = u.get<std::string>();
            else if (u.is_object() && u.contains("url") && u["url"].is_string())
                sub = u["url"].get<std::string>();
            if (sub.empty()) continue;
            ParsedConfigs sub_parsed = ParseVideoConfigFromUrl(sub, cfg);
            for (auto& c : sub_parsed.configs) out.configs.push_back(std::move(c));
            if (out.error.empty() && !sub_parsed.error.empty())
                out.error = sub_parsed.error;
        }
        return;
    }
    // 也可能是 JSON 数组（每个元素一份单仓或 site）
    if (root.is_array()) {
        for (const auto& elem : root) {
            if (elem.is_object()) {
                if (elem.contains("sites") || elem.contains("lives")) {
                    out.configs.push_back(ParseSingleRepo(elem, origin));
                } else if (elem.contains("api") || elem.contains("name")) {
                    VideoConfig c;
                    c.sites.push_back(VideoSite::FromJson(elem));
                    out.configs.push_back(std::move(c));
                }
            }
        }
        return;
    }
    // 单仓
    out.configs.push_back(ParseSingleRepo(root, origin));
}

} // namespace

std::string FindCmsApiFromExt(const nlohmann::json& ext) {
    return FindCmsApiFromExtImpl(ext);
}

std::string UnwrapDisguisedConfig(std::string_view raw, bool* hit) {
    std::string out;
    const bool ok = TryUnwrapMarkedConfig(raw, out);
    if (hit) *hit = ok;
    return ok ? out : std::string();
}

bool DecodeTvboxPayload(std::string_view raw, std::string& out,
                        std::string* kind) {
    return DecodeTvboxPayloadImpl(raw, out, kind);
}

std::map<std::string, std::string> ConfigRequestHeaders() {
    // 与 TVBox ApiConfig 的 userAgent / requestAccept 保持一致（头名小写，
    // WinHttpBlocking 内部按小写去重）。
    return {
        {"user-agent", "okhttp/3.15"},
        {"accept",
         "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
         "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9"},
    };
}

VideoSite VideoSite::FromJson(const nlohmann::json& j) {
    VideoSite site;
    auto str = [&](const char* k) -> std::string {
        if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
        return {};
    };
    site.key = str("key");
    site.name = str("name");
    site.api = str("api");
    site.play_url = str("playUrl");
    site.ext = str("ext");
    site.group = str("group");
    site.ua = str("ua");
    site.header = str("header");
    site.player = str("player");

    std::string a = site.api;
    a.erase(std::remove_if(a.begin(), a.end(),
                           [](unsigned char c) { return std::isspace(c); }), a.end());
    std::string lower;
    lower.reserve(a.size());
    for (char c : a) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower.empty()) {
        site.kind = VideoSourceKind::kUnknown;
        site.why_unsupported = "缺少 api 地址";
    } else if (lower.rfind("m3u_vod://", 0) == 0) {
        site.kind = VideoSourceKind::kLive;
    } else if (lower.find("csp_") != std::string::npos) {
        // csp_XxxGuard / csp_Xxx 是 TVBox Java 运行库里的类名。若该类已被复刻
        // 成 C++（原生 spider 注册表命中且 implemented），站点直接可播；否则
        // 保持原有的「需要 Java 运行库」提示，仅补上「原生未实现」。
        const SpiderMeta* meta = SpiderRegistry::Instance().Find(site.api);
        if (meta) {
            site.spider_id = meta->id;
            site.native_ready = meta->implemented;
        }
        if (meta && meta->implemented) {
            site.kind = VideoSourceKind::kNativeSpider;
            site.why_unsupported.clear();
        } else if (meta) {
            // 已复刻成 C++ 原生 spider，但当前因外部依赖（云盘凭据 / 本机代理 /
            // 解密密钥 / Python 规则等）暂不可用。给出真实原因，而不是笼统说
            // 「需要 Java 运行库」——它根本不需要 Java，只是依赖外部资源。
            site.kind = VideoSourceKind::kSpider;
            site.why_unsupported = "原生未实现（" + meta->id + "）：" +
                (meta->note.empty()
                     ? "需要外部依赖（凭据/密钥/代理），暂不可用"
                     : meta->note);
        } else {
            site.kind = VideoSourceKind::kSpider;
            site.why_unsupported =
                "需要 TVBox 的 Java 运行库（csp_* 是运行库里的类名，本程序无法执行）";
        }
    } else if (lower.find("drpy") != std::string::npos ||
               lower.find(".js") != std::string::npos ||
               lower.find("js://") != std::string::npos) {
        site.kind = VideoSourceKind::kSpider;
        site.why_unsupported = "需要 TVBox 的 JS/Java 运行库（csp_* 是运行库里的类名，本程序无法执行）";
    } else if (lower.find("provide/vod") != std::string::npos ||
               lower.find(".php") != std::string::npos ||
               lower.rfind("http://", 0) == 0 ||
               lower.rfind("https://", 0) == 0) {
        site.kind = VideoSourceKind::kCms;
    } else {
        site.kind = VideoSourceKind::kUnknown;
        site.why_unsupported = "无法识别的 api 类型";
    }

    // 原生 spider 的 ext 是 recipe / 内置实现的输入（对象形式也要保留成文本），
    // 不能像 CMS 那样只取字符串。
    if (site.kind == VideoSourceKind::kNativeSpider && site.ext.empty() &&
        j.contains("ext") && !j["ext"].is_null()) {
        site.ext = j["ext"].is_string() ? j["ext"].get<std::string>() : j["ext"].dump();
    }

    // csp_* / drpy 站点本程序执行不了 Java/JS 扩展，但很多站点的真实接口就
    // 写在 ext 里。解密 ext 并找出标准 MacCMS 地址后，直接改写成可播 CMS：
    // 这样「48 个站点 0 个可播」会变成至少把能还原的那几个点亮。
    // 注意：已命中原生实现的站点走 spider 引擎，不在这里改写（否则丢失类身份）。
    if (site.kind == VideoSourceKind::kSpider && j.contains("ext")) {
        std::string cms = FindCmsApiFromExtImpl(j["ext"]);
        if (!cms.empty()) {
            site.api = std::move(cms);
            // 必须清空 ext：BuildCmsUrl 会把 site.ext 原样拼进 query，
            // 加密密文或整段 JSON 会把请求 URL 彻底污染。
            site.ext.clear();
            site.kind = VideoSourceKind::kCms;
            site.why_unsupported.clear();
            site.auto_converted = true;
        }
    }
    return site;
}

std::string VideoSite::NormalizedApi() const {
    // 原生 spider 没有网络 URL，界面展示 spider_id（如 csp_T4）即可；
    // 返回 csp_* 类名会被误当成可请求地址。
    if (kind == VideoSourceKind::kNativeSpider) return spider_id;
    std::string a = api;
    // 去掉首尾空白
    size_t s = a.find_first_not_of(" \t\r\n");
    size_t e = a.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) return {};
    a = a.substr(s, e - s + 1);
    return a;
}

ParsedConfigs ParseVideoConfigFromUrl(std::string_view url, const NetworkConfig& cfg) {
    ParsedConfigs out;
    out.origin = std::string(Trim(url));
    try {
        // 图片伪装源返回的是二进制（内含 \0），必须用 DownloadBinary 保留全部字节；
        // 后续解密链和 Base64 提取都依赖原始字节序列，不能被文本转换截断。
        const auto body = musicfree::DownloadBinary(out.origin, cfg, ConfigRequestHeaders());
        const std::string text(body.begin(), body.end());
        ParseTextRecursive(text, out.origin, cfg, out, 0);
    } catch (const std::exception& e) {
        out.error = std::string("下载/解析失败: ") + e.what();
    }
    return out;
}

ParsedConfigs ParseVideoConfigFromFile(const std::filesystem::path& path) {
    ParsedConfigs out;
    {
        std::u8string u8 = path.u8string();
        out.origin.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { out.error = "无法打开本地文件"; return out; }
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    ParseTextRecursive(text, out.origin, NetworkConfig{}, out, 0);
    return out;
}

ParsedConfigs ParseVideoConfigText(std::string_view text, std::string_view origin,
                                   const NetworkConfig& cfg) {
    ParsedConfigs out;
    out.origin = std::string(origin);
    ParseTextRecursive(text, origin, cfg, out, 0);
    return out;
}

std::vector<ImportedVideoSource> FlattenConfigs(const ParsedConfigs& parsed) {
    std::vector<ImportedVideoSource> out;
    for (const auto& cfg : parsed.configs) {
        // 有站点的单仓
        if (!cfg.sites.empty()) {
            ImportedVideoSource src;
            // 配置名取第一个有效站点名或分组
            for (const auto& s : cfg.sites) {
                if (!s.name.empty()) { src.name = s.name; break; }
            }
            if (src.name.empty() && !cfg.sites.empty()) src.name = cfg.sites[0].name;
            src.sites = cfg.sites;
            src.lives = cfg.lives;
            // 解析接口必须一起搬过去：以前这里漏了，导致配置里写了 parses
            // 也没人用 —— 播放页地址永远拿不到真实媒体直链。
            src.parses = cfg.parses;
            // 订阅级 playerUrl 同样要搬过去：TVBox 的自定义播放器 JS 靠它定位。
            src.player_url = cfg.player_url;
            out.push_back(std::move(src));
        }
        // 纯直播单仓
        if (cfg.sites.empty() && !cfg.lives.empty()) {
            ImportedVideoSource src;
            src.name = cfg.lives[0].group.empty() ? "直播源" : cfg.lives[0].group;
            src.lives = cfg.lives;
            src.parses = cfg.parses;
            src.player_url = cfg.player_url;
            out.push_back(std::move(src));
        }
    }
    return out;
}

} // namespace ttplayer::video
