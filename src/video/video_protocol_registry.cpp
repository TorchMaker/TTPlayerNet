#include "ttplayer/video/video_protocol_registry.h"

#include <algorithm>
#include <filesystem>
#include <mutex>

#include "ttplayer/core/text.h"
#include "ttplayer/video/video_protocol_api.h"

namespace ttplayer::video {
namespace {

std::mutex g_proto_mutex;   // 保护 protocols_ 的装载与销毁

// 与 plugin_manager.cpp 的 IsSoundAddInName 同族的视频协议命名：
// <exe>\AddIn\ttp_v_<协议>.dll（ttp_ = 插件家族，v_ = video）。
bool IsVideoProtocolName(const std::filesystem::path& path) {
    const std::wstring name = path.filename().wstring();
    return name.size() > 10 &&                        // ttp_v_x.dll 最短
           _wcsnicmp(name.c_str(), L"ttp_v_", 6) == 0 &&
           _wcsicmp(path.extension().c_str(), L".dll") == 0;
}

} // namespace

struct LoadedProtocol {
    HMODULE       mod = nullptr;
    std::wstring  name;
    VPProtocolApi api{};
};

VideoProtocolRegistry& VideoProtocolRegistry::Instance() {
    static VideoProtocolRegistry inst;
    return inst;
}

VideoProtocolRegistry::~VideoProtocolRegistry() {
    for (LoadedProtocol* p : protocols_) {
        if (p->mod) FreeLibrary(p->mod);
        delete p;
    }
}

void VideoProtocolRegistry::LoadAll() {
    std::lock_guard<std::mutex> lock(g_proto_mutex);
    if (loaded_) return;
    loaded_ = true;

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir;
    {
        std::wstring path(exe);
        const size_t slash = path.find_last_of(L"\\/");
        dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
        // 与音乐格式插件（ttp_flac.dll 等 Sound AddIn）同一目录：AddIn。
        dir += L"\\AddIn";
    }

    // AddIn 目录加载伴生 dll 的 PATH 追加由 plugin_manager 统一做过
    // （进程级环境变量），这里不再重复。
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\ttp_v_*.dll";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (!IsVideoProtocolName(full)) continue;
        HMODULE mod = LoadLibraryW(full.c_str());
        if (!mod) continue;
        // 与音频插件的 ttpGetSoundAddIn 同构的唯一工厂导出。
        // ⚠ Win32/x86 下 WINAPI(stdcall) 的 C 导出会被链接器修饰成
        //   _ttpGetVideoProtocol@4，GetProcAddress("ttpGetVideoProtocol")
        //   会静默失败（19:01 版 HLS 插件就是这样从未被加载过的）。
        // 新插件用 /EXPORT 别名导出纯名，这里同时兼容旧/第三方的修饰名。
        FARPROC proc = GetProcAddress(mod, "ttpGetVideoProtocol");
        if (!proc) proc = GetProcAddress(mod, "_ttpGetVideoProtocol@4");
        const auto factory =
            reinterpret_cast<HRESULT(WINAPI*)(VPProtocolApi*)>(proc);
        if (!factory) {
            FreeLibrary(mod);
            continue;
        }
        auto* p = new LoadedProtocol();
        p->mod = mod;
        const HRESULT hr = factory(&p->api);
        if (FAILED(hr) || !p->api.api_version ||
            p->api.api_version() != VP_PROTOCOL_API_VERSION ||
            !p->api.info || !p->api.probe || !p->api.open || !p->api.poll ||
            !p->api.close) {
            FreeLibrary(mod);
            delete p;
            continue;
        }
        VPProtocolInfo pi{};
        p->api.info(&pi);
        p->name = pi.name ? pi.name : fd.cFileName;
        protocols_.push_back(p);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

size_t VideoProtocolRegistry::Count() const {
    return protocols_.size();
}

ProtocolSession* VideoProtocolRegistry::Open(const std::wstring& url,
                                             const std::wstring& user_agent,
                                             const std::wstring& referer,
                                             int timeout_ms,
                                             const std::wstring& options,
                                             std::wstring* out_media_url) {
    LoadAll();
    std::lock_guard<std::mutex> lock(g_proto_mutex);

    // 按 probe 置信度选路。probe 只看 URL 形状、禁止联网，UI 线程安全。
    LoadedProtocol* best = nullptr;
    int best_score = 0;
    for (LoadedProtocol* p : protocols_) {
        const int score = p->api.probe(url.c_str());
        if (score > best_score) {
            best_score = score;
            best = p;
        }
    }
    if (!best || best_score <= 0) return nullptr;

    VPHttpConfig cfg{};
    cfg.user_agent = user_agent.empty() ? nullptr : user_agent.c_str();
    cfg.referer = referer.empty() ? nullptr : referer.c_str();
    cfg.timeout_ms = timeout_ms;
    cfg.options = options.empty() ? nullptr : options.c_str();

    wchar_t media[MAX_PATH * 4]{};
    VPSession* handle =
        best->api.open(url.c_str(), &cfg, media, MAX_PATH * 4);
    if (!handle || !media[0]) {
        if (handle) best->api.close(handle);   // 没给出媒体地址视为失败
        return nullptr;
    }
    auto* s = new ProtocolSession();
    s->proto = best;
    s->handle = handle;
    if (out_media_url) *out_media_url = media;
    return s;
}

int VideoProtocolRegistry::Poll(ProtocolSession* s, std::wstring* out_err) {
    if (!s || !s->proto || !s->handle) return VP_POLL_ERROR;
    wchar_t err[512]{};
    const int state = s->proto->api.poll(reinterpret_cast<VPSession*>(s->handle),
                                         err, 512);
    if (state == VP_POLL_ERROR && out_err) *out_err = err;
    return state;
}

void VideoProtocolRegistry::Close(ProtocolSession*& s) {
    if (!s) return;
    if (s->proto && s->handle) {
        s->proto->api.close(reinterpret_cast<VPSession*>(s->handle));
    }
    delete s;
    s = nullptr;
}

} // namespace ttplayer::video
