#pragma once

// MusicFree 兼容层 —— 音乐窗（MusicFree 入口面板）
//
// 原版 TTPlayer 的「音乐窗」(查看 → 音乐窗 / F11，命令 0x7d68) 是内置的在线音乐
// 浏览器，其宿主组件（ttpcomm 的浏览器窗口）在重建版中并不存在。这里把同一个命令
// 改接到 MusicFree 兼容层的统一入口上：
//
//   * 顶部：音源选择 + 关键词搜索（走 SearchAllSources，汇总全部已挂载音源）
//   * 中部：结果列表（平台 / 歌曲名 / 歌手 / 专辑 / 时长）
//   * 底部：播放所选（ResolvePlayUrl 解析播放地址后回调播放器）、添加到列表、
//           导入音源…
//
// 窗口为非模态单例：重复调用只做「前置 + 刷新」。搜索与解析都在工作线程执行，结果
// 通过窗口消息回到 UI 线程，避免网络请求阻塞播放器消息循环。
//
// 外框：皮肤包的 <browser_window> 段（背景 + 关闭/上一个/下一个/重新搜索 + 置顶
// 勾选框 + 底部提示行）。包内没有这一段时退化成自绘浅色框体，搜索功能不受影响。

#include <functional>
#include <string>

#include <windows.h>

#include "ttplayer/musicfree/musicfree_app.h"

namespace ttplayer::skin { class LegacySkin; }

namespace ttplayer::musicfree {

class MusicFreeSourceManager;

// 用户对某条在线结果发起的动作。
enum class OnlineTrackAction {
    play,     // 播放所选：解析后立刻交给播放器
    enqueue,  // 添加到列表：解析后写入播放列表，不改动当前播放
};

// 音乐窗需要的宿主能力。全部可选：为空时音乐窗退化成可独立工作的普通窗口
// （自带整窗拖动，但没有跨窗口吸附）。由播放器主窗口实现，这样音乐窗能和歌词窗、
// 播放列表窗一样参与窗口吸附与位置记忆。
struct BrowserWindowHost {
    // 取当前皮肤；返回 nullptr 表示按无皮肤形态绘制。写成回调而不是裸指针，
    // 是为了运行期换肤后不需要重新创建音乐窗。
    std::function<const skin::LegacySkin*()> skin;
    // 以下三个的坐标都是音乐窗客户区坐标。
    std::function<void(HWND, POINT)> begin_drag;
    std::function<void(HWND, POINT)> continue_drag;
    std::function<void(HWND)> end_drag;
    // 主窗口记忆的窗口位置；返回空矩形表示没有保存过。
    std::function<RECT()> saved_bounds;
    std::function<void(const RECT&)> store_bounds;
    // 用户勾选/取消框体上的「置顶」时回调，供宿主持久化。
    std::function<void(bool)> store_top_most;
    // 音乐窗被关闭（框体关闭按钮 / ESC）时回调，宿主用来刷新自身的按钮状态。
    std::function<void()> on_closed;
    // 是否以「窗口置顶」状态打开。
    bool top_most{};
};

// 显示（或前置）MusicFree 音乐窗。
//   owner   : 父/宿主窗口，可为 nullptr。
//   mgr     : 音源管理器；为 nullptr 时内部改用进程级 AppMusicFreeManager()。
//   host    : 拖动吸附与位置记忆（可为空结构）。
//   on_track: 用户请求播放/添加某条在线结果时调用。参数为已解析出的播放地址
//             （宽字符）以及该条结果的**在线元数据**（标题/歌手/专辑/时长）。
//             调用方应当直接使用 hit 里的字段，而不是从 url 反推文件名。
//             url 为空表示解析失败，此时 hit 仍然有效，可用于提示文案。
//             返回 true 表示请求已被接受（已入列或已开始播放），音乐窗据此
//             显示成功/失败状态。回调为空时两个按钮只提示错误。
//   initial_query: 可选的初始关键词。非空时，音乐窗会把它填进搜索框并立即搜索；
//             窗口已存在时则覆盖当前关键词重新搜索（「在线搜索当前曲目」用）。
void ShowMusicFreeBrowser(HWND owner, MusicFreeSourceManager* mgr,
                          BrowserWindowHost host,
                          std::function<bool(OnlineTrackAction action,
                                             const OnlineSearchHit& hit,
                                             const std::wstring& url)> on_track,
                          const std::wstring& initial_query = {});

// 音乐窗当前是否已经创建（供主窗口同步菜单勾选状态）。
bool IsMusicFreeBrowserVisible();

// 音乐窗 HWND；未创建时为 nullptr。主窗口用它参与拖动吸附的窗口集合。
HWND MusicFreeBrowserWindow();

// 关闭音乐窗（等同框体上的关闭按钮）。未创建时是空操作。
void CloseMusicFreeBrowser();

// 换肤后按新皮肤重新贴合外框（尺寸 / 窗口区域 / 控件位置）。窗口未创建时是空操作。
void RefreshMusicFreeBrowserFrame();

} // namespace ttplayer::musicfree
