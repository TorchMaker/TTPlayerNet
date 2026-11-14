#include "ttplayer/video/video_backend.h"
#include "ttplayer/video/video_player_window.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cwctype>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <bcrypt.h>

#include "ttplayer/musicfree/musicfree_theme.h"
#include "ttplayer/musicfree/musicfree_frame.h"  // frame::Map / PaintBackground / ApplyWindowRegion
#include "ttplayer/musicfree/musicfree_http.h"   // DownloadText / DownloadBinary（HLS 拼接）
#include "ttplayer/musicfree/musicfree_types.h"  // NetworkConfig
#include "ttplayer/video/video_types.h"          // VideoParse（解析接口）
#include "ttplayer/video/video_history.h"        // 观看历史 / 续播进度
#include "ttplayer/video/video_resolve.h"        // ResolveVideoUrl（解析接口 + 播放页爬虫）
#include "ttplayer/video/video_player_js.h"      // IsBuiltinPlayerFlag（flag 派发）
#include "ttplayer/video/video_protocol_registry.h"  // 协议插件（HLS 本地代理等）
#include "ttplayer/video/video_web_host.h"       // 网页兜底宿主（exe 注入的工厂）
#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/skin/skin.h"                  // skin::BrowserSkin / DrawFrameButton
#include "ttplayer/skin/skin_frame.h"            // skin::DrawFrameBackground / CreateFrameRegion
#include "ttplayer/ui/dpi_scale.h"               // ui::ResolveScaleForWindow
#include "ttplayer/core/text.h"

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "dwmapi.lib")

namespace ttplayer::video {

// 复用音乐窗的外框 / 按钮 / 字体主题（与音乐窗渲染管线同款）。
namespace theme = ttplayer::musicfree::theme;
// 复用音乐窗的 <browser_window> 皮肤坐标映射（九宫格 / 区域）。
namespace frame = ttplayer::musicfree::frame;

namespace {

// ----------------------------------------------------- Media Foundation 后端
// 每次 backend->Open 递增：重建会话（拖动进度条按起点重开代理）时，旧 engine
// 在 Shutdown 之后仍可能投递在途的 ERROR / ENDED / PAUSE 事件，会把新会话的
// playing / error_text 打回去、误显示「播放失败」并藏掉渲染子窗口。
std::atomic<unsigned> g_media_engine_serial{0};

class MediaEngineNotify : public IMFMediaEngineNotify {
public:
    explicit MediaEngineNotify(VideoBackendCallbacks cb, unsigned serial)
        : cb_(std::move(cb)), serial_(serial) {}
    ~MediaEngineNotify() = default;

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEngineNotify) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR param1,
                                          DWORD param2) override {
        (void)param1; (void)param2;
        if (serial_ != g_media_engine_serial.load(std::memory_order_acquire))
            return S_OK;  // 旧 engine 的迟到事件，一律丢弃
        switch (event) {
            case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
                if (cb_.on_ready) cb_.on_ready();
                break;
            case MF_MEDIA_ENGINE_EVENT_PLAY:
                if (cb_.on_playing) cb_.on_playing(true);
                break;
            case MF_MEDIA_ENGINE_EVENT_PAUSE:
                if (cb_.on_playing) cb_.on_playing(false);
                break;
            case MF_MEDIA_ENGINE_EVENT_ENDED:
                if (cb_.on_end) cb_.on_end();
                break;
            case MF_MEDIA_ENGINE_EVENT_ERROR:
                if (cb_.on_error) cb_.on_error("播放失败（解码或网络错误）");
                break;
            default:
                break;
        }
        return S_OK;
    }
private:
    VideoBackendCallbacks cb_;
    unsigned serial_ = 0;
    LONG ref_ = 1;
};

class MediaFoundationBackend : public IVideoBackend {
public:
    explicit MediaFoundationBackend(VideoBackendCallbacks cb) : cb_(std::move(cb)) {
        static std::once_flag once;
        std::call_once(once, [] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            MFStartup(MF_VERSION, MFSTARTUP_FULL);
        });
    }
    ~MediaFoundationBackend() override { Close(); }

    bool Open(HWND render_hwnd, const std::wstring& url,
              const std::map<std::string, std::string>& headers) override {
        (void)headers;  // 窗口化 MediaEngine 无法自定义请求头，直链即可
        // 必须在 Close() **之前**递增：Shutdown 期间同步投递的旧事件也要被过滤。
        const unsigned my_serial = g_media_engine_serial.fetch_add(
                                       1, std::memory_order_acq_rel) + 1;
        Close();
        if (!render_hwnd) return false;

        notify_ = new MediaEngineNotify(cb_, my_serial);
        if (FAILED(MFCreateAttributes(&attr_, 2))) { Close(); return false; }
        attr_->SetUnknown(MF_MEDIA_ENGINE_CALLBACK,
                          static_cast<IMFMediaEngineNotify*>(notify_));
        attr_->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND,
                         static_cast<UINT64>(reinterpret_cast<UINT_PTR>(render_hwnd)));

        IMFMediaEngineClassFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) { Close(); return false; }
        hr = factory->CreateInstance(0, attr_, &engine_);
        factory->Release();
        if (FAILED(hr) || !engine_) { Close(); return false; }

        BSTR b = SysAllocString(url.c_str());
        hr = engine_->SetSource(b);
        SysFreeString(b);
        if (FAILED(hr)) { Close(); return false; }

        engine_->SetVolume(volume_);
        engine_->SetMuted(muted_ ? TRUE : FALSE);
        opened_ = true;
        return true;
    }

    void Play() override { if (engine_ && opened_) { engine_->Play(); playing_ = true; } }
    void Pause() override { if (engine_ && opened_) { engine_->Pause(); playing_ = false; } }
    bool IsPlaying() const override { return playing_; }

    double GetPosition() const override {
        if (!engine_ || !opened_) return 0.0;
        // GetCurrentTime 在首帧前也可能返回 NaN；统一在这里归一，避免调用方
        // 把 NaN 直接拿去算进度 / 格式化时间。
        const double t = engine_->GetCurrentTime();
        return std::isfinite(t) && t > 0.0 ? t : 0.0;
    }
    double GetDuration() const override {
        if (!engine_ || !opened_) return 0.0;
        // ⚠ IMFMediaEngine::GetDuration 在时长未知（HLS / 未加载完）时返回
        // **NaN**，直接透出会导致 `static_cast<int>(NaN)` = INT_MIN，
        // 时间显示成 "-14:-8"。这里只把有限的正当值透出，未知一律 0。
        const double d = engine_->GetDuration();
        return std::isfinite(d) && d > 0.0 ? d : 0.0;
    }
    void Seek(double seconds) override {
        if (engine_ && opened_) engine_->SetCurrentTime(seconds);
    }

    void SetVolume(double volume) override {
        volume_ = volume;
        if (engine_ && opened_) engine_->SetVolume(volume);
    }
    void SetMuted(bool muted) override {
        muted_ = muted;
        if (engine_ && opened_) engine_->SetMuted(muted ? TRUE : FALSE);
    }
    bool IsMuted() const override { return muted_; }

    // 全屏由窗口层负责几何变化，这里只记录状态供查询。
    void SetFullscreen(bool fullscreen) override { fullscreen_ = fullscreen; }
    bool IsFullscreen() const override { return fullscreen_; }

    // display aspect ratio 优先：PAL / anamorphic 片源的像素并非正方形，
    // 只按 GetNativeVideoSize 算会算出错误的比例（全屏看起来被拉伸）。
    bool GetVideoSize(int* width, int* height) const override {
        if (width) *width = 0;
        if (height) *height = 0;
        if (!engine_ || !opened_) return false;
        DWORD cx = 0, cy = 0;
        const bool have_native =
            SUCCEEDED(engine_->GetNativeVideoSize(&cx, &cy)) && cx && cy;
        DWORD ax = 0, ay = 0;
        const bool have_dar =
            SUCCEEDED(engine_->GetVideoAspectRatio(&ax, &ay)) && ax && ay;
        // 元数据未就绪时 MF 常回 S_OK + 1:1；若像素尺寸明确不是方形，
        // 别用这个假的 1:1，否则画面会被压成小方块。
        bool dar_usable = have_dar;
        if (have_dar && ax == 1 && ay == 1 && have_native) {
            const double native = static_cast<double>(cx) / cy;
            if (std::fabs(native - 1.0) > 0.05) dar_usable = false;
        }
        if (dar_usable) {
            if (width) *width = static_cast<int>(ax);
            if (height) *height = static_cast<int>(ay);
            return true;
        }
        if (have_native) {
            if (width) *width = static_cast<int>(cx);
            if (height) *height = static_cast<int>(cy);
            return true;
        }
        return false;
    }

    void Close() override {
        if (engine_) { engine_->Shutdown(); engine_->Release(); engine_ = nullptr; }
        if (attr_) { attr_->Release(); attr_ = nullptr; }
        if (notify_) { notify_->Release(); notify_ = nullptr; }
        opened_ = false; playing_ = false;
    }
private:
    VideoBackendCallbacks cb_;
    IMFAttributes* attr_ = nullptr;
    IMFMediaEngine* engine_ = nullptr;
    MediaEngineNotify* notify_ = nullptr;
    bool opened_ = false;
    bool playing_ = false;
    bool fullscreen_ = false;
    bool muted_ = false;
    double volume_ = 1.0;
};

} // namespace

std::unique_ptr<IVideoBackend> CreateDefaultVideoBackend(VideoBackendCallbacks cb) {
    return std::make_unique<MediaFoundationBackend>(std::move(cb));
}

// 网页兜底宿主工厂的注入点：WebView2 只链接在 exe 上，核心库（本文件）
// 只保存 exe 在 OleInitialize 之后注入的函数指针，不引用任何 WebView2 符号。
namespace {
VideoWebHostFactory g_web_host_factory = nullptr;
}

void SetVideoWebHostFactory(VideoWebHostFactory factory) {
    g_web_host_factory = factory;
}

VideoWebHostFactory GetVideoWebHostFactory() { return g_web_host_factory; }

std::unique_ptr<IVideoBackend> LoadVideoBackendPlugin(
    const std::filesystem::path& dll, VideoBackendCallbacks cb) {
    (void)dll; (void)cb;
    return nullptr;
}

// ----------------------------------------------------------- 视频播放窗口
// 影视仓风格深色播放器：自绘标题栏 + 控制栏，圆形播放键、可拖动进度条、
// 音量滑条，全屏时控件 3 秒无操作自动隐藏。

namespace {

constexpr COLORREF kVidBg = RGB(13, 15, 20);        // 视频区底色
constexpr COLORREF kVidBar = RGB(19, 22, 30);       // 标题栏 / 控制栏
constexpr COLORREF kVidBarLine = RGB(38, 46, 62);   // 栏分隔线
constexpr COLORREF kVidText = RGB(232, 238, 244);
constexpr COLORREF kVidDim = RGB(138, 150, 166);
constexpr COLORREF kVidTrack = RGB(48, 56, 72);     // 进度条轨道
constexpr COLORREF kVidAccent = RGB(38, 166, 205);  // 主色（与应用一致）
constexpr COLORREF kVidAccentHot = RGB(64, 190, 226);
constexpr COLORREF kVidHot = RGB(52, 62, 80);       // 控件悬停底
constexpr COLORREF kVidError = RGB(235, 100, 100);

constexpr int kTitleH = 38;
constexpr int kFooterH = 58;

HFONT PlayerTitleFont() {
    static HFONT f = theme::CreateUiFont(-15, 600);
    return f;
}
HFONT PlayerSmallFont() {
    static HFONT f = theme::CreateUiFont(-12, 400);
    return f;
}

// --------------------------------------------------------------- 状态与布局
struct HlsShared {
    std::atomic<bool> cancel{false};
    // 全部分段 #EXTINF 之和（秒）。IMFMediaEngine 在 HLS / 拼接 ts 上经常
    // 拿不到时长（GetDuration 返回 NaN），用它兜底进度条与时间显示。
    std::atomic<double> total_duration{0.0};
};

// ---- AES-128-CBC 解密（HLS AES-128 加密段用）------------------------------
// 视频源里大量 m3u8 带 #EXT-X-KEY:METHOD=AES-128 —— 分段是 AES-128-CBC
// 加密的 TS。不解密直接拼出来的文件 MF 无法解码（表现为「无法播放」）。
bool AesCbcDecrypt128(const std::vector<std::uint8_t>& key,
                      const std::uint8_t iv[16],
                      const std::vector<std::uint8_t>& in,
                      std::vector<std::uint8_t>& out) {
    if (key.size() != 16 || in.empty() || (in.size() % 16) != 0) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    bool ok = false;
    do {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
            break;
        if (!BCRYPT_SUCCESS(BCryptSetProperty(
                alg, BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                sizeof(BCRYPT_CHAIN_MODE_CBC), 0)))
            break;
        if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
                alg, &key_handle, nullptr, 0,
                const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0)))
            break;
        std::vector<std::uint8_t> buffer(in.size() + 16);
        ULONG done = 0;
        std::vector<std::uint8_t> iv_copy(iv, iv + 16);
        if (!BCRYPT_SUCCESS(BCryptDecrypt(
                key_handle, const_cast<PUCHAR>(in.data()),
                static_cast<ULONG>(in.size()), nullptr,
                iv_copy.data(), 16, buffer.data(),
                static_cast<ULONG>(buffer.size()), &done, 0)))
            break;
        buffer.resize(done);
        // 去 PKCS7 padding（解密段末尾 1..16 个同值字节）。
        if (!buffer.empty()) {
            const std::uint8_t pad = buffer.back();
            if (pad >= 1 && pad <= 16 && pad <= buffer.size())
                buffer.resize(buffer.size() - pad);
        }
        out = std::move(buffer);
        ok = true;
    } while (false);
    if (key_handle) BCryptDestroyKey(key_handle);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// 协议插件监控线程的取消标记：线程持 weak_ptr，PlayerState 销毁后自然退出。
struct ProtoCancel {
    std::atomic<bool> cancelled{false};
};

// 后台解析线程 → UI 线程（WM_USER+7）的解析结果。除了媒体直链，还要把
// 播放器 / 解析接口返回的请求头（Referer、鉴权…）并进 open_headers，否则
// HLS 主索引能下、切片和 #EXT-X-KEY 却 403 —— 这正是 TVBox 能播而我们
// 播不了的主因之一。调用方 new，WndProc 负责 delete。
struct ResolvedMedia {
    std::wstring url;
    std::map<std::string, std::string> headers;
};

struct PlayerState {
    HWND hwnd{};
    HWND video_host{};
    HWND owner{};
    std::unique_ptr<IVideoBackend> backend;
    RECT title{};        // 标题栏整条
    RECT title_rect{};   // 标题可绘制区（右侧留出按钮）
    RECT close_btn{};
    RECT fs_btn{};
    RECT content{};      // 视频画面区
    RECT footer{};       // 控制栏整条
    RECT play_btn{};     // 圆形播放 / 暂停
    RECT progress{};     // 进度条轨道
    RECT time_text{};
    RECT vol_btn{};
    RECT vol_slider{};
    bool playing = false;
    bool got_ready = false;   // LOADEDDATA 已到（开始出画面）
    bool opened_ok = false;   // backend->Open 成功
    double volume = 1.0;      // 本地音量镜像（后端接口没有 getter）
    int hover_id = 0;
    int pressed_id = 0;
    bool dragging_progress = false;
    double drag_pos = 0.0;    // 拖动中的预览位置（秒）
    double drag_ratio = 0.0;  // 拖动比例（时长未知时只用来画滑块）
    DWORD last_drag_seek = 0; // 拖动中 Seek 节流（120ms）
    bool dragging_volume = false;
    // 时长兜底：后端 GetDuration 返回 NaN/0 时用 HLS 分段 #EXTINF 之和。
    double duration_hint = 0.0;
    // 每次换集 / 换线自增：后台时长探测回来时比对，避免旧地址的时长盖到新集。
    unsigned open_serial = 0;
    // 本地代理不支持 Range（如 AES-128 HLS 走 chunked）→ 旧的 MF Seek 无效。
    // 只用于日志/诊断，不再用来阻止拖动（见 NeedsRestartSeek）。
    bool seek_unsupported = false;
    // ★ 长期「进度条拖不动」的根治（2026-11-10 第二轮，真机日志实证）：
    //   MF 对本地 HLS 代理的 chunked 流 GetCurrentTime 恒 0、SetCurrentTime
    //   完全无效（player_ui.log: SEEK target=706.2 mode=native 后画面不动）。
    //   Seek 改为「用 /s/<秒>/stream.ts 重开代理会话」，并在这里维护显示
    //   时间基准，否则重开后位置又从 0 开始、滑块依旧回弹。
    std::wstring protocol_media_url;   // 当前协议会话给出的 media URL
    double seek_offset = 0.0;          // 重开会话后画面起点（秒）
    double play_clock_base = 0.0;      // GetCurrentTime 恒 0 时的本地推断位置
    DWORD play_clock_tick = 0;
    // 本地代理偶发断流时只自动重开一次，避免无限重试；换源/换集清零。
    bool error_retried = false;
    // 全屏自动隐藏控制条：WebView2 会吞掉 WM_MOUSEMOVE，不能只靠消息判断
    // 鼠标是否移动，定时器轮询屏幕光标坐标兜底。
    POINT last_cursor{};
    POINT hide_cursor{};       // 隐藏瞬间的光标位置：没真实位移就不唤出
    bool cursor_tracked = false;
    // 「智能全屏」：按视频显示宽高比把渲染子窗口等比居中，四周留黑边，
    // 绝不把画面拉伸铺满显示器。元数据晚到时由 WM_TIMER 轮询更新。
    int video_w = 0, video_h = 0;
    RECT video_rect{};
    // 双击画面的上一次点击时刻（GetMessageTime）。
    DWORD last_click_time = 0;
    std::wstring title_text = L"视频播放";
    std::wstring line_label;
    std::wstring error_text;
    // 网页兜底：官方网页线路（腾讯/优酷/B 站等）在所有解析接口都失败时，
    // 直接用 WebView2 在画面区加载该网页。web_host 由 exe 注入的工厂创建，
    // 核心库只持有纯接口指针；web_mode 为真时不再画「播放失败」浮层。
    std::unique_ptr<VideoWebHost> web_host;
    bool web_mode = false;
    // 「先隐藏嗅探、超时再显示」：网页线路兜底时先把 WebView2 挂在 0×0 区域，
    // 让页面脚本自己算出/请求到媒体直链（TTPMS 回推），命中就切原生播放；
    // 7 秒内没命中才把网页显示出来（用户仍能点页面播放器）。
    bool web_resolve_hidden = false;
    DWORD web_resolve_deadline = 0;
    // 网页播放形态下 <video> 的状态（脚本 250ms 回推）。自绘进度条/播放键
    // 与原生形态共用同一套状态读取与 Seek 路径。
    VideoWebState web_state{};
    bool web_muted = false;
    // 标题栏左侧的应用小图标（取主窗口图标）：自绘皮肤窗口没有系统标题栏，
    // 补上它才有「应用窗口」的左上角标识，与主窗 / 音乐窗连贯。
    HICON app_icon = nullptr;
    // 可切换线路（同一集的多源）。source_index 指向当前源；open_headers 是
    // 打开当前源时携带的请求头（切源时复用）。
    std::vector<VideoPlayerSource> sources;
    int source_index = 0;
    // 同集多线路自动兜底：首条线路失效（DNS/403/token 过期）时自动换下一条，
    // 计数到 sources.size()-1 为止，避免在死线路之间无限循环。
    int auto_source_tried = 0;
    // 同源重新解析：网页线路（WEB 插件）每次打开都会重新抓播放页换新令牌。
    // 401/402（hhplayer 这类单次令牌/限流）失败时先原样重开一次同一线路。
    int same_src_retry_index = -1;   // 已经重试过的线路下标
    bool resolve_retry_pending = false;
    std::map<std::string, std::string> open_headers;
    RECT source_btn{};        // 「切换源」按钮（标题栏，全屏药丸左侧）
    // 剧集切换：整部剧的选集列表（含每集自己的多线路）。电影为空 → 不显示
    // 「选集」按钮。切换时重开同一个播放窗（ShowVideoPlayer 的复用分支）。
    std::vector<VideoPlayerEpisode> episodes;
    RECT episode_btn{};       // 「选集」按钮（控制栏，切换源左侧）
    std::wstring base_title;  // 片名（不含「千千静听 · 」前缀，切集时复用）
    // ── 观看历史 / 续播 ───────────────────────────────────────────
    // history_id 由宿主（音乐窗桥 / 播放列表「视频」栏目）提供；非空时按
    // 3 秒节奏把「当前集 + 播放进度」写回 <exe>\video\history.json。
    std::wstring history_id;
    int current_ep = -1;        // 当前集在 episodes 里的下标（电影 -1）
    int pending_start_ms = 0;   // 首帧就绪后要跳到的位置（续播）
    bool start_applied = false;
    DWORD last_history_save = 0;
    // 单击画面中间 = 暂停/继续（与双击全屏共存：先挂起，确认不是双击再执行）。
    bool pending_content_toggle = false;
    bool content_double_clicked = false;  // 刚发生了双击全屏，本次抬起不暂停
    // HLS（m3u8）拼接：Media Foundation 在 Win10 1803+ 已移除内置 HLS 源，
    // m3u8 直链无法直接播。后台线程把全部 ts 分段拼成本地 .ts 再交给 MF。
    std::atomic<int> hls_percent{0};     // 0..100
    std::atomic<bool> hls_cancel{false};
    std::shared_ptr<struct HlsShared> hls_shared;  // 后台线程的取消标记
    std::wstring hls_prepared;           // 拼接完成的本地 .ts 路径
    std::string hls_error;
    bool hls_running = false;
    // ---- MF 直连 m3u8 尝试 ----
    // 影视仓秒进的根本原因是「流式播放」：IJK/Exo 原生边下边播。我们没有
    // 流式解码器，但**本机的 Media Foundation 若带 HLS 源，IMFMediaEngine
    // 就能直接流式播 m3u8**（Windows Media Feature Pack）。所以先直连试一次，
    // 8 秒内出画面就用它（= 秒进）；不奏效再回退到「全量拼接本地 ts」。
    bool hls_direct = false;             // 正在直连尝试
    DWORD hls_direct_deadline = 0;       // 直连尝试的超时时刻（GetTickCount）
    // 带请求头下载（Referer/鉴权直链 MF 无法直接播）落盘期间为 true，
    // 用来显示「正在准备播放…」浮层并在缓冲线程跑完前保持整窗重绘。
    bool download_running = false;
    // 协议插件会话（video\protocols\*.dll，如 HLS 本地代理）。
    // proto_session 由 VideoProtocolRegistry 管理；proto_cancel 是监控线程
    // 的取消标记（weak_ptr 模式，避免线程引用已销毁的 PlayerState）。
    ProtocolSession* proto_session = nullptr;
    std::shared_ptr<struct ProtoCancel> proto_cancel;
    // 播放地址解析中（影视仓 parses / 播放页爬虫）→ 显示「正在解析播放地址…」。
    bool resolving = false;
    // 解析接口列表（影视仓 parses 段）。
    std::vector<VideoParse> parses;
    // TVBox 自定义播放器：订阅级 playerUrl / 站点 player（JS 地址或内联脚本），
    // 以及相对地址拼接基准。按线路 flag 派发，见 ResolveVideoUrl。
    std::string player_url;
    std::string player_base_url;
    // 自定义 flag 的播放串即使看起来像直链，也要先过一次播放器解析。
    // 解析结果回传会重开同一条线路，用「已试过 + 线路下标」防止无限重入。
    bool player_tried = false;
    int player_tried_index = -1;
    // 全屏（窗口几何在这里改，后端只记录）+ 控件自动隐藏。
    bool window_fullscreen = false;
    bool controls_hidden = false;
    // 线路选择菜单是模态的，弹出期间不要让 2 秒计时把控制条藏掉。
    bool source_menu_open = false;
    DWORD last_input = 0;
    bool tracking_mouse = false;
    // 全屏静止时隐藏鼠标光标：ShowCursor 是全局计数器，用此状态位保护，
    // 只在「显示/隐藏」翻转时调用一次，避免多次进入/退出全屏后计数失衡。
    bool cursor_visible = true;
    LONG_PTR saved_style = 0;
    RECT saved_rect{};
    // 皮肤（音乐窗 <browser_window> 段）：非空时外框走皮肤，否则自绘深色边框。
    const skin::BrowserSkin* browser_skin = nullptr;
    frame::Metrics skin_layout{};
    COLORREF skin_transparent = CLR_INVALID;
    bool skinned = false;
};

