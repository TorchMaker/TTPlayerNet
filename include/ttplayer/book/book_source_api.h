#pragma once

// 听书 / 广播内容源插件接口（C ABI，稳定契约）
//
// 约定与 ttp_l_* / ttp_v_* 相同：
//   · 位置   —— <exe>\AddIn\
//   · 命名   —— ttp_b_<来源>.dll（b = book）
//   · 工厂   —— 唯一导出 ttpGetBookSource（WINAPI），填 TtpBookSourceApi 表。
//
// 一个插件对应一种内容来源，互不依赖、可独立增删：
//   ttp_b_local.dll  —— 本地 TXT / EPUB 书架（离线）
//   ttp_b_legado.dll —— legado「阅读」书源规则（搜索 / 目录 / 正文）
//   ttp_b_radio.dll  —— radio-browser 公开广播数据库（电台搜索/列表）
//   后续可加其它来源，宿主无需改动。
//
// 文本约定：书名/章节/正文一律 UTF-8（char*），地点/来源标识用 UTF-16。
// content 返回的缓冲区由插件分配，必须用 free_text 释放。

#define TTPLAYER_BOOK_SOURCE_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TtpBookItem {
    unsigned size;
    const char* id;        // 来源内唯一标识（URL / 文件路径 / 电台 UUID）
    const char* title;
    const char* author;
    const char* cover;
    const char* intro;
    const char* extra;     // 插件私有（详情地址、流地址等）
} TtpBookItem;

typedef void (WINAPI* TtpBookEmitItemFn)(void* ctx, const TtpBookItem* item);

typedef struct TtpBookChapter {
    unsigned size;
    const char* title;
    const char* url;       // 章节地址 / 音频流地址（插件私有）
    int index;             // 从 0 开始的章节序号
} TtpBookChapter;

typedef void (WINAPI* TtpBookEmitChapterFn)(void* ctx, const TtpBookChapter* chapter);

typedef struct TtpBookSourceInfo {
    const char* id;        // "local" / "legado" / "radio"
    const char* name;
    const char* vendor;
    int is_audio;          // 1 = 直接给音频流（广播），0 = 文本（需要 TTS）
} TtpBookSourceInfo;

typedef struct TtpBookSourceApi {
    int (*api_version)(void);
    void (*info)(TtpBookSourceInfo* out);
    // 该插件是否认识这个来源（路径 / URL / 书源文件）。0-100，禁止联网。
    int (*probe)(const wchar_t* location);
    // 搜索（电台/书源用）；本地插件可返回 0 条。
    int (*search)(const wchar_t* keyword, int page, TtpBookEmitItemFn emit,
                  void* ctx, char* error, unsigned error_size, int timeout_ms);
    // 目录 / 电台列表；book_url 为 TtpBookItem::id 或 extra。
    int (*toc)(const char* book_url, TtpBookEmitChapterFn emit, void* ctx,
               char* error, unsigned error_size, int timeout_ms);
    // 正文（文本源）；返回 malloc 的 UTF-8 字符串，失败返回 nullptr。
    // 音频源可用 toc 直接给流地址，这里返回 nullptr 并写 error。
    char* (*content)(const char* chapter_url, char* error,
                     unsigned error_size, int timeout_ms);
    void (*free_text)(char* text);
} TtpBookSourceApi;

//   HRESULT WINAPI ttpGetBookSource(TtpBookSourceApi* out);

#ifdef __cplusplus
}
#endif
