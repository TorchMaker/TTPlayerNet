#pragma once

// 原生 spider HTTP 封装
//
// 直接复用 musicfree::PerformHttpRequest（WinHTTP，自动跟随重定向、走代理与
// 超时配置），只补两件事：
//   1) 请求头合并：站点 UA/header 优先，缺省 UA 取 NetworkConfig::user_agent；
//   2) 统一日志：spider_id / stage / URL 脱敏 / 状态码 / 耗时 / 失败响应摘要。
// 日志默认落盘 %LOCALAPPDATA%\TTPlayer\logs\spider.log，同时输出到调试器。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/video/video_types.h"

namespace ttplayer::video {

struct SpiderHttpResponse {
    unsigned long status = 0;
    std::string body;  // UTF-8（已去 BOM），二进制内容按原字节保留
    std::map<std::string, std::vector<std::string>> headers;
};

// URL 脱敏：把 query 的值替换成 ***，只保留参数名（日志用）。
std::string MaskSpiderUrl(std::string_view url);

// 日志文件路径（不存在时按需创建目录）。
std::filesystem::path SpiderHttpLogPath();

class SpiderHttp {
public:
    SpiderHttp(std::string spider_id, musicfree::NetworkConfig net,
               VideoSite site = VideoSite{});

    // 当前阶段名（home / category / search / detail / player），用于日志。
    void SetStage(std::string stage);
    const std::string& stage() const { return stage_; }

    // 追加/覆盖默认请求头（跨请求保留，例如 recipe 里的公共头）。
    void SetDefaultHeaders(const std::map<std::string, std::string>& headers);

    // 失败（网络错误 / 无响应）抛 std::runtime_error；HTTP 非 2xx 不抛，
    // 由调用方按站点协议决定（与 video_cms 一致）。
    SpiderHttpResponse Get(const std::string& url,
                           const std::map<std::string, std::string>& headers = {});
    SpiderHttpResponse Post(const std::string& url, std::string_view body,
                            const std::map<std::string, std::string>& headers = {},
                            std::string_view content_type =
                                "application/json; charset=utf-8");

private:
    SpiderHttpResponse Exchange(const std::string& url, const std::string& method,
                                const std::map<std::string, std::string>& headers,
                                const std::vector<std::uint8_t>& body);
    // 合并默认头 → 站点头 → 本次调用头（同名以先出现的为准，UA 缺省兜底）。
    std::map<std::string, std::string> MergeHeaders(
        const std::map<std::string, std::string>& headers) const;
    void Log(const std::string& method, const std::string& url,
             const SpiderHttpResponse& resp, long long elapsed_ms,
             const std::string& failure) const;

    std::string spider_id_;
    musicfree::NetworkConfig net_;
    VideoSite site_;
    std::string stage_ = "general";
    std::map<std::string, std::string> default_headers_;
};

} // namespace ttplayer::video
