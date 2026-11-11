// 图片解码插件：WebP（含 lossy VP8 / lossless VP8L / 透明 / 动画首帧）
// —— 编译为 <exe>\AddIn\ttp_i_webp.dll
//
// 与视频协议插件（ttp_v_*.dll）、音频格式插件（ttpGetSoundAddIn）同约定：
//   · 位置   —— <exe>\AddIn\
//   · 命名   —— ttp_i_<格式>.dll（i = image）
//   · 工厂   —— 唯一导出 ttpGetImageDecoder（WINAPI），调用方 GetProcAddress
//               拿到后调用一次，由本 DLL 填写 ImageDecoderApi 函数表。
//
// 为什么需要它：现代影视 CMS 的封面大量是 WebP，而库存 Windows 的 WIC / GDI+
// 解不动这两种格式（封面永远停在彩色骨架）。本插件用 libwebp（BSD，静态编入
// 本 DLL）做真·解码 —— 零外部运行时、不依赖系统 WebP 解码器，跨 Win10/11 与
// 精简版系统都能解出封面。
//
// 工厂契约见 include/ttplayer/ui/image_decoder_api.h。

#include "ttplayer/ui/image_decoder_api.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "webp/decode.h"   // libwebp（third_party/libwebp-1.4.0/src/webp）

namespace {

// 把 WebP 字节流解码为 32 位 BGRA 自顶向下 DIB（HBITMAP）。
// 成功返回非空句柄，调用方负责 DeleteObject；失败返回 NULL。
HBITMAP DecodeToDib(const unsigned char* bytes, int size, int max_edge) {
    int w = 0, h = 0;
    if (!WebPGetInfo(bytes, static_cast<size_t>(size), &w, &h) || w <= 0 || h <= 0)
        return nullptr;

    uint8_t* rgba = WebPDecodeBGRA(bytes, static_cast<size_t>(size), &w, &h);
    if (!rgba) return nullptr;

    int dw = w, dh = h;
    const int longest = (w > h) ? w : h;
    if (max_edge > 0 && longest > max_edge) {
        dw = (w * max_edge) / longest;
        dh = (h * max_edge) / longest;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = dw;
    bi.biHeight = -dh;          // 负高 = 自顶向下
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = static_cast<DWORD>(static_cast<size_t>(dw) * dh * 4U);

    void* pixels = nullptr;
    const HBITMAP bmp = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi),
                                         DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bmp || !pixels) {
        WebPFree(rgba);
        return nullptr;
    }

    const int src_stride = w * 4;
    const int dst_stride = dw * 4;
    const uint8_t* src = rgba;
    uint8_t* dst = static_cast<uint8_t*>(pixels);
    for (int y = 0; y < dh; ++y) {
        const int sy = (y * h) / dh;                 // 最近邻缩放（封面缩略图够用）
        const uint8_t* srow = src + static_cast<size_t>(sy) * src_stride;
        uint8_t* drow = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < dw; ++x) {
            const int sx = (x * w) / dw;
            const uint8_t* s = srow + static_cast<size_t>(sx) * 4;
            uint8_t* d = drow + static_cast<size_t>(x) * 4;
            d[0] = s[0];  // B
            d[1] = s[1];  // G
            d[2] = s[2];  // R
            d[3] = 255;   // A：封面按不透明处理，避免 AlphaBlend 误半透明
        }
    }

    WebPFree(rgba);
    return bmp;
}

static int ApiVersion() { return IMG_DECODER_API_VERSION; }

static void ApiInfo(ImageDecoderInfo* out) {
    if (!out) return;
    out->name = L"WebP";
    out->vendor = L"libwebp 1.4.0 (statically linked)";
    out->api_version = IMG_DECODER_API_VERSION;
}

// 只看 magic 字节，禁止联网。RIFF/WEBP 容器 → 认领。
static int ApiProbe(const unsigned char* bytes, int size) {
    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 &&
        std::memcmp(bytes + 8, "WEBP", 4) == 0)
        return 100;
    return 0;
}

static HBITMAP ApiDecode(const unsigned char* bytes, int size, int max_edge) {
    if (!bytes || size <= 0) return nullptr;
    return DecodeToDib(bytes, size, max_edge);
}

} // namespace

extern "C" __declspec(dllexport)
HRESULT WINAPI ttpGetImageDecoder(ImageDecoderApi* out) {
    if (!out) return E_POINTER;
    out->api_version = ApiVersion;
    out->info = ApiInfo;
    out->probe = ApiProbe;
    out->decode = ApiDecode;
    return S_OK;
}

// x86 下 WINAPI(stdcall) 被修饰成 _ttpGetImageDecoder@4，主程序注册表同时兼容
// 纯名与修饰名（见 image_decoder_registry.cpp）。
#pragma comment(linker, "/EXPORT:ttpGetImageDecoder=_ttpGetImageDecoder@4")
