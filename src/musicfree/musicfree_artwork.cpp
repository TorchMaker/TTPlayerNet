// MusicFree 兼容层 —— 封面图异步下载 / 解码 / 缓存实现
//
// 复用既有的 WinHTTP 下载链路（DownloadBinary，与插件搜索同一条代理 / 超时）与
// ui::DecodeCoverImage（WIC + GDI+ 解码为 32 位 BGRA DIB）。下载结果按 url 哈希
// 落到磁盘缓存，解码后的位图常驻内存由本助手持有，调用方通过 Peek 借出绘制。

#include "ttplayer/musicfree/musicfree_artwork.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <filesystem>
#include <windows.h>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/ui/cover_image.h"

namespace ttplayer::musicfree {

namespace {

// url → 文件名（不含扩展名）。封面真实类型由 DecodeCoverImage 嗅探字节，不需要扩展名。
std::string UrlHash(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL; // FNV-1a 64
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<std::uint8_t> out((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    return out;
}

} // namespace

struct ArtworkCache::Impl : std::enable_shared_from_this<ArtworkCache::Impl> {
    std::filesystem::path cache_dir;
    NetworkConfig network;
    std::mutex mutex;
    std::map<std::string, HBITMAP> memory;   // url -> 已解码位图（本助手拥有）
    std::set<std::string> inflight;          // 正在下载 / 解码的 url
    // 同一 url 多个等待者：完成（或失败）后逐一回调。
    std::map<std::string, std::vector<std::function<void(const std::string&)>>> waiting;

    // 数据一到位就把几十张封面全部投进来，若每个 url 各起一个 WinHTTP 线程，
    // 32 位进程的栈空间与 CDN 并发都会吃不消。这里限制同时在跑的任务数，
    // 队列按调用顺序启动（首屏卡片自然排在前面），完成一个立刻补一个。
    static constexpr int kMaxParallel = 8;
    struct Job { std::string url; int max_edge; };
    std::deque<Job> queued;
    int active = 0;

    explicit Impl(std::filesystem::path dir, NetworkConfig cfg)
        : cache_dir(std::move(dir)), network(std::move(cfg)) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
    }

    ~Impl() {
        for (auto& kv : memory)
            if (kv.second) DeleteObject(kv.second);
        memory.clear();
    }

    // 核心：确保 url 有一张解码好的位图（命中内存 / 磁盘则直接用，否则下载 + 解码）。
    // 返回内存里该 url 的位图（可能为空表示失败）。
    HBITMAP EnsureDecoded(const std::string& url, int max_edge) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = memory.find(url);
            if (it != memory.end()) return it->second;
        }
        const std::filesystem::path file = cache_dir / (UrlHash(url) + ".img");
        std::vector<std::uint8_t> bytes;
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            bytes = ReadAllBytes(file);
        } else {
            try {
                bytes = DownloadBinary(url, network);
            } catch (...) {
                bytes.clear();
            }
            if (!bytes.empty()) {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                if (out) out.write(reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<std::streamsize>(bytes.size()));
            }
        }
        HBITMAP bitmap = nullptr;
        if (!bytes.empty()) {
            auto cover = ui::DecodeCoverImage(
                std::span<const unsigned char>(bytes.data(), bytes.size()),
                static_cast<UINT>(max_edge));
            bitmap = cover.bitmap;
        }
        std::lock_guard<std::mutex> lock(mutex);
        // 多个并发等待者可能都已下载完毕：以先写入者为准。
        auto it = memory.find(url);
        if (it == memory.end()) {
            memory.emplace(url, bitmap);
            return bitmap;
        }
        if (bitmap && !it->second) {
            it->second = bitmap;
            return bitmap;
        }
        if (bitmap) DeleteObject(bitmap); // 已有别的副本，丢弃重复
        return it->second;
    }

    // 调用方必须已持有 mutex。
    void ScheduleLocked() {
        while (active < kMaxParallel && !queued.empty()) {
            Job job = std::move(queued.front());
            queued.pop_front();
            ++active;
            std::shared_ptr<Impl> self = shared_from_this();
            try {
                std::thread([self, job] {
                    self->EnsureDecoded(job.url, job.max_edge);
                    std::vector<std::function<void(const std::string&)>> callbacks;
                    {
                        std::lock_guard<std::mutex> lock(self->mutex);
                        self->inflight.erase(job.url);
                        auto it = self->waiting.find(job.url);
                        if (it != self->waiting.end()) {
                            callbacks = std::move(it->second);
                            self->waiting.erase(it);
                        }
                        if (self->active > 0) --self->active;
                        self->ScheduleLocked();  // 补位启动下一批
                    }
                    for (auto& cb : callbacks) cb(job.url);
                }).detach();
            } catch (...) {
                // 线程创建失败：把这个任务撤销，别让 active 计数永久占着名额。
                --active;
                inflight.erase(job.url);
                waiting.erase(job.url);
            }
        }
    }
};

ArtworkCache::ArtworkCache(std::filesystem::path cache_dir, NetworkConfig network)
    : impl_(std::make_shared<Impl>(std::move(cache_dir), std::move(network))) {}

ArtworkCache::~ArtworkCache() = default;

void ArtworkCache::Request(const std::string& url, int max_edge,
                           std::function<void(const std::string&)> on_ready) {
    if (url.empty() || !on_ready) return;
    std::shared_ptr<Impl> impl = impl_;
    bool memory_hit = false;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->memory.find(url) != impl->memory.end()) {
            memory_hit = true;
        } else {
            impl->waiting[url].push_back(std::move(on_ready));
            // 同一个 url 入队一次：队列里 / 在途 / 已完成都算已请求。
            if (impl->inflight.insert(url).second) {
                impl->queued.push_back(Impl::Job{url, max_edge});
                impl->ScheduleLocked();
            }
        }
    }
    // 内存命中时在锁外同步回调，避免回调里再调 Peek / Request 造成自锁。
    if (memory_hit) on_ready(url);
}

HBITMAP ArtworkCache::Peek(const std::string& url) {
    if (url.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->memory.find(url);
    return it == impl_->memory.end() ? nullptr : it->second;
}

} // namespace ttplayer::musicfree
