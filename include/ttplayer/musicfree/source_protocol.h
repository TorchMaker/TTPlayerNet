#pragma once

// 统一音源协议层 —— 协议无关的公共类型
//
// TTPlayer 最初只兼容 MusicFree 插件协议（CommonJS 模块导出 search /
// getMediaSource / getLyric）。社区里另有三类常见形态：
//
//   1. LX Music / 洛雪「自定义音源」：事件式脚本，globalThis.lx 上注册
//      on('request')、send('inited')，宿主提供 lx.request 网络能力。
//      音源本身**不做搜索**，只负责「给歌曲信息 → 返回音频直链」。
//   2. Listen1 provider：静态类 + 回调式 API（search(url).success(cb) /
//      bootstrap_track(track, success, failure)），强依赖浏览器宿主对象。
//   3. REST 声明式音源：NeteaseCloudMusicApi / Subsonic(Navidrome) /
//      listen1-api 等自建服务，用一份 JSON 描述 URL 模板与字段映射即可接入。
//
// 本头文件只放「协议无关」的数据结构与嗅探函数；各协议的具体运行时在
// lx_source_adapter / rest_source_adapter 中，统一由 source_protocol_registry
// 管理。新增协议时只要实现同样的 Resolve/Search 语义并登记到注册表即可。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ttplayer::musicfree {

enum class SourceProtocol {
    kMusicFree,  // CommonJS 插件（已有实现）
    kLxMusic,    // 洛雪 / LX Music 自定义音源（事件式）
    kListen1,    // Listen1 provider（回调式，识别 + 桥接骨架）
    kRest,       // 声明式 REST 音源（含 Subsonic / NeteaseCloudMusicApi 预设）
    kUnknown,
};

const char* ProtocolId(SourceProtocol protocol);
const wchar_t* ProtocolLabel(SourceProtocol protocol);
const char* ProtocolDescription(SourceProtocol protocol);

// 一个已导入的「非 MusicFree」音源的展示信息。
struct ProtocolSourceInfo {
    SourceProtocol kind = SourceProtocol::kUnknown;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string homepage;
    std::string source_url;
    std::filesystem::path cache_path;
    std::vector<std::string> sources;  // 洛雪：kw/kg/tx/wy/mg/local；REST：preset 名
    bool can_search = false;
    bool can_resolve = false;
    bool can_lyric = false;
};

// 协议层统一的曲目描述。raw 保留插件搜索结果里的全部私有字段
// （咪咕的 contentId / copyrightId 之类），解析时原样回传给音源，
// 避免「裁剪成 8 个字段 → 用 id 重建对象 → 插件拿不到私有字段」的问题。
struct ProtocolTrack {
    std::string platform;
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork;
    std::int64_t duration_ms = 0;
    std::string quality = "128k";  // 洛雪音质键；REST 音源可忽略
    nlohmann::json raw;
};

struct ProtocolResolveResult {
    std::string url;
    std::map<std::string, std::string> headers;
    std::string user_agent;
    std::string via;    // 「洛雪: 音源名」之类，便于诊断到底谁给出的直链
    std::string error;
    bool ok() const { return !url.empty(); }
};

// 内容嗅探：导入时按脚本/配置特征判断协议，避免把洛雪脚本当 MusicFree
// CommonJS 插件挂载（那样 module.exports 为空，界面显示「挂载成功但不可用」）。
// hint 为文件名或 URL（可选），用于识别 *.lx.js / *.subsonic.json 等显式后缀。
SourceProtocol DetectSourceProtocol(std::string_view code,
                                    std::string_view hint = {});

// 解析洛雪脚本头部的 @name / @description / @version / @author / @homepage
// 注释块，返回 JSON 对象（缺失字段为空串）。
nlohmann::json ParseScriptHeader(std::string_view code);

// 把 MusicFree 音源名/搜索结果 platform 映射到洛雪的 source key。
// 返回空串表示无法映射（调用方可退回音源声明的第一个可用 source）。
std::string MapToLxSource(std::string_view platform);

} // namespace ttplayer::musicfree
