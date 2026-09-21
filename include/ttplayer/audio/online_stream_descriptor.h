#pragma once

// 在线流描述侧信道
//
// AudioEngine::Play 只接受 std::filesystem::path，没法把 MusicFree 解析出的自定义
// 请求头（Referer/User-Agent/签名……）一起传进去。于是 PlayCurrent 在调用
// audio_->Play(http_url) 之前，把一个 OnlineStreamDescriptor 放进这里；
// HttpStreamSource::Open 在解码线程里用同一个 url 把它取出来消费（一次性）。
// 播放是串行的，单槽足够。

#include <map>
#include <optional>
#include <string>

namespace ttplayer::audio {

struct OnlineStreamDescriptor {
    std::string url;                              // 与交给 audio_->Play 的 url 一致（UTF-8）
    std::map<std::string, std::string> headers;  // 自定义请求头（Referer/Authorization…）
    std::string user_agent;                       // 覆盖 UA（空则用默认）
    bool is_hls = false;                          // url 末段为 .m3u8 → 走系统 HLS
};

// 由 UI 线程在 audio_->Play 之前调用。
void SetPendingOnlineStream(OnlineStreamDescriptor descriptor);

// 由解码线程（HttpStreamSource::Open）调用：若槽中 url 与给定 url 匹配则返回并清空，
// 否则返回 nullopt（说明这次播放没带自定义头，回退到默认 URL reader）。
[[nodiscard]] std::optional<OnlineStreamDescriptor> TakePendingOnlineStream(
    const std::string& url);

} // namespace ttplayer::audio
