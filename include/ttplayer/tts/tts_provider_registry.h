#pragma once

// TTS 引擎注册表：加载 <exe>\AddIn\ttp_t_*.dll
//（C ABI 见 ttplayer/tts/tts_provider_api.h）。

#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/tts/tts_provider_api.h"

namespace ttplayer::tts {

struct Voice {
    std::string id;
    std::string name;
    std::string language;
    int gender{};
};

struct TtsPluginInfo {
    std::string id;
    std::string name;
    std::string version;
    bool online{};
    std::wstring path;
};

class TtsProviderRegistry {
public:
    static TtsProviderRegistry& Instance();

    void LoadAll();

    std::vector<TtsPluginInfo> Providers() const;

    bool ListVoices(const std::string& provider_id, std::vector<Voice>* out,
                    std::string* error);

    // 合成到 out_wav_path（UTF-16）。text 为 UTF-16。
    bool Synthesize(const std::string& provider_id, const std::wstring& text,
                    const std::string& voice_id, int rate_percent,
                    const std::wstring& out_wav_path, std::string* error,
                    int timeout_ms = 60000);

private:
    TtsProviderRegistry() = default;
    ~TtsProviderRegistry();
    TtsProviderRegistry(const TtsProviderRegistry&) = delete;
    TtsProviderRegistry& operator=(const TtsProviderRegistry&) = delete;

    struct Plugin {
        HMODULE module{};
        TtsPluginInfo info;
        TtpTtsApi api{};
        bool usable{};
    };

    Plugin* FindLocked(const std::string& provider_id);

    std::vector<Plugin> plugins_;
    bool loaded_{};
    mutable std::mutex mutex_;
};

}  // namespace ttplayer::tts
