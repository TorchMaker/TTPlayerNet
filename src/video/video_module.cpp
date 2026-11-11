// ttplayer_video 模块的插件入口实现（C ABI）。
//
// 把 C++ 公开 API 按名字登记进 query() 扩展点；新增内容类型时在此追加名字，
// ABI 不变。函数指针到 void* 的转换在 MSVC/x86 下有定义良好的表示。

#include "ttplayer/video/video_module_api.h"

#include <cstring>

#include "ttplayer/video/video_cms.h"
#include "ttplayer/video/video_player_window.h"
#include "ttplayer/video/video_resolve.h"
#include "ttplayer/video/video_source_dialog.h"
#include "ttplayer/video/video_source_manager.h"
#include "ttplayer/video/video_web_host.h"

namespace {

using namespace ttplayer::video;

void* QueryApi(const char* api) {
    if (api == nullptr) return nullptr;

    if (std::strcmp(api, "video.cms") == 0)
        return reinterpret_cast<void*>(&FetchHome);
    if (std::strcmp(api, "video.resolve") == 0)
        return reinterpret_cast<void*>(&ResolveVideoUrl);
    if (std::strcmp(api, "video.source_dialog") == 0)
        return reinterpret_cast<void*>(&ShowVideoSourceDialog);
    if (std::strcmp(api, "video.player_window") == 0)
        return reinterpret_cast<void*>(&ShowVideoPlayer);
    if (std::strcmp(api, "video.source_manager") == 0)
        return reinterpret_cast<void*>(
            static_cast<VideoSourceManager* (*)(std::filesystem::path)>(
                [](std::filesystem::path dir) {
                    return new VideoSourceManager(std::move(dir));
                }));
    if (std::strcmp(api, "video.web_host") == 0)
        return reinterpret_cast<void*>(&SetVideoWebHostFactory);

    return nullptr;  // 未知名字：调用方按能力协商（向前兼容的关键）。
}

} // namespace

extern "C" __declspec(dllexport) const TtpVideoModule* WINAPI ttpGetVideoModule(void) {
    static const TtpVideoModule module = {
        sizeof(TtpVideoModule),
        TTPLAYER_VIDEO_MODULE_ABI_VERSION,
        "ttplayer_video",
        "1.0",
        &QueryApi,
    };
    return &module;
}

// x86 的 __stdcall 导出名带装饰（_ttpGetVideoModule@0）；补一个朴素名别名，
// 宿主可直接 GetProcAddress("ttpGetVideoModule")，与 ttpGetVideoProtocol 的
// 使用习惯一致。
#if defined(_WIN32) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:ttpGetVideoModule=_ttpGetVideoModule@0")
#endif
