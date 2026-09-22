#pragma once

// 产品标识：所有用户可见的「名字 + 版本」都从这里取，不要散落地写字符串字面量。
//
// 名字：与安装包 [Setup] AppName / 卸载项显示名保持一致（千千静听）。
// 版本：2026，**不是**资源 DLL 里 0x8299 的原值 "5.5"。原版从资源 DLL 取版本号，
//       重建版改成固定年份后，主窗口标题、皮肤 info 条、「关于」页面的版本行都必须
//       走这里，否则三处会不一致。
//       磁盘文件属性（右键 → 详细信息）由 src/app/resources.rc 的 VERSIONINFO 提供，
//       改动时两边要一起改。
namespace ttplayer::app {

inline constexpr wchar_t kProductName[] = L"千千静听";
inline constexpr wchar_t kProductVersion[] = L"2026";
// 用户可见的产品全名：名字 + 年份紧排（千千静听2026）。窗口标题与皮肤 info 条用它，
// 避免各处自己拼字符串导致空格/写法不一。
inline constexpr wchar_t kProductDisplayName[] = L"千千静听2026";

} // namespace ttplayer::app