PlayerState* g_player = nullptr;

// 切换全屏（定义在窗口过程之后，这里先声明供过程内调用）。
void ToggleFullscreen(PlayerState* st);

// 全屏 + 静止超时才隐藏控制条（参考其他播放器）。
// 旧实现的坑：隐藏期间 HitTestId 返回 0，而 WebView2 会吞掉 WM_MOUSEMOVE，
// 导致「鼠标移动唤不出控件」→ 全屏后进度条永远点不中。现在由 WM_TIMER
// 轮询 GetCursorPos 兜底唤出，且 WM_LBUTTONDOWN 会先 TouchInput 再命中。
bool ControlsHidden(const PlayerState* st) {
    // 用户口中的「最大化」常指 Windows 窗口最大化而不是真全屏；旧判定只看
    // window_fullscreen，最大化时控制条永不隐藏（真机反馈）。两者都算沉浸态。
    return st && st->controls_hidden &&
           (st->window_fullscreen || (st->hwnd && IsZoomed(st->hwnd)));
}

// 全屏 / 窗口最大化都算沉浸态，静止超时后隐藏控制条。
bool ImmersiveWindow(PlayerState* st, HWND hwnd) {
    return st && (st->window_fullscreen || IsZoomed(hwnd));
}

// 控制栏唯一布局入口：从右往左排列 全屏 / 音量滑条 / 音量按钮 / 时间 / 进度，
// 最后统一 clamp 到客户区内。以前全屏按钮直接按 content.right-84 算，窗口被
// 拉伸/皮肤内容区越界时会跑到客户区外被裁掉（用户看到的「右下角只有音量滑条」）；
// 且控制栏底色是后画的（旧 WM_PAINT 先画按钮再填栏底），按钮实际被底色盖掉。
void LayoutFooterControls(PlayerState* st) {
    RECT cc{};
    GetClientRect(st->hwnd, &cc);
    const int cy = (st->footer.top + st->footer.bottom) / 2;
    const int left = std::max<int>(st->footer.left, cc.left);
    st->play_btn = {left + 14, cy - 19, left + 52, cy + 19};
    int right = std::min<int>(st->footer.right, cc.right) - 16;
    if (right < st->play_btn.right + 60) right = st->play_btn.right + 60;
    // 全屏按钮固定贴控制栏最右（用户要求：不放右上角，放下方）。
    st->fs_btn = {right - 40, cy - 15, right, cy + 15};
    // ★ 用户要求「切换源要在下方，并且可以选择不同名字的源」：按钮从标题栏
    //   移到控制栏、紧挨全屏键左侧，点击弹出所有线路名供直接选择（不再只是
    //   循环切下一个）。多线路时才占位。
    // 从右往左：全屏(40) 12 [切换源(78) 12] 音量滑条(70) 10 音量按钮(46)
    //           10 时间(104) 12 进度。
    int x = st->fs_btn.left - 12;
    if (st->sources.size() > 1) {
        st->source_btn = {x - 78, cy - 15, x, cy + 15};
        x = st->source_btn.left - 12;
    } else {
        st->source_btn = {};
    }
    // 「选集」按钮：剧集（episodes > 1）时放在切换源左侧，点开列出全部集数。
    if (st->episodes.size() > 1) {
        st->episode_btn = {x - 62, cy - 15, x, cy + 15};
        x = st->episode_btn.left - 12;
    } else {
        st->episode_btn = {};
    }
    st->vol_slider = {x - 70, cy - 3, x, cy + 3};
    x = st->vol_slider.left - 10;
    st->vol_btn = {x - 46, cy - 15, x, cy + 15};
    x = st->vol_btn.left - 10;
    st->time_text = {x - 104, cy - 15, x, cy + 15};
    int progress_left = st->play_btn.right + 12;
    int progress_right = st->time_text.left - 12;
    if (progress_right - progress_left < 80) {
        // 窄窗：先牺牲时间与音量按钮，再牺牲音量滑条，全屏按钮必须留下。
        st->time_text = {};
        st->vol_btn = {};
        st->source_btn = {};   // 窄窗：优先保进度条与全屏键
        st->episode_btn = {};
        st->vol_slider = {st->fs_btn.left - 12 - 70, cy - 3,
                          st->fs_btn.left - 12, cy + 3};
        progress_right = st->vol_slider.left - 12;
        if (progress_right - progress_left < 40) {
            st->vol_slider = {};
            progress_right = st->fs_btn.left - 12;
        }
        if (progress_right - progress_left < 20) {
            progress_right = progress_left;
        }
    }
    st->progress = {progress_left, cy - 3, progress_right, cy + 3};
    // 最终 clamp：所有矩形都不许越出客户区（多显示器/皮肤内容区越界时也成立）。
    auto clamp_rect = [&cc](RECT& r) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        if (r.right > cc.right) {
            r.left -= r.right - cc.right;
            r.right = cc.right;
        }
        if (r.left < cc.left) {
            r.right += cc.left - r.left;
            r.left = cc.left;
        }
        if (r.right > cc.right) r.right = cc.right;
        if (r.bottom > cc.bottom) {
            r.top -= r.bottom - cc.bottom;
            r.bottom = cc.bottom;
        }
        if (r.top < cc.top) {
            r.bottom += cc.top - r.top;
            r.top = cc.top;
        }
        if (r.bottom > cc.bottom) r.bottom = cc.bottom;
    };
    clamp_rect(st->fs_btn);
    clamp_rect(st->source_btn);
    clamp_rect(st->episode_btn);
    clamp_rect(st->vol_slider);
    clamp_rect(st->vol_btn);
    clamp_rect(st->time_text);
    clamp_rect(st->progress);
}

// 按显示宽高比把矩形等比居中（letterbox）：取能完整放进 box 的最大缩放，
// 不裁剪、不拉伸；比例未知时原样返回 box。
RECT FitVideoRect(const RECT& box, int vw, int vh) {
    if (vw <= 0 || vh <= 0) return box;
    const double bw = static_cast<double>(box.right - box.left);
    const double bh = static_cast<double>(box.bottom - box.top);
    if (bw <= 0.0 || bh <= 0.0) return box;
    const double scale = std::min(bw / vw, bh / vh);
    const int fw = std::max(1, static_cast<int>(vw * scale));
    const int fh = std::max(1, static_cast<int>(vh * scale));
    const int x = box.left + static_cast<int>((bw - fw) / 2.0);
    const int y = box.top + static_cast<int>((bh - fh) / 2.0);
    return RECT{x, y, x + fw, y + fh};
}

void LayoutPlayer(PlayerState* st) {
    if (!st->hwnd) return;
    RECT c{};
    GetClientRect(st->hwnd, &c);
    const int w = c.right, h = c.bottom;
    const bool hide = ControlsHidden(st);
    const bool fs = st->window_fullscreen;
    // ★ 需求变更：web 模式下**不再收掉**自绘控制条。站点自带的进度条/播放框
    //   由 video_web_host.cpp 注入脚本隐藏；WebView2 只当渲染面，控制一律走
    //   自绘控制条（用户：「不要使用 web 视频的进度条和播放框，使用自定义的」）。
    // ★ 全屏：不再走皮肤坐标系，也不画皮肤位图（否则四周仍有皮肤边框）。
    // ★ 控制条隐藏时**绝不改变几何**：旧实现 hide 时把 footer 清空、content
    //   铺到 h，于是每轮 hide/show 都会 MoveWindow(video_host) 把画面缩放一次
    //   —— 既是肉眼可见的「隔几秒闪一下」，又会触发伪 WM_MOUSEMOVE 把刚隐藏
    //   的控制条立刻唤回。现在 footer/content 恒定，控制条只作为叠加层
    //   「画或不画」，画面矩形永远不动。
    if (fs) {
        st->skin_layout = {};
        st->title = {0, 0, w, 0};                 // 全屏无标题栏
        st->footer = RECT{0, h - kFooterH, w, h};
        st->content = RECT{0, 0, w, st->footer.top};
        st->close_btn = {};
        st->title_rect = {};
        // source_btn 不再属于标题栏：统一由下面的 LayoutFooterControls 布局。
    } else if (st->skinned) {
        st->skin_layout = frame::Map(c, *st->browser_skin,
                                     ui::ResolveScaleForWindow(st->hwnd));
        const int title_h = st->skin_layout.content.top;
        const int cb = st->skin_layout.content.bottom;
        st->title = {0, 0, w, title_h};
        st->footer = RECT{st->skin_layout.content.left, cb - kFooterH,
                          st->skin_layout.content.right, cb};
        st->content = RECT{st->skin_layout.content.left,
                           st->skin_layout.content.top,
                           st->skin_layout.content.right, st->footer.top};
        st->close_btn = st->skin_layout.close;
        st->title_rect = {30, 0, st->close_btn.left - 12, title_h};
    } else {
        st->title = {0, 0, w, kTitleH};
        st->footer = RECT{0, h - kFooterH, w, h};
        st->content = RECT{0, st->title.bottom, w, st->footer.top};
        st->close_btn = {w - 42, 5, w - 12, 33};
        st->title_rect = {30, 0, st->close_btn.left - 12, kTitleH};
    }
    // ★ 智能全屏：渲染子窗口只占等比矩形，四周由父窗填纯黑。
    //   旧实现把 video_host 直接铺满 content，视频比例与显示器不一致时
    //   画面被拉伸（用户 2026-11-10 反馈「全屏好像拉伸了」）。
    st->video_rect = FitVideoRect(st->content, st->video_w, st->video_h);
    if (st->video_host)
        MoveWindow(st->video_host, st->video_rect.left, st->video_rect.top,
                   st->video_rect.right - st->video_rect.left,
                   st->video_rect.bottom - st->video_rect.top, TRUE);
    // WebView2 子窗口只占画面区：底部控制栏留给父窗接收鼠标（否则 WebView2
    // 会把 WM_LBUTTONDOWN/MOUSEMOVE/LBUTTONUP 全吞掉，自绘进度条收不到点击）。
    if (st->web_host) st->web_host->Resize(st->content);
    if (hide || st->footer.bottom <= st->footer.top) {
        st->play_btn = {};
        st->fs_btn = {};
        st->source_btn = {};
        st->vol_btn = {};
        st->vol_slider = {};
        st->time_text = {};
        st->progress = {};
    } else {
        LayoutFooterControls(st);
    }
}

// 无法播放的视频源信息日志：<exe>\video\play_fail.log（UTF-8 追加）。
// 用户遇到播不了的线路时不必反复点程序：这里记录影片/集名、线路名、
// 全部线路 URL、parses 数量与失败原因，直接看日志即可定位。
void AppendPlayFailLog(const std::string& text) {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring path = dir.substr(0, slash) + L"\\video\\play_fail.log";
    // 简单轮转，避免日志无限增长。
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
        fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 512 * 1024)
        MoveFileExW(path.c_str(), (path + L".1").c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    char stamp[64]{};
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    std::fprintf(f, "\n==== %s ====\n%s\n", stamp, text.c_str());
    std::fclose(f);
}

// 日志里的 URL 去掉查询串（sign / token 等一次性凭据不落盘）。
std::string RedactUrl(std::string url) {
    const size_t q = url.find('?');
    if (q != std::string::npos) url = url.substr(0, q) + "?…";
    return url;
}

// 播放窗 UI 诊断日志：<exe>\video\player_ui.log（UTF-8，512KB 轮转）。
// 覆盖进度条命中/拖动/Seek、全屏切换、web 模式与协议链，便于无 GUI 定位。
void AppendUiLog(const std::string& text) {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring path = dir.substr(0, slash) + L"\\video\\player_ui.log";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
        fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 512 * 1024)
        MoveFileExW(path.c_str(), (path + L".1").c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &tm);
    std::fprintf(f, "[%s] %s\n", stamp, text.c_str());
    std::fclose(f);
}

// ───────────── 播放窗位置/大小持久化：<exe>\video\player_window.json ─────────────
// 以前每次都用 CW_USEDEFAULT 创建，播放窗总是从系统默认位置（左上角附近）出现。
// 这里独立存一份，不动主程序配置（视频模块属于 ttplayer_video.dll）。
std::wstring VideoWindowStatePath() {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return dir.substr(0, slash) + L"\\video\\player_window.json";
}

// 读取上次的还原态矩形与「是否最大化」。文件缺失/损坏一律返回 false。
bool LoadVideoWindowPlacement(RECT* out, bool* maximized) {
    if (out) *out = RECT{};
    if (maximized) *maximized = false;
    const std::wstring path = VideoWindowStatePath();
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return false;   // 损坏的文件直接当没有，下次会重写
    }
    if (!j.is_object()) return false;
    RECT r{};
    r.left = j.value("left", 0);
    r.top = j.value("top", 0);
    const int w = j.value("width", 0);
    const int h = j.value("height", 0);
    if (w <= 0 || h <= 0) return false;
    r.right = r.left + w;
    r.bottom = r.top + h;
    if (maximized) *maximized = j.value("maximized", false);
    if (out) *out = r;
    return true;
}

void SaveVideoWindowPlacement(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    // rcNormalPosition 是「还原态」矩形：最大化/最小化时也保存它，下次先还原
    // 到该位置再按需最大化。
    const RECT r = wp.rcNormalPosition;
    if (r.right <= r.left || r.bottom <= r.top) return;
    nlohmann::json j;
    j["left"] = r.left;
    j["top"] = r.top;
    j["width"] = r.right - r.left;
    j["height"] = r.bottom - r.top;
    j["maximized"] = (wp.showCmd == SW_SHOWMAXIMIZED);
    const std::wstring path = VideoWindowStatePath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << j.dump();
}

// 刷新「最后操作时间」并在隐藏时唤出控制条。
// ★ 调用方必须是有意义的输入：明确点击/按键，或 WM_TIMER 轮询到真实光标位移。
//   WM_MOUSEMOVE **不再无条件调用**：hide 时若改变渲染子窗口几何，Windows 会
//   投递一条伪 WM_MOUSEMOVE，旧实现立刻把刚隐藏的控制条唤回（真机日志
//   hide/show 同一秒成对出现，每 2~3 秒一轮，正是「隔几秒闪烁一次」）。
//   旧的「800ms 时间节流」也已废弃：它每 800ms 仍续一次命，2 秒阈值永远
//   达不到，反而让控制条一直不消失。
void TouchInput(PlayerState* st) {
    if (!st) return;
    st->last_input = GetTickCount();
    if (st->controls_hidden) {
        st->controls_hidden = false;
        LayoutPlayer(st);
        InvalidateRect(st->hwnd, nullptr, FALSE);
        AppendUiLog("CONTROLS show (input)");
    }
}

// 进度条 / 音量条命中区比轨道大一圈，否则 6px 高的轨道根本点不中。
bool PtInInflated(const RECT& r, const POINT& p, int dy) {
    RECT hit = r;
    InflateRect(&hit, 2, dy);
    return PtInRect(&hit, p) != FALSE;
}


int HitTestId(PlayerState* st, const POINT& p) {
    if (ControlsHidden(st)) return 0;
    if (PtInRect(&st->close_btn, p)) return 1;
    if (PtInRect(&st->play_btn, p)) return 2;
    if (PtInRect(&st->vol_btn, p)) return 3;
    if (PtInRect(&st->fs_btn, p)) return 4;
    if (PtInRect(&st->source_btn, p)) return 7;
    if (PtInRect(&st->episode_btn, p)) return 8;
    if (PtInInflated(st->progress, p, 14)) return 5;
    if (PtInInflated(st->vol_slider, p, 11)) return 6;
    return 0;
}

// NaN / Inf 保护：后端在时长未知时返回 NaN，不能让它进入任何算术。
double SafeTime(double s) {
    return (std::isfinite(s) && s > 0.0) ? s : 0.0;
}

double CurrentPosition(const PlayerState* st) {
    if (!st) return 0.0;
    if (st->web_mode) return SafeTime(st->web_state.position);
    // chunked 代理流上 MF GetCurrentTime 恒 0；此时用本地单调时钟推断的
    // play_clock_base，否则进度条永远停住、拖完立刻回弹（长期问题的视觉表现）。
    const double raw = st->backend ? SafeTime(st->backend->GetPosition()) : 0.0;
    if (raw > 0.0) return st->seek_offset + raw;
    return SafeTime(st->play_clock_base);
}

// 统一取时长：后端有限正值优先，否则用 HLS 分段 #EXTINF 累加出的 hint；
// web 模式用页面回推的 <video>.duration。
double EffectiveDuration(const PlayerState* st) {
    if (!st) return 0.0;
    if (st->web_mode) {
        const double d = SafeTime(st->web_state.duration);
        return d > 0.0 ? d : SafeTime(st->duration_hint);
    }
    if (st->backend) {
        const double d = SafeTime(st->backend->GetDuration());
        if (d > 0.0) return d;
    }
    return SafeTime(st->duration_hint);
}

// 本地 HLS 代理输出的 /stream.ts（chunked 或带 Range）→ MF 的
// SetCurrentTime 对它不可靠 / 无效（真机日志反复验证）。这类线路一律走
// 「按起点重开会话」，而不是盲调 backend->Seek。
bool NeedsRestartSeek(const PlayerState* st) {
    return st && !st->web_mode && !st->protocol_media_url.empty() &&
           st->protocol_media_url.find(L"/stream.ts") != std::wstring::npos;
}

// 用 /s/<秒>/stream.ts 重开代理会话：ttp_v_hls.dll 按 #EXTINF 累加定位到
// 目标时间所在分片，从该分片开始输出；MF 重新 Open 后画面本身就在目标处。
// 这是无 Range / AES-128 HLS 唯一真正可拖动的方案（旧 Task 7）。
bool RestartProxyAt(PlayerState* st, double target) {
    if (!st || !st->backend || !st->video_host) return false;
    const std::wstring tail = L"/stream.ts";
    std::wstring url = st->protocol_media_url;
    const std::size_t at = url.rfind(tail);
    if (at == std::wstring::npos) return false;
    // URL 带 tick 后缀（DLL 只读 /s/ 后第一段秒数，忽略后面的段）：
    // 每次跳转地址都唯一，避免 WinINet 缓存导致「第二次跳同一位置没反应」。
    wchar_t prefix[96]{};
    swprintf_s(prefix, L"/s/%.3f/%lu", target,
               static_cast<unsigned long>(GetTickCount()));
    url.replace(at, tail.size(), std::wstring(prefix) + tail);

    st->seek_offset = target;
    st->play_clock_base = target;
    st->play_clock_tick = GetTickCount();
    st->opened_ok = st->backend->Open(st->video_host, url, {});
    if (!st->opened_ok) {
        AppendUiLog("SEEK restart open FAILED target=" +
                    std::to_string(static_cast<int>(target)));
        return false;
    }
    st->backend->Play();
    st->playing = true;
    st->got_ready = true;
    st->error_text.clear();
    AppendUiLog("SEEK restart target=" + std::to_string(target) +
                " url=" + RedactUrl(core::WideToUtf8(url)));
    return true;
}

