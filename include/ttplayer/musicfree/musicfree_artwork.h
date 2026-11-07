#pragma once

// MusicFree 兼容层 —— 封面图异步下载 / 解码 / 缓存
//
// 音乐窗的排行榜 / 歌单 / 推荐页需要显示封面（MusicFree 插件的 coverImg / artwork）。
// 这里把「下载（WinHTTP）+ 磁盘缓存 + WIC/GDI+ 解码成缩略图」封装成一个可复用的
// 助手：调用方在绘制某个条目时 Request(url)，助手在后台完成下载与解码，然后通过
// 回调通知（回调只在 UI 线程里做「记录 + 触发重绘」，不要直接做 GDI 绘制）。已解码
// 的位图由本助手持有，调用方用 Peek(url) 借出来画，不要 DeleteObject。
//
// 为什么不用洛雪源协议取封面：洛雪 user-api 只暴露 musicUrl / lyric / pic（且 pic 仅
// 本地源），没有「歌单封面」这类内容能力；封面数据来自 MusicFree 插件。

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <windows.h>
#include <filesystem>

namespace ttplayer::musicfree {

struct NetworkConfig;

class ArtworkCache {
public:
    // cache_dir：封面缓存目录（<exe>/musicfree/artcache）。network：下载用的代理 /
    // 超时配置，通常取自 AppMusicFreeManager()->network()。
    ArtworkCache(std::filesystem::path cache_dir, NetworkConfig network);
    ~ArtworkCache();

    ArtworkCache(const ArtworkCache&) = delete;
    ArtworkCache& operator=(const ArtworkCache&) = delete;

    // 请求一张图。max_edge 为缩略图最长边（像素）。缓存命中（内存或磁盘）时回调
    // 可能同步触发；需要网络时后台下载 + 解码后再回调。回调参数即请求的 url，
    // 便于调用方定位要重绘的条目。同一 url 并发只下载一份，多个等待者共享结果。
    void Request(const std::string& url, int max_edge,
                 std::function<void(const std::string&)> on_ready);

    // 取出已解码的位图（借出，不转移所有权；调用方不得 DeleteObject）。未就绪返回
    // nullptr。线程安全。
    HBITMAP Peek(const std::string& url);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace ttplayer::musicfree
