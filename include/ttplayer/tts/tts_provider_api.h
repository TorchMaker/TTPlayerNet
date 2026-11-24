#pragma once

// TTS（朗读）引擎插件接口（C ABI，稳定契约）
//
//   · 位置   —— <exe>\AddIn\
//   · 命名   —— ttp_t_<引擎>.dll（t = tts）
//   · 工厂   —— 唯一导出 ttpGetTtsProvider（WINAPI），填 TtpTtsApi 表。
//
// 一个插件对应一种语音引擎，互不依赖、可独立增删：
//   ttp_t_sapi.dll —— Windows SAPI5（离线，Win7 自带，默认引擎）
//   ttp_t_edge.dll —— edge-tts 在线神经语音（可选，失败由宿主回退 SAPI）
//
// 合成结果统一写成 16-bit PCM WAV 文件，宿主用现有音频引擎播放。

#define TTPLAYER_TTS_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TtpTtsInfo {
    const char* id;        // "sapi" / "edge"
    const char* name;
    const char* vendor;
    int online;            // 1 = 需要联网
} TtpTtsInfo;

typedef struct TtpTtsVoice {
    unsigned size;
    const char* id;        // 引擎内音色标识
    const char* name;      // 显示名
    const char* language;  // BCP-47 / 引擎语言串
    int gender;            // 0 未知 1 男 2 女
} TtpTtsVoice;

typedef void (WINAPI* TtpTtsEmitVoiceFn)(void* ctx, const TtpTtsVoice* voice);

typedef struct TtpTtsApi {
    int (*api_version)(void);
    void (*info)(TtpTtsInfo* out);
    // 枚举音色；返回 0 成功。
    int (*list_voices)(TtpTtsEmitVoiceFn emit, void* ctx, char* error,
                       unsigned error_size);
    // 合成到 out_wav_path（UTF-16，16-bit PCM WAV）。
    // rate_percent：100 = 正常；返回 0 成功，error 为 UTF-8 原因。
    int (*synthesize)(const wchar_t* text, const wchar_t* voice_id,
                      int rate_percent, const wchar_t* out_wav_path,
                      char* error, unsigned error_size, int timeout_ms);
} TtpTtsApi;

//   HRESULT WINAPI ttpGetTtsProvider(TtpTtsApi* out);

#ifdef __cplusplus
}
#endif