// 播放控制统一入口：web 形态驱动页面 <video>；本地代理流按起点重启会话；
// 其它（本地文件 / 可 Range 直链）仍走 MF SetCurrentTime。
void SeekTo(PlayerState* st, double seconds) {
    if (!st || !std::isfinite(seconds) || seconds < 0.0) return;
    const double dur = EffectiveDuration(st);
    const double target = dur > 0.0 ? std::min(seconds, dur) : seconds;
    bool restarted = false;
    if (st->web_mode) {
        if (st->web_host) st->web_host->Seek(target);
        st->web_state.position = target;   // 乐观更新，避免滑块回弹
    } else if (NeedsRestartSeek(st)) {
        restarted = RestartProxyAt(st, target);
        if (!restarted && st->backend) st->backend->Seek(target);
    } else if (st->backend) {
        st->backend->Seek(target);
        st->play_clock_base = target;
        st->play_clock_tick = GetTickCount();
    }
    char t[240];
    std::snprintf(t, sizeof(t),
                  "SEEK target=%.1f dur=%.1f mode=%s restart=%d", target, dur,
                  st->web_mode ? "web" : "native", restarted ? 1 : 0);
    AppendUiLog(t);
}

bool IsPlayingNow(const PlayerState* st) {
    if (!st) return false;
    if (st->web_mode) return st->web_state.playing;
    return st->backend && st->backend->IsPlaying();
}

// 250ms 定时器调用：MF 在 chunked 流上 GetCurrentTime 恒 0，用本地单调时钟
// 推进显示位置；暂停 / 网页形态不推进。否则进度条永远停在 0，用户拖完松手
// 会看到滑块「弹回去」，这是「进度条无法拖动」最常见的视觉误判来源。
void AdvancePlayClock(PlayerState* st) {
    if (!st) return;
    const DWORD now = GetTickCount();
    // ★ 只有「真正出画面之后」才推进度：MF 在 Open() 后立刻 IsPlaying() 就为真，
    //   但首帧可能还没到（解析/缓冲中），旧逻辑会让进度条在 00:00/--:-- 时就
    //   自己往前跑（用户反馈「还没有正式播放，播放的进度不应该走」）。
    if (st->play_clock_tick == 0 || st->web_mode || !st->got_ready ||
        !IsPlayingNow(st)) {
        st->play_clock_tick = now;
        return;
    }
    const double raw = st->backend ? SafeTime(st->backend->GetPosition()) : 0.0;
    if (raw > 0.0) {
        st->play_clock_base = st->seek_offset + raw;
    } else {
        const double elapsed =
            static_cast<double>(now - st->play_clock_tick) / 1000.0;
        st->play_clock_base += std::max(0.0, std::min(elapsed, 1.0));
        const double dur = EffectiveDuration(st);
        if (dur > 0.0)
            st->play_clock_base = std::min(st->play_clock_base, dur);
    }
    st->play_clock_tick = now;
}

void SetPlayingNow(PlayerState* st, bool play) {
    if (!st) return;
    if (st->web_mode) {
        if (st->web_host) st->web_host->SetPaused(!play);
        st->web_state.playing = play;
        st->playing = play;
    } else if (st->backend) {
        if (play) {
            st->backend->Play();
        } else {
            AdvancePlayClock(st);   // 冻结前把位置推进到当前时刻
            st->backend->Pause();
        }
        st->play_clock_tick = GetTickCount();
    }
}

bool IsMutedNow(const PlayerState* st) {
    if (!st) return false;
    if (st->web_mode) return st->web_muted;
    return st->backend && st->backend->IsMuted();
}

void SetVolumeNow(PlayerState* st, double volume) {
    if (!st) return;
    st->volume = volume;
    if (st->web_mode) {
        st->web_muted = false;
        if (st->web_host) {
            st->web_host->SetMuted(false);
            st->web_host->SetVolume(volume);
        }
    } else if (st->backend) {
        st->backend->SetMuted(false);
        st->backend->SetVolume(volume);
    }
}

void ToggleMuteNow(PlayerState* st) {
    if (!st) return;
    if (st->web_mode) {
        st->web_muted = !st->web_muted;
        if (st->web_host) st->web_host->SetMuted(st->web_muted);
    } else if (st->backend) {
        st->backend->SetMuted(!st->backend->IsMuted());
    }
}

// Win11 会给 WS_POPUP 无边框窗口自动加圆角/阴影，看起来仍像「有边框的窗口」。
// 属性 33 = DWMWA_WINDOW_CORNER_PREFERENCE，1 = DWMWCP_DONOTROUND。
// 老系统不支持时返回失败，忽略即可。
void DisableWindowCorners(HWND hwnd) {
    if (!hwnd) return;
    int dont_round = 1;
    DwmSetWindowAttribute(hwnd, 33, &dont_round, sizeof(dont_round));
}

void UpdateDragPos(PlayerState* st, const POINT& p, bool allow_seek) {
    const int span = static_cast<int>(st->progress.right - st->progress.left);
    if (span <= 0) return;
    double ratio = static_cast<double>(p.x - st->progress.left) / span;
    ratio = std::max(0.0, std::min(1.0, ratio));
    st->drag_ratio = ratio;
    const double dur = EffectiveDuration(st);
    // 时长未知（NaN/0）：只更新 UI 预览，不换算成秒，抬起时也不会 Seek。
    st->drag_pos = dur > 0.0 ? ratio * dur : 0.0;
    // 拖动过程中节流 Seek：只松手才跳转时，用户会觉得“拖不动”。120ms
    // 一次既能跟手，又不会把解码器/代理流打爆。
    // ★ 以前按下/松开各调用 2 次 Seek（UpdateDragPos 里一次 + 外层又一次），
    //   日志里一次点击出现 3~4 条 SEEK，互相打架；现在由 allow_seek 统一控制。
    // ★ 需要「重开会话」的代理流拖动时只更新预览（allow_seek=false），
    //   否则每 120ms 重建一次 MF 会话会卡死；终跳在 WM_LBUTTONUP 做一次。
    // seek_unsupported 只作诊断，不再阻止拖动（重启方案已能处理无 Range 流）。
    if (allow_seek && dur > 0.0 && std::isfinite(st->drag_pos)) {
        const DWORD now = GetTickCount();
        if (now - st->last_drag_seek >= 120) {
            st->last_drag_seek = now;
            SeekTo(st, std::min(st->drag_pos, dur));
        }
    }
    if (st->footer.right > st->footer.left)
        InvalidateRect(st->hwnd, &st->footer, FALSE);
}

void UpdateVolume(PlayerState* st, const POINT& p) {
    const int span = static_cast<int>(st->vol_slider.right - st->vol_slider.left);
    if (span <= 0) return;
    double ratio = static_cast<double>(p.x - st->vol_slider.left) / span;
    ratio = std::max(0.0, std::min(1.0, ratio));
    SetVolumeNow(st, ratio);
    if (st->footer.right > st->footer.left)
        InvalidateRect(st->hwnd, &st->footer, FALSE);
}

// -------------------------------------------------------------------- 绘制

// GDI+ 抗锯齿填充（圆钮 / 手柄 / 三角形）。GDI 的 Ellipse/Polygon 无抗锯齿，
// 深色控制栏上的圆形按钮边缘锯齿明显（用户反馈）。GDI+ 已被封面解码使用，
// 同样采用「进程内一次性 Startup、不 Shutdown」的模式。
void EnsureGdiplus() {
    static ULONG_PTR token = 0;
    static bool started = false;
    if (!started) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok)
            started = true;
    }
}

void FillEllipseAA(HDC dc, const RECT& r, COLORREF color) {
    EnsureGdiplus();
    if (!r.right || !r.bottom) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        255, GetRValue(color), GetGValue(color), GetBValue(color)));
    g.FillEllipse(&brush,
                  Gdiplus::REAL(r.left), Gdiplus::REAL(r.top),
                  Gdiplus::REAL(r.right - r.left - 1),
                  Gdiplus::REAL(r.bottom - r.top - 1));
}

void FillPolygonAA(HDC dc, const POINT* pts, int count, COLORREF color) {
    EnsureGdiplus();
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        255, GetRValue(color), GetGValue(color), GetBValue(color)));
    std::vector<Gdiplus::PointF> ptsf;
    for (int i = 0; i < count; ++i)
        ptsf.emplace_back(Gdiplus::REAL(pts[i].x), Gdiplus::REAL(pts[i].y));
    g.FillPolygon(&brush, ptsf.data(), count);
}

void PaintGlyphPlay(HDC dc, const RECT& r) {
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const POINT tri[3] = {{cx - 5, cy - 10}, {cx - 5, cy + 10}, {cx + 10, cy}};
    FillPolygonAA(dc, tri, 3, kVidText);
}

// 播放中显示的「停止」图标：正中一个正方形。
// 旧实现按固定偏移画两根 7px 竖条，圆形按钮只有 38px 时两根竖条正好贴在一起，
// 看起来是一个 14×20 的长方块（用户反馈「中间方块不是正方形」）。这里改成
// 以按钮中心为基准的对称正方形，任何尺寸下都保持 1:1。
void PaintGlyphPause(HDC dc, const RECT& r) {
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const int half = std::max<int>(4, std::min(r.right - r.left, r.bottom - r.top) / 5);
    RECT sq = {cx - half, cy - half, cx + half, cy + half};
    const HBRUSH brush = CreateSolidBrush(kVidText);
    FillRect(dc, &sq, brush);
    DeleteObject(brush);
}

