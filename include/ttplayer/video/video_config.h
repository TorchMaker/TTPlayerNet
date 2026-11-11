#pragma once

// 视频源兼容层 —— 影视仓 / TVBox / MarcoMovie 配置解析
//
// 支持三种入口：
//  1) 单仓 JSON：含 sites / parses / lives 字段；
//  2) 多仓 JSON：顶层含 "urls":[...]（或 "sites" 里每个 site 自身就是单仓内容）；
//  3) M3U/TXT 直链：url 以 .m3u/.txt 结尾或 body 形如播放列表 —— 作为一组直播源。
// 也处理配置被 Base64 / "$#...#$" AES 包装的情况（basic 还原）。

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_types.h"
#include "ttplayer/video/video_types.h"

namespace ttplayer::video {

// NetworkConfig 完整定义在 ttplayer::musicfree；引入以使用其完整类型。
using ttplayer::musicfree::NetworkConfig;

// 解析结果：可能拆出多份单仓（多仓情况）。
struct ParsedConfigs {
    std::vector<VideoConfig> configs;   // 拆好的单仓
    std::string origin;                 // 入口 URL（用于相对地址补全）
    std::string error;
};

// 从一个 URL 拉取并解析（多仓会递归拉取每个子仓库）。
ParsedConfigs ParseVideoConfigFromUrl(std::string_view url,
                                      const NetworkConfig& cfg);

// 从本地文件解析。
ParsedConfigs ParseVideoConfigFromFile(const std::filesystem::path& path);

// 从一段文本（可能是 JSON / Base64 / 直链）解析；origin 用于补全相对地址。
ParsedConfigs ParseVideoConfigText(std::string_view text,
                                   std::string_view origin,
                                   const NetworkConfig& cfg);

// 影视仓 / TVBox「图片伪装配置」解包。
//
// 很多"防封"配置源返回的不是 JSON，而是一张图片（Content-Type 还写
// image/jpeg、image/x-ms-bmp），真正的 JSON 藏在图片二进制里的某个
// 「8 位标记 + **」之后，且整段做了 Base64：
//
//     <图片二进制> ....  AelmEojv**  eyJzaXRlcyI6W3si...
//
// TVBox 在 ApiConfig.FindResult() 里用正则 "[A-Za-z0]{8}\*\*" 找到标记，
// 把标记之后的整段 Base64 解出来就是配置。本函数是它的等价实现：命中返回
// 解出的文本（不保证是合法 JSON，交给调用方校验），未命中返回空串；
// hit 非空时回写是否命中了伪装格式（用于区分"根本没伪装"和"解出来是垃圾"）。
std::string UnwrapDisguisedConfig(std::string_view raw, bool* hit = nullptr);

// 统一解码入口 —— 按内容特征自动识别并还原任意 TVBox 配置响应体：
//
//   json_plain            明文（含 BOM / // 注释 / 尾随逗号 / 尾部追加 HTML）
//   image_marker_base64   JPEG/PNG/BMP 图片 + 8 位标记 + "**" + Base64
//   aes_cbc_self_2423     "2423" 开头的 hex，AES-128-CBC 自解密（key/iv 内嵌）
//   dollar_shell_base64   旧的 "$#…#$" 文本壳
//   base64_plain          整段 Base64
//
// 成功时 out 为可直接 JSON.parse 的文本，返回 true；不是 JSON 形态（例如裸
// CMS 接口列表、M3U 播放列表）时返回 false，由调用方按原有逻辑继续处理。
// kind 回写命中的解码类型，便于界面提示与日志诊断。
bool DecodeTvboxPayload(std::string_view raw, std::string& out,
                        std::string* kind = nullptr);

// 拉取配置时使用的请求头，对齐 TVBox `ApiConfig`：UA 固定 okhttp/3.15，
// Accept 用影视仓客户端那串浏览器式值。部分配置站会按 UA 决定返回 JSON
// 还是"图片 + 标记"的伪装体，也可能直接拒绝无 UA 的请求。
std::map<std::string, std::string> ConfigRequestHeaders();

// 从 csp_* 站点的 ext（明文 JSON / 字符串 / RC4 密文）里找出标准 CMS 接口。
// 供 VideoSite::FromJson 的 ext 自动还原与原生 spider 的内置实现（T4 等）共用；
// 找不到返回空串。
std::string FindCmsApiFromExt(const nlohmann::json& ext);

// 把 VideoConfig 里所有支持的站点 / 直播抽成可持久化记录。
std::vector<ImportedVideoSource> FlattenConfigs(const ParsedConfigs& parsed);

} // namespace ttplayer::video
