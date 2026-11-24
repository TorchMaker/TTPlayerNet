#include "ttplayer/tts/tts_provider_registry.h"

#include "../plugins/content_plugin_loader.h"

namespace ttplayer::tts {

namespace {

using ttplayer::plugins::AppendContentLog;
using ttplayer::plugins::FindContentPlugins;
using ttplayer::plugins::GetPluginFactory;

std::string Utf8(const std::wstring& value) {
    return ttplayer::core::WideToUtf8(value);
}

void WINAPI EmitVoice(void* ctx, const TtpTtsVoice* voice) {
    auto* out = static_cast<std::vector<Voice>*>(ctx);
    if (!ctx || !voice || !voice->id || !*voice->id) return;
    Voice value;
    value.id = voice->id;
    if (voice->name) value.name = voice->name;
    if (voice->language) value.language = voice->language;
    value.gender = voice->gender;
    out->push_back(std::move(value));
}

}  // namespace

TtsProviderRegistry& TtsProviderRegistry::Instance() {
    static TtsProviderRegistry instance;
    return instance;
}

TtsProviderRegistry::~TtsProviderRegistry() {
    for (auto& plugin : plugins_) {
        if (plugin.module) FreeLibrary(plugin.module);
    }
}

void TtsProviderRegistry::LoadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return;
    loaded_ = true;
    for (const auto& path : FindContentPlugins(L"ttp_t_")) {
        Plugin plugin;
        plugin.module = LoadLibraryW(path.c_str());
        if (!plugin.module) {
            AppendContentLog(L"tts", "LOAD fail " + Utf8(path.filename().wstring()) +
                                         " err=" +
                                         std::to_string(GetLastError()));
            continue;
        }
        using Factory = HRESULT(WINAPI*)(TtpTtsApi*);
        auto factory = reinterpret_cast<Factory>(
            GetPluginFactory(plugin.module, "ttpGetTtsProvider"));
        if (!factory) {
            AppendContentLog(L"tts", "LOAD no-factory " +
                                         Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
            continue;
        }
        TtpTtsApi api{};
        if (FAILED(factory(&api)) || !api.api_version ||
            api.api_version() != TTPLAYER_TTS_ABI_VERSION ||
            !api.synthesize) {
            AppendContentLog(L"tts", "LOAD abi-mismatch " +
                                         Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
            continue;
        }
        TtpTtsInfo info{};
        if (api.info) api.info(&info);
        plugin.info.id = info.id ? info.id : "";
        plugin.info.name = info.name ? info.name : plugin.info.id;
        plugin.info.version = info.vendor ? info.vendor : "";
        plugin.info.online = info.online != 0;
        plugin.info.path = path.wstring();
        plugin.api = api;
        plugin.usable = !plugin.info.id.empty();
        if (plugin.usable) {
            AppendContentLog(L"tts", "LOAD ok id=" + plugin.info.id + " name=" +
                                         plugin.info.name + " file=" +
                                         Utf8(path.filename().wstring()));
            plugins_.push_back(std::move(plugin));
        } else {
            AppendContentLog(L"tts", "LOAD no-id " +
                                         Utf8(path.filename().wstring()));
            FreeLibrary(plugin.module);
        }
    }
    AppendContentLog(L"tts", "LOAD total=" + std::to_string(plugins_.size()));
}

std::vector<TtsPluginInfo> TtsProviderRegistry::Providers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TtsPluginInfo> out;
    out.reserve(plugins_.size());
    for (const auto& plugin : plugins_) out.push_back(plugin.info);
    return out;
}

TtsProviderRegistry::Plugin* TtsProviderRegistry::FindLocked(
    const std::string& provider_id) {
    for (auto& plugin : plugins_) {
        if (plugin.usable && plugin.info.id == provider_id) return &plugin;
    }
    return nullptr;
}

bool TtsProviderRegistry::ListVoices(const std::string& provider_id,
                                     std::vector<Voice>* out,
                                     std::string* error) {
    if (!out) return false;
    out->clear();
    LoadAll();
    std::lock_guard<std::mutex> lock(mutex_);
    Plugin* plugin = FindLocked(provider_id);
    if (!plugin || !plugin->api.list_voices) {
        if (error) *error = "未安装朗读引擎: " + provider_id;
        return false;
    }
    char buffer[512]{};
    const int rc = plugin->api.list_voices(EmitVoice, out, buffer, sizeof(buffer));
    if (rc != 0) {
        if (error) *error = buffer[0] ? buffer : "枚举音色失败";
        AppendContentLog(L"tts", "VOICES fail id=" + provider_id + " err=" +
                                     (buffer[0] ? buffer : "unknown"));
        return false;
    }
    return true;
}

bool TtsProviderRegistry::Synthesize(const std::string& provider_id,
                                     const std::wstring& text,
                                     const std::string& voice_id,
                                     int rate_percent,
                                     const std::wstring& out_wav_path,
                                     std::string* error, int timeout_ms) {
    LoadAll();
    std::lock_guard<std::mutex> lock(mutex_);
    Plugin* plugin = FindLocked(provider_id);
    if (!plugin) {
        if (error) *error = "未安装朗读引擎: " + provider_id;
        AppendContentLog(L"tts", "SYNTH miss-plugin id=" + provider_id);
        return false;
    }
    const std::wstring wide_voice = ttplayer::core::Utf8ToWide(voice_id);
    char buffer[512]{};
    const int rc = plugin->api.synthesize(
        text.c_str(), wide_voice.empty() ? nullptr : wide_voice.c_str(),
        rate_percent, out_wav_path.c_str(), buffer, sizeof(buffer), timeout_ms);
    if (rc != 0) {
        if (error) *error = buffer[0] ? buffer : "语音合成失败";
        AppendContentLog(L"tts", "SYNTH fail id=" + provider_id + " err=" +
                                     (buffer[0] ? buffer : "unknown"));
        return false;
    }
    AppendContentLog(L"tts", "SYNTH ok id=" + provider_id + " chars=" +
                                 std::to_string(text.size()));
    return true;
}

}  // namespace ttplayer::tts
