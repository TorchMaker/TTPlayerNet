#pragma once

// 直播源插件接口（C ABI，稳定契约）
//
// 与视频协议插件（ttpGetVideoProtocol）同一套约定：
//   · 位置   —— <exe>\AddIn\
//   · 命名   —— ttp_l_<格式>.dll（l = live，避免被音频 AddIn 识别）
//   · 工厂   —— 唯一导出 ttpGetLiveSource（WINAPI），宿主 GetProcAddress 后
//               调用一次，由 DLL 填写 TtpLiveSourceApi 函数表。
//
// 一个插件对应一种「源格式 / 上游项目」，互不依赖，可独立增删：
//   ttp_l_m3u.dll    —— 通用 M3U / TXT（iptv-org/iptv、Guovin/iptv-api、
//                        TVBox 导出的 m3u/txt 等）
//   ttp_l_tvbox.dll  —— TVBox / FongMi 配置里的 lives（JSON）
//   后续可加 ttp_l_xxx.dll，宿主不需要改代码。
//
// 契约要点：纯 C ABI、字符串 UTF-16、错误文本 UTF-8、可从任意线程调用，
// 网络请求超时由 timeout_ms 控制；不得在启动导入表引入 Win7 不可用依赖。

#define TTPLAYER_LIVE_SOURCE_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// 一条直播频道（字段由插件在回调期间提供，回调返回后即可失效）。
typedef struct TtpLiveChannel {
    unsigned size;          // sizeof(TtpLiveChannel)，向前兼容
    const wchar_t* name;    // 频道名
    const wchar_t* url;     // 播放地址（m3u8/flv/ts/rtmp…）
    const wchar_t* group;   // 分组名，可空
    const wchar_t* logo;    // 台标 URL，可空
} TtpLiveChannel;

typedef void (WINAPI* TtpLiveEmitFn)(void* ctx, const TtpLiveChannel* channel);

typedef struct TtpLiveSourceInfo {
    const char* id;         // 插件标识："m3u" / "tvbox"
    const char* name;       // 显示名
    const char* vendor;     // 版本 / 来源备注
} TtpLiveSourceInfo;

typedef struct TtpLiveSourceApi {
    int (*api_version)(void);                 // 必须返回 ABI 版本
    void (*info)(TtpLiveSourceInfo* out);
    // 该插件是否认识这个源（URL 或本地路径）。返回 0-100 置信度，只看形状、
    // 禁止联网；宿主在 UI 线程调用。
    int (*probe)(const wchar_t* location);
    // 解析 / 拉取源，逐条回调频道。
    // 返回 0 = 成功；非 0 = 失败，error 内为 UTF-8 原因（可空）。
    int (*enumerate)(const wchar_t* location, TtpLiveEmitFn emit, void* ctx,
                     char* error, unsigned error_size, int timeout_ms);
} TtpLiveSourceApi;

// 每个直播源 DLL 的唯一导出：
//   HRESULT WINAPI ttpGetLiveSource(TtpLiveSourceApi* out);
// 成功填 *out 并返回 S_OK；失败返回 E_FAIL（该 DLL 会被跳过）。

#ifdef __cplusplus
}
#endif
