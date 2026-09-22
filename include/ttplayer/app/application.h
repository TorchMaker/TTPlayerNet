#pragma once

#include "ttplayer/app/single_instance.h"
#include "ttplayer/plugins/plugin_manager.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace ttplayer::app {
struct ParsedCommandLine {
    std::filesystem::path file;
    std::vector<std::wstring> switches;
    bool smoke_test{};
    // --mf-selftest=<关键词>：无界面跑一遍 MusicFree「搜索 → 解析 → 在线入列」，
    // 结果写进 TTPLAYER_MF_TRACE 日志后立即退出。用于核对在线元数据是否正确
    // 落到播放列表条目上，不需要 UI 自动化或截图。
    std::string mf_selftest;
    // --dpi-selftest：无界面跑一遍高 DPI 几何回归（镜像幂等/反累乘、线性、
    // 矩形合法性、字号、区域、配置迁移往返），结果打到 stdout 与 trace 日志，
    // 有失败则进程退出码为 1。不需要高分屏。
    bool dpi_selftest{};
    [[nodiscard]] bool HasSwitch(std::wstring_view name) const;
    // 004C038B/004C0DB2: /a -> 1, /e -> 2, ordinary open -> 0.
    [[nodiscard]] ULONG_PTR FileMode() const;
};

ParsedCommandLine ParseCommandLine();
int TTPlayer_RunApplicationSession(HINSTANCE instance, int show_command,
                                   SingleInstanceIpc& single_instance,
                                   HMODULE resource_module,
                                   HMODULE ttpcomm_module,
                                   const plugins::PluginManager& sound_library);
} // namespace ttplayer::app
