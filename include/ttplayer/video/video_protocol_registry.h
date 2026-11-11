#pragma once

// 视频协议插件加载器：扫描 <exe>\AddIn\ttp_v_*.dll，按 URL 路由。
// 接口契约见 video_protocol_api.h。

#include <string>
#include <vector>

#include <windows.h>

#include "ttplayer/video/video_protocol_api.h"

namespace ttplayer::video {

struct LoadedProtocol;   // 内部实现，头文件里不暴露

// 指向一个已打开的协议会话（由 registry 创建/销毁，播放窗只持有不透明指针）。
struct ProtocolSession {
    LoadedProtocol* proto = nullptr;
    void*           handle = nullptr;
};

class VideoProtocolRegistry {
public:
    static VideoProtocolRegistry& Instance();

    // 扫描并加载协议 DLL（幂等：已加载则直接返回）。首次 Open 前自动调用。
    void LoadAll();

    size_t Count() const;

    // 按 probe 置信度选路并 VPOpen。
    // 成功：返回会话，*out_media_url 为交给播放器的地址（本地代理等）。
    // 失败：返回 nullptr（无人认领或打开失败，调用方走原有直连路径）。
    // options 为协议私有参数（UTF-16 JSON，网页协议用它收 parses）。
    ProtocolSession* Open(const std::wstring& url,
                          const std::wstring& user_agent,
                          const std::wstring& referer,
                          int timeout_ms,
                          const std::wstring& options,
                          std::wstring* out_media_url);

    // —— 以下三个是 ProtocolSession 的包装，转发到对应协议 DLL 的导出 ——
    static int  Poll(ProtocolSession* s, std::wstring* out_err);
    static void Close(ProtocolSession*& s);

private:
    VideoProtocolRegistry() = default;
    ~VideoProtocolRegistry();
    VideoProtocolRegistry(const VideoProtocolRegistry&) = delete;
    VideoProtocolRegistry& operator=(const VideoProtocolRegistry&) = delete;

    std::vector<LoadedProtocol*> protocols_;
    bool loaded_ = false;
};

} // namespace ttplayer::video
