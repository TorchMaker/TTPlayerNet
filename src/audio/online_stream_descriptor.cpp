// 在线流描述侧信道实现（见 online_stream_descriptor.h）。

#include "ttplayer/audio/online_stream_descriptor.h"

#include <mutex>

namespace ttplayer::audio {

namespace {
std::mutex g_mutex;
std::optional<OnlineStreamDescriptor> g_pending;
} // namespace

void SetPendingOnlineStream(OnlineStreamDescriptor descriptor) {
    std::scoped_lock lock(g_mutex);
    g_pending = std::move(descriptor);
}

std::optional<OnlineStreamDescriptor> TakePendingOnlineStream(
    const std::string& url) {
    std::scoped_lock lock(g_mutex);
    if (!g_pending || g_pending->url != url) return std::nullopt;
    std::optional<OnlineStreamDescriptor> taken = std::move(g_pending);
    g_pending.reset();
    return taken;
}

} // namespace ttplayer::audio
