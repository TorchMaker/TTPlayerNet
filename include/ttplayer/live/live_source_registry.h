#pragma once

// 直播源插件注册表：加载 <exe>\AddIn\ttp_l_*.dll（C ABI 见
// ttplayer/live/live_source_api.h），提供按插件 id 或自动探测的频道枚举。

#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/live/live_source_api.h"

namespace ttplayer::live {

struct LiveChannel {
    std::wstring name;
    std::wstring url;
    std::wstring group;
    std::wstring logo;
};

struct LiveSourcePluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::wstring path;
};

class LiveSourceRegistry {
public:
    static LiveSourceRegistry& Instance();

    // 扫描并加载（幂等）。插件缺失/ABI 不符只记录日志。
    void LoadAll();

    std::vector<LiveSourcePluginInfo> Sources() const;

    // 用指定插件解析源（location 可以是 URL 或本地文件路径）。
    bool Enumerate(const std::string& plugin_id, const std::wstring& location,
                   std::vector<LiveChannel>* out, std::string* error,
                   int timeout_ms = 20000);

    // 自动挑一个 probe 置信度最高的插件。
    bool EnumerateAuto(const std::wstring& location,
                       std::vector<LiveChannel>* out, std::string* error,
                       std::string* used_plugin = nullptr,
                       int timeout_ms = 20000);

private:
    LiveSourceRegistry() = default;
    ~LiveSourceRegistry();
    LiveSourceRegistry(const LiveSourceRegistry&) = delete;
    LiveSourceRegistry& operator=(const LiveSourceRegistry&) = delete;

    struct Plugin {
        HMODULE module{};
        LiveSourcePluginInfo info;
        TtpLiveSourceApi api{};
        bool usable{};
    };

    Plugin* FindLocked(const std::string& plugin_id);
    const Plugin* FindLocked(const std::string& plugin_id) const;

    std::vector<Plugin> plugins_;
    bool loaded_{};
    mutable std::mutex mutex_;
};

}  // namespace ttplayer::live
