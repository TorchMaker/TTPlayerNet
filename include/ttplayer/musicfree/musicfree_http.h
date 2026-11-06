#pragma once

// MusicFree 兼容层 —— axios 后端 + cheerio 垫片
//
// MusicFree 插件通过 require("axios") 做网络请求。由于沙箱内必须落到真实
// 网络，这里用 TTPlayer 既有的 WinHTTP 实现 axios 的 get/post/defaults/
// create/interceptors，接口形状对齐 MusicFree（基于 axios + react-native）：
//   axios.get(url, config) -> Promise<{ data, status, statusText, headers, ... }>
//   axios.post(url, data, config) -> Promise<...>
//   axios.defaults.{ headers, timeout }
//   axios.create(config) -> 新的 axios 实例
//   axios.interceptors.response.use(cb) -> 记录回调（尽力）
//
// 关键：get/post 执行“阻塞式”WinHTTP，拿到响应后同步 resolve Promise。这样
// 插件代码里 await axios.get(...) 的续体作为微任务入队，由 QuickJsEngine::Await
// 用 JS_ExecutePendingJob 泵完，无需外部事件循环。
//
// 安全：与歌词子系统一致，绝不禁用 TLS 校验。

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// JSValue 在 quickjs-ng 中是指针 typedef，必须包含真实头文件而非前向声明。
#include <quickjs.h>

namespace ttplayer::musicfree {
struct NetworkConfig;
class QuickJsEngine;

// 构造 axios 模块对象（module.exports）。调用方负责 JS_FreeValue。
JSValue CreateAxiosModule(JSContext* ctx, const NetworkConfig& cfg, QuickJsEngine* engine);

// 构造 cheerio 垫片（尽力实现 load/text/attr/find/each）。返回模块对象。
JSValue CreateCheerioShim(JSContext* ctx);

// 构造全局 fetch 函数（复刻 RN/浏览器 fetch；部分插件直接用 fetch 而非 axios）。
// 返回可直接 SetGlobal("fetch", ...) 的函数值。
JSValue CreateFetchGlobal(JSContext* ctx);

// 通用阻塞式 HTTP 交换（供多协议音源层复用）。
//
// MusicFree 插件走 axios 模块；洛雪(lx.request) / Listen1 / REST 声明式音源
// 需要直接发请求并拿到原始状态码、响应头与二进制 body，故把 WinHttpBlocking
// 以「协议无关」的形状导出。method 支持任意动词，headers 为 UTF-8 键值对，
// body 为空即无请求体。失败（含网络错误）抛 std::runtime_error；HTTP 非 2xx
// **不抛**，由调用方按协议决定如何处理。
struct HttpExchange {
    unsigned long status = 0;
    std::string status_text;
    std::map<std::string, std::vector<std::string>> headers;
    std::vector<std::uint8_t> body;
};

HttpExchange PerformHttpRequest(
    const std::string& url, const std::string& method,
    const std::map<std::string, std::string>& headers,
    const std::vector<std::uint8_t>& body, const NetworkConfig& cfg);

// 下载文本（注册表 JSON / 插件 JS）。失败抛 std::runtime_error。
std::string DownloadText(const std::string& url, const NetworkConfig& cfg);

// 下载二进制资源（音频文件）的原始字节。与 DownloadText 走同一条 WinHTTP 链路
// （自动跟随 301/302 重定向、走 NetworkConfig 里的代理与超时）。失败抛
// std::runtime_error。之所以单独开一个返回 vector 而不是复用 DownloadText：
// std::string 会把 0x00 字节当成终止符截断，音频里出现空字节是家常便饭。
std::vector<std::uint8_t> DownloadBinary(const std::string& url,
                                         const NetworkConfig& cfg,
                                         const std::map<std::string, std::string>& headers = {});

} // namespace ttplayer::musicfree
