#pragma once

#include <windows.h>
#include <span>
#include <string>
#include <mutex>
#include <vector>
#include <memory>
#include "ttplayer/ui/image_decoder_api.h"  // ImageDecoderApi（成员 api 按值默认构造，需完整类型）

namespace ttplayer::ui {

// 图片解码插件注册表（懒加载单例）。发现 <exe>\AddIn\ttp_i_*.dll，
// 调用 ttpGetImageDecoder 工厂，按 probe 置信度选一个能解该字节流的 DLL 兜底。
//
// 仅在 WIC / GDI+ 都解不动时才需要它（WebP / AVIF 等现代格式），因此
// cover_image.cpp 的 DecodeCoverImage 只在 automatic 模式下、WIC 失败后调用。
class ImageDecoderRegistry {
public:
    static ImageDecoderRegistry& Instance();

    // 解码字节流为 32 位 BGRA 自顶向下 DIB。无任何插件能解时返回 NULL。
    // 返回的 HBITMAP 由调用方 DeleteObject 释放。线程安全。
    HBITMAP Decode(std::span<const unsigned char> bytes, int max_edge);

private:
    ImageDecoderRegistry() = default;
    ~ImageDecoderRegistry();
    ImageDecoderRegistry(const ImageDecoderRegistry&) = delete;
    ImageDecoderRegistry& operator=(const ImageDecoderRegistry&) = delete;

    void LoadAll();

    struct LoadedDecoder {
        HMODULE          mod = nullptr;
        std::wstring     name;
        ImageDecoderApi  api {};   // typedef struct 见 image_decoder_api.h；直接用 typedef 名，勿加 struct 关键字
    };

    std::mutex                          mutex_;
    bool                                loaded_ = false;
    std::vector<std::unique_ptr<LoadedDecoder>> decoders_;
};

} // namespace ttplayer::ui
