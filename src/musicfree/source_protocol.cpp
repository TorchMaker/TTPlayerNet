// 统一音源协议层 —— 协议识别与通用工具实现
//
// 判定顺序很关键：洛雪脚本常同时出现 require / Promise / module 等字样，
// 因此「事件式特征」必须先于「CommonJS 特征」判断；REST 则先看是不是 JSON。

#include "ttplayer/musicfree/source_protocol.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace ttplayer::musicfree {
namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool Contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

} // namespace

const char* ProtocolId(SourceProtocol protocol) {
    switch (protocol) {
        case SourceProtocol::kMusicFree: return "musicfree";
        case SourceProtocol::kLxMusic:   return "lx-music";
        case SourceProtocol::kListen1:   return "listen1";
        case SourceProtocol::kRest:      return "rest";
        case SourceProtocol::kUnknown:   return "unknown";
    }
    return "unknown";
}

const wchar_t* ProtocolLabel(SourceProtocol protocol) {
    switch (protocol) {
        case SourceProtocol::kMusicFree: return L"MusicFree 插件";
        case SourceProtocol::kLxMusic:   return L"洛雪 LX Music";
        case SourceProtocol::kListen1:   return L"Listen1 Provider";
        case SourceProtocol::kRest:      return L"REST 音源";
        case SourceProtocol::kUnknown:   return L"未识别";
    }
    return L"未识别";
}

const char* ProtocolDescription(SourceProtocol protocol) {
    switch (protocol) {
        case SourceProtocol::kMusicFree:
            return "CommonJS 插件：module.exports = { platform, search, "
                   "getMediaSource, getLyric }，自带搜索与解析。";
        case SourceProtocol::kLxMusic:
            return "事件式脚本：globalThis.lx 的 on/send/request，"
                   "声明 inited.sources(kw/kg/tx/wy/mg/local)，只提供播放直链、"
                   "歌词与封面，不提供搜索（搜索仍由 MusicFree/REST 音源负责）。";
        case SourceProtocol::kListen1:
            return "回调式 provider：search(url).success(cb) / "
                   "bootstrap_track(track, success, failure)，依赖 get/post、"
                   "crypto-js 等浏览器宿主对象。当前仅做协议识别与元数据登记，"
                   "运行时桥接见 docs/SOURCE_PROTOCOLS.md。";
        case SourceProtocol::kRest:
            return "声明式 JSON：URL 模板 + 字段映射，内置 "
                   "NeteaseCloudMusicApi / Subsonic(Navidrome) / listen1-api 预设，"
                   "可直接对接自建服务。";
        case SourceProtocol::kUnknown:
            return "无法识别的音源内容。";
    }
    return {};
}

SourceProtocol DetectSourceProtocol(std::string_view code,
                                    std::string_view hint) {
    const std::string h = Lower(hint);
    if (Contains(h, ".lx.js") || Contains(h, "lx-music") ||
        Contains(h, "lxmusic")) {
        return SourceProtocol::kLxMusic;
    }
    if (Contains(h, ".subsonic.json") || Contains(h, ".rest.json")) {
        return SourceProtocol::kRest;
    }

    // JSON 配置：先看能否解析，再看是否具备 REST 音源特征。
    std::string text(code);
    const auto first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first != std::string::npos && text[first] == '{') {
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            const std::string declared =
                j.contains("protocol") && j["protocol"].is_string()
                    ? Lower(j["protocol"].get<std::string>())
                    : std::string{};
            if (declared == "rest" || declared == "subsonic" ||
                declared == "neteaseapi" || declared == "listen1api" ||
                (j.contains("baseUrl") &&
                 (j.contains("search") || j.contains("resolve")))) {
                return SourceProtocol::kRest;
            }
            // 音源注册表 JSON（MusicFree 的 {plugins:[...]}）仍由原管理器处理。
            if (j.contains("plugins") && j["plugins"].is_array()) {
                return SourceProtocol::kMusicFree;
            }
        }
    }

    // 洛雪：事件名 + globalThis.lx 是决定性特征。
    if (Contains(code, "EVENT_NAMES.inited") || Contains(code, "globalThis.lx") ||
        Contains(code, "window.lx") ||
        (Contains(code, "EVENT_NAMES") && Contains(code, "inited"))) {
        return SourceProtocol::kLxMusic;
    }

    // Listen1 provider：静态类 + 回调式方法名。
    if (Contains(code, "bootstrap_track") ||
        (Contains(code, "show_playlist") && Contains(code, "get_playlist")) ||
        Contains(code, "class MusicProvider")) {
        return SourceProtocol::kListen1;
    }

    // MusicFree：CommonJS 导出 + 至少一个约定方法。
    if ((Contains(code, "module.exports") || Contains(code, "exports.") ||
         Contains(code, "export default")) &&
        (Contains(code, "getMediaSource") || Contains(code, "getLyric") ||
         Contains(code, "search"))) {
        return SourceProtocol::kMusicFree;
    }

    // 洛雪脚本头部注释 + sources 声明，但没写 globalThis.lx 的退化情形。
    if (Contains(code, "@name") && Contains(code, "qualitys") &&
        Contains(code, "musicUrl")) {
        return SourceProtocol::kLxMusic;
    }
    return SourceProtocol::kUnknown;
}

nlohmann::json ParseScriptHeader(std::string_view code) {
    nlohmann::json out = nlohmann::json::object();
    for (const char* key : {"name", "description", "version", "author",
                            "homepage"}) {
        out[key] = "";
    }
    const std::size_t begin = code.find("/**");
    if (begin == std::string_view::npos) return out;
    const std::size_t end = code.find("*/", begin + 3);
    if (end == std::string_view::npos) return out;

    std::string_view block = code.substr(begin + 3, end - begin - 3);
    std::size_t pos = 0;
    while (pos < block.size()) {
        const std::size_t nl = block.find('\n', pos);
        std::string line = Trim(std::string(
            block.substr(pos, nl == std::string_view::npos ? nl : nl - pos)));
        pos = nl == std::string_view::npos ? block.size() : nl + 1;
        if (!line.empty() && line[0] == '*') line = Trim(line.substr(1));
        const std::size_t at = line.find('@');
        if (at == std::string::npos) continue;
        const std::size_t sp = line.find_first_of(" \t", at);
        const std::string key = Lower(std::string_view(line).substr(
            at + 1, sp == std::string::npos ? sp : sp - at - 1));
        const std::string value =
            sp == std::string::npos ? std::string{} : Trim(line.substr(sp + 1));
        if (out.contains(key)) out[key] = value;
    }
    return out;
}

std::string MapToLxSource(std::string_view platform) {
    const std::string p = Lower(platform);
    auto has = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (p.find(k) != std::string::npos) return true;
        return false;
    };
    // 顺序：先判长名再判两字母缩写，避免 "migu" 里的 "mg" 之类误命中。
    if (has({"kuwo", "\xe9\x85\xb7\xe6\x88\x91", "kw"})) return "kw";
    if (has({"kugou", "\xe9\x85\xb7\xe7\x8b\x97", "kg"})) return "kg";
    if (has({"qq", "\xe4\xbc\x81\xe9\xb9\x85", "tx", "\xe8\x85\xbe\xe8\xae\xaf"})) return "tx";
    if (has({"netease", "cloudmusic", "\xe7\xbd\x91\xe6\x98\x93", "wy"})) return "wy";
    if (has({"migu", "\xe5\x92\xaa\xe5\x92\x95", "mg"})) return "mg";
    return {};
}

} // namespace ttplayer::musicfree
