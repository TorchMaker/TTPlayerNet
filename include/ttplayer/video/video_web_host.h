#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include "ttplayer/video/video_export.h"
namespace ttplayer::video {
// 视频弹出窗的「网页兜底」宿主：官方网页线路的所有解析接口都失败时，
// 直接用 WebView2 在画面区加载该网页播放。
//
// 本头文件**不得**包含 WebView2.h：video_player_window.cpp 属于静态库
// ttplayer_core，而 WebView2 只链接在 exe ttplayer_rebuild 上。
// exe 在 OleInitialize 成功后经 SetVideoWebHostFactory 注入真正的实现。
// 网页里 <video> 的播放状态：由注入脚本定时回推，供 Win32 自绘控制条显示。
struct VideoWebState {
  double position = 0.0;   // currentTime（秒）
  double duration = 0.0;   // duration（秒，未知为 0）
  bool playing = false;    // !paused
  bool has_video = false;  // 页面里是否已找到 video 元素
};

class VideoWebHost {
 public:
  virtual ~VideoWebHost() = default;
  virtual void Resize(RECT content) = 0;   // 父窗口客户区坐标
  virtual bool Ready() const = 0;          // 导航完成且成功
  virtual bool Failed() const = 0;         // 创建/导航失败
  virtual const char* LastError() const = 0;
  // 页面请求进入/退出全屏（WebView2 ContainsFullScreenElementChanged）。
  // 宿主播放窗据此切换真全屏；默认空实现，旧宿主不受影响。
  virtual void SetFullscreenCallback(std::function<void(bool)> fn) {
    (void)fn;
  }
  // 页面直链嗅探：注入脚本 hook fetch/XHR/<video src>/hls.js，发现 m3u8、mp4
  // 等媒体地址就回推。宿主播放窗拿到后关掉网页、切回原生播放（Bttwoo 这类
  // 「直链由页面 WASM/JS 现算」的站点只靠 HTTP 解析拿不到地址）。
  virtual void SetMediaSniffCallback(std::function<void(const std::string&)> fn) {
    (void)fn;
  }
  // ── 自定义控制条驱动网页播放器 ──────────────────────────────────────
  // 用户要求「不要使用 web 视频的进度条和播放框，使用自定义的」：
  // WebView2 仍只当渲染面，站点自带控件由注入脚本隐藏；播放/暂停/跳转/音量
  // 全部由 Win32 自绘控制条经下面这些接口驱动页面里的 <video>。
  virtual void Seek(double seconds) { (void)seconds; }
  // 页面未回推 duration 时按比例拖动（页面端用 video.duration / seekable
  // 自己换算），否则宿主拿不到时长就完全不发跳转，看起来又是「拖不动」。
  virtual void SeekRatio(double ratio) { (void)ratio; }
  virtual void SetPaused(bool paused) { (void)paused; }
  virtual void SetVolume(double volume) { (void)volume; }
  virtual void SetMuted(bool muted) { (void)muted; }
  virtual void SetStateCallback(std::function<void(const VideoWebState&)> fn) {
    (void)fn;
  }
};
using VideoWebHostFactory = std::unique_ptr<VideoWebHost> (*)(
    HWND parent, RECT content, const std::wstring& url);
TTPLAYER_VIDEO_API void SetVideoWebHostFactory(VideoWebHostFactory factory);
TTPLAYER_VIDEO_API VideoWebHostFactory GetVideoWebHostFactory();
}  // namespace ttplayer::video
