#pragma once

// 原生 spider 通用工具（五个模板引擎共享）
//
// 只放与站点无关的纯函数：字符串、URL、编解码、哈希、JSONPath-lite。
// 所有实现零第三方依赖（哈希走 wincrypt，已是项目既有做法）。

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ttplayer::video::spider_util {

// ---------- 字符串 ----------
std::string Trim(std::string_view s);
std::string ToLower(std::string_view s);
bool StartsWith(std::string_view s, std::string_view prefix);
bool EndsWith(std::string_view s, std::string_view suffix);
std::string ReplaceAll(std::string s, std::string_view from, std::string_view to);
// 按分隔符切分（跳过空段）。
std::vector<std::string> Split(std::string_view s, char sep);
// 只保留数字（站点 id 常见 "123.html" 这类形态）。
std::string DigitsOnly(std::string_view s);

// ---------- URL ----------
std::string UrlEncode(std::string_view s);
std::string UrlDecode(std::string_view s);
// base 与 path 拼接：path 已是绝对地址则原样返回；处理 / 与 ../。
std::string JoinUrl(std::string_view base, std::string_view path);
// 用分隔符连接字符串列表。
std::string JoinStrings(const std::vector<std::string>& parts, std::string_view sep);
// 从 URL 取 query 参数值（不存在返回空）。
std::string QueryParam(std::string_view url, std::string_view key);

// ---------- HTML ----------
std::string HtmlDecode(std::string_view s);
std::string StripTags(std::string_view html);

// ---------- 编解码 / 哈希 ----------
std::string HexEncode(std::string_view bytes);
bool HexDecode(std::string_view hex, std::string* out);
std::string Base64Encode(std::string_view bytes);
bool Base64Decode(std::string_view text, std::string* out);
std::string Md5Hex(std::string_view data);
std::string Sha1Hex(std::string_view data);
std::string Sha256Hex(std::string_view data);

// AES-128/192/256-CBC 解密（PKCS#7 去填充）；key/iv 长度需匹配算法。
// 用于 app 模板里「响应体 AES 加密」的站点（App99/YueTing 等）：一旦从 smali
// 逆向出 key/iv，在 recipe 的 response.decrypt 里填好即可被通用引擎解密。
// 任何一步失败返回空串。
std::string AesCbcDecrypt(std::string_view key, std::string_view iv,
                          std::string_view ciphertext);

// ---------- 时间 / 随机 ----------
long long UnixSeconds();
long long UnixMilliseconds();
std::string RandomHex(int bytes);
std::string UuidV4();

// ---------- JSON ----------
// 宽松解析：去 BOM、截取首个 { 或 [ 到配平的结尾；失败返回 null。
nlohmann::json ParseJsonLoose(std::string_view text);

// JSONPath-lite：支持 "$.a.b[0].c" 与 "$.list[*]"（也接受不带 $ 前缀）。
const nlohmann::json* JsonFind(const nlohmann::json& root, std::string_view path);
std::vector<const nlohmann::json*> JsonFindAll(const nlohmann::json& root,
                                               std::string_view path);

// 从对象取字段（字符串/数字/布尔都转成字符串）；缺少或 null 返回 def。
std::string JsonString(const nlohmann::json& obj, std::string_view key,
                       std::string_view def = {});
// 按顺序尝试多个字段名，返回第一个非空。
std::string JsonPick(const nlohmann::json& obj,
                     std::initializer_list<std::string_view> keys,
                     std::string_view def = {});

} // namespace ttplayer::video::spider_util
