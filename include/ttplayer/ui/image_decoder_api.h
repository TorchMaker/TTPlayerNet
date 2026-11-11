#pragma once

#include <windows.h>   // HBITMAP / wchar_t 等契约类型；第三方插件可单独包含本头文件

// 图片解码 DLL 插件接口（C ABI，稳定契约）
//
// 与视频协议插件（<exe>\AddIn\ttp_v_*.dll）、音频格式插件
// （ttpGetSoundAddIn）**同一套约定**：
//   · 位置   —— <exe>\AddIn\
//   · 命名   —— ttp_i_<格式>.dll（ttp_ 前缀家族；i = image）
//   · 工厂   —— 唯一导出 ttpGetImageDecoder（WINAPI），与音频插件的
//               ttpGetSoundAddIn、视频插件的 ttpGetVideoProtocol 同构：
//               调用方 GetProcAddress 拿到后调用一次，由 DLL 填写
//               ImageDecoderApi 函数表。
//
// 为什么需要它：现代影视 CMS 的封面大量使用 WebP（甚至 AVIF），而库存
// Windows 的 WIC / GDI+ 解不动这两种格式，导致封面永远停在彩色骨架。
// 把这些「WIC/GDI+ 解不动的格式」各自做成独立 DLL，主程序在 WIC/GDI+
// 失败后按字节 magic 选一个能解的 DLL 兜底，零改动主程序即可扩展。
//
// 契约要点：
//   · 纯 C ABI、零 CRT/STL 跨界导出 —— 任何 C/C++ 编译器都能实现插件；
//   · 所有函数可从任意线程调用；
//   · decode 返回的 HBITMAP（32 位 BGRA 自顶向下 DIB）由调用方 DeleteObject
//     释放（GDI 对象可跨模块释放，无需 DLL 自己提供释放函数）；
//   · probe 只看 magic 字节，禁止联网 —— 注册表在 UI 线程调用。
//
// 主程序侧加载器：ui/image_decoder_registry.{h,cpp}

#define IMG_DECODER_API_VERSION 1

// 解码器描述（主程序列出来源用）。
typedef struct ImageDecoderInfo {
    const wchar_t* name;         // 格式名，如 L"WebP"
    const wchar_t* vendor;       // 作者/版本备注，可空
    int            api_version;  // 必须返回 IMG_DECODER_API_VERSION
} ImageDecoderInfo;

// 函数表：由 ttpGetImageDecoder 工厂填写。
typedef struct ImageDecoderApi {
    int (*api_version)(void);
    void (*info)(ImageDecoderInfo* out);
    // 这个 DLL 能解这种字节流吗？返回 0-100 置信度（只看 magic，禁止联网）。
    int (*probe)(const unsigned char* bytes, int size);
    // 解码成 32 位 BGRA 自顶向下 DIB（HBITMAP）。成功返回非空句柄，
    // 调用方负责 DeleteObject；失败返回 NULL。max_edge>0 时把最长边缩放到
    // max_edge 以内（保持比例），封面缩略图以此避免模糊/过大。
    HBITMAP (*decode)(const unsigned char* bytes, int size, int max_edge);
} ImageDecoderApi;

// ── 每个图片解码 DLL 的唯一导出（对齐音频/视频插件的工厂模式）──
//
//   HRESULT WINAPI ttpGetImageDecoder(ImageDecoderApi* out);
//
// 成功：填 *out 并返回 S_OK；失败返回 E_FAIL（该 DLL 会被跳过）。
// 无需 .def 文件：__declspec(dllexport) + WINAPI 即可被 GetProcAddress 找到；
// x86 下会被修饰成 _ttpGetImageDecoder@4，主程序同时兼容纯名与修饰名。
