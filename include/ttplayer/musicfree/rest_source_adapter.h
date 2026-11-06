#pragma once

// 声明式 REST 音源适配器
//
// 除了插件脚本，社区里还有大量「自建 HTTP 服务」形态的音源：
// NeteaseCloudMusicApi、Subsonic / Navidrome、listen1-api 包装服务等。
// 它们不是 JS 插件，而是一组固定 URL + JSON 结构，用一份配置即可描述：
//
//   { "protocol": "rest", "preset": "neteaseapi" | "subsonic" | "custom",
//     "name": "我的音源", "baseUrl": "http://127.0.0.1:3000",
//     "auth": { "user": "", "password": "", "token": "" },
//     "headers": { "X-Token": "..." },
//     "search": { "path": "/search", "params": {"keywords": "{query}"},
//                 "result": "result.songs", "fields": {"id":"id", ...} },
//     "resolve": { "path": "/song/url", "params": {"id":"{id}"},
//                  "url": "data[0].url", "direct": false } }
//
// 模板变量：{query} {id} {page} {offset} {limit} {user} {password}
// {token} {salt} {client} {version} {format}，除数字外自动 URL 编码。
// JSON 路径支持点号与数组下标：result.songs / data[0].url。

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/musicfree/source_protocol.h"

namespace ttplayer::musicfree {

class RestSourceAdapter {
public:
    RestSourceAdapter();
    ~RestSourceAdapter();
    RestSourceAdapter(const RestSourceAdapter&) = delete;
    RestSourceAdapter& operator=(const RestSourceAdapter&) = delete;

    bool Mount(const nlohmann::json& config, const NetworkConfig& network,
               std::string_view source_url, std::string* error);

    const ProtocolSourceInfo& info() const;
    bool IsMounted() const;

    std::vector<ProtocolTrack> Search(const std::string& query, int page = 1);
    ProtocolResolveResult ResolveUrl(const ProtocolTrack& track);
    std::string Describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 返回内置预设的默认配置（用户配置中未写的字段从这里补齐）。
nlohmann::json RestPresetConfig(std::string_view preset);

} // namespace ttplayer::musicfree