void PaintBar(HDC dc, int x, int y, int w, int h, COLORREF color) {
    if (w <= 0 || h <= 0) return;
    RECT r{x, y, x + w, y + h};
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

// 全屏图标：四角外扩的四条边角。纯 GDI 短条，不依赖字体里是否有 ⛶ 之类的
// 符号（中文字体经常缺字，按钮会变成空白），任何皮肤下都能看清。
void PaintGlyphFullscreen(HDC dc, const RECT& r, COLORREF color) {
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const int hw = 8, hh = 6, len = 5, th = 2;
    PaintBar(dc, cx - hw, cy - hh, len, th, color);
    PaintBar(dc, cx - hw, cy - hh, th, len, color);
    PaintBar(dc, cx + hw - len, cy - hh, len, th, color);
    PaintBar(dc, cx + hw - th, cy - hh, th, len, color);
    PaintBar(dc, cx - hw, cy + hh - th, len, th, color);
    PaintBar(dc, cx - hw, cy + hh - len, th, len, color);
    PaintBar(dc, cx + hw - len, cy + hh - th, len, th, color);
    PaintBar(dc, cx + hw - th, cy + hh - len, th, len, color);
}

// 还原图标：一个空心小方框（窗口化）。
void PaintGlyphRestore(HDC dc, const RECT& r, COLORREF color) {
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const int w = 14, h = 10, th = 2;
    PaintBar(dc, cx - w / 2, cy - h / 2, w, th, color);
    PaintBar(dc, cx - w / 2, cy + h / 2 - th, w, th, color);
    PaintBar(dc, cx - w / 2, cy - h / 2, th, h, color);
    PaintBar(dc, cx + w / 2 - th, cy - h / 2, th, h, color);
}

std::wstring FormatTime(double s) {
    // NaN / Inf（时长未知）直接给 --:--；否则 static_cast<int>(NaN) 会变成
    // INT_MIN，%02d 打出来就是 "-14:-8"（用户截图）。
    if (!std::isfinite(s) || s < 0.0) return L"--:--";
    const int t = static_cast<int>(std::min(s, 86399.0));
    const int hh = t / 3600, mm = (t % 3600) / 60, ss = t % 60;
    wchar_t buf[32];
    if (hh > 0) swprintf_s(buf, 32, L"%d:%02d:%02d", hh, mm, ss);
    else swprintf_s(buf, 32, L"%02d:%02d", mm, ss);
    return buf;
}

// 皮肤异形 region 的唯一入口：全屏必清、非全屏按当前布局重贴。
// 以前 SetWindowRgn(nullptr) 在 3 处、ApplyWindowRegion 在 3 处各写一遍，
// SetWindowPos 同步投递的 WM_SIZE 与函数末尾的兜底互相穿插，region 可能被
// 重新贴回全屏窗口 → 全屏四角仍带皮肤边框。现在统一到这一个函数。
void ApplyWindowRegionForState(PlayerState* st) {
    if (!st || !st->hwnd || !st->skinned) return;
    if (st->window_fullscreen) {
        SetWindowRgn(st->hwnd, nullptr, TRUE);
        return;
    }
    RECT c{};
    GetClientRect(st->hwnd, &c);
    frame::ApplyWindowRegion(st->hwnd, *st->browser_skin, st->skin_layout, c,
                             st->skin_transparent,
                             ui::ResolveScaleForWindow(st->hwnd));
}

// 切换全屏：真全屏（WS_POPUP、无皮肤/系统边框、无 DWM 圆角、覆盖显示器）。
// 自绘控制条常驻底部，全屏下依旧可以拖动进度（旧版 3 秒自动隐藏会让
// HitTestId 返回 0，进度条点不中）。
// 全屏静止时隐藏鼠标光标（ShowCursor 是全局引用计数，必须成对调用）。
// 仅在显示/隐藏状态翻转时调用，避免多次进出全屏造成计数失衡。
void SetCursorHidden(PlayerState* st, bool hidden) {
    if (!st || st->cursor_visible == !hidden) return;
    st->cursor_visible = !hidden;
    ShowCursor(hidden ? FALSE : TRUE);
}

void ToggleFullscreen(PlayerState* st) {
    if (!st || !st->hwnd) return;
    st->window_fullscreen = !st->window_fullscreen;
    st->last_input = GetTickCount();
    st->hover_id = 0;
    st->pressed_id = 0;
    st->dragging_progress = false;
    st->dragging_volume = false;
    if (GetCapture() == st->hwnd) ReleaseCapture();
    st->controls_hidden = false;
    DisableWindowCorners(st->hwnd);
    if (st->window_fullscreen) {
        GetWindowRect(st->hwnd, &st->saved_rect);
        st->saved_style = GetWindowLongPtrW(st->hwnd, GWL_STYLE);
        // 丢掉 WS_THICKFRAME / WS_CAPTION 等一切会产生系统边框的样式位。
        SetWindowLongPtrW(st->hwnd, GWL_STYLE,
                          WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        HMONITOR mon = MonitorFromWindow(st->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi))
            SetWindowPos(st->hwnd, HWND_TOPMOST, mi.rcMonitor.left,
                         mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        // 进入全屏立即隐藏鼠标；WM_TIMER 检测到真实移动后再显示。
        SetCursorHidden(st, true);
    } else {
        SetWindowLongPtrW(st->hwnd, GWL_STYLE, st->saved_style);
        SetWindowPos(st->hwnd, HWND_NOTOPMOST, st->saved_rect.left,
                     st->saved_rect.top,
                     st->saved_rect.right - st->saved_rect.left,
                     st->saved_rect.bottom - st->saved_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        // 退出全屏恢复鼠标光标。
        SetCursorHidden(st, false);
    }
    if (st->backend) st->backend->SetFullscreen(st->window_fullscreen);
    LayoutPlayer(st);
    ApplyWindowRegionForState(st);
    {
        RECT cc{};
        GetClientRect(st->hwnd, &cc);
        RECT wr{};
        GetWindowRect(st->hwnd, &wr);
        char t[260];
        std::snprintf(t, sizeof(t),
                      "FULLSCREEN on=%d style=0x%08llX client=(%ld,%ld) "
                      "window=(%ld,%ld,%ld,%ld)",
                      st->window_fullscreen ? 1 : 0,
                      static_cast<unsigned long long>(
                          GetWindowLongPtrW(st->hwnd, GWL_STYLE)),
                      cc.right, cc.bottom, wr.left, wr.top, wr.right, wr.bottom);
        AppendUiLog(t);
    }
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

} // namespace

// URL 工具函数定义在文件后段，但下面的错误处理要用到。
// ⚠ 这两个声明必须放在匿名 namespace **之外**：放进匿名 namespace 会多出一个
// 内部链接的同名函数，与后面 ttplayer::video 里的定义构成重载歧义
// （error C2668 对重载函数的调用不明确）。
bool LooksPlayableMediaUrl(const std::wstring& url);
bool LooksLikeHlsUrl(const std::wstring& url);

// 打开指定线路 / 启动 HLS 拼接回退：定义在文件后段，但 WndProc 里要用
// （WM_USER+4 报错回退、WM_USER+7 解析成功后重开、WM_USER+8 直连超时回退）。
void OpenSourceAt(PlayerState* st, int index);
void StartHlsConcat(PlayerState* st, const VideoPlayerSource& src);
void CloseProtocolSession(PlayerState* st);

// 切到下一条线路并重新打开（定义在 ShowVideoPlayer 附近）。
void SwitchSource(PlayerState* st);
// 控制栏「切换源」弹出全部线路名供选择（定义在 ShowVideoPlayer 附近）。
void ShowSourceMenu(PlayerState* st);
// 控制栏「选集」弹出剧集列表（定义在 ShowVideoPlayer 附近）。
void ShowEpisodeMenu(PlayerState* st);

namespace {

// 网页兜底：当前线路是 http(s) 网页（不是直链媒体）且解析全部失败时，
// 用 exe 注入的 WebView2 工厂在画面区加载该网页播放。
// 返回 true 表示已接管显示（调用方不要再画「播放失败」浮层）。
bool TryStartWebFallback(PlayerState* st) {
    if (!st) return false;
    if (st->web_mode && st->web_host) {  // 已在网页模式：保持
        st->error_text.clear();
        return true;
    }
    const VideoWebHostFactory factory = GetVideoWebHostFactory();
    if (!factory) return false;  // exe 未注入（例如旧宿主）：保持原有失败提示
    if (st->source_index < 0 ||
        st->source_index >= static_cast<int>(st->sources.size()))
        return false;
    const std::wstring& url = st->sources[st->source_index].url;
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0)
        return false;
    if (LooksPlayableMediaUrl(url)) return false;  // 直链媒体不交给网页
    if (IsCloudShareUrl(core::WideToUtf8(url))) {
        // 网盘分享页：不打开网页兜底（否则用户被丢到登录页）。明确提示，
        // 让用户切其它线路 / 其它源。
        st->error_text = L"该线路是网盘分享页（需登录网盘），已跳过。请切换其它线路或片源";
        return false;
    }
    if (ttplayer::video::LooksLikeJsonApiUrl(core::WideToUtf8(url))) {
        // 数据接口（返回 JSON）：打开网页兜底只会把整屏 JSON 文本糊在画面上
        // （用户截图就是这个）。宁可提示，也不要显示一屏接口数据。
        st->error_text =
            L"该线路是数据接口（返回 JSON），不是网页播放页，已跳过。\n\n"
            L"请点标题栏「切换源」换一条线路，或换一个片源。";
        return false;
    }

    st->web_host.reset();
    // ★ 需求变更：web 形态下自绘控制条**照常显示**（站点控件由注入脚本隐藏）。
    //   WebView2 只占画面区，底部控制栏属于父窗，点击不会被吞。
    st->web_mode = true;
    st->web_state = VideoWebState{};
    st->web_muted = false;
    AppendUiLog("MODE web=1 native-controls=1 url=" +
                RedactUrl(core::WideToUtf8(url)).substr(0, 160));
    LayoutPlayer(st);
    // 先按 0×0 创建：页面照常执行 JS/发请求，但不闪网页；嗅探命中会切原生。
    RECT hidden{};
    st->web_host = factory(st->hwnd, hidden, url);
    if (!st->web_host) {
        st->web_mode = false;
        LayoutPlayer(st);
        return false;
    }
    st->web_resolve_hidden = true;
    st->web_resolve_deadline = GetTickCount() + 7000;
    {
        const HWND host = st->hwnd;
        // 页面播放器点全屏 → 宿主播放窗进入真全屏（否则只是窗口内最大化）。
        st->web_host->SetFullscreenCallback([host](bool on) {
            PostMessageW(host, WM_USER + 12, on ? 1 : 0, 0);
        });
        // <video> 状态回推：只投递消息，不在 COM 回调里直接改 PlayerState
        //（窗口销毁时在途回调会踩已释放对象）。
        st->web_host->SetStateCallback(
            [host](const VideoWebState& s) {
                if (!::IsWindow(host)) return;
                auto* copy = new VideoWebState(s);
                if (!::PostMessageW(host, WM_USER + 14, 0,
                                    reinterpret_cast<LPARAM>(copy)))
                    delete copy;
            });
        // 直链嗅探：网页自己算出 m3u8 后自动切回原生播放（Bttwoo 这类站的
        // 地址由页面 WASM 现算，HTTP 层拿不到）。
        st->web_host->SetMediaSniffCallback(
            [host](const std::string& media) {
                if (!::IsWindow(host) || media.empty()) return;
                auto* copy = new std::string(media);
                if (!::PostMessageW(host, WM_USER + 15, 0,
                                    reinterpret_cast<LPARAM>(copy)))
                    delete copy;
            });
    }
    st->error_text.clear();
    // WebView2 接管画面后没有 MF 的 on_ready 回调：不清这些标志会让
    // WM_TIMER 一直按「中央浮层」整窗重绘（且 web 页上层浮层无意义）。
    st->got_ready = true;
    st->resolving = false;
    st->hls_running = false;
    st->download_running = false;
    if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
    AppendPlayFailLog("类型: 已回退网页内嵌播放（WebView2）\n影片/集名: " +
                      core::WideToUtf8(st->title_text) + "\n当前线路: " +
                      core::WideToUtf8(st->line_label) + "\n当前地址: " +
                      core::WideToUtf8(url) + "\n");
    return true;
}

}  // namespace

LRESULT CALLBACK VideoPlayerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PlayerState* st = reinterpret_cast<PlayerState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            st = reinterpret_cast<PlayerState*>(cs->lpCreateParams);
            if (st) {
                st->hwnd = hwnd;
                g_player = st;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(st));
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_CREATE: {
            if (!st) return -1;
            st->video_host = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                0, 0, 10, 10, hwnd, nullptr, nullptr, nullptr);
            if (!st->video_host) return -1;
            // 渲染子窗口不吃鼠标：命中测试会跳过被禁用的窗口（连同它内部的
            // MF 渲染子窗），于是画面区的点击 / 双击 / 移动都能落到父窗，
            // 双击画面切全屏、全屏下动鼠标唤出控件才生效。禁用不影响绘制。
            EnableWindow(st->video_host, FALSE);
            // Win11 自动圆角会让无边框 WS_POPUP 看起来仍带一圈边，先关掉。
            DisableWindowCorners(hwnd);
            st->last_input = GetTickCount();
            SetTimer(hwnd, 1, 250, nullptr);
            return 0;
        }
        case WM_USER + 14: {  // 网页 <video> 状态回推：lp = new VideoWebState
            auto* state = reinterpret_cast<VideoWebState*>(lp);
            if (state) {
                if (st && st->web_mode) {
                    st->web_state = *state;
                    st->playing = st->web_state.playing;
                    if (st->footer.right > st->footer.left)
                        InvalidateRect(hwnd, &st->footer, FALSE);
                }
                delete state;
            }
            return 0;
        }
        case WM_USER + 15: {  // 网页直链嗅探命中：lp = new std::string(媒体地址)
            auto* sniffed = reinterpret_cast<std::string*>(lp);
            if (!sniffed) return 0;
            const std::string media = *sniffed;
            delete sniffed;
            if (!st || !st->web_mode || media.empty()) return 0;
            AppendUiLog("SNIFF media=" + RedactUrl(media).substr(0, 160));
            // 拿到真实直链：把网页线路换成这条直链，走原生播放链
            //（OpenSourceAt 内部会拆网页宿主、恢复原生控制条、重新解析/HLS 代理）。
            VideoPlayerSource sniff_src;
            sniff_src.url = core::Utf8ToWide(media);
            sniff_src.name = L"网页嗅探";
            st->sources.push_back(std::move(sniff_src));
            const int idx = static_cast<int>(st->sources.size()) - 1;
            st->web_resolve_hidden = false;
            OpenSourceAt(st, idx);
            return 0;
        }
        case WM_USER + 5:  // HLS 拼接进度（百分比）
            if (st) {
                st->hls_percent = static_cast<int>(wp);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_USER + 10:  // HLS 分段 #EXTINF 之和（毫秒）→ 时长兜底
            if (st) {
                const double seconds = static_cast<double>(wp) / 1000.0;
                st->duration_hint = SafeTime(seconds);
                if (st->footer.right > st->footer.left)
                    InvalidateRect(hwnd, &st->footer, FALSE);
            }
            return 0;
        case WM_USER + 13:  // 本地代理不支持 Range（AES-128 HLS）
            if (st) {
                st->seek_unsupported = true;
                AppendUiLog("SEEK unsupported: proxy 无 Range（AES-128 HLS）");
                if (st->footer.right > st->footer.left)
                    InvalidateRect(hwnd, &st->footer, FALSE);
            }
            return 0;
        case WM_USER + 12: {  // 网页播放器请求全屏 / 退出
            if (!st) return 0;
            const bool want = wp != 0;
            AppendUiLog(std::string("WEBBFS want=") + (want ? "1" : "0") +
                        " current=" + (st->window_fullscreen ? "1" : "0"));
            if (want != st->window_fullscreen) ToggleFullscreen(st);
            return 0;
        }
        case WM_USER + 11:  // HLS 时长探测（wp=毫秒，lp=打开的序号）
            if (st && static_cast<unsigned>(lp) == st->open_serial) {
                st->duration_hint =
                    SafeTime(static_cast<double>(wp) / 1000.0);
                if (st->footer.right > st->footer.left)
                    InvalidateRect(hwnd, &st->footer, FALSE);
            }
            return 0;
        case WM_USER + 6: {  // HLS 拼接完成：lp = new std::wstring(本地 ts 路径)
            if (st) {
                std::wstring* path = reinterpret_cast<std::wstring*>(lp);
                if (path) {
                    if (!path->empty() && st->backend) {
                        st->hls_running = false;
                        st->download_running = false;
                        st->hls_prepared = *path;
                        // 拼接完成，渲染子窗口重新放回来，让 MF 把本地 ts 画出来。
                        // 先隐藏再开新引擎：避免旧引擎在可见态被拆掉时 video_host 灰闪。
                        if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
                        st->opened_ok = st->backend->Open(st->video_host, *path,
                                                          {});
                        if (st->opened_ok) st->backend->Play();
                        else {
                            st->error_text = L"无法打开拼接后的视频文件";
                            if (st->video_host)
                                ShowWindow(st->video_host, SW_HIDE);
                        }
                    }
                    delete path;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_USER + 7: {  // 解析成功：lp = new ResolvedMedia(直链 + 请求头)
            if (st) {
                ResolvedMedia* resolved = reinterpret_cast<ResolvedMedia*>(lp);
                st->resolving = false;
                if (resolved) {
                    // 播放器返回的 Referer / UA / 鉴权头必须在重开之前并进去：
                    // MF 后端、HLS 拼接线程、时长探测都读 st->open_headers。
                    for (const auto& kv : resolved->headers)
                        if (!kv.first.empty() && !kv.second.empty())
                            st->open_headers[kv.first] = kv.second;
                    if (!resolved->url.empty() &&
                        st->source_index >= 0 &&
                        st->source_index < static_cast<int>(st->sources.size())) {
                        // 把解析出的直链写回当前线路，后续「切换源」循环也用它。
                        st->sources[st->source_index].url = resolved->url;
                        OpenSourceAt(st, st->source_index);
                    }
                    delete resolved;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_USER + 9: {  // 协议插件/解析层失败：lp = _strdup 的 UTF-8 详细原因
            // 与 WM_USER+4（MF 解码错误）分开：这里的原因必须原样透出，
            // 不能被「该线路是网页播放地址」这种笼统文案覆盖 —— 用户需要
            // 知道是哪个解析接口失败、播放页里有没有直链。
            if (st) {
                st->playing = false;
                st->resolving = false;
                st->hls_running = false;
                st->download_running = false;
                if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
                if (lp) {
                    st->error_text = core::Utf8ToWide(
                        reinterpret_cast<const char*>(lp));
                    free(reinterpret_cast<void*>(lp));
                }
                if (st->error_text.empty())
                    st->error_text = L"播放地址解析失败（原因未返回）";
                // 把无法播放的源信息完整落盘：以后不用再点一遍程序，
                // 直接看 <exe>\video\play_fail.log 就能定位是哪条线路、
                // 什么地址、有没有解析接口、失败原因是什么。
                std::string log = "类型: 协议/解析层播放失败\n";
                log += "影片/集名: " + core::WideToUtf8(st->title_text) + "\n";
                log += "当前线路: " + core::WideToUtf8(st->line_label) + "\n";
                log += "线路数: " + std::to_string(st->sources.size()) +
                       "，当前序号: " + std::to_string(st->source_index + 1) + "\n";
                if (st->source_index >= 0 &&
                    st->source_index < static_cast<int>(st->sources.size()))
                    log += "当前地址: " +
                           core::WideToUtf8(st->sources[st->source_index].url) +
                           "\n";
                log += "配置解析接口(parses): " +
                       std::to_string(st->parses.size()) + " 个\n";
                log += "失败原因: " + core::WideToUtf8(st->error_text) + "\n";
                log += "该片所有线路:\n";
                for (size_t i = 0; i < st->sources.size(); ++i)
                    log += "  [" + std::to_string(i + 1) + "] " +
                           core::WideToUtf8(st->sources[i].name) + " -> " +
                           core::WideToUtf8(st->sources[i].url) + "\n";
                AppendPlayFailLog(log);
                // 同源重新解析：网页线路每次打开都会重新抓播放页、拿新令牌
                //（hhplayer 的 auth_key 是一次性/限流，重开即可恢复）。
                // 每条线路只重试一次，仍失败才进入自动换线。
                if (st->source_index >= 0 &&
                    st->same_src_retry_index != st->source_index &&
                    st->source_index <
                        static_cast<int>(st->sources.size())) {
                    const std::wstring& current =
                        st->sources[st->source_index].url;
                    const std::string current_utf8 = core::WideToUtf8(current);
                    // 网盘分享页 / JSON 数据接口重开多少次都一样，别浪费一次
                    // 重新抓页+解析，直接进入自动换线。
                    if (!LooksPlayableMediaUrl(current) &&
                        !LooksLikeHlsUrl(current) &&
                        !IsCloudShareUrl(current_utf8) &&
                        !LooksLikeJsonApiUrl(current_utf8)) {
                        st->same_src_retry_index = st->source_index;
                        st->resolve_retry_pending = true;
                        SetTimer(hwnd, 2, 700, nullptr);
                        AppendUiLog(
                            "RETRY same-source resolve #" +
                            std::to_string(st->source_index + 1) + " " +
                            core::WideToUtf8(st->sources[st->source_index].name));
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
                // 同一集的其它线路先试一遍再去开网页：影视仓的「多线」里经常
                // 只有首条线路挂了（域名停用 / token 过期），以前会直接退回
                // 内嵌网页，看起来就是「一开播就变网页、而且根本播不了」。
                if (st->sources.size() > 1 && st->source_index >= 0 &&
                    st->auto_source_tried + 1 <
                        static_cast<int>(st->sources.size())) {
                    ++st->auto_source_tried;
                    const int next =
                        (st->source_index + 1) %
                        static_cast<int>(st->sources.size());
                    AppendUiLog("AUTO next source #" +
                                std::to_string(next + 1) + " " +
                                core::WideToUtf8(st->sources[next].name));
                    OpenSourceAt(st, next);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                // 所有解析接口都失败：官方网页线路直接在播放窗内加载网页播放，
                // 不再只显示「播放失败」。（直链媒体 / 无工厂时返回 false）
                if (TryStartWebFallback(st)) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_USER + 8: {  // MF 直连 HLS 超时 → 回退到「全量拼接」路径
            if (st && st->hls_direct && !st->got_ready) {
                st->hls_direct = false;
                if (st->backend) st->backend->Close();
                st->opened_ok = false;
                if (st->source_index >= 0 &&
                    st->source_index < static_cast<int>(st->sources.size())) {
                    const VideoPlayerSource src = st->sources[st->source_index];
                    if (st->hls_shared) st->hls_shared->cancel = true;
                    st->hls_shared.reset();
                    StartHlsConcat(st, src);
                }
            }
            return 0;
        }
        case WM_DESTROY:
            // 关闭时记住位置/大小。GetWindowPlacement 的 rcNormalPosition 是
            // 「还原态」矩形，全屏/最大化时保存的也仍是上次的窗口位置。
            SaveVideoWindowPlacement(hwnd);
            KillTimer(hwnd, 3);
            if (st && !st->history_id.empty()) {
                const double pos = CurrentPosition(st);
                const double dur = EffectiveDuration(st);
                UpdateVideoHistoryProgress(
                    st->history_id, st->current_ep,
                    pos > 0.0 ? static_cast<int>(pos * 1000) : 0,
                    dur > 0.0 ? static_cast<int>(dur * 1000) : 0);
                AppendUiLog("HISTORY close save id=" +
                            core::WideToUtf8(st->history_id) + " pos=" +
                            std::to_string(static_cast<int>(pos)) + "s");
            }
            if (st && st->hls_shared) st->hls_shared->cancel = true;
            if (st) CloseProtocolSession(st);   // 协议插件会话（VPClose 内部 join）
            if (st && st->backend) st->backend->Close();
            if (st) { st->web_host.reset(); st->web_mode = false; }
            if (st) { delete st; g_player = nullptr; }
            return 0;
        case WM_USER + 1:  // on_ready：有画面了
            if (st) {
                st->got_ready = true;
                st->playing = true;
                st->auto_source_tried = 0;   // 出画面即视为本条线路可用
                st->same_src_retry_index = -1;   // 下一条故障线路仍可同源重试
                st->resolve_retry_pending = false;
                KillTimer(hwnd, 2);
                // 续播：首帧就绪后跳到上次位置（只跳一次；换集/换片会重置）。
                if (!st->start_applied && st->pending_start_ms > 1) {
                    st->start_applied = true;
                    double target = st->pending_start_ms / 1000.0;
                    const double dur = EffectiveDuration(st);
                    if (dur > 0.0)
                        target = std::min(target, std::max(0.0, dur - 2.0));
                    if (target > 1.0) {
                        SeekTo(st, target);
                        AppendUiLog("HISTORY resume " +
                                    std::to_string(static_cast<int>(target)) +
                                    "s id=" + core::WideToUtf8(st->history_id));
                    }
                    st->pending_start_ms = 0;
                }
                // 协议插件解析/缓冲期间用过这两个标志；出画面后必须清掉，
                // 否则中央浮层会一直盖着「正在解析…/正在缓冲 0%」。
                st->resolving = false;
                st->hls_running = false;
                // 直连 HLS 成功 → 取消兜底回退线程（它 8 秒后会误触发回退）。
                if (st->hls_direct) {
                    st->hls_direct = false;
                    if (st->hls_shared) st->hls_shared->cancel = true;
                }
                // 真正出画面时才把渲染子窗口放出来（此前为了露出浮层藏起来了）。
                // 网页兜底模式下画面区归 WebView2，不能再把 MF 子窗口放出来。
                if (st->video_host && !st->web_mode)
                    ShowWindow(st->video_host, SW_SHOW);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_USER + 2:  // on_playing
            if (st) {
                st->playing = (wp != 0);
                if (st->footer.bottom > st->footer.top)
                    InvalidateRect(hwnd, &st->footer, FALSE);
            }
            return 0;
        case WM_USER + 3:  // on_end
            if (st) {
                st->playing = false;
                if (st->footer.bottom > st->footer.top)
                    InvalidateRect(hwnd, &st->footer, FALSE);
            }
            return 0;
        case WM_USER + 4: {  // on_error
            if (st) {
                // ★ 本地 HLS 代理偶发断流（分片暂未就绪 / 旧 RANGE probe 抢
                //   资源）会让 MF 在开播几秒后报「解码或网络错误」。第一次
                //   ERROR 不直接判死，用不带起点的原始 media URL 重开一次。
                //   error_retried 在换源/换集时清零，不会无限重试。
                if (!st->error_retried && !st->web_mode &&
                    NeedsRestartSeek(st) && st->backend && st->video_host) {
                    st->error_retried = true;
                    if (lp) {
                        st->error_text = core::Utf8ToWide(
                            reinterpret_cast<const char*>(lp));
                        free(reinterpret_cast<void*>(lp));
                        lp = 0;   // 下面的正常路径不要二次 free
                    }
                    st->seek_offset = 0.0;
                    st->play_clock_base = 0.0;
                    st->play_clock_tick = GetTickCount();
                    st->opened_ok = st->backend->Open(
                        st->video_host, st->protocol_media_url, {});
                    if (st->opened_ok) {
                        st->backend->Play();
                        st->playing = true;
                        st->got_ready = true;
                        st->resolving = false;
                        st->hls_running = false;
                        st->download_running = false;
                        st->error_text.clear();
                        ShowWindow(st->video_host, SW_SHOW);
                        AppendUiLog("ERROR auto-retry original proxy url (once)");
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    // 重开失败：保留 error_text，继续走下面的错误显示。
                }
                st->playing = false;
                st->download_running = false;   // 下载回退失败也要复位浮层状态
                // MF 直连 HLS 报错 = 本机没有 HLS 源 → 不算失败，回退拼接。
                if (st->hls_direct) {
                    st->hls_direct = false;
                    if (st->backend) st->backend->Close();
                    st->opened_ok = false;
                    if (st->source_index >= 0 &&
                        st->source_index < static_cast<int>(st->sources.size())) {
                        const VideoPlayerSource src = st->sources[st->source_index];
                        if (st->hls_shared) st->hls_shared->cancel = true;
                        st->hls_shared.reset();
                        StartHlsConcat(st, src);
                    }
                    return 0;
                }
                // 同样要藏渲染子窗口，否则父窗的「播放失败」浮层被它盖住。
                if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
                if (lp) {
                    st->error_text = core::Utf8ToWide(
                        reinterpret_cast<const char*>(lp));
                    free(reinterpret_cast<void*>(lp));
                }
                // 地址本身不像直链媒体（扩展名既不是 m3u8/mp4/flv/mkv）时，
                // 多半是 CMS 给的「网页播放地址」（HTML 页面，如
                // https://host/play/xxxx）。这时笼统的「解码或网络错误」对用户
                // 毫无意义 —— 换成能照着做的提示。⚠ 只在**已经报错**的前提下改写
                // 文案，不会误伤扩展名缺失但其实能播的直链。
                if (st->source_index >= 0 &&
                    st->source_index < static_cast<int>(st->sources.size()) &&
                    !LooksPlayableMediaUrl(st->sources[st->source_index].url)) {
                    if (st->parses.empty()) {
                        st->error_text =
                            L"该线路是网页播放地址（不是直链媒体文件），且当前"
                            L"没有配置任何解析接口，播放器无法直接播放。\n\n"
                            L"请在 TTPlayer\\video\\protocols\\builtin_parses.json"
                            L" 里填入可用的解析接口（jx 接口，地址含 ?url= ），"
                            L"或导入带 parses 段的影视仓配置；也可点标题栏"
                            L"「切换源」换一条 m3u8/mp4 直链线路。";
                    } else {
                        st->error_text =
                            L"该线路是网页播放地址，已尝试内置/配置里的解析接口，"
                            L"但都没能解出直链（接口可能已失效或不支持该站点）。\n\n"
                            L"请更新 TTPlayer\\video\\protocols\\builtin_parses.json"
                            L" 中的解析接口地址，或导入带 parses 段的影视仓配置；"
                            L"也可点标题栏「切换源」换一条 m3u8/mp4 直链线路。";
                    }
                }
                // 同集还有其它线路时，解码/网络错误也自动换下一条：这类错误
                // 同样常是本条线路的问题（直播看板分片 404、CDN 丢包），
                // 以前只提示错误、要用户手动点「切换源」。
                // 只在「这一条从没出过画面」或「重开一次后仍失败」时换，
                // 避免把偶发的解码抖动当成线路故障。
                if (!st->web_mode && (!st->got_ready || st->error_retried) &&
                    st->sources.size() > 1 && st->source_index >= 0 &&
                    st->auto_source_tried + 1 <
                        static_cast<int>(st->sources.size())) {
                    ++st->auto_source_tried;
                    const int next =
                        (st->source_index + 1) %
                        static_cast<int>(st->sources.size());
                    AppendUiLog("AUTO next source after play error #" +
                                std::to_string(next + 1) + " " +
                                core::WideToUtf8(st->sources[next].name));
                    OpenSourceAt(st, next);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                // 网页线路解析/代理播放失败：同样回退到内嵌网页播放。
                if (TryStartWebFallback(st)) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_EXITSIZEMOVE:
            // 拖动/缩放结束就落盘：即使进程随后被强杀，下次仍回到这个位置。
            SaveVideoWindowPlacement(hwnd);
            break;
        case WM_SIZE:
            LayoutPlayer(st);
            ApplyWindowRegionForState(st);  // 与 ToggleFullscreen 同一个入口，不再三处重复
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_SETCURSOR: {
            // 光标随内容变化：控制栏按钮 / 进度条 / 音量条用小手，其它区域
            // 保持箭头（此前窗口类没设 hCursor，整窗永远是箭头，按钮上也没有
            // 「可点」提示，和音乐窗的「全是手」是同一类问题）。
            if (LOWORD(lp) != HTCLIENT || !st) break;
            // 控制条隐藏期间连鼠标指针一起隐藏，观感与其它播放器一致。
            if (ControlsHidden(st)) { SetCursor(nullptr); return TRUE; }
            POINT p{};
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            const bool hand = HitTestId(st, p) != 0;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_NCHITTEST: {
            // 真全屏：一律 HTCLIENT，不再返回四边缩放带 / 标题栏拖动命中，
            // 否则鼠标靠近屏幕边缘仍出现「窗口边框可缩放」的观感与光标。
            if (st && st->window_fullscreen) return HTCLIENT;
            const LRESULT base = DefWindowProcW(hwnd, msg, wp, lp);
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            if (st && st->skinned && !ControlsHidden(st)) {
                // 标题条（不含按钮）当作拖动条
                if (PtInRect(&st->title, p) &&
                    !PtInRect(&st->close_btn, p) &&
                    !PtInRect(&st->source_btn, p))
                    return HTCAPTION;
                // 四边 + 四角缩放带：必须用**客户区**边界。以前用
                // st->content.bottom（= 控制栏顶）判断下边，鼠标一进控制栏
                // 就被当成底部缩放带，光标永远是双向箭头（用户实测）。
                RECT cc{};
                GetClientRect(hwnd, &cc);
                const int band = frame::kResizeBand;
                const bool top = p.y < band;
                const bool bottom = p.y >= (cc.bottom - band);
                const bool left = p.x < band;
                const bool right = p.x >= (cc.right - band);
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                return HTCLIENT;
            }
            if (st && !st->skinned && !ControlsHidden(st)) {
                // 客户区顶部的标题栏充当拖动条（保留边缘调整大小）。
                if (base == HTCLIENT &&
                    PtInRect(&st->title, p) &&
                    !PtInRect(&st->close_btn, p) && !PtInRect(&st->fs_btn, p))
                    return HTCAPTION;
            }
            return base;
        }
        case WM_ERASEBKGND:
            // WM_PAINT 会全量重绘整个客户区（皮肤背景 + 标题 + 控制栏），无需擦除
            // 背景；默认擦除会用 BLACK_BRUSH 把整窗刷黑，与重绘之间存在可见的「黑闪」，
            // 切换源 / 缓冲 / 出错时的整窗闪烁即源于此。直接返回已处理，消除闪烁。
            return 1;
        case WM_PAINT: {
            if (!st) break;
            PAINTSTRUCT ps{};
            HDC screen = BeginPaint(hwnd, &ps);
            RECT c{};
            GetClientRect(hwnd, &c);
            const bool hide = ControlsHidden(st);
            // ---- 整窗双缓冲 ------------------------------------------------
            // 以前直接画到窗口 DC：hover / 点击 / 250ms 定时器每次都会让屏幕
            // 逐块刷新（背景先黑一下再画控件），肉眼就是「整窗闪烁」。现在全部
            // 画进后台位图，最后一次 BitBlt 上屏，屏幕只会一次性更新。
            HDC dc = screen;
            HDC back_dc = nullptr;
            HBITMAP back_bmp = nullptr;
            HGDIOBJ back_old = nullptr;
            const int cw = c.right - c.left, ch = c.bottom - c.top;
            if (cw > 0 && ch > 0) {
                back_dc = CreateCompatibleDC(screen);
                if (back_dc) {
                    back_bmp = CreateCompatibleBitmap(screen, cw, ch);
                    if (back_bmp) {
                        back_old = SelectObject(back_dc, back_bmp);
                        dc = back_dc;
                    } else {
                        DeleteDC(back_dc);
                        back_dc = nullptr;
                    }
                }
            }
            // ★ 全屏不画皮肤位图：九宫格里含左 8 / 右 9 / 上 31 / 下 31 的
            //   边框像素，旧代码无条件 PaintBackground 导致全屏四周仍留一圈
            //   「窗口边框」。全屏只铺纯色底，边界与显示器完全贴合。
            if (st->skinned && !st->window_fullscreen) {
                // 兜底底色：皮肤位图里完全透明的像素（圆角 / 左侧 logo）不再
                // 露出未绘制的黑色。WM_ERASEBKGND 已被吃掉，不先铺底就是黑块。
                theme::FillSolidRect(dc, c, kVidBg);
                // 九宫格铺满皮肤位图：边框 / 标题条 / 内容区保持原始像素，
                // 只有 content 区随窗口变大 —— 与音乐窗同一套绘制管线。
                frame::PaintBackground(dc, *st->browser_skin, st->skin_layout, c);
                // 与音乐窗（musicfree_browser_window.cpp PaintFrame）完全同一套：
                // 窗口被拉宽后九宫格顶行中间格会被横向拉伸，把皮肤位图里自带的
                // 标题文字 / 左上角 logo 拉成变形条。这里按 1:1 从源位图重画
                // 左上角标题区，右侧渐变继续跟随窗口拉伸 —— 左上角因此与音乐窗
                // 逐像素一致（不再用取样色整块覆盖）。
                {
                    const int natural_w = st->skin_layout.natural.cx;
                    if (natural_w > 0 && (c.right - c.left) > natural_w) {
                        const int cluster_right = std::max(
                            std::max(st->browser_skin->backward.bounds.right,
                                     st->browser_skin->forward.bounds.right),
                            st->browser_skin->refresh.bounds.right);
                        const int title_w = std::min(
                            natural_w,
                            std::max(160, cluster_right + 6));
                        const int paint_h = st->skin_layout.resize_rect.top > 0
                            ? std::min<int>(st->skin_layout.resize_rect.top, 31)
                            : 31;
                        st->browser_skin->background.image.Draw(
                            dc, 0, 0, title_w, paint_h,
                            0, 0, title_w, paint_h, st->skin_transparent);
                    }
                }
                // 隐藏控制条时标题栏仍要画（窗口最大化 ≠ 真全屏）：否则连
                // 关闭按钮都消失，用户只能靠 Alt+F4 退出。
                if (!hide || !st->window_fullscreen) {
                    // 关闭按钮用皮肤自带位图（hover 高亮由状态帧体现）。
                    skin::DrawFrameButton(dc, st->browser_skin->close, st->close_btn,
                                          st->hover_id == 1 ? 1 : 0,
                                          st->skin_transparent);
                }
                // 视频区铺深色底：皮肤内容区本身是**白色**的，白底既看不出视频
                // 区域边界，浮层文字（kVidDim 灰蓝）也几乎不可见 —— 播放失败时
                // 用户只看到一片空白，没有任何提示（用户反馈「一直无法正常播放」）。
                // 真正出画面时 MF 的渲染子窗口负责画面；父窗只负责 letterbox
                // 黑边。WS_CLIPCHILDREN 会自动裁掉子窗口覆盖区，所以可以安全
                // 地把整个 content 铺黑，只有等比矩形之外的部分会被画出来。
                theme::FillSolidRect(dc, st->content, RGB(0, 0, 0));
            } else {
                theme::FillSolidRect(dc, c, kVidBg);
                if (st->content.right > st->content.left)
                    theme::FillSolidRect(dc, st->content, RGB(0, 0, 0));
            }
            // 控制条隐藏时把原本控制栏那条也填黑（只补背景色，几何不变），
            // 避免底部留下一条与控制栏等高的深色块。
            if (hide) theme::FillSolidRect(dc, st->footer, RGB(0, 0, 0));
            // 真全屏隐藏时连标题栏一起不画；窗口最大化隐藏只收控制条，标题栏
            // 保留（footer 已清空，下面的控制栏段本身不会绘制）。
            if (!hide || !st->window_fullscreen) {
                // 全屏没有标题栏 / 关闭按钮；只保留底部自绘控制条。
                if (!st->window_fullscreen) {
                // ---- 标题栏 -------------------------------------------------
                if (!st->skinned) {
                    theme::FillSolidRect(dc, st->title, kVidBar);
                    theme::FillSolidRect(dc, {st->title.left, st->title.bottom - 2,
                                              st->title.right, st->title.bottom},
                                         kVidAccent);
                }
                // 皮肤标题栏：标题文字与左上角 logo 都由皮肤位图自带（和音乐窗
                // 一致），这里不再叠加应用图标 / 片名，否则会和皮肤标题重叠。
                // 只有无皮肤兜底形态才自绘图标 + 标题 + 线路标签。
                if (!st->skinned) {
                    // 左上角应用图标：WS_POPUP 自绘窗不会自动显示图标。
                    const int icon_y = (st->title.bottom - 16) / 2;
                    HICON ic = st->app_icon ? st->app_icon
                                            : LoadIconW(nullptr, IDI_APPLICATION);
                    if (ic)
                        DrawIconEx(dc, 9, icon_y > 0 ? icon_y : 0, ic, 16, 16,
                                   0, nullptr, DI_NORMAL);
                    // 标题：白字 + 1px 暗影，保证在任意底色可读。
                    theme::DrawTextIn(dc,
                                      {st->title_rect.left + 1,
                                       st->title_rect.top + 1,
                                       st->title_rect.right + 1,
                                       st->title_rect.bottom + 1},
                                      st->title_text.c_str(), RGB(0, 0, 0),
                                      PlayerTitleFont(),
                                      theme::kLabelFormat | DT_END_ELLIPSIS);
                    theme::DrawTextIn(dc, st->title_rect,
                                      st->title_text.c_str(), kVidText,
                                      PlayerTitleFont(),
                                      theme::kLabelFormat | DT_END_ELLIPSIS);
                    if (!st->line_label.empty()) {
                        // 线路标签画在标题后面：先量出标题实际宽度。
                        SIZE sz{};
                        const HGDIOBJ old_font =
                            SelectObject(dc, PlayerTitleFont());
                        GetTextExtentPoint32W(
                            dc, st->title_text.c_str(),
                            static_cast<int>(st->title_text.size()), &sz);
                        SelectObject(dc, old_font);
                        const RECT lr = {st->title_rect.left + sz.cx + 8, 0,
                                         st->title_rect.right - 8,
                                         st->title.bottom};
                        theme::DrawTextIn(
                            dc, lr, (L"线路 " + st->line_label).c_str(),
                            kVidDim, PlayerSmallFont(),
                            theme::kLabelFormat | DT_END_ELLIPSIS);
                    }
                }
                // 切换源按钮已移到下方控制栏（用户要求），标题栏不再画。
                // 关闭（皮肤窗口用皮肤位图，已在上面画过）
                if (!st->skinned) {
                    if (st->hover_id == 1)
                        theme::DrawRoundedBox(dc, st->close_btn, RGB(198, 62, 62),
                                              CLR_INVALID, 4);
                    theme::DrawTextIn(dc, st->close_btn, L"✕",
                                      st->hover_id == 1 ? kVidText : kVidDim,
                                      PlayerSmallFont(), theme::kCenterFormat);
                }
                }  // if (!window_fullscreen)：标题栏绘制结束
                // ---- 控制栏 -------------------------------------------------
                // 原生形态 / web 形态 / 全屏都画这一层自绘控制条；只有 footer
                // 为空（控件被显式隐藏）时才跳过，避免空矩形把图标画到左上角。
                if (!hide && st->footer.bottom > st->footer.top) {
                // 整窗已双缓冲，这里直接画进后台位图即可（不再嵌套一层）。
                theme::FillSolidRect(dc, st->footer, kVidBar);
                theme::FillSolidRect(dc, {st->footer.left, st->footer.top,
                                          st->footer.right, st->footer.top + 1},
                                     kVidBarLine);
                // 圆形播放键
                {
                    const bool hot = st->hover_id == 2;
                    FillEllipseAA(dc, st->play_btn,
                                  hot ? kVidAccentHot : kVidAccent);
                    if (IsPlayingNow(st)) PaintGlyphPause(dc, st->play_btn);
                    else PaintGlyphPlay(dc, st->play_btn);
                }
                // 进度条：轨道 + 已播填充 + 圆形滑块。时长一律走
                // EffectiveDuration（NaN/0 时用 HLS 分段累加值，仍未知则按 0 处理）。
                const double dur = EffectiveDuration(st);
                const double pos = st->dragging_progress
                                       ? (dur > 0.0 ? st->drag_pos : CurrentPosition(st))
                                       : CurrentPosition(st);
                double ratio = dur > 0.0 ? std::min(1.0, std::max(0.0, pos / dur)) : 0.0;
                if (dur <= 0.0 && st->dragging_progress) ratio = st->drag_ratio;
                theme::DrawRoundedBox(dc, st->progress, kVidTrack, CLR_INVALID, 3);
                int thumb_x = st->progress.left;
                const int span = st->progress.right - st->progress.left;
                if (span > 0) {
                    const int fill_w = static_cast<int>(span * ratio);
                    if (dur > 0.0 && fill_w > 0) {
                        RECT fill = st->progress;
                        fill.right = fill.left + fill_w;
                        theme::DrawRoundedBox(dc, fill, kVidAccent, CLR_INVALID, 3);
                    }
                    thumb_x = st->progress.left + fill_w;
                }
                const int cy = (st->footer.top + st->footer.bottom) / 2;
                {
                    const int r = (st->hover_id == 5 || st->dragging_progress)
                                      ? 8 : 6;
                    FillEllipseAA(dc, {thumb_x - r, cy - r, thumb_x + r, cy + r},
                                  kVidText);
                }
                // 时间：时长未知显示 --:--（FormatTime 已挡住 NaN）
                std::wstring t = FormatTime(pos) + L" / " +
                                 (dur > 0.0 ? FormatTime(dur) : L"--:--");
                // 不再显示「该线路不支持拖动」：代理流现在通过按起点重启
                // 会话实现跳转，该提示只会误导用户以为拖不了。
                theme::DrawTextIn(dc, st->time_text, t.c_str(), kVidDim,
                                  PlayerSmallFont(), theme::kCenterFormat);
                // 音量按钮 + 滑条
                const bool muted = IsMutedNow(st);
                if (st->hover_id == 3)
                    theme::DrawRoundedBox(dc, st->vol_btn, kVidHot,
                                          CLR_INVALID, 4);
                theme::DrawTextIn(dc, st->vol_btn, muted ? L"静音" : L"音量",
                                  muted ? kVidError
                                        : (st->hover_id == 3 ? kVidText : kVidDim),
                                  PlayerSmallFont(), theme::kCenterFormat);
                theme::DrawRoundedBox(dc, st->vol_slider, kVidTrack,
                                      CLR_INVALID, 3);
                {
                    const double vol = !muted ? st->volume : 0.0;
                    RECT fill = st->vol_slider;
                    fill.right = fill.left + static_cast<LONG>(
                        (fill.right - fill.left) * std::min(1.0, vol));
                    if (fill.right > fill.left)
                        theme::DrawRoundedBox(dc, fill, kVidAccent, CLR_INVALID, 3);
                    const int vx = std::max(fill.right, st->vol_slider.left);
                    FillEllipseAA(dc, {vx - 6, cy - 6, vx + 6, cy + 6}, kVidText);
                }
                // 选集（剧集才有）：显示当前集名，点开列出全部集数。
                if (st->episodes.size() > 1 &&
                    st->episode_btn.right > st->episode_btn.left) {
                    const bool hot = st->hover_id == 8;
                    theme::DrawRoundedBox(dc, st->episode_btn,
                                          hot ? kVidAccentHot : kVidHot,
                                          kVidBarLine, 4);
                    std::wstring label = L"选集";
                    // 当前集：按 URL 匹配，匹配不到就只显示「选集」。
                    for (const auto& ep : st->episodes) {
                        if (st->source_index >= 0 &&
                            st->source_index <
                                static_cast<int>(st->sources.size()) &&
                            ep.url == st->sources[st->source_index].url &&
                            !ep.title.empty()) {
                            label = ep.title;
                            break;
                        }
                    }
                    if (label.size() > 8) label = label.substr(0, 7) + L"…";
                    theme::DrawTextIn(dc, st->episode_btn, label.c_str(),
                                      kVidText, PlayerSmallFont(),
                                      theme::kCenterFormat);
                }
                // 切换源（控制栏内，紧挨全屏键）：按钮直接显示当前线路名，
                // 点击弹菜单列出全部线路名供选择，不再只是循环切下一个。
                if (st->sources.size() > 1 &&
                    st->source_btn.right > st->source_btn.left) {
                    const bool hot = st->hover_id == 7;
                    theme::DrawRoundedBox(dc, st->source_btn,
                                          hot ? kVidAccentHot : kVidHot,
                                          kVidBarLine, 4);
                    std::wstring label = L"切换源";
                    if (st->source_index >= 0 &&
                        st->source_index < static_cast<int>(st->sources.size()) &&
                        !st->sources[st->source_index].name.empty())
                        label = st->sources[st->source_index].name;
                    if (label.size() > 10) label = label.substr(0, 9) + L"…";
                    theme::DrawTextIn(dc, st->source_btn, label.c_str(),
                                      kVidText, PlayerSmallFont(),
                                      theme::kCenterFormat);
                }
                // 全屏 / 还原按钮：必须画在控制栏底色之后（旧代码把它画在标题
                // 栏阶段，随后被这里的 kVidBar 整栏填底盖掉，所以永远看不见）。
                theme::DrawRoundedBox(dc, st->fs_btn,
                                      st->hover_id == 4 ? kVidAccentHot : kVidHot,
                                      kVidBarLine, 4);
                if (st->window_fullscreen)
                    PaintGlyphRestore(dc, st->fs_btn, kVidText);
                else
                    PaintGlyphFullscreen(dc, st->fs_btn, kVidText);
                }   // if (footer 非空)
            }
            // ---- 中央浮层：错误 / 加载中 ------------------------------------
            // 网页兜底接管画面区时不再画「播放失败」浮层。
            if (!st->error_text.empty() && !st->web_mode) {
                const int cx = c.right / 2, cym = c.bottom / 2;
                const RECT box = {cx - 190, cym - 46, cx + 190, cym + 46};
                theme::DrawRoundedBox(dc, box, RGB(30, 22, 24), kVidError, 8);
                theme::DrawTextIn(dc, {box.left + 14, box.top + 8, box.right - 14,
                                       box.top + 30},
                                  L"播放失败", kVidError, PlayerSmallFont(),
                                  theme::kCenterFormat);
                theme::DrawTextIn(dc, {box.left + 14, box.top + 32, box.right - 14,
                                       box.bottom - 8},
                                  st->error_text.c_str(), kVidDim,
                                  PlayerSmallFont(),
                                  DT_CENTER | DT_WORDBREAK | DT_NOPREFIX |
                                      DT_END_ELLIPSIS);
            } else if (st->hls_running) {
                // HLS 拼接中：显示缓冲百分比（分段拼接完成度）。
                const int pct = st->hls_percent.load();
                const int cx = c.right / 2, cym = c.bottom / 2;
                theme::DrawTextIn(dc, {cx - 150, cym - 16, cx + 150, cym + 16},
                                  (L"正在缓冲 " + std::to_wstring(pct) +
                                   L"%").c_str(),
                                  kVidDim, PlayerSmallFont(),
                                  theme::kCenterFormat);
            } else if (st->download_running) {
                // 带请求头下载落盘中（Referer/鉴权直链 MF 无法直接播放的回退路径）。
                const int cx = c.right / 2, cym = c.bottom / 2;
                theme::DrawTextIn(dc, {cx - 150, cym - 16, cx + 150, cym + 16},
                                  L"正在准备播放…", kVidDim, PlayerSmallFont(),
                                  theme::kCenterFormat);
            } else if (st->resolving) {
                // 正在把「播放页地址」解析成媒体直链（影视仓 parses / 爬虫）。
                const int cx = c.right / 2, cym = c.bottom / 2;
                theme::DrawTextIn(dc, {cx - 170, cym - 16, cx + 170, cym + 16},
                                  L"正在解析播放地址…", kVidDim, PlayerSmallFont(),
                                  theme::kCenterFormat);
            } else if (!st->got_ready) {
                const int dots = static_cast<int>((GetTickCount() / 400) % 4);
                const std::wstring text = L"正在加载" + std::wstring(dots, L'·');
                const int cx = c.right / 2, cym = c.bottom / 2;
                theme::DrawTextIn(dc, {cx - 120, cym - 16, cx + 120, cym + 16},
                                  text.c_str(), kVidDim, PlayerSmallFont(),
                                  theme::kCenterFormat);
            }
            // 后台位图一次性上屏：屏幕只在这里更新一次，避免逐块刷新闪烁。
            if (dc != screen) {
                BitBlt(screen, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
                SelectObject(dc, back_old);
                DeleteObject(back_bmp);
                DeleteDC(dc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!st) break;
            TouchInput(st);
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int id = HitTestId(st, p);
            {
                char t[220];
                std::snprintf(
                    t, sizeof(t),
                    "CLICK id=%d pt=(%ld,%ld) progress=(%ld,%ld,%ld,%ld) "
                    "footer=(%ld,%ld,%ld,%ld) web=%d",
                    id, p.x, p.y, st->progress.left, st->progress.top,
                    st->progress.right, st->progress.bottom, st->footer.left,
                    st->footer.top, st->footer.right, st->footer.bottom,
                    st->web_mode ? 1 : 0);
                AppendUiLog(t);
            }
            // web 模式没有 MF 后端，但自绘进度条必须照样可拖（用户要求）。
            if (id == 5) {
                SetForegroundWindow(hwnd);
                SetFocus(hwnd);
                st->dragging_progress = true;
                st->last_drag_seek = 0;   // 保证按下这一次一定发出 Seek
                SetCapture(hwnd);
                // 代理流要重开会话，按下时只更新预览，松手才真正跳一次。
                UpdateDragPos(st, p, !NeedsRestartSeek(st));
                break;
            }
            if (id == 6) {
                st->dragging_volume = true;
                SetCapture(hwnd);
                UpdateVolume(st, p);
                break;
            }
            if (id) {
                st->pressed_id = id;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            if (PtInRect(&st->content, p)) {
                // 双击画面切换全屏（渲染子窗口已禁用，点击能落到这里）。
                const DWORD now = GetMessageTime();
                const bool double_clicked =
                    st->last_click_time != 0 &&
                    now - st->last_click_time < GetDoubleClickTime();
                if (double_clicked) {
                    // 双击不是「单击暂停」：取消已挂起的切换，且本次抬起不再
                    // 触发暂停（否则双击会「暂停→继续」闪一下）。
                    st->pending_content_toggle = false;
                    st->content_double_clicked = true;
                    KillTimer(hwnd, 3);
                    ToggleFullscreen(st);
                }
                st->last_click_time = now;
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (!st) break;
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (st->dragging_progress) {
                st->dragging_progress = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                UpdateDragPos(st, p, false);   // 只更新预览，不在这里再 Seek
                // 松开终跳：一次点击只发一次终跳，时长未知则不 Seek。
                // 代理流在这里重开会话；seek_unsupported 不再作为拦截条件。
                const double dur = EffectiveDuration(st);
                if (dur > 0.0 && std::isfinite(st->drag_pos)) {
                    SeekTo(st, std::min(st->drag_pos, dur));
                } else if (st->web_mode && st->web_host) {
                    // 网页形态没回推 duration 时按比例跳，不能因为宿主拿不到
                    // 时长就完全不动作（否则又是「进度条拖不动」）。
                    st->web_host->SeekRatio(st->drag_ratio);
                    AppendUiLog("SEEK ratio=" +
                                std::to_string(st->drag_ratio) +
                                " mode=web (duration unknown)");
                }
                break;
            }
            if (st->dragging_volume) {
                st->dragging_volume = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                UpdateVolume(st, p);
                break;
            }
            const int id = st->pressed_id;
            st->pressed_id = 0;
            if (id && GetCapture() == hwnd) ReleaseCapture();
            if (id == 1 && PtInRect(&st->close_btn, p)) DestroyWindow(hwnd);
            else if (id == 4 && PtInRect(&st->fs_btn, p)) ToggleFullscreen(st);
            else if (id == 7 && PtInRect(&st->source_btn, p)) ShowSourceMenu(st);
            else if (id == 8 && PtInRect(&st->episode_btn, p)) ShowEpisodeMenu(st);
            else if (id == 2 && PtInRect(&st->play_btn, p)) {
                SetPlayingNow(st, !IsPlayingNow(st));
            } else if (id == 3 && PtInRect(&st->vol_btn, p)) {
                ToggleMuteNow(st);
            } else if (id == 0 && PtInRect(&st->content, p)) {
                // 单击画面中间：暂停 / 继续。延后一个双击间隔再执行，避免与
                // 「双击全屏」抢同一次点击（双击时由 WM_LBUTTONDOWN 取消）。
                if (st->content_double_clicked) {
                    st->content_double_clicked = false;
                } else {
                    st->pending_content_toggle = true;
                    SetTimer(hwnd, 3, GetDoubleClickTime(), nullptr);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case WM_CAPTURECHANGED:
            // 别处抢走鼠标（例如弹出对话框 / 窗口失活）时结束拖动态，
            // 否则进度条会卡在「一直拖动」而不再跟随真实播放位置。
            if (st) {
                st->dragging_progress = false;
                st->dragging_volume = false;
                st->pressed_id = 0;
            }
            break;
        case WM_MOUSEMOVE: {
            if (!st) break;
            // ★ 唤出/续命统一交给 WM_TIMER 的光标位移轮询。这里若像旧代码那样
            //   无条件 TouchInput，隐藏时几何变化产生的伪 WM_MOUSEMOVE 会在
            //   同一秒把刚隐藏的控制条唤回，形成 2~3 秒一轮的闪烁。
            if (!st->tracking_mouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                st->tracking_mouse = true;
            }
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (st->dragging_progress) {
                UpdateDragPos(st, p, !NeedsRestartSeek(st));
                break;
            }
            if (st->dragging_volume) { UpdateVolume(st, p); break; }
            const int id = HitTestId(st, p);
            if (id != st->hover_id) {
                st->hover_id = id;
                // 只刷标题栏与控制栏（hover 只影响这两处的按钮），不整窗重绘。
                if (st->title.bottom > st->title.top)
                    InvalidateRect(hwnd, &st->title, FALSE);
                if (st->footer.bottom > st->footer.top)
                    InvalidateRect(hwnd, &st->footer, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (st) {
                st->tracking_mouse = false;
                if (st->hover_id) {
                    st->hover_id = 0;
                    if (st->title.bottom > st->title.top)
                        InvalidateRect(hwnd, &st->title, FALSE);
                    if (st->footer.bottom > st->footer.top)
                        InvalidateRect(hwnd, &st->footer, FALSE);
                }
            }
            break;
        case WM_TIMER:
            if (st && wp == 3) {
                // 单击画面的延迟动作：双击已被取消时这里不会执行。
                KillTimer(hwnd, 3);
                if (st->pending_content_toggle) {
                    st->pending_content_toggle = false;
                    SetPlayingNow(st, !IsPlayingNow(st));
                    AppendUiLog(std::string("CLICK center toggle playing=") +
                                (IsPlayingNow(st) ? "1" : "0"));
                    if (st->footer.bottom > st->footer.top)
                        InvalidateRect(hwnd, &st->footer, FALSE);
                }
                return 0;
            }
            if (st && wp == 2) {
                // 同源重试定时器：与主刷新定时器（id=1）分开，避免互相顶掉。
                KillTimer(hwnd, 2);
                if (st->resolve_retry_pending) {
                    st->resolve_retry_pending = false;
                    AppendUiLog("RETRY reopen source #" +
                                std::to_string(st->source_index + 1));
                    OpenSourceAt(st, st->source_index);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (st) {
                // 观看历史进度（播放列表「视频」栏目续播）：10 秒落盘一次，
                // 关窗时再补一次。写失败/没有 id 时静默忽略。
                if (!st->history_id.empty() &&
                    GetTickCount() - st->last_history_save > 10000) {
                    st->last_history_save = GetTickCount();
                    const double pos = CurrentPosition(st);
                    const double dur = EffectiveDuration(st);
                    UpdateVideoHistoryProgress(
                        st->history_id, st->current_ep,
                        pos > 0.0 ? static_cast<int>(pos * 1000) : 0,
                        dur > 0.0 ? static_cast<int>(dur * 1000) : 0);
                }
                // 隐藏嗅探窗口超时：页面脚本没能自己算出直链 → 显示网页，
                // 退回原来的「内嵌网页播放」形态（不额外提示，行为不变）。
                if (st->web_mode && st->web_resolve_hidden &&
                    GetTickCount() >= st->web_resolve_deadline) {
                    st->web_resolve_hidden = false;
                    if (st->web_host) st->web_host->Resize(st->content);
                    AppendUiLog("WEB reveal after sniff timeout");
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                AdvancePlayClock(st);
                // 视频元数据（DAR）可能晚于首帧到达：轮询到变化时重排渲染窗口，
                // 否则视频会用「比例未知 = 铺满」的旧几何一直拉伸下去。
                if (st->backend) {
                    int vw = 0, vh = 0;
                    if (st->backend->GetVideoSize(&vw, &vh) && vw > 0 && vh > 0 &&
                        (vw != st->video_w || vh != st->video_h)) {
                        st->video_w = vw;
                        st->video_h = vh;
                        LayoutPlayer(st);
                        AppendUiLog("VIDEO aspect=" + std::to_string(vw) + ":" +
                                    std::to_string(vh) + " rect=(" +
                                    std::to_string(st->video_rect.left) + "," +
                                    std::to_string(st->video_rect.top) + "," +
                                    std::to_string(st->video_rect.right) + "," +
                                    std::to_string(st->video_rect.bottom) + ")");
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                // 全屏自动隐藏控制条：静止 2 秒隐藏，鼠标一动立即回来。
                // ★ 旧实现用 GetForegroundWindow() == hwnd 判定，MF / WebView2
                //   子窗口持有焦点时该判定失效 → 鼠标移动唤不出控件；3 秒阈值
                //   也比用户要的「几秒之内」长。改用「光标下窗口是否属于本窗口」
                //   （WindowFromPoint + IsChild），并保留 GetCursorPos 轮询兜底
                //   —— WebView2 会吞掉父窗的 WM_MOUSEMOVE，不能只靠消息。
                const bool immersive = ImmersiveWindow(st, hwnd);
                POINT cur{};
                const bool has_cur = GetCursorPos(&cur) != FALSE;
                if (has_cur) {
                    // 3px 阈值：光标 1px 抖动不算「移动」，否则隐藏后被反复唤出。
                    const bool moved = !st->cursor_tracked ||
                        std::abs(cur.x - st->last_cursor.x) > 3 ||
                        std::abs(cur.y - st->last_cursor.y) > 3;
                    st->cursor_tracked = true;
                    st->last_cursor = cur;
                    if (moved && immersive) {
                        const HWND under = WindowFromPoint(cur);
                        if (under == hwnd || IsChild(hwnd, under)) {
                            // 隐藏态要求相对 hide_cursor 有 >8px 的真实位移；
                            // 光标没动时绝不因任何伪消息把控制条唤回。
                            const bool real_move = !st->controls_hidden ||
                                std::abs(cur.x - st->hide_cursor.x) > 8 ||
                                std::abs(cur.y - st->hide_cursor.y) > 8;
                            if (real_move) {
                                const bool was_hidden = st->controls_hidden;
                                TouchInput(st);
                                // 真实移动时一并唤出鼠标光标（静止时才藏）。
                                SetCursorHidden(st, false);
                                if (was_hidden)
                                    AppendUiLog("CONTROLS show (cursor moved)");
                            }
                        }
                    }
                }
                if (immersive && !st->controls_hidden &&
                    !st->source_menu_open &&
                    !st->dragging_progress && !st->dragging_volume &&
                    GetTickCount() - st->last_input > 2000) {
                    st->controls_hidden = true;
                    st->hide_cursor = st->last_cursor;
                    // 控制条隐藏时一并藏起鼠标光标（移动立即唤回）。
                    SetCursorHidden(st, true);
                    LayoutPlayer(st);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    AppendUiLog(std::string("CONTROLS hide (idle 2s) ") +
                        (st->window_fullscreen ? "fullscreen" : "maximized"));
                }
                // 进度 / 音量随播放推进刷新：只重绘控制栏，避免 250ms 定时器把
                // 视频画面区也一起刷（那是「整窗闪烁」的另一半来源）。只有中央
                // 浮层（缓冲 / 准备 / 解析 / 加载动画）需要整窗刷新。
                // ★ 沉浸态隐藏控制条后完全不做定时重绘：进度条没在显示，
                //   250ms 一次的整窗 / footer 刷新只会制造周期性闪烁（用户
                //   「隔几秒就会闪烁一次」的另一半来源）。错误/加载等状态
                //   变化本身会主动 InvalidateRect，不依赖这里的定时器。
                if (!st->controls_hidden) {
                    const bool overlay = !st->error_text.empty() || st->hls_running ||
                                         st->download_running || st->resolving ||
                                         !st->got_ready;
                    if (overlay)
                        InvalidateRect(hwnd, nullptr, FALSE);
                    else if (st->footer.right > st->footer.left &&
                             st->footer.bottom > st->footer.top)
                        InvalidateRect(hwnd, &st->footer, FALSE);
                }
            }
            break;
        case WM_KEYDOWN: {
            if (!st) break;
            TouchInput(st);
            if (wp == VK_SPACE) {
                SetPlayingNow(st, !IsPlayingNow(st));
                if (st->footer.bottom > st->footer.top)
                    InvalidateRect(hwnd, &st->footer, FALSE);
            } else if (wp == VK_ESCAPE && st->window_fullscreen) {
                ToggleFullscreen(st);
            } else if (wp == VK_RIGHT) {
                const double dur = EffectiveDuration(st);
                if (dur > 0.0)
                    SeekTo(st, std::min(dur, CurrentPosition(st) + 10.0));
            } else if (wp == VK_LEFT) {
                SeekTo(st, std::max(0.0, CurrentPosition(st) - 10.0));
            }
            break;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

// ------------------------------------------------------------ HLS（m3u8）支持
// Media Foundation 在 Windows 10 1803+ 已移除内置 HLS 源，m3u8 直链无法直接
// 交给 MF 播放。这里把点播 m3u8 的全部分段后台拼接成本地 .ts 临时文件，再用
// MF 打开（MF 仍支持 MPEG-TS 字节流）。直播流（无限列表）不支持。

// 是否是可直接交给 MF/拼接器的媒体直链（否则多半是网页分享页，
// 需要影视仓的「解析接口 / spider」才能拿到真实地址）。
bool LooksPlayableMediaUrl(const std::wstring& url) {
    std::wstring lower = url;
    for (auto& ch : lower)
        ch = static_cast<wchar_t>(std::towlower(ch));
    // .mpd(DASH) / .m3u,.txt(直播列表) 也是「地址」而非网页：插件缺失时
    // 应走 MF 直连并明确报错，而不是被当成网页去跑一遍解析爬虫。
    static const wchar_t* kExts[] = {L".m3u8", L".mp4", L".flv", L".mkv",
                                     L".m4v", L".mov", L".webm", L".ts",
                                     L".mp3", L".aac", L".m4a",
                                     L".mpd", L".m3u", L".txt"};
    for (const wchar_t* ext : kExts)
        if (lower.find(ext) != std::wstring::npos) return true;
    return false;
}

bool LooksLikeHlsUrl(const std::wstring& url) {
    std::wstring lower = url;
    for (auto& ch : lower)
        ch = static_cast<wchar_t>(std::towlower(ch));
    return lower.find(L".m3u8") != std::wstring::npos;
}

// 相对段名补全成绝对地址（基于 m3u8 的 URL）。
std::string JoinSegmentUrl(const std::string& base, const std::string& ref) {
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
    const std::string b = base;
    const size_t scheme_end = b.find("://");
    if (scheme_end == std::string::npos) return ref;
    if (!ref.empty() && ref[0] == '/') {
        const size_t host_start = scheme_end + 3;
        const size_t slash = b.find('/', host_start);
        return slash == std::string::npos ? b + ref : b.substr(0, slash) + ref;
    }
    size_t slash = b.rfind('/');
    if (slash == std::string::npos || slash < scheme_end + 3) return ref;
    return b.substr(0, slash + 1) + ref;
}

// 后台线程：解析 m3u8（支持一层主列表嵌套）、顺序拼接 ts 分段、按段数回报进度。
void HlsWorkerProc(std::shared_ptr<HlsShared> shared, HWND hwnd,
                   std::wstring url, std::map<std::string, std::string> headers) {
    std::wstring final_path;
    std::string error;
    try {
        musicfree::NetworkConfig cfg;
        for (const auto& h : headers)
            if (h.first == "User-Agent" || h.first == "user-agent")
                cfg.user_agent = core::Utf8ToWide(h.second);
        std::string playlist = musicfree::DownloadText(core::WideToUtf8(url), cfg,
                                                       {});
        if (playlist.find("#EXTM3U") == std::string::npos)
            throw std::runtime_error("不是有效的 m3u8 内容");
        // 主播放列表：取第一个子 m3u8 再解析一层。
        if (playlist.find("#EXT-X-STREAM-INF") != std::string::npos) {
            std::string child;
            bool take_next = false;
            std::stringstream ss(playlist);
            std::string ln;
            while (std::getline(ss, ln)) {
                while (!ln.empty() && (ln.back() == '\r' || ln.back() == '\n'))
                    ln.pop_back();
                if (ln.empty()) continue;
                if (take_next) { child = ln; break; }
                if (ln.rfind("#EXT-X-STREAM-INF", 0) == 0) take_next = true;
            }
            if (child.empty()) throw std::runtime_error("m3u8 主列表没有子列表");
            playlist = musicfree::DownloadText(JoinSegmentUrl(
                core::WideToUtf8(url), child), cfg, {});
        }
        // 媒体播放列表：收集段地址，同时抓 #EXT-X-KEY（AES-128 加密信息）。
        std::vector<std::string> segs;
        // 每段时长（#EXTINF）：用于「分段就绪即播」时把各 part 的播放进度
        // 换算成整片视频里的绝对时间。
        std::vector<double> seg_durations;
        double pending_duration = 0.0;
        std::string key_uri;
        std::uint8_t key_iv[16] = {};
        bool key_iv_explicit = false;
        std::stringstream ss(playlist);
        std::string ln;
        while (std::getline(ss, ln)) {
            while (!ln.empty() && (ln.back() == '\r' || ln.back() == '\n'))
                ln.pop_back();
            if (ln.empty()) continue;
            if (ln.rfind("#EXTINF:", 0) == 0) {
                // #EXTINF:<秒数>,可选标题
                const size_t comma = ln.find(',');
                const std::string num = ln.substr(
                    8, comma == std::string::npos ? std::string::npos : comma - 8);
                try {
                    pending_duration = std::stod(num);
                } catch (...) {
                    pending_duration = 0.0;
                }
                continue;
            }
            if (ln.rfind("#EXT-X-KEY:", 0) == 0) {
                if (ln.find("METHOD=AES-128") != std::string::npos) {
                    const size_t up = ln.find("URI=\"");
                    if (up != std::string::npos) {
                        const size_t ue = ln.find('"', up + 5);
                        if (ue != std::string::npos)
                            key_uri = ln.substr(up + 5, ue - up - 5);
                    }
                    const size_t ivp = ln.find("IV=0x");
                    if (ivp != std::string::npos && ivp + 5 + 32 <= ln.size()) {
                        for (int b = 0; b < 16; ++b) {
                            const std::string byte_hex = ln.substr(
                                ivp + 5 + b * 2, 2);
                            key_iv[b] = static_cast<std::uint8_t>(
                                std::strtoul(byte_hex.c_str(), nullptr, 16));
                        }
                        key_iv_explicit = true;
                    }
                }
                continue;
            }
            if (ln[0] == '#') continue;
            segs.push_back(JoinSegmentUrl(core::WideToUtf8(url), ln));
            seg_durations.push_back(pending_duration);
            pending_duration = 0.0;
        }
        if (segs.empty()) throw std::runtime_error("m3u8 里没有可用的分段");
        // 各段 #EXTINF 之和 = 整片时长。MF 播拼接出来的 ts 时 GetDuration()
        // 常常返回 NaN，没有这个 hint 进度条就永远拖不动、时间显示成 -14:-8。
        // 线程不能直接碰 PlayerState，写进 HlsShared 并通过 hwnd 回传主线程。
        double total_duration = 0.0;
        for (double d : seg_durations)
            if (std::isfinite(d) && d > 0.0) total_duration += d;
        if (total_duration > 0.0) {
            if (shared) shared->total_duration.store(total_duration);
            PostMessageW(hwnd, WM_USER + 10,
                         static_cast<WPARAM>(total_duration * 1000.0), 0);
        }
        // 下载解密 key（16 字节）。
        std::vector<std::uint8_t> aes_key;
        if (!key_uri.empty()) {
            const std::string key_url = JoinSegmentUrl(core::WideToUtf8(url), key_uri);
            aes_key = musicfree::DownloadBinary(key_url, cfg, {});
            if (aes_key.size() != 16)
                throw std::runtime_error("解密 key 获取失败（HLS AES-128）");
            if (!key_iv_explicit) {
                // 缺省 IV = 媒体序列号（16 字节大端；这里序列号按 0 处理，
                // 绝大多数点播源都显式给出 IV）。
                std::memset(key_iv, 0, sizeof(key_iv));
            }
        }

        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path dir =
            std::filesystem::path(temp) / L"TTPlayerHLS";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        // 文件名由源 url 哈希决定：同一地址复用已完成的拼接结果。
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char ch : core::WideToUtf8(url)) {
            h ^= ch; h *= 1099511628211ULL;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.ts",
                      static_cast<unsigned long long>(h));
        const std::filesystem::path out = dir / name;
        final_path = out.wstring();
        if (std::filesystem::exists(out, ec) &&
            std::filesystem::file_size(out, ec) > 0) {
            PostMessageW(hwnd, WM_USER + 6, 100,
                         reinterpret_cast<LPARAM>(new std::wstring(out.wstring())));
            return;
        }
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("无法创建临时拼接文件");
        // ---- 分段并行下载（影视仓秒进的另一半：并发拉取，不是串行排队）----
        // 以前是一段一段顺序下载：N 段 = N 次往返累加，20~30s 全花在等网络。
        // 现在固定数量的 worker 抢同一个下标，写盘仍按段序（TS 必须顺序拼接）。
        const size_t total = segs.size();
        const int kWorkers = std::max<int>(
            1, std::min<int>(6, static_cast<int>(total)));
        std::vector<std::vector<std::uint8_t>> buffers(total);
        std::vector<char> ready(total, 0);
        std::mutex ready_mtx;
        std::atomic<size_t> next_index{0};
        std::atomic<bool> failed{false};
        std::string worker_error;
        std::mutex err_mtx;
        auto worker = [&]() {
            while (!failed.load()) {
                if (shared->cancel) { failed.store(true); break; }
                const size_t i = next_index.fetch_add(1);
                if (i >= total) break;
                try {
                    auto data = musicfree::DownloadBinary(segs[i], cfg, {});
                    if (data.empty()) throw std::runtime_error("分段下载失败");
                    std::vector<std::uint8_t> plain;
                    if (!aes_key.empty()) {
                        if (!AesCbcDecrypt128(aes_key, key_iv, data, plain))
                            throw std::runtime_error("分段解密失败（HLS AES-128）");
                    } else {
                        plain = std::move(data);
                    }
                    // 明文 TS 首字节必须是 0x47（sync）；否则说明源不是可拼接的 TS
                    // （例如 fMP4/CMAF 分片），MF 无法用这种方式播放。
                    if (plain.empty() || plain[0] != 0x47)
                        throw std::runtime_error(
                            "该视频源不是可拼接的 TS 格式（可能是 fMP4），暂不支持");
                    buffers[i] = std::move(plain);
                    {
                        std::lock_guard<std::mutex> lk(ready_mtx);
                        ready[i] = 1;
                    }
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lk(err_mtx);
                    if (worker_error.empty()) worker_error = e.what();
                    failed.store(true);
                    break;
                }
            }
        };
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (int w = 0; w < kWorkers; ++w) workers.emplace_back(worker);
        int done = 0;
        for (size_t i = 0; i < total; ++i) {
            // 等第 i 段就绪（worker 是乱序完成的，写盘必须按段序）。
            for (;;) {
                {
                    std::lock_guard<std::mutex> lk(ready_mtx);
                    if (ready[i]) break;
                }
                if (failed.load()) break;
                if (shared->cancel) { failed.store(true); break; }
                Sleep(8);
            }
            if (failed.load()) break;
            file.write(reinterpret_cast<const char*>(buffers[i].data()),
                       static_cast<std::streamsize>(buffers[i].size()));
            ++done;
            PostMessageW(hwnd, WM_USER + 5, done * 100 / (int)total, 0);
        }
        for (auto& t : workers) t.join();
        if (failed.load()) {
            std::lock_guard<std::mutex> lk(err_mtx);
            if (!worker_error.empty()) throw std::runtime_error(worker_error);
            throw std::runtime_error("已取消");
        }
        file.flush();
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "拼接失败";
    }
    if (!error.empty()) {
        PostMessageW(hwnd, WM_USER + 4, 0,
                     reinterpret_cast<LPARAM>(_strdup(error.c_str())));
        return;
    }
    PostMessageW(hwnd, WM_USER + 6, 100,
                 reinterpret_cast<LPARAM>(new std::wstring(final_path)));
}

void StartHlsConcat(PlayerState* st, const VideoPlayerSource& src) {
    if (!st) return;
    st->hls_running = true;
    st->hls_percent = 0;
    // 拼接期间 MF 还没用上渲染子窗口，先把它藏起来：子窗口永远盖在父窗之上，
    // 它一挡着，父窗的「正在缓冲 n%」浮层就看不见（用户只看到一片白）。
    if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
    st->hls_shared = std::make_shared<HlsShared>();
    const HWND hwnd = st->hwnd;
    std::thread([shared = st->hls_shared, hwnd, src,
                 headers = st->open_headers]() {
        HlsWorkerProc(shared, hwnd, src.url, headers);
    }).detach();
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void SwitchSource(PlayerState* st);

// 关闭协议插件会话（换线路 / 关窗时调用）：置取消标记让监控线程退出，
// 再销毁会话（VPClose 内部 join 全部线程）。
void CloseProtocolSession(PlayerState* st) {
    if (st->proto_cancel) st->proto_cancel->cancelled = true;
    st->proto_cancel.reset();
    if (st->proto_session) {
        VideoProtocolRegistry::Close(st->proto_session);
        st->proto_session = nullptr;
    }
}

// 把影视仓配置里的 parses 序列化成协议插件 options（UTF-16 JSON 字符串）。
// DLL 只认 C ABI，拿不到主程序的 std::vector<VideoParse>，用 JSON 传递。
std::wstring BuildProtocolOptions(const PlayerState* st) {
    nlohmann::json root;
    root["parses"] = nlohmann::json::array();
    for (const auto& p : st->parses) {
        if (p.url.empty() || p.type == 0) continue;  // type 0=嗅探，需要 WebView
        nlohmann::json e;
        e["name"] = p.name;
        e["url"] = p.url;
        e["type"] = p.type;
        root["parses"].push_back(std::move(e));
    }
    // 播放地址的内联请求头（Referer/Cookie/Origin…）：MEDIA 中转插件按原样带给上游。
    // 网页/HLS 插件只用 user_agent/referer，不认识这个字段也无害。
    if (!st->open_headers.empty()) {
        nlohmann::json headers = nlohmann::json::object();
        for (const auto& kv : st->open_headers)
            if (!kv.first.empty() && !kv.second.empty())
                headers[kv.first] = kv.second;
        root["headers"] = std::move(headers);
    }
    return core::Utf8ToWide(root.dump());
}

// 内置默认解析接口兜底（影视仓 parses 段）。
// 影视仓配置普遍不带 parses 段，导致优酷/腾讯/哔哩哔哩等官方网页线路「播放页
// → 媒体直链」这一步无人处理而失败（日志里就是「配置解析接口(parses): 0 个」）。
// 主程序在配置未提供 parses 时，自动启用一份可编辑的默认解析接口：
//   1) 优先读 <exe>\video\protocols\builtin_parses.json（数组，元素 {name,type,url}），
//      用户可随时换成自己可用的解析接口，无需重新编译；
//   2) 文件缺失/损坏/为空 → 退回编译期内置表（当前为空，行为等同「无内置解析接口」）。
// WEB 协议插件（经 options 传 parses）与主程序兜底 ResolveVideoUrl 都复用 st->parses，
// 这里在入口一次性注入，全链路解析路径自动受益。
std::vector<VideoParse> LoadBuiltinParses() {
    std::vector<VideoParse> out;

    // ---- ① 优先从可编辑 JSON 加载（推荐：用户自行维护可用解析接口）----
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring p(exe);
        const size_t s = p.find_last_of(L"\\/");
        if (s != std::wstring::npos)
            p = p.substr(0, s) + L"\\video\\protocols\\builtin_parses.json";
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            std::ifstream f(p, std::ios::binary);
            if (f) {
                std::stringstream ss; ss << f.rdbuf();
                try {
                    auto root = nlohmann::json::parse(ss.str());
                    if (root.is_array()) {
                        for (const auto& e : root) {
                            if (!e.is_object()) continue;
                            VideoParse vp;
                            if (e.contains("name") && e["name"].is_string())
                                vp.name = e["name"].get<std::string>();
                            if (e.contains("type") && e["type"].is_number())
                                vp.type = e["type"].get<int>();
                            if (e.contains("url") && e["url"].is_string())
                                vp.url = e["url"].get<std::string>();
                            // type 0=嗅探（需 WebView，本程序无法处理），跳过。
                            if (!vp.url.empty() && vp.type != 0)
                                out.push_back(std::move(vp));
                        }
                    }
                } catch (...) {}
            }
        }
    }

    // ---- ② 文件为空/缺失 → 退回编译期内置（默认空表，可在此源码补）----
    if (out.empty()) {
        // 示例（取消注释并替换成你验证过可用的解析接口）：
        //   VideoParse p;
        //   p.name = "示例解析"; p.type = 2; p.url = "https://你的解析接口/?url=";
        //   out.push_back(p);
    }
    return out;
}

// 协议会话统一开播：HLS（m3u8 → 本地 TS 代理）与 WEB（网页播放地址 →
// 真实直链代理）共用这条路径，差别只是解析阶段的浮层文案。
bool StartProtocolPlayback(PlayerState* st, ProtocolSession* session,
                           const std::wstring& media_url) {
    const bool web_phase =
        st->source_index >= 0 &&
        st->source_index < static_cast<int>(st->sources.size()) &&
        !LooksPlayableMediaUrl(st->sources[st->source_index].url) &&
        !LooksLikeHlsUrl(st->sources[st->source_index].url);
    st->proto_session = session;
    st->hls_direct = false;
    st->resolving = web_phase;
    st->hls_running = !web_phase;
    if (!web_phase) st->hls_percent = 0;
    st->seek_unsupported = false;
    // 记下代理给出的 media URL：拖动进度条时用它构造 /s/<秒>/stream.ts
    // 重开会话（NeedsRestartSeek / RestartProxyAt）。
    st->protocol_media_url = media_url;
    st->seek_offset = 0.0;
    st->play_clock_base = 0.0;
    st->play_clock_tick = GetTickCount();
    st->error_retried = false;
    st->opened_ok = st->backend &&
                    st->backend->Open(st->video_host, media_url, {});
    AppendUiLog("PLAYBACK protocol open ok=" +
                std::string(st->opened_ok ? "1" : "0") + " media=" +
                RedactUrl(core::WideToUtf8(media_url)).substr(0, 160));
    if (!st->opened_ok) {
        st->resolving = false;
        st->hls_running = false;
        st->proto_session = nullptr;
        return false;
    }
    st->backend->Play();
    // ★ 2026-11-10 实测删除「后台 RANGE probe」：这是「播放约 8 秒后
    //   弹出播放失败（解码或网络错误）」的元凶。
    //   player_ui.log: 19:47:12 Open → 19:47:17 RANGE probe status=200 →
    //   19:47:20 用户关窗（正好 00:08）。对 chunked 代理（sizes_ok=false）
    //   这个带 Range 的请求拿不到 206，代理仍按 ServeChunked 输出**整条无限
    //   流**；探测线程一路读下去，抢占 DownloadLoop 的分片下载，并把
    //   served_index 重置回首片，MF 主连接被饿死 → ERROR。
    //   seek_unsupported 早已降级为纯诊断（拖动改走按起点重启会话），
    //   这个探测现在没有任何用途，只留下破坏性，故整块移除。
    AppendUiLog("PLAYBACK protocol ready (range probe disabled)");
    // 监控线程只持 weak_ptr 取消标记与 hwnd，不碰 PlayerState 裸指针。
    st->proto_cancel = std::make_shared<ProtoCancel>();
    std::weak_ptr<ProtoCancel> weak = st->proto_cancel;
    const HWND hwnd = st->hwnd;
    std::thread([weak, hwnd, session]() {
        for (;;) {
            Sleep(250);
            auto alive = weak.lock();
            if (!alive || alive->cancelled) return;
            std::wstring err;
            const int state = VideoProtocolRegistry::Poll(session, &err);
            if (state == VP_POLL_END) {
                PostMessageW(hwnd, WM_USER + 3, 0, 0);
                return;
            }
            if (state == VP_POLL_ERROR) {
                PostMessageW(hwnd, WM_USER + 9, 0,
                             reinterpret_cast<LPARAM>(_strdup(
                                 core::WideToUtf8(err).c_str())));
                return;
            }
        }
    }).detach();
    InvalidateRect(st->hwnd, nullptr, FALSE);
    return true;
}

// 切到下一条线路并重新打开（保留给键盘/其它调用；控制栏按钮已改走菜单）。
void SwitchSource(PlayerState* st) {
    if (!st || st->sources.size() < 2) return;
    st->auto_source_tried = 0;   // 用户手动换线：重置自动兜底计数
    st->same_src_retry_index = -1;
    st->resolve_retry_pending = false;
    const int next = (st->source_index + 1) %
                     static_cast<int>(st->sources.size());
    OpenSourceAt(st, next);
}

// 控制栏「切换源」：列出全部线路名，当前线路打勾，点哪条直接切哪条。
// 用户要求「可以选择不同名字的源」——旧的循环切换看不到名字也选不了指定线路。
void ShowSourceMenu(PlayerState* st) {
    if (!st || !st->hwnd || st->sources.size() < 2) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (std::size_t i = 0; i < st->sources.size(); ++i) {
        std::wstring label = st->sources[i].name.empty()
            ? L"线路 " + std::to_wstring(i + 1)
            : st->sources[i].name;
        if (label.size() > 48) label = label.substr(0, 47) + L"…";
        UINT flags = MF_STRING | MF_ENABLED;
        if (static_cast<int>(i) == st->source_index) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(i + 1), label.c_str());
    }
    POINT pt{st->source_btn.left, st->source_btn.top};
    ClientToScreen(st->hwnd, &pt);
    TouchInput(st);
    st->source_menu_open = true;
    const int cmd = static_cast<int>(TrackPopupMenu(
        menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, st->hwnd, nullptr));
    st->source_menu_open = false;
    DestroyMenu(menu);
    if (cmd > 0 && cmd <= static_cast<int>(st->sources.size())) {
        const int index = cmd - 1;
        if (index != st->source_index) {
            AppendUiLog("SOURCE menu pick=" + std::to_string(index) +
                        " name=" + core::WideToUtf8(
                            st->sources[index].name));
            st->auto_source_tried = 0;   // 用户手动选线：重置自动兜底计数
            st->same_src_retry_index = -1;
            st->resolve_retry_pending = false;
            OpenSourceAt(st, index);
        }
    }
}

// 控制栏「选集」：列出整部剧的集数，选中后**在同一个播放窗内**切集。
// 以前切换到新一集只能回音乐窗的「剧集」再点一次；电影只有切换源，剧集
// 在播放窗里没有任何换集入口。
void ShowEpisodeMenu(PlayerState* st) {
    if (!st || !st->hwnd || st->episodes.size() < 2) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // 菜单过长时 Windows 会滚动；这里再限 400 条，避免极端剧集把菜单撑爆。
    const std::size_t limit = std::min<std::size_t>(st->episodes.size(), 400);
    // 当前正在播的集：按「当前源地址 == 集地址」精确匹配（换源后地址可能变化，
    // 只有真正同一集才该打勾，不能简单用下标兜底）。
    int current_episode = -1;
    {
        std::wstring playing_url;
        if (st->source_index >= 0 &&
            st->source_index < static_cast<int>(st->sources.size()))
            playing_url = st->sources[static_cast<std::size_t>(st->source_index)].url;
        // 一集可能有多个线路地址：当前播的地址命中任意一条都算这一集
        // （否则在「切换源」换过线之后，选集菜单会丢掉勾选）。
        for (std::size_t i = 0; i < limit && !playing_url.empty(); ++i) {
            if (st->episodes[i].url == playing_url) {
                current_episode = static_cast<int>(i);
                break;
            }
            for (const auto& source : st->episodes[i].sources) {
                if (source.url == playing_url) {
                    current_episode = static_cast<int>(i);
                    break;
                }
            }
            if (current_episode >= 0) break;
        }
        // 地址被清洗/解析改写过时按记录的下标兜底。
        if (current_episode < 0 && st->current_ep >= 0 &&
            st->current_ep < static_cast<int>(limit))
            current_episode = st->current_ep;
    }
    for (std::size_t i = 0; i < limit; ++i) {
        std::wstring label = st->episodes[i].title.empty()
                                 ? L"第 " + std::to_wstring(i + 1) + L" 集"
                                 : st->episodes[i].title;
        if (label.size() > 48) label = label.substr(0, 47) + L"…";
        UINT flags = MF_STRING | MF_ENABLED;
        // 与「切换源」菜单同款：当前播放的集前面打勾。
        if (static_cast<int>(i) == current_episode) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(i + 1), label.c_str());
    }
    POINT pt{st->episode_btn.left, st->episode_btn.top};
    ClientToScreen(st->hwnd, &pt);
    TouchInput(st);
    AppendUiLog("EPISODE menu items=" + std::to_string(limit) + " checked=" +
                std::to_string(current_episode) + " first=" +
                core::WideToUtf8(st->episodes.front().title));
    // 复用「菜单打开」标志：菜单期间控制条不自动隐藏（与切换源一致）。
    st->source_menu_open = true;
    const int cmd = static_cast<int>(TrackPopupMenu(
        menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, st->hwnd, nullptr));
    st->source_menu_open = false;
    DestroyMenu(menu);
    if (cmd <= 0) return;
    const std::size_t index = static_cast<std::size_t>(cmd - 1);
    if (index >= st->episodes.size()) return;
    // 拷贝一份：ShowVideoPlayer 复用同一窗口时会整体覆盖 st->episodes。
    const VideoPlayerEpisode ep = st->episodes[index];
    if (ep.url.empty()) return;
    AppendUiLog("EPISODE pick=" + std::to_string(index) + " name=" +
                core::WideToUtf8(ep.title));
    const std::wstring history_id = st->history_id;
    ShowVideoPlayer(
        st->hwnd, ep.url, st->base_title, ep.line, st->browser_skin,
        st->open_headers, ep.sources.empty() ? st->sources : ep.sources,
        st->parses, st->player_url, st->player_base_url, st->episodes,
        0, history_id);
    // 换集后进度从 0 开始：立即落盘，避免历史里仍是上一集的位置。
    if (!history_id.empty())
        UpdateVideoHistoryProgress(history_id, static_cast<int>(index), 0, 0);
}

namespace {

// HLS 总时长探测：协议插件 / MF 直连路径都不会走「全量拼接」线程，而
// IMFMediaEngine 在 HLS 流上通常拿不到时长（GetDuration 返回 NaN）→ 时间
// 恒显示 --:--，进度条也没有可映射的总长度（用户反馈「总时长没显示、拖不动」）。
// 这里单独拉一次 m3u8，把 #EXTINF 之和回传主线程（WM_USER+11）。
void ProbeHlsDuration(HWND hwnd, const std::wstring& url,
                      const std::map<std::string, std::string>& headers,
                      unsigned serial) {
    if (!hwnd || url.empty()) return;
    std::thread([hwnd, url, headers, serial] {
        try {
            musicfree::NetworkConfig cfg;
            for (const auto& h : headers)
                if (_stricmp(h.first.c_str(), "user-agent") == 0)
                    cfg.user_agent = core::Utf8ToWide(h.second);
            const std::string base = core::WideToUtf8(url);
            // 带上站点配置的 UA / Referer：不少 m3u8 直链缺 Referer 会 403。
            std::string playlist = musicfree::DownloadText(base, cfg, headers);
            if (playlist.find("#EXTM3U") == std::string::npos) return;
            // 主列表：取第一个子 m3u8 再解析一层（与拼接链路同一规则）。
            if (playlist.find("#EXT-X-STREAM-INF") != std::string::npos) {
                std::string child;
                bool take_next = false;
                std::stringstream ss(playlist);
                std::string ln;
                while (std::getline(ss, ln)) {
                    while (!ln.empty() &&
                           (ln.back() == '\r' || ln.back() == '\n'))
                        ln.pop_back();
                    if (ln.empty()) continue;
                    if (take_next && ln[0] != '#') { child = ln; break; }
                    if (ln.rfind("#EXT-X-STREAM-INF", 0) == 0)
                        take_next = true;
                }
                if (child.empty()) return;
                playlist = musicfree::DownloadText(JoinSegmentUrl(base, child),
                                                   cfg, headers);
            }
            double total = 0.0;
            std::stringstream ss(playlist);
            std::string ln;
            while (std::getline(ss, ln)) {
                if (ln.rfind("#EXTINF:", 0) != 0) continue;
                const size_t comma = ln.find(',');
                const std::string num = ln.substr(
                    8, comma == std::string::npos ? std::string::npos
                                                  : comma - 8);
                try {
                    const double d = std::stod(num);
                    if (std::isfinite(d) && d > 0.0) total += d;
                } catch (...) {
                }
            }
            if (total > 0.0)
                PostMessageW(hwnd, WM_USER + 11,
                             static_cast<WPARAM>(total * 1000.0),
                             static_cast<LPARAM>(serial));
        } catch (...) {
            // 探测失败不影响播放：时长继续用后端返回值。
        }
    }).detach();
}

} // namespace

void OpenSourceAt(PlayerState* st, int index) {
    if (!st || index < 0 || index >= static_cast<int>(st->sources.size())) return;
    st->source_index = index;
    // 换线路 / 重新打开源：先拆掉上一条线路的网页宿主，否则旧网页会盖在新源上。
    st->web_host.reset();
    st->web_resolve_hidden = false;
    const bool was_web = st->web_mode;
    st->web_mode = false;
    if (was_web) AppendUiLog("MODE web=0 (switch to native)");
    // 从网页内嵌播放切回原生播放：重新排一次，恢复原生控制栏。
    if (was_web) LayoutPlayer(st);
    // 同一播放窗换集 / 换线：上一轮 HLS 拼接、下载、解析若还在跑，必须取消并
    // 复位标志，否则新地址会被「已有拼接在跑」直接跳过，旧的完成消息还会把
    // 上一集画面顶上来。
    if (st->hls_shared) {
        st->hls_shared->cancel = true;
        st->hls_shared.reset();
    }
    st->hls_running = false;
    st->hls_direct = false;
    st->hls_percent = 0;
    st->download_running = false;
    st->resolving = false;
    // catvod 播放串常把请求头内联在尾部（.m3u8${"Referer":"..."} /
    // @Referer=...）。必须在「判定是否直链 / 交给 MF」之前剥掉，并进
    // open_headers —— 否则 HLS 切片请求没有 Referer 一律 403。
    {
        std::map<std::string, std::string> inline_headers;
        const std::string clean = ttplayer::video::ParseInlinePlayHeaders(
            core::WideToUtf8(st->sources[index].url), inline_headers);
        if (!clean.empty() &&
            clean != core::WideToUtf8(st->sources[index].url)) {
            st->sources[index].url = core::Utf8ToWide(clean);
            for (const auto& kv : inline_headers)
                if (!kv.first.empty() && !kv.second.empty())
                    st->open_headers[kv.first] = kv.second;
        }
    }
    const VideoPlayerSource& src = st->sources[index];
    // 换线路重置一次「已试过播放器」；同一线路因解析结果重开时保持 true，
    // 避免 ResolveVideoUrl 反复跑 player 造成无限循环。
    if (st->player_tried_index != index) {
        st->player_tried_index = index;
        st->player_tried = false;
    }
    st->line_label = src.name.empty()
        ? (L"线路 " + std::to_wstring(index + 1)) : src.name;
    st->error_text.clear();
    st->got_ready = false;
    st->opened_ok = false;
    st->hls_prepared.clear();
    // 换线路：清掉上一条线路的时长兜底与拖动状态，避免旧时长挂在新视频上。
    ++st->open_serial;   // 让上一轮后台时长探测的迟到结果失效
    st->duration_hint = 0.0;
    st->drag_pos = 0.0;
    st->drag_ratio = 0.0;
    st->dragging_progress = false;
    // 换集/换线路必须清「上一轮代理不支持 Range」标志，否则本来可拖的新线路
    // 会被旧标志一直挡住（旧代码只在 StartProtocolPlayback 里清）。
    st->seek_unsupported = false;
    st->web_state = VideoWebState{};
    // 换集/换线路：清掉上一轮的代理 URL 与时间基准，避免对本地 ts /
    // 直链也误用「按起点重启会话」。StartProtocolPlayback 会重新设置。
    st->protocol_media_url.clear();
    st->seek_offset = 0.0;
    st->play_clock_base = 0.0;
    st->play_clock_tick = GetTickCount();
    st->error_retried = false;
    st->video_w = 0;          // 换集/换线路：重新探测显示比例，别套用上一部片
    st->video_h = 0;
    // HLS：独立探测一次总时长（协议插件 / 直连都不会走拼接线程的时长回传）。
    if (LooksLikeHlsUrl(src.url))
        ProbeHlsDuration(st->hwnd, src.url, st->open_headers, st->open_serial);
    CloseProtocolSession(st);   // 换线路：先收掉上一条线路的协议会话
    // 切换源全程保持渲染子窗口隐藏：旧 MF 引擎在「可见」状态下被 Close() 拆掉，
    // video_host（STATIC 默认背景）会闪一下灰底，整片视频区都跟着闪，看起来就是
    // 「整个窗口闪烁」。正确做法是拆旧引擎、开新引擎时都保持隐藏，等 on_ready
    // （首帧就绪，WM_USER+1）再把子窗口放出来——那时 MF 已有画面，无灰闪。
    if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
    if (st->backend) st->backend->Close();

    // ---- 数据接口快速通道：返回 JSON 的选集地址 ----
    // 6huo 这类「预告片」源的选集地址是 ?view=api&mode=download-data，
    // 请求它直接返回 {"movurl":"http://…/xxx.mov"}。若交给下面的网页协议
    // 插件，它会当播放页爬十几秒；再兜底到 WebView2 就变成「一屏 JSON 文本」。
    // 这里先直接把 JSON 里的直链取出来重开，通常 1 秒内出画面。
    if (ttplayer::video::LooksLikeJsonApiUrl(core::WideToUtf8(src.url))) {
        st->resolving = true;
        if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
        const std::string api_url = core::WideToUtf8(src.url);
        const auto headers = st->open_headers;
        const HWND hwnd = st->hwnd;
        AppendUiLog("JSONAPI src=" +
                    RedactUrl(api_url).substr(0, 120));
        std::thread([api_url, headers, hwnd]() {
            std::string media;
            try {
                musicfree::NetworkConfig cfg;
                for (const auto& h : headers)
                    if (_stricmp(h.first.c_str(), "user-agent") == 0)
                        cfg.user_agent = core::Utf8ToWide(h.second);
                media = ttplayer::video::ResolveJsonApiMedia(api_url, cfg, headers);
            } catch (...) {
            }
            if (media.empty()) {
                PostMessageW(hwnd, WM_USER + 9, 0,
                             reinterpret_cast<LPARAM>(_strdup(
                                 "该线路是数据接口（返回 JSON），里面没有可播放的"
                                 "媒体直链。请点标题栏「切换源」换一条线路。")));
                return;
            }
            PostMessageW(hwnd, WM_USER + 7, 0,
                         reinterpret_cast<LPARAM>(
                             new ResolvedMedia{core::Utf8ToWide(media), {}}));
        }).detach();
        InvalidateRect(st->hwnd, nullptr, FALSE);
        return;
    }

    // ---- 通用协议路由：所有 URL 先问 AddIn\ttp_v_*.dll ----
    // HLS(m3u8) / WEB(网页页) / DASH(.mpd) / M3U(直播列表) 各按 probe
    // 置信度认领；无人认领或打不开时才落到下面的解析 / 直连 / 拼接兜底。
    // ⚠ 以前只有 m3u8 和网页两类会问插件，新增协议插件必须走这条统一入口。
    {
        std::wstring media_url, ua, ref;
        for (const auto& h : st->open_headers) {
            if (_stricmp(h.first.c_str(), "user-agent") == 0)
                ua = core::Utf8ToWide(h.second);
            else if (_stricmp(h.first.c_str(), "referer") == 0)
                ref = core::Utf8ToWide(h.second);
        }
        const std::wstring options = BuildProtocolOptions(st);
        ProtocolSession* session = VideoProtocolRegistry::Instance().Open(
            src.url, ua, ref, 8000, options, &media_url);
        if (session) {
            // WEB 协议常把分享页解析成「远程 m3u8」。直接把这个 m3u8 交给 MF：
            // 多数机器没有 HLS 媒体源 → 播放失败 → 回退内嵌网页（站点自带控制条
            // 与我们的控制条叠成两层，而且外层进度条控制不了内嵌播放器）。
            // 这里改成再用 HLS 协议打开这个 m3u8，拿本地 Range 代理流播放：
            // 单层原生控制条、可拖动进度、可探测总时长。
            std::wstring play_media = media_url;
            ProtocolSession* play_session = session;
            ProtocolSession* hls_session = nullptr;
            std::wstring hls_media;
            const bool remote_hls =
                LooksLikeHlsUrl(media_url) &&
                media_url.find(L"127.0.0.1") == std::wstring::npos &&
                media_url.find(L"localhost") == std::wstring::npos;
            if (remote_hls) {
                hls_session = VideoProtocolRegistry::Instance().Open(
                    media_url, ua, ref, 8000, options, &hls_media);
                if (hls_session && !hls_media.empty()) {
                    play_session = hls_session;
                    play_media = hls_media;
                } else if (hls_session) {
                    VideoProtocolRegistry::Close(hls_session);
                    hls_session = nullptr;
                }
            }
            AppendUiLog(
                "PROTO src=" +
                RedactUrl(core::WideToUtf8(src.url)).substr(0, 120) +
                " web_media=" +
                RedactUrl(core::WideToUtf8(media_url)).substr(0, 120) +
                " remote_hls=" + (remote_hls ? "1" : "0") +
                " hls_session=" + (hls_session ? "1" : "0"));
            if (StartProtocolPlayback(st, play_session, play_media)) {
                if (play_session != session)
                    VideoProtocolRegistry::Close(session);  // WEB 会话已不需要
                if (remote_hls)
                    ProbeHlsDuration(st->hwnd, media_url, st->open_headers,
                                     st->open_serial);
                return;
            }
            if (hls_session) VideoProtocolRegistry::Close(hls_session);
            VideoProtocolRegistry::Close(session);
        }
    }

    // ---- 网页地址兜底：主程序内解析链（parses + 播放页正则）----
    // 只有网页协议插件不存在/不认领时才到这里；失败原因走 WM_USER+9 原样显示。
    const std::string line_flag = core::WideToUtf8(src.name);  // vod_play_from
    // 自定义 flag 的播放串（加密串常伪装成 .m3u8）即使看起来是直链，也要先过
    // 一次播放器 / flag 解析器。只试一次，避免解析结果回传后无限重入。
    const bool need_player = !st->player_tried && !line_flag.empty() &&
                             !ttplayer::video::IsBuiltinPlayerFlag(line_flag);
    if (need_player) st->player_tried = true;
    if (need_player ||
        (!LooksPlayableMediaUrl(src.url) && !LooksLikeHlsUrl(src.url))) {
        st->resolving = true;
        if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
        const auto url = src.url;
        const auto parses = st->parses;
        const auto headers = st->open_headers;
        // 重开（player_tried 已置位）时不再把 flag 传进去，防止重复跑 JS 播放器。
        const std::string flag = need_player ? line_flag : std::string();
        const std::string player_url = need_player ? st->player_url : std::string();
        const std::string player_base_url = st->player_base_url;
        const HWND hwnd = st->hwnd;
        std::thread([url, parses, headers, flag, player_url, player_base_url, hwnd]() {
            try {
                musicfree::NetworkConfig cfg;
                for (const auto& h : headers)
                    if (_stricmp(h.first.c_str(), "user-agent") == 0)
                        cfg.user_agent = core::Utf8ToWide(h.second);
                const auto r = ResolveVideoUrl(core::WideToUtf8(url), parses, cfg,
                                               headers, flag, player_url,
                                               player_base_url);
                if (r.url.empty()) {
                    PostMessageW(hwnd, WM_USER + 9, 0,
                                 reinterpret_cast<LPARAM>(_strdup(
                                     "网页协议插件与本地解析链都没能拿到媒体直链。\n\n"
                                     "优酷/腾讯等官方网页线路需要站外解析接口："
                                     "请在「视频源」里导入带 parses 段的影视仓配置；"
                                     "分享页线路则必须由站点自己的解析接口处理。\n\n"
                                     "也可以点标题栏「切换源」换一条 m3u8/mp4 线路。")));
                    return;
                }
                PostMessageW(hwnd, WM_USER + 7, 0,
                             reinterpret_cast<LPARAM>(
                                 new ResolvedMedia{core::Utf8ToWide(r.url), r.headers}));
            } catch (const std::exception& e) {
                PostMessageW(hwnd, WM_USER + 9, 0,
                             reinterpret_cast<LPARAM>(_strdup(e.what())));
            }
        }).detach();
        InvalidateRect(st->hwnd, nullptr, FALSE);
        return;
    }

    if (LooksLikeHlsUrl(src.url)) {
        // 协议插件（ttp_v_hls.dll）已在上面的通用协议路由里尝试过；
        // 走到这里说明 DLL 不存在或 MF 打不开本地代理 → 用原链路兜底。
        // ---- 无协议插件 → MF 直连试 8 秒 → 全量拼接（原有链路）----
        if (st->backend && !st->hls_running) {
            st->hls_direct = true;
            st->hls_direct_deadline = GetTickCount() + 8000;
            st->opened_ok = st->backend->Open(st->video_host, src.url,
                                               st->open_headers);
            if (st->opened_ok) {
                st->backend->Play();
                st->hls_running = true;      // 复用「正在缓冲」浮层
                st->hls_percent = 0;
                st->hls_shared = std::make_shared<HlsShared>();
                std::weak_ptr<HlsShared> weak = st->hls_shared;
                const HWND hwnd = st->hwnd;
                // 兜底线程：8 秒内既没出画面也没报错 → 主动回退到拼接路径。
                std::thread([weak, hwnd]() {
                    const auto deadline = GetTickCount() + 8500;
                    while (GetTickCount() < deadline) {
                        if (!weak.lock()) return;
                        Sleep(200);
                    }
                    if (auto s = weak.lock())
                        PostMessageW(hwnd, WM_USER + 8, 0, 0);
                }).detach();
                InvalidateRect(st->hwnd, nullptr, FALSE);
                return;
            }
            st->hls_direct = false;   // 系统不支持 HLS 源 → 走拼接
        }
        // ---- ② 回退：后台拼接全部 ts 分段成本地 .ts ----
        // 拼接期间窗口显示「正在缓冲 n%」。
        if (st->hls_running) return;   // 已有一轮拼接在跑
        StartHlsConcat(st, src);
        return;
    }
    if (st->backend) {
        st->opened_ok = st->backend->Open(st->video_host, src.url,
                                          st->open_headers);
        if (st->opened_ok) {
            st->backend->Play();
        } else if (!st->open_headers.empty() &&
                   LooksPlayableMediaUrl(src.url)) {
            // MF 窗口化后端无法携带请求头，带 Referer/鉴权的直链直接交给 MF
            // 会失败（表现为「无法播放」）。退回：用带请求头的下载链路把整段落盘
            // 成临时文件，再交给 MF（与 HLS 拼接同源）。后台线程跑，避免阻塞 UI。
            st->download_running = true;
            const auto headers = st->open_headers;
            const auto url = src.url;
            const HWND hwnd = st->hwnd;
            std::thread([headers, url, hwnd]() {
                try {
                    musicfree::NetworkConfig cfg;
                    for (const auto& h : headers)
                        if (_stricmp(h.first.c_str(), "user-agent") == 0)
                            cfg.user_agent = core::Utf8ToWide(h.second);
                    auto data = musicfree::DownloadBinary(core::WideToUtf8(url),
                                                         cfg, headers);
                    if (!data.empty()) {
                        wchar_t tmp[MAX_PATH]{};
                        GetTempPathW(MAX_PATH, tmp);
                        auto dir = std::filesystem::path(tmp) / L"TTPlayerVid";
                        std::error_code ec;
                        std::filesystem::create_directories(dir, ec);
                        // 扩展名沿用 URL，便于 MF 选对解封装器。
                        std::wstring ext = L".mp4";
                        const std::string u8 = core::WideToUtf8(url);
                        const size_t dot = u8.rfind('.');
                        if (dot != std::string::npos) {
                            const std::string e = u8.substr(dot);
                            if (e.size() <= 6) ext = core::Utf8ToWide(e);
                        }
                        std::uint64_t h = 1469598103934665603ULL;
                        for (unsigned char ch : u8) {
                            h ^= ch; h *= 1099511628211ULL;
                        }
                        wchar_t fn[40];
                        swprintf_s(fn, L"%016llx%ls",
                                   static_cast<unsigned long long>(h), ext.c_str());
                        const auto p = dir / fn;
                        std::ofstream f(p, std::ios::binary | std::ios::trunc);
                        f.write(reinterpret_cast<const char*>(data.data()),
                                static_cast<std::streamsize>(data.size()));
                        f.close();
                        if (f)
                            PostMessageW(hwnd, WM_USER + 6, 100,
                                         reinterpret_cast<LPARAM>(
                                             new std::wstring(p.wstring())));
                        else
                            PostMessageW(hwnd, WM_USER + 4, 0,
                                         reinterpret_cast<LPARAM>(_strdup(
                                             "下载落盘失败")));
                    } else {
                        PostMessageW(hwnd, WM_USER + 4, 0,
                                     reinterpret_cast<LPARAM>(_strdup(
                                         "下载失败（空响应）")));
                    }
                } catch (const std::exception& e) {
                    PostMessageW(hwnd, WM_USER + 4, 0,
                                 reinterpret_cast<LPARAM>(_strdup(e.what())));
                }
            }).detach();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        } else {
            st->error_text = LooksPlayableMediaUrl(src.url)
                ? L"无法打开该视频地址"
                : L"该线路是网页分享链接，需要「视频源」配置里带 parses 解析"
                  L"接口，网页协议插件才能拿到真实播放地址。\n\n"
                  L"请点标题栏「切换源」换一条线路，或换一部影片。";
            // 渲染子窗口盖在父窗之上，不藏起来的话「播放失败」浮层看不见，
            // 用户只看到一片空白（用户反馈「播放窗口一直无法正常播放」）。
            AppendPlayFailLog(
                "类型: MF 直接打开失败\n影片/集名: " +
                core::WideToUtf8(st->title_text) + "\n当前线路: " +
                core::WideToUtf8(st->line_label) + "\n当前地址: " +
                core::WideToUtf8(src.url) + "\n失败原因: " +
                core::WideToUtf8(st->error_text) + "\n");
            if (st->video_host) ShowWindow(st->video_host, SW_HIDE);
        }
    }
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void ShowVideoPlayer(HWND owner, const std::wstring& url, const std::wstring& title,
                     const std::wstring& line_label,
                     const skin::BrowserSkin* browser_skin,
                     const std::map<std::string, std::string>& headers,
                     const std::vector<VideoPlayerSource>& sources,
                     const std::vector<VideoParse>& parses,
                     const std::string& player_url,
                     const std::string& player_base_url,
                     const std::vector<VideoPlayerEpisode>& episodes,
                     int start_ms, const std::wstring& history_id) {
    // 当前集下标：历史记录用它续播「第 N 集」，切集后也据此更新。
    auto find_episode = [](const std::vector<VideoPlayerEpisode>& eps,
                           const std::wstring& target) -> int {
        for (size_t i = 0; i < eps.size(); ++i)
            if (!eps[i].url.empty() && eps[i].url == target)
                return static_cast<int>(i);
        return eps.empty() ? -1 : 0;
    };
    // ── 复用已打开的播放窗 ──────────────────────────────────────────
    // 点另一集 / 另一部片时不再叠一个新窗口：直接让现有播放窗切到新地址、
    // 更新标题与线路表，并置于前台（用户要求「在原有窗口播放」）。
    if (g_player && IsWindow(g_player->hwnd)) {
        PlayerState* st = g_player;
        st->owner = owner;
        st->base_title = title;
        // 选集列表：切集/切片都要刷新（电影传空 → 隐藏「选集」按钮）。
        st->episodes = episodes;
        st->current_ep = find_episode(episodes, url);
        st->history_id = history_id;
        st->pending_start_ms = start_ms;
        st->start_applied = false;
        st->last_history_save = GetTickCount();
        st->title_text = L"千千静听 · " +
                         (title.empty() ? std::wstring(L"视频播放") : title);
        st->open_headers = headers;
        st->parses = parses;
        st->player_url = player_url;
        st->player_base_url = player_base_url;
        if (st->parses.empty()) st->parses = LoadBuiltinParses();
        if (browser_skin) {
            st->browser_skin = browser_skin;
            st->skinned = true;
            st->skin_transparent =
                frame::TransparentColor(nullptr, *browser_skin);
        }
        std::wstring clean_url = url;
        {
            const size_t scheme = clean_url.find(L"://");
            const size_t hash = clean_url.find(L'#');
            if (scheme != std::wstring::npos && hash != std::wstring::npos &&
                hash > scheme + 2)
                clean_url = clean_url.substr(0, hash);
        }
        if (sources.empty()) {
            st->sources = {{clean_url, line_label}};
            st->source_index = 0;
        } else {
            st->sources = sources;
            st->source_index = -1;
            for (size_t i = 0; i < st->sources.size(); ++i)
                if (st->sources[i].url == clean_url) {
                    st->source_index = static_cast<int>(i);
                    break;
                }
            if (st->source_index < 0) {
                st->sources.push_back({clean_url, line_label});
                st->source_index =
                    static_cast<int>(st->sources.size()) - 1;
            } else {
                st->sources[st->source_index].url = clean_url;
            }
        }
        st->line_label = line_label;
        st->auto_source_tried = 0;   // 新一集/新片：重新允许自动换线兜底
        st->same_src_retry_index = -1;
        st->resolve_retry_pending = false;
        KillTimer(st->hwnd, 2);
        st->error_text.clear();
        st->web_host.reset();
        st->web_mode = false;
        SetWindowTextW(st->hwnd, st->title_text.c_str());
        LayoutPlayer(st);
        OpenSourceAt(st, st->source_index);
        if (IsIconic(st->hwnd)) ShowWindow(st->hwnd, SW_RESTORE);
        SetForegroundWindow(st->hwnd);
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = VideoPlayerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TTPlayerVideoPlayerWnd";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    // 复用主窗口的应用图标：自绘 WS_POPUP 窗口不会自动显示系统标题栏图标，
    // 左上角会显得「不像一个应用窗口」。任务栏 / Alt-Tab 也一并用它。
    HICON app_icon = nullptr;
    {
        const HWND main = FindWindowW(L"TTPlayer_PlayerWnd", nullptr);
        if (main) {
            app_icon = reinterpret_cast<HICON>(
                SendMessageW(main, WM_GETICON, ICON_SMALL, 0));
            if (!app_icon)
                app_icon = reinterpret_cast<HICON>(
                    SendMessageW(main, WM_GETICON, ICON_BIG, 0));
        }
        if (!app_icon) app_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wc.hIcon = app_icon;
    RegisterClassW(&wc);

    auto* st = new PlayerState();
    st->owner = owner;
    st->app_icon = app_icon;
    st->base_title = title;
    st->episodes = episodes;   // 剧集选集列表（电影为空）
    st->current_ep = find_episode(episodes, url);
    st->history_id = history_id;
    st->pending_start_ms = start_ms;
    st->last_history_save = GetTickCount();
    // 标题栏带应用名前缀，左上角（图标 + 应用名 + 片名）形成完整窗口标识。
    st->title_text = L"千千静听 · " +
                     (title.empty() ? std::wstring(L"视频播放") : title);
    st->line_label = line_label;
    st->last_input = GetTickCount();
    st->browser_skin = browser_skin;
    st->skinned = browser_skin != nullptr;
    st->open_headers = headers;
    st->parses = parses;   // 影视仓 parses：播放页地址 → 媒体直链
    st->player_url = player_url;
    st->player_base_url = player_base_url;
    // 配置未提供 parses（影视仓配置普遍不带）→ 启用内置默认解析接口，
    // 否则优酷/腾讯/哔哩哔哩等官方网页线路无法「播放页 → 媒体直链」而失败。
    if (st->parses.empty())
        st->parses = LoadBuiltinParses();
    // 部分源把集名拼在地址后面（"...m3u8#第1期上"）——交给播放器前去掉 # 片段。
    std::wstring clean_url = url;
    {
        const size_t scheme = clean_url.find(L"://");
        const size_t hash = clean_url.find(L'#');
        if (scheme != std::wstring::npos && hash != std::wstring::npos &&
            hash > scheme + 2)
            clean_url = clean_url.substr(0, hash);
    }
    if (sources.empty()) {
        st->sources = {{clean_url, line_label}};
        st->source_index = 0;
    } else {
        st->sources = sources;
        st->source_index = -1;
        for (size_t i = 0; i < st->sources.size(); ++i)
            if (st->sources[i].url == clean_url) {
                st->source_index = static_cast<int>(i);
                break;
            }
        if (st->source_index < 0) {
            // ⚠ 不能用「覆盖第 0 项」兜底：那会把别的线路的名字安到当前
            // 地址上（真机出现过「线路 ffm3u8 却播 /share/ 网页」）。
            // 找不到就追加一条属于当前线路的记录。
            st->sources.push_back({clean_url, line_label});
            st->source_index = static_cast<int>(st->sources.size()) - 1;
        } else {
            st->sources[st->source_index].url = clean_url;  // 存清理版
        }
    }
    if (st->skinned)
        st->skin_transparent = frame::TransparentColor(nullptr, *browser_skin);
    st->auto_source_tried = 0;   // 新一集/新片：重新允许自动换线兜底
    st->same_src_retry_index = -1;
    st->resolve_retry_pending = false;
    // WS_CLIPCHILDREN：video_host（MF 渲染面子窗口）区域不被父窗重绘覆盖，
    // 否则播放中任何一次窗口重绘都会先把画面刷黑再恢复（肉眼可见的闪烁）。
    const DWORD style = (st->skinned ? (WS_POPUP | WS_VISIBLE)
                                     : (WS_POPUP | WS_VISIBLE | WS_THICKFRAME)) |
                        WS_CLIPCHILDREN;
    // 恢复上次的位置/大小（首次运行仍用系统默认位置）。保存的坐标可能因换
    // 显示器/分辨率整体落在屏幕外，这时夹回主屏工作区，避免「打开了却看不见」。
    int win_x = CW_USEDEFAULT, win_y = CW_USEDEFAULT, win_w = 880, win_h = 540;
    bool restore_maximized = false;
    RECT saved{};
    if (LoadVideoWindowPlacement(&saved, &restore_maximized)) {
        const int w = saved.right - saved.left;
        const int h = saved.bottom - saved.top;
        if (!MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(
                    MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY),
                    &mi)) {
                const int work_w = mi.rcWork.right - mi.rcWork.left;
                const int work_h = mi.rcWork.bottom - mi.rcWork.top;
                win_w = std::min(w, work_w - 48);
                win_h = std::min(h, work_h - 48);
                win_x = mi.rcWork.left + 24;
                win_y = mi.rcWork.top + 24;
            }
        } else {
            win_x = saved.left;
            win_y = saved.top;
            win_w = w;
            win_h = h;
        }
    }
    // ⚠ 父/属主窗口传 nullptr：主窗口最小化时不能把播放窗一起最小化
    // （用户要求「最小化主窗口，播放窗继续显示」）。WS_EX_APPWINDOW 仍让
    // 它出现在任务栏；owner 参数只保留给「选集」菜单之类的弹窗归属。
    st->hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"TTPlayerVideoPlayerWnd", L"",
        style, win_x, win_y, win_w, win_h, nullptr, nullptr,
        GetModuleHandleW(nullptr), st);
    if (!st->hwnd) { delete st; return; }
    // 诊断：入口 / 皮肤 / 选集数量（排查「两个入口样式不一致」时先看这行）。
    AppendUiLog(std::string("PLAYER open skinned=") +
                (st->skinned ? "1" : "0") + " episodes=" +
                std::to_string(st->episodes.size()) + " history=" +
                (st->history_id.empty() ? "0" : "1") + " resume=" +
                std::to_string(st->pending_start_ms));
    if (restore_maximized) ShowWindow(st->hwnd, SW_MAXIMIZE);
    if (app_icon) {
        SendMessageW(st->hwnd, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(app_icon));
        SendMessageW(st->hwnd, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(app_icon));
    }
    SetWindowTextW(st->hwnd, st->title_text.c_str());  // 任务栏 / Alt-Tab 显示片名
    LayoutPlayer(st);
    if (st->skinned) {
        RECT c{};
        GetClientRect(st->hwnd, &c);
        frame::ApplyWindowRegion(st->hwnd, *st->browser_skin, st->skin_layout, c,
                                 st->skin_transparent,
                                 ui::ResolveScaleForWindow(st->hwnd));
    }

    VideoBackendCallbacks cb;
    cb.on_ready = [hwnd = st->hwnd] { PostMessageW(hwnd, WM_USER + 1, 0, 0); };
    cb.on_playing = [hwnd = st->hwnd](bool playing) {
        PostMessageW(hwnd, WM_USER + 2, playing ? 1 : 0, 0);
    };
    cb.on_end = [hwnd = st->hwnd] { PostMessageW(hwnd, WM_USER + 3, 0, 0); };
    cb.on_error = [hwnd = st->hwnd](const std::string& e) {
        PostMessageW(hwnd, WM_USER + 4, 0, reinterpret_cast<LPARAM>(_strdup(e.c_str())));
    };
    st->backend = CreateDefaultVideoBackend(std::move(cb));
    OpenSourceAt(st, st->source_index);
    SetForegroundWindow(st->hwnd);
    (void)g_player;
}

} // namespace ttplayer::video
