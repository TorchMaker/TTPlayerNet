#pragma once

#include <windows.h>
#include <shellapi.h>
#include <array>
#include "ttplayer/i18n/i18n.h"

namespace ttplayer::ui::detail {

// The 5.7.9 menu 0x8a uses the 0x9d placeholder for related links.
inline constexpr UINT kMenuRelatedLinks = 0x009d;

struct ProjectLink {
    UINT command;
    const wchar_t* label;
    const wchar_t* url;
    UINT image_command;
};

// Shared by the main context menu and the Options window.
// 两个固定入口：本项目源码仓库 + 上游 MusicFree 项目（本机的音源兼容层用的就是
// 它的插件协议）。原来的「提交反馈」入口已被 MusicFree 取代。
// 标签直接用 <owner>/<repo> 形式，一眼能看出指向哪个仓库。
inline constexpr std::array<ProjectLink, 2> kProjectLinks{{
    {0xb650, L"Sonic853/TTPlayer", L"https://github.com/Sonic853/TTPlayer", kMenuRelatedLinks},
    {0xb651, L"maotoumao/MusicFree", L"https://github.com/maotoumao/MusicFree", 0x802c},
}};

inline const wchar_t* ProjectLinkLabel(const ProjectLink& link) {
    if (link.command == kProjectLinks[0].command) return i18n::Literal(L"Sonic853/TTPlayer");
    if (link.command == kProjectLinks[1].command) return i18n::Literal(L"maotoumao/MusicFree");
    return link.label;
}

inline constexpr const ProjectLink* FindProjectLink(UINT command) noexcept {
    for (const auto& link : kProjectLinks)
        if (link.command == command) return &link;
    return nullptr;
}

inline bool OpenProjectLink(HWND owner, UINT command,
                           decltype(&ShellExecuteW) open = &ShellExecuteW) {
    const auto* link = FindProjectLink(command);
    if (!link) return false;
    open(owner, L"open", link->url, nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

} // namespace ttplayer::ui::detail
