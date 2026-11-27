#pragma once

// 视频解码后端接口（插件式，与音频 AddIn 思路一致）
//
// 不同视频流（HLS / MP4 / 直播 / 私有协议）由不同后端兼容。默认后端使用 Windows
// 自带的 Media Foundation（无需外部库）；用户可放置实现本接口的 DLL 来覆盖默认
// 后端（例如 VLC / MPV 后端），命名约定：导出 CreateVideoBackend()。

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <windows.h>

namespace ttplayer::video {

// 播放事件回调（后端 → UI）。
struct VideoBackendCallbacks {
    std::function<void()> on_ready;      // 可播放（已缓冲首段）
    std::function<void()> on_end;        // 播放结束
    std::function<void(const std::string&)> on_error;  // 出错
    std::function<void(bool)> on_playing; // 开始/暂停切换
    // 数据不足进入等待 / 数据恢复（MF_MEDIA_ENGINE_EVENT_WAITING / STALLED /
    // PLAYING）。true=开始缓冲（画面冻结），false=恢复。用于「正在缓冲」提示、
    // 冻结假进度（本地时钟），以及缓冲超时后的自动重试。
    std::function<void(bool)> on_buffering;
};

class IVideoBackend {
public:
    virtual ~IVideoBackend() = default;

    // 打开一个视频地址（url 为已解析出的直链；headers 为可选请求头）。
    virtual bool Open(HWND render_hwnd, const std::wstring& url,
                      const std::map<std::string, std::string>& headers) = 0;

    virtual void Play() = 0;
    virtual void Pause() = 0;
    virtual bool IsPlaying() const = 0;

    // 位置 / 时长（秒）。
    virtual double GetPosition() const = 0;
    virtual double GetDuration() const = 0;
    virtual void Seek(double seconds) = 0;

    virtual void SetVolume(double volume) = 0;   // 0..1
    virtual void SetMuted(bool muted) = 0;
    virtual bool IsMuted() const = 0;

    virtual void SetFullscreen(bool fullscreen) = 0;
    virtual bool IsFullscreen() const = 0;

    // 视频的显示宽高比（DAR，优先于像素尺寸）。播放窗用它把渲染子窗口
    // 等比居中（letterbox），避免全屏铺满导致画面被拉伸。
    // 元数据还没就绪 / 后端不支持时返回 false，调用方按铺满处理。
    virtual bool GetVideoSize(int* width, int* height) const {
        if (width) *width = 0;
        if (height) *height = 0;
        return false;
    }

    virtual void Close() = 0;
};

// 创建默认（Media Foundation）后端。
std::unique_ptr<IVideoBackend> CreateDefaultVideoBackend(VideoBackendCallbacks cb);

// 从 DLL 加载自定义后端；失败返回 nullptr。DLL 需导出：
//   extern "C" __declspec(dllexport)
//   IVideoBackend* CreateVideoBackend(VideoBackendCallbacks cb);
std::unique_ptr<IVideoBackend> LoadVideoBackendPlugin(
    const std::filesystem::path& dll, VideoBackendCallbacks cb);

} // namespace ttplayer::video
