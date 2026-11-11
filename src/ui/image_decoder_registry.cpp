#include "ttplayer/ui/image_decoder_registry.h"

#include <filesystem>
#include <mutex>
#include <vector>

#include "ttplayer/ui/image_decoder_api.h"

namespace ttplayer::ui {
namespace {

// 与视频协议插件（ttp_v_）、音频格式插件（ttp_ 前缀 Sound AddIn）同族的
// 图片解码命名：<exe>\AddIn\ttp_i_<格式>.dll（i = image）。
bool IsImageDecoderName(const std::filesystem::path& path) {
    const std::wstring name = path.filename().wstring();
    return name.size() > 10 &&                              // ttp_i_x.dll 最短
           _wcsnicmp(name.c_str(), L"ttp_i_", 6) == 0 &&
           _wcsicmp(path.extension().c_str(), L".dll") == 0;
}

} // namespace

ImageDecoderRegistry& ImageDecoderRegistry::Instance() {
    static ImageDecoderRegistry inst;
    return inst;
}

ImageDecoderRegistry::~ImageDecoderRegistry() {
    for (auto& p : decoders_) {
        if (p->mod) FreeLibrary(p->mod);
    }
    decoders_.clear();
}

void ImageDecoderRegistry::LoadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return;
    loaded_ = true;

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir;
    {
        std::wstring path(exe);
        const size_t slash = path.find_last_of(L"\\/");
        dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
        dir += L"\\AddIn";   // 与视频协议插件、音频格式插件同一目录
    }

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\ttp_i_*.dll";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (!IsImageDecoderName(full)) continue;
        HMODULE mod = LoadLibraryW(full.c_str());
        if (!mod) continue;
        // 同视频插件：x86 下 WINAPI(stdcall) 被修饰成 _ttpGetImageDecoder@4，
        // 这里同时兼容纯名与修饰名。
        FARPROC proc = GetProcAddress(mod, "ttpGetImageDecoder");
        if (!proc) proc = GetProcAddress(mod, "_ttpGetImageDecoder@4");
        const auto factory =
            reinterpret_cast<HRESULT(WINAPI*)(ImageDecoderApi*)>(proc);
        if (!factory) {
            FreeLibrary(mod);
            continue;
        }
        auto p = std::make_unique<LoadedDecoder>();
        p->mod = mod;
        const HRESULT hr = factory(&p->api);
        if (FAILED(hr) || !p->api.api_version ||
            p->api.api_version() != IMG_DECODER_API_VERSION ||
            !p->api.info || !p->api.probe || !p->api.decode) {
            FreeLibrary(mod);
            continue;
        }
        ImageDecoderInfo ii{};
        p->api.info(&ii);
        p->name = ii.name ? ii.name : fd.cFileName;
        decoders_.push_back(std::move(p));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

HBITMAP ImageDecoderRegistry::Decode(std::span<const unsigned char> bytes,
                                     int max_edge) {
    if (bytes.empty()) return nullptr;
    LoadAll();
    std::lock_guard<std::mutex> lock(mutex_);

    // 按 probe 置信度选一个能解的 DLL（magic 嗅探，纯本地、不联网）。
    LoadedDecoder* best = nullptr;
    int best_score = 0;
    for (auto& p : decoders_) {
        const int score = p->api.probe(bytes.data(),
                                      static_cast<int>(bytes.size()));
        if (score > best_score) {
            best_score = score;
            best = p.get();
        }
    }
    if (!best || best_score <= 0) return nullptr;

    return best->api.decode(bytes.data(), static_cast<int>(bytes.size()),
                           max_edge);
}

} // namespace ttplayer::ui
