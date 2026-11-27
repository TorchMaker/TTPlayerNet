#pragma once

// 视频协议 DLL 插件接口（C ABI，稳定契约）
//
// 与音乐格式插件（<exe>\AddIn\ttp_flac.dll 等 Sound AddIn）**同一套约定**：
//   · 位置   —— <exe>\AddIn\
//   · 命名   —— ttp_v_<协议>.dll（ttp_ 前缀家族；v = video，避开音频
//               格式名如 ttp_vqf.dll；plugin_manager 的音频插件筛选
//               IsSoundAddInName 已排除 ttp_v_ 前缀）
//   · 工厂   —— 唯一导出 ttpGetVideoProtocol（WINAPI），与音频插件的
//               ttpGetSoundAddIn 同构：调用方 GetProcAddress 拿到后调用
//               一次，由 DLL 填写 VPProtocolApi 函数表。
//
// 协议 DLL 的职责：把一种协议的 URL 变成 Media Foundation 能直接打开的
// 播放地址（通常是本地代理 URL，例如 HLS → http://127.0.0.1:port/stream.ts）。
//
// 契约要点：
//   · 纯 C ABI、零 CRT/STL 跨界导出 —— 任何 C++ 编译器都能实现协议 DLL；
//   · VPOpen 必须快速返回（网络探测/预取在 DLL 内部线程做）；
//   · 所有函数可从任意线程调用，但同一 session 的生命周期由调用方保证
//     （VPClose 之后不得再碰该 session）；
//   · 字符串一律 UTF-16（wchar_t）。
//
// 主程序侧加载器：video_protocol_registry.{h,cpp}

#define VP_PROTOCOL_API_VERSION 1

// VPPoll 返回的状态
#define VP_POLL_CONNECTING 0   // 解析/预取进行中，还不能出画面
#define VP_POLL_READY      1   // 首批数据就绪（或流已建立），可以出画面
#define VP_POLL_ERROR      2   // 协议层失败（err 文本有效），应换线路/回退
#define VP_POLL_END        3   // 数据耗尽（点播播完）

// 会话句柄：由协议 DLL 内部定义（不透明）。
typedef struct VPSession VPSession;

// 打开会话时的网络配置（引用在 VPOpen 返回后即失效，DLL 需自拷贝）。
typedef struct VPHttpConfig {
    const wchar_t* user_agent;   // 可空 = 系统默认
    const wchar_t* referer;      // 可空
    int            timeout_ms;   // 单请求超时；<=0 视为 8000
    // 协议私有可选参数（UTF-16 JSON，可空）。网页解析协议用它接收
    // 影视仓配置里的 parses：{"parses":[{"name":"…","url":"…","type":1}]}。
    // ⚠ 必须放在结构体末尾：旧版本协议 DLL 只认识前三个字段，追加字段
    // 对它们仍然是二进制兼容的（ABI v1 保持不变）。
    const wchar_t* options;
} VPHttpConfig;

// 协议描述（主程序列出协议用）。
typedef struct VPProtocolInfo {
    const wchar_t* name;         // 协议名，如 L"HLS"
    const wchar_t* vendor;       // 作者/版本备注，可空
    int            api_version;  // 必须返回 VP_PROTOCOL_API_VERSION
} VPProtocolInfo;

// 函数表：由 ttpGetVideoProtocol 工厂填写。
typedef struct VPProtocolApi {
    int (*api_version)(void);
    void (*info)(VPProtocolInfo* out);
    // 这个 DLL 能处理该 URL 吗？返回 0-100 置信度（0 = 不认领）。
    // 只允许看 URL 形状，禁止联网 —— 主程序会在 UI 线程调用。
    int (*probe)(const wchar_t* url);
    // 打开会话。成功返回非空句柄，并把「交给播放器的媒体地址」写入
    // out_media_url（如本地代理 http://127.0.0.1:port/stream.ts）。
    // 失败返回 NULL。必须快速返回（<100ms），重活起内部线程。
    VPSession* (*open)(const wchar_t* url, const VPHttpConfig* cfg,
                       wchar_t* out_media_url, int cch_media_url);
    // 查询状态（VP_POLL_*）。out_err 仅在返回 VP_POLL_ERROR 时填写。
    int (*poll)(VPSession* s, wchar_t* out_err, int cch_err);
    // 停止内部线程、释放会话。可从任意线程调用；内部需自行 join。
    void (*close)(VPSession* s);
    // ── ABI v1 末尾追加（与 VPHttpConfig.options 同一兼容策略）──
    // 查询当前缓冲进度 0-100（播放位置前方连续就绪的数据比例）；未知/不支持
    // 返回 -1。**可为 NULL**：旧协议 DLL 不填此指针，宿主回退到无百分比提示。
    int (*poll_buffering)(VPSession* s);
    // 前方连续就绪的**秒数**（cap 300，供进度条画「已缓冲到哪」的浅色段；
    // poll_buffering 的 15s 百分比目标在长片上只有几像素，看不出进度）。
    // 同样可为 NULL。
    int (*poll_buffered)(VPSession* s);
} VPProtocolApi;

// ── 每个协议 DLL 的唯一导出（对齐音频插件 ttpGetSoundAddIn 的工厂模式）──
//
//   HRESULT WINAPI ttpGetVideoProtocol(VPProtocolApi* out);
//
// 成功：填 *out 并返回 S_OK；失败返回 E_FAIL（该 DLL 会被跳过）。
// 无需 .def 文件：__declspec(dllexport) + WINAPI 即可被 GetProcAddress 找到。
