// MusicFree 兼容层 —— 音乐窗实现
//
// 把「查看 → 音乐窗」(0x7d68) 变成 MusicFree 的入口：搜索 / 播放 / 导入音源。
// 搜索与播放地址解析都在工作线程执行，完成后用 PostMessage 把结果送回窗口。
//
// 外框沿用皮肤包的 <browser_window> 段（原版内嵌浏览器用的就是这一段）：背景位图 +
// 关闭 / 上一个音源 / 下一个音源 / 重新搜索 / 窗口置顶 + 底部提示行。皮肤没有这一段
// 时退化成自绘的浅色框体，搜索功能不受影响。
//
// 拖动吸附与位置记忆由宿主（播放器主窗口）提供，音乐窗因此和歌词窗、播放列表窗一样
// 参与窗口吸附，并随主窗口一起移动。

#include "ttplayer/musicfree/musicfree_browser_window.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/ui/dpi_scale.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_artwork.h"
#include "ttplayer/musicfree/musicfree_frame.h"
#include "ttplayer/musicfree/musicfree_import_dialog.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/musicfree/musicfree_theme.h"
#include "ttplayer/skin/skin.h"
#include "ttplayer/skin/skin_frame.h"
#include "ttplayer/video/video_source_manager.h"
#include "ttplayer/video/video_source_dialog.h"
#include "ttplayer/video/video_cms.h"
#include "ttplayer/video/video_player_window.h"

// WebView2 页面宿主 / C++↔页面桥 / 页面资产 / 三域命令注册入口（音乐窗 WebView2 化）。
// 这些头与本文件同在 src/musicfree/：带 "musicfree/" 前缀的写法要求 CMake 把
// src 加进头文件搜索路径（ttplayer_core 当前没加），用 __has_include 两种写法
// 都接受，避免统一编译时找不到头文件。
#if __has_include("musicfree/musicfree_webview_host.h")
#include "musicfree/musicfree_webview_host.h"
#include "musicfree/musicfree_bridge.h"
#include "musicfree/webui_assets.h"
#include "musicfree/bridge_cmds_music.h"
#include "musicfree/bridge_cmds_video.h"
#include "musicfree/bridge_cmds_misc.h"
#else
#include "musicfree_webview_host.h"
#include "musicfree_bridge.h"
#include "webui_assets.h"
#include "bridge_cmds_music.h"
#include "bridge_cmds_video.h"
#include "bridge_cmds_misc.h"
#endif
// 与上面两种 include 写法无关：两个分支都要用运行时选择 / 自动安装。
#include "ttplayer/musicfree/webview2_runtime.h"

#include <set>

// ---- 影视仓式外壳：顶部分类导航行 + 图标功能栏（仅视频页显示）-----------
struct VideoNavTab {
    RECT rect;
    int cat_index;   // -1 = 推荐；≥0 = video_agg_categories 下标
};
struct VideoIconBtn {
    RECT rect;
    int icon_id;     // 见 VideoIconId
};
enum class VideoIconId {
    kHistory = 0, kLive, kSearch, kSetting, kLine,
    kDrive, kCollect, kPush, kConfig
};
// 图标功能栏顺序（与 VideoIconId 一一对应）：历史/直播/搜索/配置/线路/网盘/收藏/推送/设置
static const wchar_t* kVideoIconLabels[] = {
    L"历史", L"直播", L"搜索", L"配置", L"线路", L"网盘", L"收藏", L"推送", L"设置"
};
// 影视仓式首页分区：一个分类一段海报墙（"热播电影" / "热播剧集" / "动漫" …）。
struct VideoHomeSection {
    std::wstring title;  // 分区标题（已加「热播」等前缀）
    int start = 0;       // 在 state->video_home_items 中的起始下标
    int count = 0;       // 该分区卡片数
};

namespace ttplayer::musicfree {

namespace {

// 调色板与基本图元（填充/描边/圆角框/文字/字体）都在共用主题里，音乐窗与导入窗
// 共用同一套，免得两处各画一份风格漂移。
using namespace theme;

struct BrowserState;  // 定义见文件下方
// 前向声明：视频（影视仓 / TVBox）相关函数（定义在 OnTabChanged 附近）。
bool IsVideoMode(BrowserState* state);
bool IsSearchMode(BrowserState* state);
void SetVideoStatus(BrowserState* state, const std::wstring& text);
void SetVideoSheetCaption(BrowserState* state, const std::wstring& text);
void FillVideoList(BrowserState* state);
void FillDiscoverList(BrowserState* state);
void DrawDiscoverRowVideo(BrowserState* state, const DRAWITEMSTRUCT& item);
bool HitVideoCard(BrowserState* state, int* group, int* item);
// 音乐页海报墙卡片命中（视频页内部转调 HitVideoCard）。
bool HitDiscoverCard(BrowserState* state, int* group, int* sheet);
// 分类页签命中与加载（影视仓式页签，定义在 StartLoadVideo 之后）。
bool HitVideoChip(BrowserState* state, int* group, int* cat_index);
void StartLoadVideoCategory(BrowserState* state, int group_index,
                            const ttplayer::video::VideoCategory& cat);
// 顶部「刷新」按当前页签分派到对应的加载入口（定义在文件下方）。
void StartLoadDiscover(BrowserState* state, bool force);
void StartLoadVideo(BrowserState* state, bool force);
void MaybeRefreshVideoSources(BrowserState* state);
// 直播源卡片（定义在 StartLoadVideo 下方）：本地数据铺卡 + 点开拉频道表。
void AddLiveSourceCards(BrowserState* state);
void StartOpenVideoLive(BrowserState* state, const ttplayer::video::VideoItem& item);
// 直接播放结果列表里选中的一行（定义在 SetVideoSheetCaption 下方）。
void PlaySelectedVideoHit(BrowserState* state);
// 影视仓式外壳用的两个入口（定义见文件后方，先前向声明以便顶部自绘函数调用）。
void StartLoadAggVideoCategory(BrowserState* state, int cat_index);
void OpenVideoSourceDialog(BrowserState* state);
// 影视仓式外壳：视频页顶部分类导航行 + 图标功能栏（自绘 + 点击）
std::vector<VideoNavTab> LayoutVideoNavTabs(HDC dc, BrowserState* state, const RECT& rc);
std::vector<VideoIconBtn> LayoutVideoIconBtns(HDC dc, BrowserState* state, const RECT& rc);
void PaintVideoNavbar(BrowserState* state, HWND control);
void PaintVideoIconbar(BrowserState* state, HWND control);
void HandleVideoNavClick(BrowserState* state, HWND control);
void HandleVideoIconClick(BrowserState* state, HWND control);

constexpr int kIdPlatform = 3001;
constexpr int kIdQuery = 3002;
constexpr int kIdSearch = 3003;
constexpr int kIdImport = 3004;
constexpr int kIdList = 3005;
constexpr int kIdPlay = 3006;
constexpr int kIdClose = 3007;
constexpr int kIdStatus = 3008;
constexpr int kIdEnqueue = 3009;
constexpr int kIdDownload = 3010;
// 新增：页签 / 收藏列表 / 标签 / 加载 / 打开
constexpr int kIdTab = 3020;
constexpr int kIdSheetList = 3021;
constexpr int kIdTagCombo = 3022;   // 保留控件但隐藏：推荐标签已并入发现页自动聚合
constexpr int kIdLoadSheets = 3023; // 「刷新」
constexpr int kIdOpenSheet = 3024;  // 「打开歌单」
constexpr int kIdTracksCaption = 3025; // 曲目区状态条（选中/加载/成功/失败都可见）
// 影视仓式外壳：视频页顶部的分类导航行 + 图标功能栏（音乐/搜索页隐藏）
constexpr int kIdVideoNav = 3040;     // 顶部分类导航行（推荐 + video_agg_categories）
constexpr int kIdVideoIcons = 3041;   // 图标功能栏（历史/直播/搜索/配置/线路/网盘/收藏/推送/设置）

// 导入路径诊断：设置 TTPLAYER_IMPORT_TRACE=1 时，把「点了导入之后走了哪个分支」
// 追加写入 %TEMP%\ttplayer_import.log。只在真机上才复现的「点了没反应」问题，
// 靠它区分「命令没到」「被 busy 挡掉」「对话框创建失败」三种情况。
void TraceImport(const char* fmt, ...) {
    static const bool enabled =
        GetEnvironmentVariableW(L"TTPLAYER_IMPORT_TRACE", nullptr, 0) > 0;
    if (!enabled) return;
    wchar_t dir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, dir)) return;
    const std::wstring path = std::wstring(dir) + L"ttplayer_import.log";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") != 0 || !file) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

// 发现页卡片网格：自绘 ListBox 的一行里放若干张竖向封面卡片。
constexpr int kCardWidth = 168;
// 卡片行高与封面边长一起决定「封面下方留给标题 + 来源」的高度：
// row - 6(外边距) - 6(封面顶距) - thumb = 文字区。164/100 → 52px，
// 足够容纳 ui_font(-12) 的标题与 small_font(-11) 的来源，且 DT_VCENTER
// 不会再裁掉中文字形的上下笔画。改动此处时务必同步检查 DrawDiscoverRow 的文字矩形。
constexpr int kCardRowHeight = 164;
constexpr int kCardThumb = 100;
constexpr int kGroupRowHeight = 30;
constexpr int kSheetRowHeight = 60;  // WM_MEASUREITEM 兜底
constexpr int kSheetThumb = 48;

// 视频页海报卡（影视仓风格）：竖版 2:3 海报 + 深色底。与音乐页共用 sheet_list
// 自绘管线，但卡片度量独立一套 —— 海报比方形封面高得多，行高 / 列宽分开算。
constexpr int kVideoCardWidth = 152;
constexpr int kVideoRowHeight = 244;  // 3(边距)+4+196(海报)+4+18(标题)+2+14(备注)+3
constexpr int kVideoPosterH = 196;    // 海报高；宽 = kVideoCardWidth - 20 = 132，≈2:3

// ---- 视频页配色（影视仓风格深色）--------------------------------------
// 放在文件头部：sheet_list 的自绘底色（WM_PAINT，行 ~1900）也要用。
constexpr COLORREF kVideoPageBg   = RGB(15, 17, 23);   // 页面底
constexpr COLORREF kVideoCardBg   = RGB(30, 35, 46);   // 海报占位底
constexpr COLORREF kVideoCardLine = RGB(52, 60, 76);   // 卡片描边
constexpr COLORREF kVideoSelLine  = RGB(56, 162, 194); // 选中描边（主色）
constexpr COLORREF kVideoTitleTx  = RGB(232, 238, 244);
constexpr COLORREF kVideoSubTx    = RGB(140, 152, 168);
constexpr COLORREF kVideoChipBg   = RGB(36, 42, 56);   // 分类页签底
constexpr COLORREF kVideoChipLine = RGB(64, 74, 94);

// 影视仓式外壳条高度（视频页顶部：分类导航行 + 图标功能栏），音乐/搜索页不占。
constexpr int kVideoNavH = 30;   // 顶部分类导航行高
constexpr int kVideoIconH = 48;  // 图标功能栏高（图标方块 + 标签）

constexpr UINT kMsgSheetsReady = WM_APP + 0x514;       // 发现页：单个音源的一批内容到达
constexpr UINT kMsgSheetTracksReady = WM_APP + 0x515;  // 歌单详情曲目加载完成
constexpr UINT kMsgArtworkReady = WM_APP + 0x516;      // 封面解码完成（触发重绘）
constexpr UINT kMsgDiscoverStart = WM_APP + 0x517;     // 窗口显示后再启动异步加载
constexpr UINT kMsgSourcesReady = WM_APP + 0x518;      // 后台插件挂载完成（只刷搜索下拉框）

// 视频（影视仓 / TVBox）相关消息：首页聚合 / 详情选集 / 搜索结果。
constexpr UINT kMsgVideoHomeReady = WM_APP + 0x520;    // 单个视频站点的首页卡片到达
constexpr UINT kMsgVideoDetailReady = WM_APP + 0x521;  // 视频详情选集加载完成
constexpr UINT kMsgVideoSearchReady = WM_APP + 0x522;  // 视频搜索结果到达
constexpr UINT kMsgVideoStart = WM_APP + 0x523;        // 窗口显示后再启动视频异步加载
constexpr UINT kMsgVideoSourcesRefreshed = WM_APP + 0x524;  // 后台自动更新已写入新配置
constexpr UINT kMsgWebView2Installed = WM_APP + 0x526;     // WebView2 运行库后台装好，重试创建页面

// 封面补抓：下载失败（停在彩色骨架）的封面按 3 秒节流统一重发，每个 url 限 2 次。
constexpr int kVideoArtMaxRetries = 2;
// 海报到位后的重绘合并：多张图在 250ms 窗口内只触发一次全网格重绘。
// （16 并发下载时每秒可达 10+ 张完成，逐张 InvalidateRect 会让海报墙
// 以每秒十几次的频率全量重绘——肉眼看到的就是「疯狂闪烁」。）
constexpr UINT_PTR kArtworkRepaintTimerId = 1;
constexpr UINT kMsgVideoPendingCat = WM_APP + 0x525;       // 加载中排队的分类切换

// 发现页内容缓存：窗口每次重建都会重新跑一遍「等挂载 + 逐音源联网」，实测
// 首批真实卡片要 14s。把聚合结果（含 raw）落到磁盘，缓存有效期内开窗直接渲染，
// 完全不碰网络；过期则「先显示缓存 + 后台静默更新」，用户全程有内容可看。
constexpr const wchar_t* kDiscoverCacheName = L"discover_cache.json";
constexpr std::int64_t kDiscoverCacheTtlSeconds = 12 * 3600;  // 12 小时
constexpr std::uintmax_t kDiscoverCacheMaxBytes = 8 * 1024 * 1024;

// 三个页签：0=音乐（原「发现」，所有音源的榜单/推荐同页展示），1=视频（影视仓/
// TVBox 源），2=搜索（原有搜索功能）。
enum class BrowserMode { kMusic, kVideo, kSearch };

// 结果列表右键菜单的命令号（TrackPopupMenu 返回值，不是窗口命令）。
constexpr int kMenuPlay = 4001;
constexpr int kMenuEnqueue = 4002;
constexpr int kMenuDownload = 4003;
constexpr int kMenuCopyTitle = 4004;

constexpr UINT kMsgSearchDone = WM_APP + 0x510;
constexpr UINT kMsgResolveDone = WM_APP + 0x511;
// 下载是两段式的：先解析地址（kMsgDownloadReady），用户确认落盘位置后再拉字节
// （kMsgDownloadDone）。扩展名要从解析出来的地址推断，所以不能颠倒。
constexpr UINT kMsgDownloadReady = WM_APP + 0x512;
constexpr UINT kMsgDownloadDone = WM_APP + 0x513;

// 皮肤包没有 <browser_window> 时使用的兜底窗口尺寸。
constexpr int kFallbackWidth = 720;
constexpr int kFallbackHeight = 462;

// 外框内容区四周的内缩量。皮肤形态下只用来决定标题栏按钮簇贴左时留多少边距，
// 兜底形态下同时是内容区的内缩量，两边保持一致视觉才对得上。
constexpr int kFramePad = 10;

// 窗口缩放：可抓取的边框厚度，以及最小尺寸的上下限。
//
// 最小尺寸默认取皮肤背景位图的自然尺寸 —— 窗口小于位图时 skin::DrawFrameBackground
// 会退化成「整幅 1:1 缩放」（见该函数的守卫），边框与标题栏会被拉变形。把最小尺寸
// 钉在自然尺寸上，边框就永远保持原始像素厚度，窗口只往大变（拖内是无效的，和任何
// 已到最小尺寸的普通窗口一样）。皮肤自然尺寸特别大时用上限兜一下，免得小屏上窗口
// 一开就顶到边。
//
// ★ 抓取带厚度统一取 frame::kResizeBand（自检也用同一个值，不会各自漂移）。
// 必须小于皮肤左右/上下边框厚度，否则会把内容区（尤其是列表右侧的滚动条）的
// 点击抢走。实测内置皮肤的边框：左 8px、右 9px、上 31px、下 31px。
// kResizeBand 是 96-DPI 逻辑值，实际判定时按窗口 DPI 换算（§6.9 High-DPI 规格）。
constexpr int kResizeBorder = frame::kResizeBand;

// The grab band is a logical thickness: on a 200% display a 6px band is only
// 3 logical pixels and becomes impossible to hit.
int ScaledResizeBorder(HWND hwnd) {
    return std::max(kResizeBorder,
                    ui::ResolveScaleForWindow(hwnd).X(kResizeBorder));
}
constexpr int kMinTrackWidthCap = 640;   // 宽度不再随缩放变化，仅作超大皮肤的兜底上限
constexpr int kMinTrackHeightCap = 260;  // 高度允许缩小到的下限
constexpr int kMaxTrackHeightCap = 1600; // 高度允许放大的上限（拖动只改高度）

constexpr int kRowHeight = 22;
constexpr int kButtonHeight = 22;
constexpr int kColumnCount = 5;

// 框体上可命中的皮肤按钮。
enum FrameHit {
    kFrameHitNone = 0,
    kFrameHitClose,
    kFrameHitPrevSource,
    kFrameHitNextSource,
    kFrameHitRefresh,
    kFrameHitTopMost,
};

// 框体自绘部分的位置（当前客户区坐标）。皮肤有效时由 frame::Map 把
// <browser_window> 的位图坐标换算过来，再把标题栏按钮簇贴左修正（按钮左对齐、
// 底部行下锚定）；否则按兜底尺寸推算。
struct FrameLayout {
    bool skinned{};
    RECT content{};
    RECT close{};
    RECT prev_source{};
    RECT next_source{};
    RECT refresh{};
    RECT check_box{};
    RECT check_label{};
    RECT link_text{};
    // 皮肤形态的换算结果（九宫格拉伸区、背景绘制、窗口区域都要用同一份）。
    frame::Metrics skin{};
    // 兜底外框的分区（标题栏 / 标题文字 / 页脚 …）。导航簇与标题文字的互不重叠
    // 由 theme::BuildFrameChrome 保证；皮肤形态下不使用。
    theme::FrameChrome chrome{};
    // 搜索输入框的外描边。EDIT 自带的是系统凹陷边（WM_CTLCOLOREDIT 只换得了
    // 底色，换不掉那圈 3D 边），所以把边框改成自己画在子控件下面的一像素圆角线。
    RECT query_field{};
};


// 子控件回调上下文。四个按钮和一个结果列表都需要 hover 高亮，而 Win32 不会把子控件
// 上的鼠标移动转给父窗口，因此统一挂一层子类化回调。
struct ChildProbe {
    void* state{};      // BrowserState*，声明成 void* 以便在这里前置定义
    WNDPROC base{};
    int id{};
    bool is_list{};
    bool tracking_leave{};
};

struct BrowserState {
    HWND hwnd{};
    HWND owner{};
    HWND platform{};
    HWND query{};
    HWND search_button{};
    HWND import_button{};
    HWND status{};
    HWND tracks_caption{};   // 曲目区上方：当前歌单 + 加载/结果/失败状态
    HWND list{};
    HWND play_button{};
    HWND enqueue_button{};
    HWND download_button{};
    MusicFreeSourceManager* mgr{};
    BrowserWindowHost host;
    // 发现页（默认）：同一页面按音源分组展示所有可用榜单 / 推荐卡片；
    // 搜索页保留原有搜索功能。两者只是同一批子控件的不同显隐。
    BrowserMode mode{BrowserMode::kMusic};
    HWND tab{};
    HWND sheet_list{};      // 发现页卡片网格（owner-draw variable，行内多张卡片）
    HWND tag_combo{};       // 保留但隐藏
    HWND load_sheets_button{};
    HWND open_sheet_button{};

    struct DiscoverGroup {
        std::string platform;
        std::vector<MusicSheet> sheets;
    };
    struct DiscoverRow {
        bool header = false;   // true = 音源分组标题行
        int group = -1;
        int first = 0;         // 该行第一张卡片在 group.sheets 中的下标
        int count = 0;         // 该行卡片数
        int section_idx = -1;  // 影视仓式分区标题行：video_home_sections 下标
    };
    std::vector<DiscoverGroup> groups;       // 无数据 / 异常音源不入列
    std::vector<DiscoverRow> discover_rows;  // groups 展开后的绘制行
    int discover_cols = 4;                   // 每行卡片数（随窗口宽度重算）
    int discover_cols_applied = 0;           // 上次铺行时的列数（变了才重建行）
    int selected_group = -1;
    int selected_sheet = -1;
    // 发现页曲目区是否已展开：首次进入只显示封面卡片，点开歌单后才让出下方
    // 空间给曲目列表；切页 / 后台刷新 / 窗口缩放都不改变这个状态。
    bool tracks_visible = false;
    int skipped_sources = 0;
    int finished_sources = 0;
    int updated_sources = 0;             // 本轮成功刷新了几个音源
    bool discover_loaded = false;
    bool discover_placeholder = false;   // 挂载未完成时先显示骨架封面卡片
    std::atomic<bool> discover_busy{false};
    // 发现页缓存：<exe>/musicfree/discover_cache.json；saved_at 是 Unix 秒。
    std::filesystem::path discover_cache_path;
    std::int64_t discover_cache_saved_at = 0;
    bool discover_cache_fresh = false;   // 未过期 → 本次开窗完全不联网
    bool sources_ready = false;          // 后台插件挂载完成（kMsgSourcesReady 置位）

    std::set<std::string> art_requested;   // 已发起封面请求的 url，避免重复
    std::unique_ptr<ArtworkCache> artwork; // 封面异步下载/缓存/解码

    // ---- 视频（影视仓 / TVBox）状态：与发现页完全平行的一套 ----
    // 视频站点 = 分组、视频条目 = 卡片、选集 = 曲目，复用同一套卡片网格与列表。
    std::unique_ptr<ttplayer::video::VideoSourceManager> video_mgr;
    struct VideoGroup {
        std::string site_key;
        std::string site_name;
        std::vector<ttplayer::video::VideoItem> items;
        // CMS 分类（影视仓式页签）：首页响应的 class 数组；点页签 → FetchCategory。
        std::vector<ttplayer::video::VideoCategory> categories;
        std::string active_category;       // 空 = 首页推荐（「全部」）
        std::wstring active_category_name;
        bool category_loading = false;
    };
    std::vector<VideoGroup> video_groups;       // [0] = 聚合推荐组，其余 = 直播组
    // 影视仓式首页分区视图：「推荐」页签下，把聚合流按分类名切成多个分区
    // （热播电影 / 热播剧集 / 动漫 …），每个分区一段海报墙。与 video_groups[0]
    // 同源（都是聚合流），只是换了一种呈现方式。
    std::vector<ttplayer::video::VideoItem> video_home_items;  // 分区顺序铺平后的卡片
    std::vector<VideoHomeSection> video_home_sections;         // 分区标题 + 起止
    bool video_in_sections = false;                           // 当前是否处于分区视图
    // 影视仓式聚合推荐：所有 CMS 站点的影片**合并去重、按源轮询交错**成一条
    // 统一推荐流（video_groups[0].items），不再「每个源一段」。每站原始批次
    // 留在 agg_batches，批次渐进到达时增量重排；分类切换结果放 cat_batches
    // （非空时优先于 home 批次参与混排，「推荐」页签清空它即可秒回首页）。
    struct VideoAggBatch {
        std::string site_key;
        std::string site_name;
        std::vector<ttplayer::video::VideoItem> items;
    };
    // 跨站点同名分类合并（影视仓分类页签的聚合版）：一部「电影」在 4 个源里
    // 是 4 个不同的 class id，点页签时要对每个源各取各的 id。
    struct VideoAggCategory {
        std::string name;
        std::string norm_name;   // 归一化名（去首尾空白 + 小写），仅用于跨源去重
        std::vector<std::pair<std::string, std::string>> per_site;  // (site_key, cat_id)
    };
    std::vector<VideoAggBatch> video_agg_batches;       // 首页各站点原始批次
    std::vector<VideoAggBatch> video_cat_batches;       // 分类切换后各站点批次
    std::vector<VideoAggCategory> video_agg_categories; // 聚合分类页签（跨站合并）
    int video_active_cat = -1;                          // 当前选中页签（-1 = 推荐）
    int video_cat_expected = 0;                         // 分类聚合还没回来的站点数
    std::vector<DiscoverRow> video_rows;        // 复用 DiscoverRow（header + first/count）
    int video_cols = 4;
    int video_cols_applied = 0;                 // 上次铺行时的列数（变了才重建行）
    std::map<std::string, int> video_art_retries;   // 封面补抓次数（url → 已重试次数）
    std::int64_t video_art_retry_at = 0;            // 上次补抓时间（3 秒节流）
    bool artwork_dirty{false};                      // 有新海报待上屏（250ms 合并重绘）
    int selected_video_group = -1;
    int selected_video_item = -1;
    std::vector<OnlineSearchHit> video_hits;    // 选集（剧集）作为曲目
    std::wstring video_status;
    std::wstring video_caption;
    bool video_tracks_visible = false;
    std::atomic<bool> video_busy{false};        // 首页 / 搜索等聚合任务
    // 加载期间用户点过的分类：-2 表示无排队，-1 推荐，>=0 分类索引。
    // 以前 busy 时直接静默 return，用户会觉得「所有分类都点不动」。
    std::atomic<int> video_pending_cat{-2};
    std::atomic<bool> video_detail_busy{false}; // 详情选集加载
    bool video_loaded = false;
    bool video_placeholder = false;
    // 占位卡片上的提示文字：加载中是「正在准备…」，一个源都没有时改成引导用户
    // 点「导入…」—— 否则空窗口挂着四张「正在准备…」会让人一直等下去。
    std::wstring video_placeholder_text = L"正在准备…";
    bool video_sources_ready = false;
    // 最近一次导入的结果。首页重新聚合完成后状态行会被「已加载 N 个视频源」
    // 覆盖，导入失败的信息就看不见了；这里存一份，加载完成时优先显示它。
    std::wstring video_import_note;
    // 影视仓式外壳：视频页顶部的分类导航行 + 图标功能栏（音乐/搜索页隐藏）。
    HWND video_navbar = nullptr;
    HWND video_iconbar = nullptr;
    int hot_video_icon = -1;   // 图标功能栏的悬停高亮下标（-1 = 无）
    // 在「视频源」对话框里导入 / 删除源时，首页可能正在后台聚合（站点表是启动
    // 那一刻的快照）。这里挂个标志，等当前那轮结束再按新表重载，避免两个聚合
    // 线程同时往 video_groups 里写。
    bool video_reload_pending = false;
    std::set<std::string> video_art_requested;

    // 返回 true 表示播放器已接受该请求（已入列/已开始播放）。
    std::function<bool(OnlineTrackAction, const OnlineSearchHit&,
                       const std::wstring&)> on_track;
    std::vector<OnlineSearchHit> hits;   // 当前页显示的曲目
    // 发现页与搜索页各自持有曲目与状态文本：切换页签只做存档 / 取回，
    // 绝不把一张榜单的曲目带到搜索页（反之亦然）。
    std::vector<OnlineSearchHit> discover_hits;
    std::vector<OnlineSearchHit> search_hits;
    std::wstring discover_status;
    std::wstring search_status;
    std::wstring discover_caption;
    std::wstring open_sheet_title;  // 正在打开的歌单名，供结果状态条显示
    // 搜索/解析同一时刻只允许一个后台任务，避免插件沙箱被并发重入。
    std::atomic<bool> busy{false};
    std::wstring query_pending;  // 供后台线程读取的搜索条件（UI 线程写、任务启动前定格）
    // 首次创建时要预填并立即搜索的关键词。来自「在线搜索当前曲目」：以前那个命令
    // 会 ShellExecute 打开 qianqian.com，现在改成在音乐窗里按歌名搜。
    std::wstring initial_query;
    FrameLayout layout;
    bool top_most{};
    bool dragging{};
    // 重入保护：ApplyFrameMetrics 里的 SetWindowPos 会同步投递 WM_SIZE，
    // 处理 WM_SIZE 又会回头调用 ApplyFrameMetrics。
    bool applying_frame{};
    // 用户是否手动调过窗口大小。调过之后换肤就不再回到皮肤位图尺寸。
    bool user_resized{};
    // 鼠标是否停在可缩放的边框上（WM_NCHITTEST 会返回 HTLEFT/HTBOTTOMRIGHT…）。
    bool resizing{};
    int frame_hover{};
    int frame_pressed{};
    int hot_button{};
    int hot_row{-1};
    POINT drag_anchor{};
    RECT drag_start_bounds{};
    std::wstring link_text;
    HFONT ui_font{};
    HFONT bold_font{};
    HFONT small_font{};
    HFONT link_font{};
    LOGFONTW link_logfont{};
    HBRUSH panel_brush{};
    HBRUSH video_panel_brush{};  // 视频页深色底（海报墙区域露白时用它）
    std::vector<std::unique_ptr<ChildProbe>> probes;

    // ---- WebView2 页面宿主（音乐窗 WebView2 化 Task 3.2）----
    // 只有创建成功才非空；创建失败置 webview_failed 熔断（不再反复尝试），
    // 旧 Win32 控件只隐藏不删除，随时可回退（TTPLAYER_NO_WEBVIEW 强制回退）。
    std::unique_ptr<ttplayer::musicfree::WebviewHost> webview;     // 页面宿主
    std::unique_ptr<ttplayer::musicfree::MusicFreeBridge> bridge;  // C++↔页面桥
    bool webview_failed = false;   // 一旦失败就不再反复尝试创建
    std::string page_tab;          // 页面 tab.switch 记录的当前页签（后续任务接渲染）
};

// 搜索线程 → UI 线程的结果负载（含每音源的诊断信息）。
struct SearchPayload {
    std::vector<OnlineSearchHit> hits;
    OnlineSearchDiagnostics diagnostics;
    bool for_search = false;  // 结果归属哪个页签（后台任务发起时定格）
};

// 解析线程 → UI 线程的负载：播放地址 + 失败原因 + 触发本次解析的在线结果。
// 带上元数据（而非只带 URL）是刻意的：入列时要用音源返回的歌曲名/歌手/专辑，
// 而不是从播放地址反推出来的文件名。
struct ResolvePayload {
    OnlineTrackAction action{OnlineTrackAction::play};
    OnlineSearchHit hit;
    std::wstring url;
    std::wstring error;
};

// 下载线程 → UI 线程的负载。「解析播放地址」也在这个线程里做：下载必须等地址
// 解析出来，分两步走只会让 busy 标志在两次切换之间出现空窗。
struct DownloadPayload {
    OnlineSearchHit hit;
    std::wstring path;    // 目标文件（用户在选择框里确认过的路径）
    std::wstring error;
    std::int64_t bytes{};
    bool ok{};
};

// 视频：首页聚合的每站一批内容 / 完成信号。
struct VideoHomePayload {
    std::string site_key;
    std::string site_name;
    std::vector<ttplayer::video::VideoItem> items;
    // 首页响应携带的 CMS 分类表（影视仓式页签）。
    std::vector<ttplayer::video::VideoCategory> categories;
    // true：分类页签切换的结果，按 site_key 找到已有组替换 items（不再新增组）。
    bool category_load = false;
    std::string error;
    bool finished = false;
    int total = 0;
};
// 视频：详情选集加载结果。
struct VideoDetailPayload {
    std::string title;
    std::string site_name;
    std::string play_url;   // 站点 play_url 前缀，用于补全选集地址
    std::vector<ttplayer::video::VideoLine> lines;
    std::string error;
    bool live = false;      // 直播源：lines[0] 里装的是频道而不是选集
};
// 视频：搜索结果（直接进卡片网格，点击再开详情）。
struct VideoSearchPayload {
    std::vector<ttplayer::video::VideoItem> items;
    int sites_total = 0;
    int sites_ok = 0;
    std::string error;
};

std::shared_ptr<BrowserState> g_browser;

// （g_plugin_io 已移出本匿名命名空间 —— 见文件下方 } // namespace 之后的
//  「跨编译单元导出符号」区，声明在 musicfree_browser_window.h。）

// ------------------------------------------------ 发现页内容缓存（进程内 + 磁盘）
//
// 每次打开音乐窗都要重跑「等后台挂载 13.6s + 6 个音源逐个联网取榜单」，
// 窗口虽然不卡，但真实卡片要十几秒才出现。这里把聚合结果（含插件 raw）持久化：
//   · 进程内缓存：同一进程里反复开关音乐窗，连文件都不用读；
//   · 磁盘缓存  ：<exe>/musicfree/discover_cache.json，跨进程复用；
//   · TTL 12h   ：未过期直接渲染、完全不联网；过期则「先显示缓存 + 后台静默更新」；
//   · 手动刷新  ：「刷新」按钮强制走网络并重写缓存。
// 封面位图另有 <exe>/musicfree/artcache 磁盘缓存，两者叠加后二次开窗是纯本地渲染。

struct DiscoverCacheData {
    std::int64_t saved_at = 0;
    std::vector<BrowserState::DiscoverGroup> groups;
};

std::mutex g_discover_cache_mutex;
DiscoverCacheData g_discover_cache;

std::int64_t NowUnixSeconds() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path DiscoverCachePath() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(),
                                       static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) return {};
    exe.resize(n);
    return std::filesystem::path(exe).parent_path() / "musicfree" /
           kDiscoverCacheName;
}

template <typename T>
T JsonValue(const nlohmann::json& j, const char* key, T fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    try { return j.at(key).get<T>(); } catch (...) { return fallback; }
}

nlohmann::json SheetToJson(const MusicSheet& s) {
    nlohmann::json j = nlohmann::json::object();
    j["id"] = s.id;
    j["platform"] = s.platform;
    j["title"] = s.title;
    j["description"] = s.description;
    j["artwork"] = s.artwork;
    j["work_id"] = s.work_id;
    j["play_count"] = s.play_count;
    j["create_at"] = s.create_at;
    j["group"] = s.group;
    // raw 必须原样保留：咪咕 / 酷狗的详情调用依赖 contentId 等私有字段。
    j["raw"] = s.raw.is_null() ? nlohmann::json::object() : s.raw;
    return j;
}

MusicSheet SheetFromCacheJson(const nlohmann::json& j) {
    MusicSheet s;
    s.id = JsonValue<std::string>(j, "id", "");
    s.platform = JsonValue<std::string>(j, "platform", "");
    s.title = JsonValue<std::string>(j, "title", "");
    s.description = JsonValue<std::string>(j, "description", "");
    s.artwork = JsonValue<std::string>(j, "artwork", "");
    s.work_id = JsonValue<std::string>(j, "work_id", "");
    s.play_count = JsonValue<std::int64_t>(j, "play_count", 0);
    s.create_at = JsonValue<std::int64_t>(j, "create_at", 0);
    s.group = JsonValue<std::string>(j, "group", "");
    if (j.is_object() && j.contains("raw")) s.raw = j["raw"];
    if (!s.raw.is_object()) s.raw = nlohmann::json::object();
    if (s.raw.find("platform") == s.raw.end()) s.raw["platform"] = s.platform;
    if (s.raw.find("id") == s.raw.end()) s.raw["id"] = s.id;
    return s;
}

bool ReadDiscoverCacheFile(const std::filesystem::path& path,
                           DiscoverCacheData* out) {
    if (!out || path.empty()) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kDiscoverCacheMaxBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    nlohmann::json root;
    try { in >> root; } catch (...) { return false; }
    if (!root.is_object() || JsonValue<int>(root, "version", 0) != 1) return false;
    DiscoverCacheData data;
    data.saved_at = JsonValue<std::int64_t>(root, "saved_at", 0);
    if (data.saved_at <= 0 || !root.contains("groups") ||
        !root["groups"].is_array()) return false;
    for (const auto& g : root["groups"]) {
        if (!g.is_object()) continue;
        BrowserState::DiscoverGroup group;
        group.platform = JsonValue<std::string>(g, "platform", "");
        if (group.platform.empty() || !g.contains("sheets") ||
            !g["sheets"].is_array()) continue;
        for (const auto& sj : g["sheets"])
            group.sheets.push_back(SheetFromCacheJson(sj));
        if (!group.sheets.empty()) data.groups.push_back(std::move(group));
    }
    if (data.groups.empty()) return false;
    *out = std::move(data);
    return true;
}

// 先写 .tmp 再原子替换，避免中途退出留下半个 JSON。写入量只有几十~几百 KB，
// 放在刷新完成的消息里同步做（后台线程会被进程退出直接掐掉，缓存就丢了）。
void WriteDiscoverCacheFile(const std::filesystem::path& path,
                            const DiscoverCacheData& data) {
    if (path.empty() || data.groups.empty() || data.saved_at <= 0) return;
    try {
        nlohmann::json groups = nlohmann::json::array();
        for (const auto& g : data.groups) {
            nlohmann::json jg = nlohmann::json::object();
            jg["platform"] = g.platform;
            nlohmann::json sheets = nlohmann::json::array();
            for (const auto& s : g.sheets) sheets.push_back(SheetToJson(s));
            jg["sheets"] = std::move(sheets);
            groups.push_back(std::move(jg));
        }
        nlohmann::json root = nlohmann::json::object();
        root["version"] = 1;
        root["saved_at"] = data.saved_at;
        root["groups"] = std::move(groups);
        const std::string text = root.dump();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path tmp(path.wstring() + L".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            std::filesystem::remove(tmp, ec);
    } catch (...) {}
}

// 后台刷新时按音源替换 / 追加分组，缓存里的旧卡片一直留在界面上，不做清空闪烁。
void UpsertDiscoverGroup(BrowserState* state, const std::string& platform,
                         std::vector<MusicSheet> sheets) {
    for (auto& g : state->groups) {
        if (g.platform == platform) { g.sheets = std::move(sheets); return; }
    }
    BrowserState::DiscoverGroup group;
    group.platform = platform;
    group.sheets = std::move(sheets);
    state->groups.push_back(std::move(group));
}

// 缓存命中时本次不跑网络，但搜索页的音源下拉框仍要等后台挂载完才能填全。
// 用一个纯等待线程补上这一步，绝不占用 UI 线程。
void WatchSourcesReady(BrowserState* state) {
    if (!state) return;
    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak] {
        BgThreadGuard guard;
        if (IsMusicFreeShuttingDown()) return;
        try { WaitForMusicFreeSources(); } catch (...) {}
        if (auto sp = weak.lock())
            PostMessageW(sp->hwnd, kMsgSourcesReady, 0, 0);
    }).detach();
}

BrowserState* StateOf(const ChildProbe* probe) {
    return probe ? static_cast<BrowserState*>(probe->state) : nullptr;
}

// ------------------------------------------------------------------ 基础

std::wstring WindowText(HWND control) {
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

std::wstring Utf8ToWideString(const std::string& value) {
    return core::Utf8ToWide(value);
}

// 状态文本按页签分别存档。后台任务晚到时可以直接写目标页的状态而不污染
// 用户当前正在看的那一页。
void SetStatusFor(BrowserState* state, bool for_search,
                  const std::wstring& text) {
    if (!state) return;
    (for_search ? state->search_status : state->discover_status) = text;
    if (state->status &&
        (state->mode == BrowserMode::kSearch) == for_search)
        SetWindowTextW(state->status, text.c_str());
}

void SetStatus(BrowserState* state, const std::wstring& text) {
    if (state)
        SetStatusFor(state, state->mode == BrowserMode::kSearch, text);
}

// 曲目操作按钮只在「搜索页」或「发现页已展开曲目区」且没有后台任务时可用。
// LayoutChildren 与 SetBusyUi 都走这里，避免任务结束后把隐藏列表的按钮又点亮。
void SyncTrackActionButtons(BrowserState* state, bool busy) {
    if (!state) return;
    const bool tracks_on =
        IsVideoMode(state) ? state->video_tracks_visible : state->tracks_visible;
    const BOOL enabled =
        (!busy && (state->mode == BrowserMode::kSearch || tracks_on))
            ? TRUE : FALSE;
    if (state->play_button) EnableWindow(state->play_button, enabled);
    if (state->enqueue_button) EnableWindow(state->enqueue_button, enabled);
    if (state->download_button) EnableWindow(state->download_button, enabled);
}

constexpr wchar_t kSheetHint[] =
    L"点击任意封面卡片即可查看曲目（展开后显示歌曲列表）";
constexpr wchar_t kVideoHint[] =
    L"点击卡片查看剧集，双击条目开始播放（视频窗口外框与音乐窗一致）";

// 未展开曲目区时，状态条固定显示引导文字。只有文本变化才写控件，避免
// WM_SIZE 反复触发无谓的 STATIC 重绘。
void EnsureSheetHint(BrowserState* state) {
    if (!state || !state->tracks_caption) return;
    const wchar_t* hint = !IsVideoMode(state) ? kSheetHint : kVideoHint;
    wchar_t buf[256]{};
    GetWindowTextW(state->tracks_caption, buf, 256);
    if (std::wstring(buf) != hint) {
        state->discover_caption = hint;
        SetWindowTextW(state->tracks_caption, hint);
    }
}

// 曲目区状态条：卡片网格和曲目列表之间的一行醒目文字。以前打开歌单只有顶部
// 16px 的状态栏在变，用户很容易以为「点了没反应、也没有任何提示」。
void SetSheetCaption(BrowserState* state, const std::wstring& text) {
    if (!state) return;
    state->discover_caption = text;
    if (state->tracks_caption)
        SetWindowTextW(state->tracks_caption, text.c_str());
}

// 状态栏 + 曲目区状态条一起更新，保证两个位置都能看到当前进度。
void SetSheetProgress(BrowserState* state, const std::wstring& caption,
                      const std::wstring& status) {
    SetSheetCaption(state, caption);
    SetStatusFor(state, false, status);  // 歌单进度只属于发现页
}

void InvalidateFrame(BrowserState* state) {
    if (state && state->hwnd) InvalidateRect(state->hwnd, nullptr, FALSE);
}

// 后台任务进行中时禁用会再触发任务的按钮，并把「重新搜索」画成禁用帧。
void SetBusyUi(BrowserState* state, bool busy) {
    if (!state) return;
    if (state->search_button) EnableWindow(state->search_button, !busy);
    SyncTrackActionButtons(state, busy);
    if (state->import_button) EnableWindow(state->import_button, !busy);
    InvalidateFrame(state);
}

std::wstring FormatDuration(std::int64_t duration_ms) {
    if (duration_ms <= 0) return L"-";
    const std::int64_t seconds = duration_ms / 1000;
    std::wstring text = std::to_wstring(seconds / 60) + L":";
    const std::int64_t remainder = seconds % 60;
    if (remainder < 10) text += L"0";
    text += std::to_wstring(remainder);
    return text;
}

const skin::LegacySkin* ActiveLegacySkin(const BrowserState* state) {
    if (!state || !state->host.skin) return nullptr;
    return state->host.skin();
}

const skin::BrowserSkin* ActiveSkin(const BrowserState* state) {
    const skin::LegacySkin* legacy = ActiveLegacySkin(state);
    if (!legacy || !legacy->Browser().valid) return nullptr;
    return &legacy->Browser();
}

COLORREF FrameTransparent(const BrowserState* state,
                          const skin::BrowserSkin& browser) {
    return frame::TransparentColor(ActiveLegacySkin(state), browser);
}

// ------------------------------------------------------------------ 字体

void EnsureFonts(BrowserState* state) {
    if (!state) return;
    if (!state->ui_font) state->ui_font = CreateUiFont(-12, FW_NORMAL);
    if (!state->bold_font) state->bold_font = CreateUiFont(-12, FW_SEMIBOLD);
    if (!state->small_font) state->small_font = CreateUiFont(-11, FW_NORMAL);
    const skin::BrowserSkin* browser = ActiveSkin(state);
    const int height = browser && browser->link.font_size > 0
        ? -browser->link.font_size : -12;
    const wchar_t* face = browser && !browser->link.font.empty()
        ? browser->link.font.c_str() : L"Tahoma";
    if (!state->link_font || state->link_logfont.lfHeight != height ||
        _wcsicmp(state->link_logfont.lfFaceName, face) != 0) {
        if (state->link_font) DeleteObject(state->link_font);
        state->link_font = MakeFont(height, FW_NORMAL, face);
        state->link_logfont = {};
        if (state->link_font)
            GetObjectW(state->link_font, sizeof(LOGFONTW), &state->link_logfont);
    }
}

void DiscardFonts(BrowserState* state) {
    if (!state) return;
    for (HFONT* font : {&state->ui_font, &state->bold_font, &state->small_font,
                        &state->link_font}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
}

// ------------------------------------------------------------------ 命中测试

bool HitTest(const RECT& rect, POINT point) {
    return theme::HitRect(rect, point);
}


// ------------------------------------------------------------------ 布局

// 内容区顶部那条工具行：音源下拉 + 关键词输入 + 搜索 / 导入。布局与绘制共用同一份
// 计算，免得描边和子控件位置漂移。
struct ToolbarLayout {
    RECT combo{};
    RECT query{};
    RECT search{};
    RECT import{};
};

ToolbarLayout BuildToolbarLayout(const RECT& content) {
    ToolbarLayout layout;
    const LONG inner_left = content.left + 6;
    const LONG inner_right = content.right - 6;
    const LONG top = content.top + 4;
    const LONG width = std::max<LONG>(160, inner_right - inner_left);
    const LONG combo_w = std::min<LONG>(104, width / 4);
    const LONG button_w = 58;
    layout.combo = {inner_left, top, inner_left + combo_w, top + kButtonHeight};
    layout.import = {inner_right - button_w, top, inner_right, top + kButtonHeight};
    layout.search = {layout.import.left - 6 - button_w, top,
                     layout.import.left - 6, top + kButtonHeight};
    const LONG query_left = layout.combo.right + 6;
    const LONG query_right = std::max<LONG>(query_left + 60, layout.search.left - 6);
    layout.query = {query_left, top, query_right, top + kButtonHeight};
    return layout;
}

// 皮肤有效时窗口的自然尺寸就是背景位图尺寸，框体元素按 frame::Map 由位图坐标换算
// 到当前客户区（窗口缩放后内容区变大、按钮右锚定、底部行下锚定）；否则按兜底尺寸
// 推算一条与皮肤风格相近的边框。
FrameLayout BuildFrameLayout(const RECT& client,
                             const skin::BrowserSkin* browser,
                             const ui::DpiScale& scale) {
    FrameLayout layout;
    if (browser) {
        layout.skinned = true;
        layout.skin = frame::Map(client, *browser, scale);
        layout.content = layout.skin.content;
        layout.close = layout.skin.close;
        layout.prev_source = layout.skin.backward;
        layout.next_source = layout.skin.forward;
        layout.refresh = layout.skin.refresh;
        // 皮肤里的标题栏按钮原本是**右锚定**的（frame::Map 会按窗口加宽量整体
        // 右移）。窗口被钉在 720px 而位图更窄时，整簇就停在标题栏中间，左边空出
        // 一大块，鼠标要走很远才点得到 —— 用户反馈「按钮挪到前面去」。
        // 这里把三连按钮（◀ ▶ 重新搜索）整体平移到标题栏最左侧，**保持皮肤里
        // 原有的相对间距与按钮尺寸**，只改 X；✕ 仍留在右上角。
        // ⚠ 不要把三连按钮平移到标题栏最左侧：皮肤位图在 x≈12..100、
        // y≈5..26 画着「千千音乐窗」标题文字，按钮压过去会把整行字切得只剩
        // 第一个字的一角，左上角看起来就是一块青色残片（用户反馈的皮肤问题）。
        //
        // 现在窗口允许拉宽（见 WM_GETMINMAXINFO / ResizeHitCode）。frame::Map 会把
        // 标题栏按钮右锚定（X += grow_x），拉宽后整簇会跑到标题栏中间；
        // 但也不能像旧逻辑那样把它平移到最左 —— 那会切碎「千千音乐窗」文字。
        // 正确做法：直接减掉 grow_x，取消右锚定，让按钮恒停在皮肤设计位 x≈106
        // （文字右侧）。✕ 仍保持右锚定贴右上角。
        {
            const int natural_width = layout.skin.natural.cx > 0
                ? layout.skin.natural.cx : (client.right - client.left);
            const int grow_x = (client.right - client.left) - natural_width;
            if (grow_x > 0) {
                RECT* cluster[3] = {&layout.prev_source, &layout.next_source,
                                    &layout.refresh};
                for (RECT* rect : cluster) {
                    if (rect->right <= rect->left) continue;
                    rect->left -= grow_x;
                    rect->right -= grow_x;
                }
            }
        }
        layout.check_box = layout.skin.check_box;
        layout.check_label = layout.skin.check_label;
        layout.link_text = layout.skin.link;
        layout.query_field = BuildToolbarLayout(layout.content).query;
        return layout;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int pad = kFramePad;
    // 标题栏 / 内容面板 / 页脚三段，以及**左对齐**的导航簇（音源 ◀ ▶ 重新搜索）。
    // ✕ 仍贴右上角。标题文字跟在导航簇后面，由 BuildFrameChrome 裁好，不会重叠。
    layout.chrome = theme::BuildFrameChrome(client, pad, 30, 30);
    layout.content = layout.chrome.content;
    layout.close = layout.chrome.close;
    layout.prev_source = layout.chrome.prev_source;
    layout.next_source = layout.chrome.next_source;
    layout.refresh = layout.chrome.refresh;
    layout.check_box = {pad, height - 26, pad + 13, height - 13};
    layout.check_label = {pad + 18, height - 27, pad + 86, height - 12};
    layout.link_text = {width - pad - 230, height - 27, width - pad, height - 12};
    layout.query_field = BuildToolbarLayout(layout.content).query;
    return layout;
}

void SizeResultColumns(BrowserState* state) {
    if (!state || !state->list) return;
    RECT client{};
    GetClientRect(state->list, &client);
    const int available = (client.right - client.left) - GetSystemMetrics(SM_CXVSCROLL);
    if (available < 200) return;
    // 平台 / 歌曲名 / 歌手 / 专辑 / 时长
    const double ratio[kColumnCount] = {0.13, 0.35, 0.21, 0.20, 0.11};
    int used = 0;
    for (int index = 0; index < kColumnCount - 1; ++index) {
        const int column = std::max(40, static_cast<int>(available * ratio[index]));
        ListView_SetColumnWidth(state->list, index, column);
        used += column;
    }
    ListView_SetColumnWidth(state->list, kColumnCount - 1,
                            std::max(38, available - used));
}

// WebView2 页面接管视觉后，把旧 Win32 子控件整批隐藏（只藏不删，回退路径完整
// 保留：TTPLAYER_NO_WEBVIEW 或宿主创建失败时本函数不会被调用，旧渲染原样可用）。
//
// 隐藏范围 = 内容区 + 顶栏页签 + 底部控制条 + 搜索工具栏：页面已提供同等的
// 三个页签与播放控制条，Win32 侧只留皮肤边框、标题栏自绘按钮与窗口本身。
// load_sheets/open_sheet 两个旧按钮已不再创建（句柄恒空，判空即过）。
void HideLegacyContentControls(BrowserState* state, bool hide) {
    if (!state) return;
    const int show = hide ? SW_HIDE : SW_SHOW;
    HWND* controls[] = {
        &state->tab,                // 顶栏页签（音乐 / 视频 / 搜索）
        &state->sheet_list,         // 卡片网格（LBS_OWNERDRAWVARIABLE 自绘）
        &state->list,               // 曲目 / 搜索结果列表（LVS_OWNERDRAWFIXED）
        &state->tracks_caption,     // 曲目区状态条
        &state->status,             // 底部状态行
        &state->platform,           // 搜索页音源下拉
        &state->query,              // 搜索输入框
        &state->search_button,      // 「搜索」按钮（BS_OWNERDRAW）
        &state->import_button,      // 「导入…」按钮（BS_OWNERDRAW）
        &state->play_button,        // ┐
        &state->enqueue_button,     // ├ 底部控制条（页面提供同等控件）
        &state->download_button,    // ┘
        &state->tag_combo,          // 保留但常隐的标签下拉
        &state->load_sheets_button, // 旧「刷新」按钮（现句柄恒空）
        &state->open_sheet_button,  // 旧「打开歌单」按钮（现句柄恒空）
        &state->video_navbar,       // 视频页顶部分类导航行（自绘 STATIC）
        &state->video_iconbar,      // 视频页图标功能栏（自绘 STATIC）
    };
    for (HWND* control : controls)
        if (*control) ShowWindow(*control, show);
}

void LayoutChildren(BrowserState* state) {
    if (!state || !state->hwnd) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    state->layout = BuildFrameLayout(client, ActiveSkin(state),
                            ui::ResolveScaleForWindow(state->hwnd));

    const RECT content = state->layout.content;
    const int tab_h = 24;
    if (state->tab)
        MoveWindow(state->tab, content.left, content.top,
                   content.right - content.left, tab_h, TRUE);
    const RECT sub = {content.left, content.top + tab_h + 2, content.right,
                      content.bottom};
    const LONG inner_left = sub.left + 6;
    const LONG inner_right = sub.right - 6;
    const LONG top = sub.top;
    const int buttons_y = sub.bottom - kButtonHeight;
    const LONG width = inner_right - inner_left;

    // 底部操作按钮（所有模式通用，作用于曲目列表）
    MoveWindow(state->play_button, inner_left, buttons_y, 92, kButtonHeight, TRUE);
    MoveWindow(state->enqueue_button, inner_left + 98, buttons_y, 108,
               kButtonHeight, TRUE);
    MoveWindow(state->download_button, inner_left + 212, buttons_y, 70,
               kButtonHeight, TRUE);

    const bool search = state->mode == BrowserMode::kSearch;
    // 卡片列数随宽度变化（窗口宽度被主窗口钉死，通常是 4 列）。
    // 视频页海报卡更窄，同行能多放一张；行高也按模式各用一套。
    const int card_w = IsVideoMode(state) ? kVideoCardWidth : kCardWidth;
    const int card_h = IsVideoMode(state) ? kVideoRowHeight : kCardRowHeight;
    // 影视仓式海报墙的列数随窗口宽度增加（宽屏右侧不再留大片空白），
    // 仍设上限防止超宽窗口把卡片行拉得过长。
    const int cols_max = IsVideoMode(state) ? 8 : 8;
    const int cols = std::max(2, std::min(cols_max, static_cast<int>(width) / card_w));
    if (IsVideoMode(state)) state->video_cols = cols;
    else state->discover_cols = cols;
    // 列数跨越阈值时重建绘制行：否则拖宽后仍按旧列数拆行，卡片布局不跟随
    // 窗口缩放（用户反馈「缩放时内容也要跟随显示」）。只在跨越阈值的那一帧
    // 重建（低频），拖动中同列数的其余帧零开销。
    if (IsVideoMode(state)) {
        if (state->video_cols_applied != cols) {
            state->video_cols_applied = cols;
            if (state->sheet_list && !state->video_rows.empty())
                FillVideoList(state);
        }
    } else {
        if (state->discover_cols_applied != cols) {
            state->discover_cols_applied = cols;
            if (state->sheet_list && !state->discover_rows.empty())
                FillDiscoverList(state);
        }
    }

    // 状态行统一收到底部：紧贴「下载」按钮右侧，单行、垂直居中，把顶部那一行
    // 让给内容区（原来顶部有一整行 刷新 / 打开歌单 按钮，纯浪费高度）。
    const LONG download_right = inner_left + 212 + 70;  // 下载按钮右沿
    const LONG status_left = download_right + 10;
    const int status_y = buttons_y + (kButtonHeight - 16) / 2;

    if (search) {
        // 搜索页不显示影视仓外壳条。
        if (state->video_navbar) ShowWindow(state->video_navbar, SW_HIDE);
        if (state->video_iconbar) ShowWindow(state->video_iconbar, SW_HIDE);
        const ToolbarLayout toolbar = BuildToolbarLayout(sub);
        MoveWindow(state->platform, toolbar.combo.left, toolbar.combo.top,
                   toolbar.combo.right - toolbar.combo.left, 240, TRUE);
        MoveWindow(state->import_button, toolbar.import.left, toolbar.import.top,
                   toolbar.import.right - toolbar.import.left, kButtonHeight, TRUE);
        MoveWindow(state->search_button, toolbar.search.left, toolbar.search.top,
                   toolbar.search.right - toolbar.search.left, kButtonHeight, TRUE);
        // 可见性由布局统一决定：先定位再显示，杜绝「停在占位位置」的残影。
        ShowWindow(state->search_button, SW_SHOW);
        // 输入框缩进一像素，让框体自绘的那圈描边露出来（代替系统的凹陷边）。
        MoveWindow(state->query, toolbar.query.left + 1, toolbar.query.top + 1,
                   std::max<LONG>(40, toolbar.query.right - toolbar.query.left - 2),
                   kButtonHeight - 2, TRUE);
        // 音乐/视频页会把输入框藏起来（那里没有地址栏），回到搜索页要显式显示，
        // 否则搜索框会一直不出现。
        ShowWindow(state->query, SW_SHOW);
        ShowWindow(state->status, SW_SHOW);
        // 状态行放到底部「下载」右侧；列表从工具栏下方直接开始，纵向多一行结果。
        MoveWindow(state->status, status_left, status_y,
                   std::max<LONG>(120, inner_right - status_left), 16, TRUE);
        const int list_top = top + kButtonHeight + 6;
        const int list_bottom = std::max(list_top + 40, buttons_y - 8);
        MoveWindow(state->list, inner_left, list_top, width,
                   list_bottom - list_top, TRUE);
        ShowWindow(state->list, SW_SHOW);
        ShowWindow(state->tracks_caption, SW_HIDE);
        SizeResultColumns(state);
    } else {
        // 发现 / 视频页：取消顶部按钮行（刷新改用标题栏的刷新按钮，单击卡片即可
        // 打开歌单/视频），导入按钮放到底部最右，状态行夹在「下载」与「导入」之间。
        // 卡片网格直接从内容区顶部开始，纵向比原来多出一整行按钮的高度。
        MoveWindow(state->import_button, inner_right - 58, buttons_y, 58,
                   kButtonHeight, TRUE);
        const LONG status_right =
            std::max<LONG>(status_left + 120, inner_right - 58 - 8);
        ShowWindow(state->query, SW_HIDE);
        // 搜索按钮同样要藏：它的 MoveWindow 只写在上面的 search 分支里，
        // 音乐/视频页没人定位它，会以 10x10 占位尺寸显示在窗口左上角（用户看到的
        // 「柒」残影）。与输入框保持一致 —— 只有搜索页才有顶部工具栏。
        ShowWindow(state->search_button, SW_HIDE);
        ShowWindow(state->status, SW_SHOW);
        MoveWindow(state->status, status_left, status_y,
                   status_right - status_left, 16, TRUE);

        // 影视仓式外壳：视频页顶部预留「分类导航行 + 图标功能栏」；音乐页不占。
        int nav_h = 0, icon_h = 0;
        if (IsVideoMode(state)) {
            nav_h = kVideoNavH;
            icon_h = kVideoIconH;
            if (state->video_navbar) {
                MoveWindow(state->video_navbar, inner_left, top + 2, width, nav_h, TRUE);
                ShowWindow(state->video_navbar, SW_SHOW);
                InvalidateRect(state->video_navbar, nullptr, FALSE);
            }
            if (state->video_iconbar) {
                MoveWindow(state->video_iconbar, inner_left, top + 2 + nav_h,
                           width, icon_h, TRUE);
                ShowWindow(state->video_iconbar, SW_SHOW);
                InvalidateRect(state->video_iconbar, nullptr, FALSE);
            }
        }

        const int list_top = top + 2 + nav_h + icon_h;
        const int list_bottom = std::max(list_top + 60, buttons_y - 6);
        // 首次进入尚未点开任何榜单时，曲目列表整块隐藏，卡片网格一直铺到底部
        // 按钮行；点开歌单后再让出 tracks_h 给曲目列表。显隐统一由这里决定，
        // 窗口缩放 / 切页 / 后台刷新都不会残留错误布局。
        const bool expanded = IsVideoMode(state) ? state->video_tracks_visible
                                                 : state->tracks_visible;
        const int tracks_h = std::min(160, std::max(90, (list_bottom - list_top) / 3));
        int grid_bottom = expanded
            ? std::max(list_top + 80, list_bottom - tracks_h - 6)
            : list_bottom;
        // 卡片网格底部让出 18px 给「当前歌单 / 加载状态」文字；未展开时这行
        // 就是「点击封面卡片查看曲目」的引导。
        const int caption_h = 18;
        int grid_h = std::max(60, grid_bottom - list_top - caption_h);
        // 高度向下吸附到「分组行 + 若干完整卡片行」。否则窗口边界会正好切在
        // 某一行封面下沿，那一行只剩封面、标题与来源全被裁掉（用户反馈
        // 「封面图文本显示不全」）。省下的高度展开时给曲目列表，未展开时留白。
        // ⚠ 视频页例外：影视仓分区视图是「标题行+卡片行」多组连续滚动的内容，
        // 单组吸附公式会把网格压成「1 组标题 + 1 行卡片」（274px），其余空间
        // 黑屏（真机实测 rows=19、sheet client=647x274）。滚动内容允许底部
        // 半截行（影视仓同样如此）；音乐页单组网格继续吸附。
        if (!IsVideoMode(state)) {
            const int full_rows = std::max(1,
                (grid_h - kGroupRowHeight) / card_h);
            const int snapped_h = kGroupRowHeight + full_rows * card_h;
            if (snapped_h <= grid_h) {
                grid_h = snapped_h;
                grid_bottom = list_top + grid_h + caption_h;
            }
        }
        MoveWindow(state->sheet_list, inner_left, list_top, width,
                   grid_h, TRUE);
        MoveWindow(state->tracks_caption, inner_left, list_top + grid_h + 2,
                   width, caption_h - 2, TRUE);
        if (expanded)
            MoveWindow(state->list, inner_left, grid_bottom + 6, width,
                       std::max(40, list_bottom - grid_bottom - 6), TRUE);
        ShowWindow(state->tracks_caption, SW_SHOW);
        ShowWindow(state->list, expanded ? SW_SHOW : SW_HIDE);
        if (!expanded) EnsureSheetHint(state);
        SizeResultColumns(state);
    }
    SyncTrackActionButtons(state, state->busy.load());
    // WebView2 接管期间，上面每个分支的 ShowWindow 都会把旧控件重新放出来
    // （切页 / 缩放 / 展开曲目区全走这里），统一在收尾补一次隐藏；宿主创建
    // 失败（webview_failed）或运行中报错（Failed）时不藏 —— 旧界面保持显示。
    if (state->webview && !state->webview_failed && !state->webview->Failed())
        HideLegacyContentControls(state, true);
}

void ApplyFrameRegion(BrowserState* state) {
    if (!state || !state->hwnd) return;
    const skin::BrowserSkin* browser = ActiveSkin(state);
    if (!browser) {
        // 兜底框是规则矩形，不需要区域。
        SetWindowRgn(state->hwnd, nullptr, TRUE);
        return;
    }
    RECT client{};
    GetClientRect(state->hwnd, &client);
    // 区域必须和背景用同一份九宫格映射，否则拉伸之后透明边角会对不上。
    frame::ApplyWindowRegion(state->hwnd, *browser, state->layout.skin, client,
                             FrameTransparent(state, *browser),
                             ui::ResolveScaleForWindow(state->hwnd));
}

// 外框的自然尺寸：皮肤背景位图尺寸；没有皮肤时用兜底尺寸。
SIZE NaturalFrameSize(const BrowserState* state) {
    SIZE size{kFallbackWidth, kFallbackHeight};
    if (const skin::BrowserSkin* browser = ActiveSkin(state)) {
        if (browser->background.size.cx > 0 && browser->background.size.cy > 0)
            size = browser->background.size;
    }
    return size;
}

// 允许的最小尺寸。宽度锁定在自然位图宽度，高度允许在 [下限, 上限] 内随拖拽变化。
//
// ★ 宽度钉死在自然位图宽度：低于它 skin::DrawFrameBackground 会退化成整幅 1:1 缩放、
// 标题栏变形；高于它标题栏被横向拉糊、按钮错位。两端都钉在同一宽度上，窗口宽度
// 永远不变。高度则放开上下限，内容区随之下移/拉伸（标题栏与底栏始终保持原始像素）。
SIZE MinFrameSize(const BrowserState* state) {
    const SIZE natural = NaturalFrameSize(state);
    SIZE size{};
    size.cx = natural.cx;  // 宽度锁定：始终等于皮肤自然宽度
    size.cy = std::clamp<int>(natural.cy, kMinTrackHeightCap, kMaxTrackHeightCap);
    return size;
}

// 视频任务结束：释放 busy 锁，并把加载期间用户点过的分类排到消息循环里执行。
// 所有 video_busy 的复位都必须走这里 —— 以前散落的 store(false) 中只要漏一处，
// 分类就会永久卡在「正在加载，请稍候再切换分类」。
void ReleaseVideoBusy(BrowserState* state) {
    if (!state) return;
    state->video_busy = false;   // 故意不用 store()：批量替换时不要命中函数自身
    const int pending = state->video_pending_cat.exchange(-2);
    if (pending != -2 && state->hwnd)
        PostMessageW(state->hwnd, kMsgVideoPendingCat, 0,
                     static_cast<LPARAM>(pending));
}

// 换肤 / 缩放后的统一收尾：窗口区域 + 子控件位置 + 重画。
// reset_size 为 true 时把窗口拉回自然尺寸（只在皮肤变化需要重贴合时用）；
// 否则保留用户当前的尺寸，只做最小尺寸夹取 —— 音乐窗的宽高因此是「活的」，
// 不再每次唤出都被写死的位图尺寸覆盖。
void ApplyFrameMetrics(BrowserState* state, bool reset_size) {
    if (!state || !state->hwnd || !IsWindow(state->hwnd)) return;
    if (state->applying_frame) return;
    state->applying_frame = true;

    RECT current{};
    GetWindowRect(state->hwnd, &current);
    const SIZE natural = NaturalFrameSize(state);
    const SIZE minimum = MinFrameSize(state);
    int width = reset_size ? natural.cx : current.right - current.left;
    int height = reset_size ? natural.cy : current.bottom - current.top;
    width = std::max<int>(width, minimum.cx);
    height = std::max<int>(height, minimum.cy);
    if (width != current.right - current.left ||
        height != current.bottom - current.top) {
        // 尺寸变化会同步投递 WM_SIZE；下面重排一次即可，不必依赖那条消息。
        SetWindowPos(state->hwnd, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT client{};
    GetClientRect(state->hwnd, &client);
    state->layout = BuildFrameLayout(client, ActiveSkin(state),
                            ui::ResolveScaleForWindow(state->hwnd));
    ApplyFrameRegion(state);
    LayoutChildren(state);
    // 拖动缩放期间这条每帧都跑：bErase=TRUE 会对每个子控件多安排一次擦底，
    // 表现为拖动闪烁；主窗自身双缓冲 + WS_EX_COMPOSITED 已覆盖背景，FALSE 足够。
    InvalidateRect(state->hwnd, nullptr, FALSE);

    // WebView2 内容区跟随重排的统一收口：WM_SIZE / 拖动缩放 / 换肤 / 再次唤出
    // 都经本函数（BuildFrameLayout 刚刷新过 state->layout）。宿主未就绪或已
    // 失败时不打扰它；置顶最小化（SIZE_MINIMIZED）根本不会走到这里。
    if (state->webview && state->webview->Ready())
        state->webview->Resize(state->layout.content);

    state->applying_frame = false;
}

// --------------------------------------------------------------- 框体绘制

// 皮肤按钮的当前状态帧。禁用时恒为最后一帧（皮肤的四态图里通常是灰态）。
int FrameButtonState(const BrowserState* state, int hit, bool enabled = true) {
    if (!enabled) return 3;
    if (state->frame_pressed == hit && state->frame_hover == hit) return 2;
    return state->frame_hover == hit ? 1 : 0;
}

// 拖动下边缩放时把窗口底部吸附到其它 TTPlayer 窗口的底部（容差内对齐），
// 让音乐窗能与主窗 / 播放列表 / 歌词窗 / 均衡器窗拼成整齐的一列。
void SnapBrowserBottomEdge(BrowserState* state, RECT& rc) {
    if (!state || !state->hwnd) return;
    const wchar_t* classes[] = {L"TTPlayer_PlayerWnd", L"TTPlayer_PlayListWnd",
                                L"TTPlayer_LyricWnd", L"TTPlayer_EqualizerWnd"};
    constexpr int kSnapPx = 14;
    LONG best = 0;
    LONG best_dist = kSnapPx + 1;
    for (const wchar_t* cls : classes) {
        const HWND other = FindWindowW(cls, nullptr);
        if (!other || other == state->hwnd || !IsWindowVisible(other)) continue;
        RECT orr{};
        if (!GetWindowRect(other, &orr)) continue;
        const LONG dist = orr.bottom - rc.bottom;
        const LONG ad = dist < 0 ? -dist : dist;
        if (ad <= kSnapPx && ad < best_dist) {
            best_dist = ad;
            best = orr.bottom;
        }
    }
    if (!best) return;
    const LONG min_height = MinFrameSize(state).cy;
    if (best - rc.top >= min_height) rc.bottom = best;
}

void PaintFallbackFrame(BrowserState* state, HDC dc, const RECT& client) {
    (void)client;
    const FrameLayout& layout = state->layout;
    // 分区与外观都交给共用主题；这里只负责把当前 hover/pressed 状态翻成按钮帧号。
    // FrameButtonState 的 0..3 与 theme::ButtonState 同序（常态/热/按下/禁用）。
    theme::ChromeStates states;
    states.close = FrameButtonState(state, kFrameHitClose);
    states.prev_source = FrameButtonState(state, kFrameHitPrevSource);
    states.next_source = FrameButtonState(state, kFrameHitNextSource);
    states.refresh = FrameButtonState(state, kFrameHitRefresh, !state->busy.load());
    // 标题跟着当前页签走：三个页签共用一个窗口，标题写死「音乐窗」在视频页
    // 会让人以为切错了。
    const wchar_t* title = state->mode == BrowserMode::kVideo ? L"视频 · 影视仓"
                          : state->mode == BrowserMode::kSearch ? L"搜索 · MusicFree"
                          : L"音乐窗 · MusicFree";
    theme::PaintFrameChrome(dc, layout.chrome, title,
                            state->bold_font, state->ui_font, states);
}

void PaintCheckBox(BrowserState* state, HDC dc, const skin::BrowserSkin* browser) {
    const RECT box = state->layout.check_box;
    if (browser && browser->check.image) {
        // 皮肤的四态图：0 未选 / 1 热 / 2 选中 / 3 灰。
        skin::DrawFrameButton(dc, browser->check, box, state->top_most ? 2 : 0,
                              FrameTransparent(state, *browser));
    } else {
        const bool hot = state->frame_hover == kFrameHitTopMost;
        DrawRoundedBox(dc, box, state->top_most ? kAccent : kPanel,
                       hot ? kAccentHot : kOutline, 4);
        if (state->top_most)
            DrawTextIn(dc, box, L"✓", RGB(255, 255, 255), state->small_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    const bool hot = state->frame_hover == kFrameHitTopMost;
    DrawTextIn(dc, state->layout.check_label, L"窗口置顶",
               state->top_most ? kAccent : (hot ? kText : kSubtleText),
               state->small_font, kLabelFormat);
}

void PaintFrame(BrowserState* state, HDC dc) {
    if (!state || !state->hwnd) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    EnsureFonts(state);
    const skin::BrowserSkin* browser = ActiveSkin(state);
    state->layout = BuildFrameLayout(
        client, browser, ui::ResolveScaleForWindow(state->hwnd));

    if (!browser) {
        PaintFallbackFrame(state, dc, client);
    } else {
        // 九宫格铺满客户区：边框 / 标题栏 / 页脚保持原始像素，只有内容区随窗口
        // 变大，所以放大之后的框体和原尺寸下看起来是同一套像素。
        frame::PaintBackground(dc, *browser, state->layout.skin, client);
        const COLORREF transparent = FrameTransparent(state, *browser);
        // 窗口拉宽后九宫格「顶行中间格」会被横向拉伸，把位图里的
        // 「千千音乐窗」标题文字拉成变形条。用源位图左上角按 1:1 重画标题区，
        // 文字保持原像素，右侧渐变继续随窗口拉伸。
        // ⚠ 重绘宽度必须覆盖三连按钮簇的皮肤设计位（默认皮肤 backward 106、
        // forward 137、refresh 169..194）：按钮四态贴图是从位图原位抠出来的、
        // 自带背景；落在 160 之外的按钮（refresh）背景会与拉伸后的渐变对不上
        // （用户反馈「返回/前进/刷新位置与颜色跟皮肤对不上」）。取簇右边界 +6。
        {
            const int natural_w = state->layout.skin.natural.cx;
            if (natural_w > 0 && (client.right - client.left) > natural_w) {
                const int cluster_right = std::max(
                    std::max(browser->backward.bounds.right,
                             browser->forward.bounds.right),
                    browser->refresh.bounds.right);
                const int title_w = std::min(
                    natural_w, std::max(160, cluster_right + 6));
                const int title_h = state->layout.skin.resize_rect.top > 0
                    ? std::min<int>(state->layout.skin.resize_rect.top, 31) : 31;
                browser->background.image.Draw(
                    dc, 0, 0, title_w, title_h,
                    0, 0, title_w, title_h, transparent);
            }
        }
        // ⚠ 这里曾放「盖左上角柒 logo」的代码，两版都失败，根因已查明：
        //   ① 用 GetPixel 检测青色：内存 DC 上 GetPixel 返回 0，检测恒漏判。
        //   ② 改用 StretchBlt 从「标题栏右侧列」横向拉伸覆盖：源坐标用的是
        //      皮肤自然宽度（natural.cx-40），而 dc 是已按窗口缩放后的客户区
        //      DC，源点取到的是内容区像素 —— 音乐页恰好复制到皮肤色（看不出来），
        //      视频页复制到别的东西，就在左上角留下一块 10x10 色块（用户看到的
        //      「柒」）。取样坐标与绘制坐标系不一致，是这两次尝试共同的坑。
        // 真正的「柒」来源另行定位（见 .workbuddy/memory 日志），此处不再覆盖。
        {
            const int natural_w = state->layout.skin.natural.cx;
            if (natural_w > 0) {
                // 保留一个空实现锚点，避免后续误加绘制。
                (void)natural_w;
            }
        }
        const bool busy = state->busy.load();
        skin::DrawFrameButton(dc, browser->close, state->layout.close,
                              FrameButtonState(state, kFrameHitClose), transparent);
        skin::DrawFrameButton(dc, browser->backward, state->layout.prev_source,
                              FrameButtonState(state, kFrameHitPrevSource),
                              transparent);
        skin::DrawFrameButton(dc, browser->forward, state->layout.next_source,
                              FrameButtonState(state, kFrameHitNextSource),
                              transparent);
        skin::DrawFrameButton(dc, browser->refresh, state->layout.refresh,
                              FrameButtonState(state, kFrameHitRefresh, !busy),
                              transparent);
    }

    // 视频页：内容区统一深色底。卡片网格高度吸附后剩下的空间若不填，
    // 会露出皮肤内容区的浅色位图，看起来像「下半屏什么内容都没有」。
    if (IsVideoMode(state)) {
        HBRUSH brush = CreateSolidBrush(kVideoPageBg);
        FillRect(dc, &state->layout.content, brush);
        DeleteObject(brush);
    }

    // WebView2 页面（音乐/视频都走 webui）在控件创建 → 页面首帧之间默认露
    // **白底**（SDK 没有 DefaultBackgroundColor 接口）。只要页面宿主在用，
    // 内容区一律先铺深色底 + 「正在加载页面…」字样；WebView 显示后自然会
    // 盖住这层，用户从头到尾看不到白块。
    if (state->webview && !state->webview_failed) {
        HBRUSH brush = CreateSolidBrush(kVideoPageBg);
        FillRect(dc, &state->layout.content, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(120, 132, 144));
        DrawTextW(dc, L"正在加载页面…", -1, &state->layout.content,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    PaintCheckBox(state, dc, browser);

    // 关键词输入框的描边：画在 EDIT 下面，EDIT 缩进一像素嵌在里面。
    DrawRoundedBox(dc, state->layout.query_field, kPanel, kOutline, 4);

    const COLORREF link_color = browser ? browser->link.color : RGB(96, 122, 134);
    DrawTextIn(dc, state->layout.link_text, state->link_text.c_str(), link_color,
               state->link_font ? state->link_font : state->small_font,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// --------------------------------------------------- 子控件自绘（按钮）

struct ButtonVisual {
    const wchar_t* text{};
    bool primary{};
    bool enabled{true};
};

ButtonVisual ButtonVisualFor(const BrowserState* state, int id) {
    const bool busy = state->busy.load();
    switch (id) {
        case kIdSearch: return {L"搜索", true, !busy};
        case kIdImport: return {L"导入…", false, !busy};
        case kIdPlay: return {L"播放所选", true, !busy};
        case kIdEnqueue: return {L"添加到列表", false, !busy};
        case kIdDownload: return {L"下载", false, !busy};
        case kIdLoadSheets: return {L"刷新", false, !busy};
        case kIdOpenSheet: return {L"打开歌单", false, !busy};
        default: return {nullptr, false, true};
    }
}

void DrawOwnerButton(BrowserState* state, const DRAWITEMSTRUCT& item) {
    const ButtonVisual visual = ButtonVisualFor(state, static_cast<int>(item.CtlID));
    if (!visual.text) return;
    const bool hover = state->hot_button == static_cast<int>(item.CtlID);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool enabled = visual.enabled && (item.itemState & ODS_DISABLED) == 0;
    const int button_state = !enabled ? theme::kButtonDisabled
        : (pressed ? theme::kButtonPressed
                   : (hover ? theme::kButtonHot : theme::kButtonNormal));

    FillSolidRect(item.hDC, item.rcItem, kPanel);
    RECT rect = item.rcItem;
    InflateRect(&rect, -1, -1);
    // 走共用主题：导入窗的按钮用的是同一个 PanelButtonSkin，两处外观必然一致。
    theme::PaintButton(item.hDC, rect, visual.text,
                       visual.primary ? state->bold_font : state->ui_font,
                       theme::PanelButtonSkin(visual.primary, button_state));
}

// ------------------------------------------------- 结果列表自绘（行绘制）

std::wstring ListItemText(HWND list, int row, int subitem) {
    wchar_t buffer[512]{};
    LVITEMW item{};
    item.iSubItem = subitem;
    item.pszText = buffer;
    item.cchTextMax = static_cast<int>(std::size(buffer));
    SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
    return buffer;
}

void DrawResultRow(BrowserState* state, const DRAWITEMSTRUCT& item) {
    const int row = static_cast<int>(item.itemID);
    if (row < 0) return;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    HDC dc = item.hDC;

    COLORREF background = kPanel;
    if (selected) background = kRowSelected;
    else if (state->hot_row == row) background = kRowHot;
    else if ((row % 2) != 0) background = kRowAlternate;
    FillSolidRect(dc, item.rcItem, background);

    if (selected) {
        RECT accent = item.rcItem;
        accent.right = accent.left + 3;
        FillSolidRect(dc, accent, kAccent);
    }

    const struct { COLORREF color; HFONT font; UINT format; } style[kColumnCount] = {
        {kSubtleText, state->small_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kText, state->bold_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kSubtleText, state->ui_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kSubtleText, state->small_font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX},
        {kSubtleText, state->small_font,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX},
    };

    LONG x = item.rcItem.left;
    for (int column = 0; column < kColumnCount; ++column) {
        const int width = ListView_GetColumnWidth(state->list, column);
        const RECT cell{x + 6, item.rcItem.top, x + width - 6, item.rcItem.bottom};
        if (cell.right > cell.left) {
            const std::wstring text = ListItemText(state->list, row, column);
            DrawTextIn(dc, cell, text.c_str(), style[column].color,
                       style[column].font, style[column].format);
        }
        x += width;
    }

    const RECT separator{item.rcItem.left, item.rcItem.bottom - 1,
                         item.rcItem.right, item.rcItem.bottom};
    FillSolidRect(dc, separator, kRowSeparator);
}

// 表头用轻量自绘贴近内容面板，避免默认的灰色 3D 立体感。
LRESULT HandleHeaderCustomDraw(BrowserState* state, NMLVCUSTOMDRAW* draw) {
    (void)state;
    switch (draw->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            draw->clrText = kSubtleText;
            draw->clrTextBk = kHeaderFill;
            return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT: {
            const RECT bounds = draw->nmcd.rc;
            if (bounds.right - bounds.left < 8) return CDRF_DODEFAULT;
            const RECT separator{bounds.right - 1, bounds.top + 4, bounds.right,
                                 bounds.bottom - 4};
            FillSolidRect(draw->nmcd.hdc, separator, kHeaderSeparator);
            return CDRF_DODEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

// ------------------------------------------------------------------ 列表

void InitializeColumns(BrowserState* state) {
    static const wchar_t* const kTitles[kColumnCount] = {
        L"平台", L"歌曲名", L"歌手", L"专辑", L"时长"};
    for (int index = 0; index < kColumnCount; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.iSubItem = index;
        column.cx = 80;
        column.pszText = const_cast<wchar_t*>(kTitles[index]);
        ListView_InsertColumn(state->list, index, &column);
    }
}

void FillResultList(BrowserState* state) {
    if (!state || !state->list) return;
    SendMessageW(state->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state->list);
    for (size_t index = 0; index < state->hits.size(); ++index) {
        const OnlineSearchHit& hit = state->hits[index];
        const std::wstring platform = Utf8ToWideString(hit.platform);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(platform.c_str());
        const int row = ListView_InsertItem(state->list, &item);
        if (row < 0) continue;
        const std::wstring title = Utf8ToWideString(hit.title);
        const std::wstring artist = Utf8ToWideString(hit.artist);
        const std::wstring album = Utf8ToWideString(hit.album);
        const std::wstring duration = FormatDuration(hit.duration_ms);
        ListView_SetItemText(state->list, row, 1, const_cast<wchar_t*>(title.c_str()));
        ListView_SetItemText(state->list, row, 2, const_cast<wchar_t*>(artist.c_str()));
        ListView_SetItemText(state->list, row, 3, const_cast<wchar_t*>(album.c_str()));
        ListView_SetItemText(state->list, row, 4,
                             const_cast<wchar_t*>(duration.c_str()));
    }
    state->hot_row = -1;
    SendMessageW(state->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->list, nullptr, TRUE);
}

// ------------------------------------------------------------------ 音源

// 当前选中的音源 platform 名（UTF-8；"全部音源" 返回空串）。
std::string SelectedPlatform(BrowserState* state) {
    if (!state || !state->platform) return {};
    const int selection = static_cast<int>(
        SendMessageW(state->platform, CB_GETCURSEL, 0, 0));
    if (selection <= 0) return {};
    wchar_t buffer[256]{};
    if (SendMessageW(state->platform, CB_GETLBTEXT, selection,
                     reinterpret_cast<LPARAM>(buffer)) == CB_ERR)
        return {};
    return core::WideToUtf8(buffer);
}

void RefreshPlatformCombo(BrowserState* state) {
    if (!state || !state->platform) return;
    const std::wstring previous = WindowText(state->platform);
    SendMessageW(state->platform, CB_RESETCONTENT, 0, 0);
    SendMessageW(state->platform, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"全部音源"));
    if (state->mgr) {
        for (const auto& meta : state->mgr->ListPlugins()) {
            if (!meta.can_search) continue;
            const std::wstring name = Utf8ToWideString(meta.name);
            if (name.empty()) continue;
            SendMessageW(state->platform, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    int selection = 0;
    const int count = static_cast<int>(
        SendMessageW(state->platform, CB_GETCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
        wchar_t buffer[256]{};
        if (SendMessageW(state->platform, CB_GETLBTEXT, index,
                         reinterpret_cast<LPARAM>(buffer)) == CB_ERR)
            continue;
        if (previous == buffer) { selection = index; break; }
    }
    SendMessageW(state->platform, CB_SETCURSEL, selection, 0);
}

// --------------------------------------------------------------- 后台任务

void StartSearch(BrowserState* state) {
    if (IsMusicFreeShuttingDown()) return;
    if (!state || !state->mgr) {
        SetStatus(state, L"没有可用的音源管理器");
        return;
    }
    const std::wstring query = WindowText(state->query);
    if (query.empty()) {
        SetStatus(state, L"请输入搜索关键词");
        SetFocus(state->query);
        return;
    }
    if (state->busy.exchange(true)) {
        SetStatus(state, L"正在处理上一个请求…");
        return;
    }

    const std::string utf8_query = core::WideToUtf8(query);
    const std::string platform = SelectedPlatform(state);
    const bool for_search = state->mode == BrowserMode::kSearch;
    state->hits.clear();
    FillResultList(state);
    SetBusyUi(state, true);
    SetStatus(state, L"正在搜索 " + query + L" …（音源返回较慢，请稍候）");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, utf8_query, platform, for_search] {
        BgThreadGuard guard;
        if (IsMusicFreeShuttingDown()) return;
        auto* payload = new SearchPayload();
        payload->for_search = for_search;
        std::lock_guard<std::mutex> plugin_lock(g_plugin_io);
        try {
            payload->hits = SearchAllSources(utf8_query, platform, 1,
                                             &payload->diagnostics);
        } catch (const std::exception& e) {
            // 单个音源异常不应让整个音乐窗失效，失败即空结果 + 诊断文本。
            payload->diagnostics.errors.push_back(std::string("搜索失败: ") + e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgSearchDone, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void StartResolve(BrowserState* state, OnlineTrackAction action) {
    if (IsMusicFreeShuttingDown()) return;
    if (!state) return;
    const int index = ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
    if (index < 0 || static_cast<size_t>(index) >= state->hits.size()) {
        SetStatus(state, L"请先在上方列表中选择一首歌曲");
        return;
    }
    if (state->busy.exchange(true)) {
        SetStatus(state, L"正在处理上一个请求…");
        return;
    }
    const OnlineSearchHit hit = state->hits[static_cast<size_t>(index)];
    const bool enqueue = action == OnlineTrackAction::enqueue;
    SetBusyUi(state, true);
    SetStatus(state, (enqueue ? L"正在解析后加入列表：" : L"正在解析播放地址：") +
                     Utf8ToWideString(hit.title) + L" …");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, hit, action] {
        BgThreadGuard guard;
        if (IsMusicFreeShuttingDown()) return;
        auto* payload = new ResolvePayload();
        std::lock_guard<std::mutex> plugin_lock(g_plugin_io);
        payload->action = action;
        payload->hit = hit;
        try {
            std::string error;
            const std::string url =
                ResolvePlayUrl(hit.platform, hit.id, &error, &hit.raw);
            payload->url = core::Utf8ToWide(url);
            payload->error = core::Utf8ToWide(error);
        } catch (const std::exception& e) {
            payload->error = core::Utf8ToWide(e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgResolveDone, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// ------------------------------------------------------------ 下载 / 右键菜单
//
// 文件名的清洗与默认目录规则（SanitizeFileName / GuessExtension /
// DefaultDownloadName / DefaultDownloadDirectory）已经提到 musicfree_app 里，
// 播放列表的「下载歌曲」用同一套，两边不会再各写一份。

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (block) {
        if (void* target = GlobalLock(block)) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(block);
            ok = SetClipboardData(CF_UNICODETEXT, block) != nullptr;
        }
        if (!ok) GlobalFree(block);
    }
    CloseClipboard();
    return ok;
}

int SelectedRow(BrowserState* state) {
    if (!state || !state->list) return -1;
    const int row = ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
    if (row < 0 || static_cast<size_t>(row) >= state->hits.size()) return -1;
    return row;
}

// 下载的第一步：解析播放地址。地址解析出来后才弹「另存为」，因为文件扩展名
// 要从地址推断（音源返回的 url 通常是 .mp3/.flac/.m4a）。
void StartDownload(BrowserState* state) {
    if (!state) return;
    const int index = SelectedRow(state);
    if (index < 0) {
        SetStatus(state, L"请先在上方列表中选择一首歌曲");
        return;
    }
    if (state->busy.exchange(true)) {
        SetStatus(state, L"正在处理上一个请求…");
        return;
    }
    const OnlineSearchHit hit = state->hits[static_cast<size_t>(index)];
    SetBusyUi(state, true);
    SetStatus(state, L"正在解析下载地址：" + Utf8ToWideString(hit.title) + L" …");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, hit] {
        BgThreadGuard guard;
        if (IsMusicFreeShuttingDown()) return;
        auto* payload = new ResolvePayload();
        std::lock_guard<std::mutex> plugin_lock(g_plugin_io);
        payload->action = OnlineTrackAction::play;  // 此路径只做解析，不使用该字段
        payload->hit = hit;
        try {
            std::string error;
            const std::string url =
                ResolvePlayUrl(hit.platform, hit.id, &error, &hit.raw);
            payload->url = core::Utf8ToWide(url);
            payload->error = core::Utf8ToWide(error);
        } catch (const std::exception& e) {
            payload->error = core::Utf8ToWide(e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgDownloadReady, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 下载的第二步：用户确认落盘位置后真正拉取字节。
void BeginDownloadToFile(BrowserState* state, const OnlineSearchHit& hit,
                         const std::wstring& url, const std::wstring& path) {
    SetStatus(state, L"正在下载：" + Utf8ToWideString(hit.title) + L" …");
    const std::wstring wide_url = url;
    const std::wstring target = path;
    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, hit, wide_url, target] {
        BgThreadGuard guard;
        if (IsMusicFreeShuttingDown()) return;
        auto* payload = new DownloadPayload();
        payload->hit = hit;
        payload->path = target;
        try {
            std::string error;
            payload->ok = DownloadTrackFile(wide_url, std::filesystem::path(target),
                                            &error);
            if (!payload->ok) payload->error = core::Utf8ToWide(error);
            else {
                std::error_code size_error;
                const auto size = std::filesystem::file_size(target, size_error);
                if (!size_error) payload->bytes = static_cast<std::int64_t>(size);
            }
        } catch (const std::exception& e) {
            payload->error = core::Utf8ToWide(e.what());
        } catch (...) {
        }
        auto state = weak.lock();
        if (!state) { delete payload; return; }
        if (!PostMessageW(state->hwnd, kMsgDownloadDone, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 结果列表右键菜单。列表本身是自绘的，菜单用系统弹出菜单（文件对话框同理）——
// 这两处是系统级 UI，逐项自绘的收益不如保持一致。
void ShowResultContextMenu(BrowserState* state, POINT screen) {
    if (!state || !state->list) return;
    POINT client = screen;
    ScreenToClient(state->list, &client);
    LVHITTESTINFO info{};
    info.pt = client;
    const int row = ListView_HitTest(state->list, &info);
    if (row < 0) return;  // 空白处右键不弹菜单

    ListView_SetItemState(state->list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(state->list, row,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    SetFocus(state->list);
    if (static_cast<size_t>(row) >= state->hits.size()) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuPlay, L"播放");
    AppendMenuW(menu, MF_STRING, kMenuEnqueue, L"添加到列表");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuDownload, L"下载到…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuCopyTitle, L"复制歌曲名");
    if (state->busy.load()) {
        for (const UINT command : {static_cast<UINT>(kMenuPlay),
                                   static_cast<UINT>(kMenuEnqueue),
                                   static_cast<UINT>(kMenuDownload)})
            EnableMenuItem(menu, command, MF_BYCOMMAND | MF_GRAYED);
    }
    const int command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screen.x, screen.y, 0, state->hwnd, nullptr);
    DestroyMenu(menu);
    switch (command) {
        case kMenuPlay: StartResolve(state, OnlineTrackAction::play); break;
        case kMenuEnqueue: StartResolve(state, OnlineTrackAction::enqueue); break;
        case kMenuDownload: StartDownload(state); break;
        case kMenuCopyTitle: {
            const std::wstring text = ListItemText(state->list, row, 1);
            SetStatus(state, CopyTextToClipboard(state->hwnd, text)
                ? L"已复制歌曲名：" + text
                : L"复制到剪贴板失败");
            break;
        }
        default: break;
    }
}

// 切换音源（框体上的 ◀ / ▶）。只有关键词非空时才顺带重搜，避免空查询报错。
void StepPlatform(BrowserState* state, int delta) {
    if (!state || !state->platform) return;
    const int count = static_cast<int>(
        SendMessageW(state->platform, CB_GETCOUNT, 0, 0));
    if (count <= 1) {
        SetStatus(state, L"可用音源只有 0~1 个，先用「导入…」添加更多音源");
        return;
    }
    if (state->busy.load()) {
        SetStatus(state, L"正在搜索/解析，请稍候再切换音源");
        return;
    }
    int selection = static_cast<int>(
        SendMessageW(state->platform, CB_GETCURSEL, 0, 0));
    if (selection < 0) selection = 0;
    selection = ((selection + delta) % count + count) % count;
    SendMessageW(state->platform, CB_SETCURSEL, selection, 0);
    const std::wstring name = WindowText(state->platform);
    if (WindowText(state->query).empty()) {
        SetStatus(state, L"已切换到音源：" + name);
        return;
    }
    StartSearch(state);
}

// ------------------------------------------------------------- 框体交互

int FrameHitTest(const BrowserState* state, POINT point) {
    const FrameLayout& layout = state->layout;
    if (HitTest(layout.close, point)) return kFrameHitClose;
    if (HitTest(layout.prev_source, point)) return kFrameHitPrevSource;
    if (HitTest(layout.next_source, point)) return kFrameHitNextSource;
    if (HitTest(layout.refresh, point)) return kFrameHitRefresh;
    if (HitTest(layout.check_box, point) || HitTest(layout.check_label, point))
        return kFrameHitTopMost;
    return kFrameHitNone;
}

// 鼠标落在窗口边框上时返回对应的 HT*，落在别处返回 HTCLIENT。
//
// 窗口是 WS_POPUP，没有系统边框可以拖，缩放完全靠 WM_NCHITTEST 自己报。注意
// **框体按钮必须先判**：默认皮肤里关闭按钮右边缘离窗口右边只有 6 像素，缩放带
// 会把它抢走，点关闭就变成拉伸了。
//
// 真正的几何判定在 frame::ResizeHitCode（纯算术、可被自检直接调用），这里只负责
// 取当前客户区尺寸。见该函数上方关于「不要按最小尺寸屏蔽某条边」的说明。
int ResizeHitTest(BrowserState* state, POINT point) {
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const SIZE size{client.right - client.left, client.bottom - client.top};
    return frame::ResizeHitCode(size, ScaledResizeBorder(state->hwnd), point);
}

// ------------------------------------------------------- 缩放带自检（诊断）
//
// 缩放带失效时**没有任何视觉提示** —— 只是鼠标到了边上不变形，靠肉眼很难定位，
// 以前只能反复手动去拖。这里把「鼠标落在四个边 / 四个角 / 内容区中心时分别拿到
// 哪个 HT*」直接写进 trace。
//
// 判定顺序和 WM_NCHITTEST 保持一致：**框体按钮优先于缩放带**（关闭按钮右边缘
// 距窗口右边只有 6px，两者会争抢同一条带）。被按钮抢走的位置标 (button)。
void TraceResizeBands(BrowserState* state) {
    if (!state || !state->hwnd || !IsWindow(state->hwnd)) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    const LONG inset = ScaledResizeBorder(state->hwnd) / 2;  // 探针落在带**内**，不是压在边界线上
    struct Probe {
        const char* name;
        POINT point;
    };
    const Probe probes[] = {
        {"left", {inset, height / 2}},
        {"right", {width - 1 - inset, height / 2}},
        {"top", {width / 2, inset}},
        {"bottom", {width / 2, height - 1 - inset}},
        {"top_left", {inset, inset}},
        {"top_right", {width - 1 - inset, inset}},
        {"bottom_left", {inset, height - 1 - inset}},
        {"bottom_right", {width - 1 - inset, height - 1 - inset}},
        {"content", {width / 2, height / 2}},
    };
    std::string detail = "client=" + std::to_string(width) + "x" +
                         std::to_string(height) +
                         " band=" + std::to_string(ScaledResizeBorder(state->hwnd));
    for (const Probe& probe : probes) {
        const int frame_hit = FrameHitTest(state, probe.point);
        const int code = frame_hit != kFrameHitNone
                             ? HTCLIENT
                             : ResizeHitTest(state, probe.point);
        detail += std::string("\n") + probe.name + "=" + frame::HitCodeName(code) +
                  (frame_hit != kFrameHitNone ? " (button)" : "");
    }
    TraceDiagnostic("BROWSER_RESIZE_HIT", detail);
}

void SetTopMost(BrowserState* state, bool top_most) {
    if (!state || !state->hwnd) return;
    state->top_most = top_most;
    SetWindowPos(state->hwnd, top_most ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (state->host.store_top_most) state->host.store_top_most(top_most);
    InvalidateFrame(state);
}

void InvokeFrameHit(BrowserState* state, int hit) {
    switch (hit) {
        case kFrameHitClose: PostMessageW(state->hwnd, WM_CLOSE, 0, 0); break;
        case kFrameHitPrevSource: StepPlatform(state, -1); break;
        case kFrameHitNextSource: StepPlatform(state, 1); break;
        case kFrameHitRefresh:
            // WebView2 页面在用时：刷新 = 重新加载页面。reload 完成后宿主的
            // NavigationCompleted 会重发 bridge.ready，页面握手重新跑 —— 既能
            // 拉新数据，也能把卡在「正在连接宿主…」的页面拉回来。此前这里
            // 直接走旧管线函数（StartLoadVideo/StartLoadDiscover），对页面
            // 完全无效，用户点刷新像没反应。
            if (state->webview && !state->webview_failed &&
                state->webview->Ready()) {
                state->webview->Reload();
                break;
            }
            // 宿主未就绪/已失败（旧 Win32 控件模式）：按页签走旧管线。
            if (state->mode == BrowserMode::kVideo) {
                if (state->video_busy.load()) SetVideoStatus(state, L"视频源正在加载，请稍候");
                else StartLoadVideo(state, true);
            } else if (state->mode == BrowserMode::kSearch) {
                if (state->busy.load()) SetStatus(state, L"正在搜索/解析，请稍候");
                else StartSearch(state);
            } else {
                if (state->busy.load()) SetStatus(state, L"正在加载榜单，请稍候");
                else StartLoadDiscover(state, true);
            }
            break;
        case kFrameHitTopMost: SetTopMost(state, !state->top_most); break;
        default: break;
    }
}

// 拖动。宿主在时由主窗口统一处理（带十像素吸附与工作区约束）；宿主缺席时退化成
// 自带的整窗拖动 + 工作区夹取，至少保证窗口还能移动。
void BeginDrag(BrowserState* state, POINT point) {
    state->dragging = true;
    if (state->host.begin_drag) {
        state->host.begin_drag(state->hwnd, point);
        return;
    }
    POINT screen = point;
    ClientToScreen(state->hwnd, &screen);
    state->drag_anchor = screen;
    GetWindowRect(state->hwnd, &state->drag_start_bounds);
    SetCapture(state->hwnd);
}

void ContinueDrag(BrowserState* state, POINT point) {
    if (state->host.continue_drag) {
        state->host.continue_drag(state->hwnd, point);
        return;
    }
    if (!state->dragging || GetCapture() != state->hwnd) return;
    POINT screen = point;
    ClientToScreen(state->hwnd, &screen);
    RECT bounds = state->drag_start_bounds;
    OffsetRect(&bounds, screen.x - state->drag_anchor.x,
               screen.y - state->drag_anchor.y);
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &info)) {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        bounds.left = std::clamp(bounds.left, info.rcWork.left,
                                 std::max(info.rcWork.left,
                                          info.rcWork.right - width));
        bounds.top = std::clamp(bounds.top, info.rcWork.top,
                                std::max(info.rcWork.top,
                                         info.rcWork.bottom - height));
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }
    SetWindowPos(state->hwnd, nullptr, bounds.left, bounds.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EndDrag(BrowserState* state) {
    if (!state || !state->hwnd) return;
    // 先落 dragging 再交给宿主：host.end_drag 里会 ReleaseCapture，而释放捕获
    // 会同步送回 WM_CAPTURECHANGED；那时若 dragging 还是 true，就会二次进入
    // 这里、把同一份窗口位置存两遍。
    const bool was_dragging = state->dragging;
    state->dragging = false;
    if (state->host.end_drag) {
        state->host.end_drag(state->hwnd);
    } else if (was_dragging) {
        if (GetCapture() == state->hwnd) ReleaseCapture();
    }
    if (state->host.store_bounds) {
        RECT bounds{};
        GetWindowRect(state->hwnd, &bounds);
        if (bounds.right > bounds.left && bounds.bottom > bounds.top)
            state->host.store_bounds(bounds);
    }
}

// ------------------------------------------------------- 子控件光标策略
// ⚠ 以前这里只有一行「非 ListView 就 IDC_HAND」，于是鼠标进音乐窗几乎到处
// 是手型：搜索输入框、海报墙的空白与分区标题、状态条、分类行/图标栏里按钮
// 之间的空隙全是手，用户完全看不出哪里真的能点（反馈「都是手的形状，应该
// 根据里面的内容改变」）。
// 现在按「控件类型 + 局部命中」分派：
//   · 搜索框（EDIT）            → IBeam（可编辑，系统默认就是这个）
//   · 结果/选集列表（ListView） → 箭头（行是选中不是链接）
//   · 海报墙（ListBox）：卡片/页签 → 手；标题行、空白、加载占位 → 箭头
//   · 顶部分类行 / 图标功能栏   → 命中按钮才手，行内空隙箭头
//   · 其余按钮（搜索/导入/播放/加入/下载…）→ 手（禁用时箭头）
HCURSOR CursorForChild(ChildProbe* probe, BrowserState* state, HWND control) {
    const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    const HCURSOR hand = LoadCursorW(nullptr, IDC_HAND);
    const bool enabled = IsWindowEnabled(control) != FALSE;
    switch (probe->id) {
        case kIdQuery:
            return enabled ? LoadCursorW(nullptr, IDC_IBEAM) : arrow;
        case kIdTab: {
            // 页签本身可点（切换音乐 / 视频 / 搜索）：命中页签才手型。
            // 同进程内 TCM_HITTEST 可以直接取真实命中，不必自己算页签宽度。
            if (!state || !enabled) return arrow;
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(control, &pt);
            TCHITTESTINFO info{};
            info.pt = pt;
            const int idx = static_cast<int>(SendMessageW(
                control, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&info)));
            return idx >= 0 ? hand : arrow;
        }
        case kIdTagCombo:   // 隐藏保留控件；下拉框自身光标由系统处理
        case kIdList:       // 结果 / 选集列表
            return arrow;
        case kIdSheetList: {
            if (!state || !enabled) return arrow;
            int g = -1, i = -1;
            if (IsVideoMode(state)) {
                if (HitVideoCard(state, &g, &i)) return hand;
                // 遗留的分组标题页签（音乐页那套）在视频页已不再绘制，
                // 这里留着是为了与点击路径保持一致。
                if (HitVideoChip(state, &g, &i)) return hand;
                return arrow;
            }
            return HitDiscoverCard(state, &g, &i) ? hand : arrow;
        }
        case kIdVideoNav: {
            if (!state || !enabled) return arrow;
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(control, &pt);
            RECT rc{};
            GetClientRect(control, &rc);
            const HDC dc = GetDC(control);
            const int saved = SaveDC(dc);
            const auto tabs = LayoutVideoNavTabs(dc, state, rc);
            RestoreDC(dc, saved);
            ReleaseDC(control, dc);
            for (const auto& tab : tabs)
                if (PtInRect(&tab.rect, pt)) return hand;
            return arrow;
        }
        case kIdVideoIcons: {
            if (!state || !enabled) return arrow;
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(control, &pt);
            RECT rc{};
            GetClientRect(control, &rc);
            const HDC dc = GetDC(control);
            const int saved = SaveDC(dc);
            const auto btns = LayoutVideoIconBtns(dc, state, rc);
            RestoreDC(dc, saved);
            ReleaseDC(control, dc);
            for (const auto& btn : btns)
                if (PtInRect(&btn.rect, pt)) return hand;
            return arrow;
        }
        default:
            return enabled ? hand : arrow;
    }
}

// 只有这些自绘控件需要 hover 高亮。海报墙 / 搜索框 / 状态条 / 组合框不该在
// 每次鼠标移动时被 InvalidateRect（海报墙整幅重绘代价很高）。
bool IsHoverButton(int id) {
    switch (id) {
        case kIdSearch:
        case kIdImport:
        case kIdPlay:
        case kIdEnqueue:
        case kIdDownload:
            return true;
        default:
            return false;
    }
}

// ------------------------------------------------------- 子控件子类化

LRESULT CALLBACK ChildProc(HWND control, UINT message, WPARAM wparam,
                           LPARAM lparam) {
    auto* probe = reinterpret_cast<ChildProbe*>(
        GetWindowLongPtrW(control, GWLP_USERDATA));
    if (!probe || !probe->base)
        return DefWindowProcW(control, message, wparam, lparam);
    BrowserState* state = StateOf(probe);

    switch (message) {
        case WM_MOUSEMOVE: {
            if (state) {
                if (probe->id == kIdVideoIcons) {
                    // 图标功能栏：逐按钮命中，只有当前悬停的按钮高亮。
                    POINT pt{};
                    GetCursorPos(&pt);
                    ScreenToClient(control, &pt);
                    RECT rc{};
                    GetClientRect(control, &rc);
                    HDC dc = GetDC(control);
                    const auto btns = LayoutVideoIconBtns(dc, state, rc);
                    ReleaseDC(control, dc);
                    int hit = -1;
                    for (size_t i = 0; i < btns.size(); ++i)
                        if (PtInRect(&btns[i].rect, pt)) { hit = static_cast<int>(i); break; }
                    if (hit != state->hot_video_icon) {
                        state->hot_video_icon = hit;
                        InvalidateRect(control, nullptr, FALSE);
                    }
                } else if (probe->is_list) {
                    LVHITTESTINFO info{};
                    info.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    const int row = ListView_HitTest(control, &info);
                    const int hot = row >= 0 ? row : -1;
                    if (hot != state->hot_row) {
                        state->hot_row = hot;
                        InvalidateRect(control, nullptr, FALSE);
                    }
                } else if (IsHoverButton(probe->id) &&
                           state->hot_button != probe->id) {
                    // 只有按钮需要 hover 高亮。以前对海报墙 / 搜索框 / 状态条
                    // 也走这条分支，每次鼠标移动都整幅 InvalidateRect 海报墙
                    // （双缓冲全墙重绘），既卡又毫无视觉收益。
                    state->hot_button = probe->id;
                    InvalidateRect(control, nullptr, FALSE);
                }
            }
            if (!probe->tracking_leave) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
                if (TrackMouseEvent(&track)) probe->tracking_leave = true;
            }
            break;
        }
        case WM_MOUSELEAVE:
            probe->tracking_leave = false;
            if (state) {
                if (probe->id == kIdVideoIcons) {
                    if (state->hot_video_icon != -1) {
                        state->hot_video_icon = -1;
                        InvalidateRect(control, nullptr, FALSE);
                    }
                } else if (probe->is_list) {
                    if (state->hot_row != -1) {
                        state->hot_row = -1;
                        InvalidateRect(control, nullptr, FALSE);
                    }
                } else if (state->hot_button == probe->id) {
                    state->hot_button = 0;
                    InvalidateRect(control, nullptr, FALSE);
                }
            }
            break;
        case WM_SETCURSOR:
            // 光标按控件类型与局部命中分派（见 CursorForChild）：不再一律手型。
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(CursorForChild(probe, state, control));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            // 搜索框（或视频源地址栏）回车即提交，省得每次都去点按钮。
            if (probe->id == kIdQuery && wparam == VK_RETURN && state &&
                state->hwnd) {
                PostMessageW(state->hwnd, WM_COMMAND,
                             MAKEWPARAM(kIdSearch, BN_CLICKED), 0);
                return 0;
            }
            break;
        case WM_CONTEXTMENU:
            // ListView 不会把右键上下文菜单消息转发给父窗口，
            // 这里显式转给 BrowserProc，由它统一弹「播放/下载」菜单。
            if (state && state->hwnd) {
                LRESULT handled = SendMessageW(state->hwnd, WM_CONTEXTMENU,
                                               wparam, lparam);
                if (handled) return handled;
            }
            break;
        case WM_ERASEBKGND:
            // 封面卡片列表由下面的双缓冲 WM_PAINT 整幅覆盖。系统默认会先擦一遍
            // 纯色背景再逐行自绘，滚动时这一下就是肉眼可见的闪烁。
            if (probe->id == kIdSheetList) return 1;
            // 影视仓外壳条整幅自绘，跳过系统擦背景。
            if (probe->id == kIdVideoNav || probe->id == kIdVideoIcons) return 1;
            break;
        case WM_PAINT:
            if (probe->id == kIdVideoNav && state) {
                PaintVideoNavbar(state, control);
                return 0;
            }
            if (probe->id == kIdVideoIcons && state) {
                PaintVideoIconbar(state, control);
                return 0;
            }
            if (probe->id == kIdSheetList && state) {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(control, &paint);
                if (!dc) return 0;
                RECT rc{};
                GetClientRect(control, &rc);
                const int width = rc.right - rc.left;
                const int height = rc.bottom - rc.top;
                HDC buffer = (width > 0 && height > 0)
                    ? CreateCompatibleDC(dc) : nullptr;
                HBITMAP canvas = buffer
                    ? CreateCompatibleBitmap(dc, width, height) : nullptr;
                if (buffer && canvas) {
                    const HGDIOBJ old = SelectObject(buffer, canvas);
                    // 视频页是深色海报墙：网格未铺满的剩余区域也用深色底，
                    // 不然下面会露出一条白色（浅色主题的 panel 底色）。
                    FillRect(buffer, &rc,
                             IsVideoMode(state) && state->video_panel_brush
                                 ? state->video_panel_brush
                                 : (state->panel_brush
                                        ? state->panel_brush
                                        : reinterpret_cast<HBRUSH>(
                                            COLOR_WINDOW + 1)));
                    const int count = static_cast<int>(
                        SendMessageW(control, LB_GETCOUNT, 0, 0));
                    int first = static_cast<int>(
                        SendMessageW(control, LB_GETTOPINDEX, 0, 0));
                    if (first < 0) first = 0;
                    for (int i = first; i < count; ++i) {
                        RECT row{};
                        if (SendMessageW(control, LB_GETITEMRECT,
                                         static_cast<WPARAM>(i),
                                         reinterpret_cast<LPARAM>(&row)) == LB_ERR)
                            break;
                        if (row.top >= height) break;
                        DRAWITEMSTRUCT item{};
                        item.CtlType = ODT_LISTBOX;
                        item.CtlID = kIdSheetList;
                        item.itemID = static_cast<UINT>(i);
                        item.itemAction = ODA_DRAWENTIRE;
                        item.hwndItem = control;
                        item.hDC = buffer;
                        item.rcItem = row;
                        // 走父窗口原有的自绘分支，绘制目标换成内存 DC。
                        SendMessageW(GetParent(control), WM_DRAWITEM,
                                     MAKEWPARAM(kIdSheetList, 0),
                                     reinterpret_cast<LPARAM>(&item));
                    }
                    BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
                    SelectObject(buffer, old);
                }
                if (canvas) DeleteObject(canvas);
                if (buffer) DeleteDC(buffer);
                EndPaint(control, &paint);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            // 上下文由 BrowserState::probes 持有；这里只是摘掉反向引用。
            SetWindowLongPtrW(control, GWLP_USERDATA, 0);
            break;
        case WM_LBUTTONDOWN: {
            if (!state) break;
            if (probe->id == kIdVideoNav) {
                HandleVideoNavClick(state, control);
                return 0;
            }
            if (probe->id == kIdVideoIcons) {
                HandleVideoIconClick(state, control);
                return 0;
            }
            break;
        }
        default:
            break;
    }
    return CallWindowProcW(probe->base, control, message, wparam, lparam);
}

void TrackControl(BrowserState* state, HWND control, int id, bool is_list) {
    if (!control) return;
    auto probe = std::make_unique<ChildProbe>();
    probe->state = state;
    probe->id = id;
    probe->is_list = is_list;
    probe->base = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ChildProc)));
    SetWindowLongPtrW(control, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(probe.get()));
    state->probes.push_back(std::move(probe));
}

// ---------------------------------------------------------------- 发现页（单页聚合 + 异步加载）

// 后台线程 → UI 线程：一个音源的一批榜单 / 推荐卡片。每完成一个音源就投递
// 一次，界面逐批追加；慢音源 / 失效音源既不会拖住窗口，也不会挡住其它音源。
struct DiscoverPayload {
    std::string platform;
    std::vector<MusicSheet> sheets;
    std::string error;
    bool finished = false;
    // 仅 finished 批次携带：本轮实际存在的音源，用来剪掉缓存里已删除的音源。
    std::vector<std::string> platforms;
};

// 搜索页的音源下拉框只列可搜索音源。后台插件挂载完成后会再刷新一次，
// 这样首开窗口时下拉框不会因为「后台还没挂载完」而只剩一两个音源。
void RefreshSourceComboForMode(BrowserState* state) {
    if (!state->platform) return;
    const std::wstring previous = WindowText(state->platform);
    SendMessageW(state->platform, CB_RESETCONTENT, 0, 0);
    SendMessageW(state->platform, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"全部音源"));
    std::vector<std::string> names;
    if (state->mgr) {
        for (const auto& m : state->mgr->ListPlugins()) {
            if (!m.can_search || m.name.empty()) continue;
            if (std::find(names.begin(), names.end(), m.name) == names.end())
                names.push_back(m.name);
        }
    }
    // 缓存命中时窗口秒开，但后台插件还在逐个挂载（约 14s），实时列表此刻
    // 只有 0~1 个音源。先用缓存里的音源名补齐，等 kMsgSourcesReady 到达后
    // 再换成权威列表；这样热启动后立刻切到「搜索」页也是完整的下拉框。
    if (!state->sources_ready) {
        for (const auto& g : state->groups) {
            if (g.platform.empty()) continue;
            if (std::find(names.begin(), names.end(), g.platform) == names.end())
                names.push_back(g.platform);
        }
    }
    for (const auto& name : names) {
        const std::wstring w = Utf8ToWideString(name);
        if (!w.empty())
            SendMessageW(state->platform, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(w.c_str()));
    }
    int sel = 0;
    const int count = static_cast<int>(
        SendMessageW(state->platform, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        wchar_t b[256]{};
        if (SendMessageW(state->platform, CB_GETLBTEXT, i,
                         reinterpret_cast<LPARAM>(b)) != CB_ERR &&
            previous == b) { sel = i; break; }
    }
    SendMessageW(state->platform, CB_SETCURSEL, sel, 0);
}

// 推荐标签已由发现页自动聚合，旧的下拉框不再使用（保留空实现避免残留引用）。
void RefreshTagCombo(BrowserState*) {}

// groups → 绘制行：每个音源一条分组标题 + 若干张卡片行。
void RebuildDiscoverRows(BrowserState* state) {
    state->discover_rows.clear();
    const int cols = std::max(1, state->discover_cols);
    // 后台音源仍在挂载时先铺一行骨架卡片，窗口一打开就有卡片网格的形态；
    // 第一批真实数据到达后整行被替换成带封面/标题的卡片。
    if (state->groups.empty() && state->discover_placeholder) {
        BrowserState::DiscoverRow skeleton;
        skeleton.header = true;
        skeleton.group = -1;
        skeleton.count = 1;
        state->discover_rows.push_back(skeleton);
        return;
    }
    for (int g = 0; g < static_cast<int>(state->groups.size()); ++g) {
        const auto& group = state->groups[g];
        if (group.sheets.empty()) continue;
        BrowserState::DiscoverRow header;
        header.header = true;
        header.group = g;
        state->discover_rows.push_back(header);
        const int total = static_cast<int>(group.sheets.size());
        for (int first = 0; first < total; first += cols) {
            BrowserState::DiscoverRow cards;
            cards.group = g;
            cards.first = first;
            cards.count = std::min(cols, total - first);
            state->discover_rows.push_back(cards);
        }
    }
}

// 封面不再是「绘制到哪张才下载哪张」的懒加载：卡片数据一到位（读缓存、
// 后台逐音源投递、强制刷新）就把本页所有封面一次性投递给 ArtworkCache。
// ArtworkCache 内部限流并发，这里只是入队，不阻塞 UI 线程。
void PrefetchAllArtwork(BrowserState* state) {
    if (!state || !state->artwork) return;
    HWND target = state->hwnd;
    std::weak_ptr<BrowserState> weak = g_browser;
    for (const auto& group : state->groups) {
        for (const auto& sheet : group.sheets) {
            const std::string& url = sheet.artwork;
            if (url.empty() ||
                state->art_requested.find(url) != state->art_requested.end())
                continue;
            state->art_requested.insert(url);
            state->artwork->Request(url, kCardThumb,
                [weak, target](const std::string& url) {
                    if (auto sp = weak.lock())
                        if (target && IsWindow(target))
                            // 只在真的拿到图时才通知重绘；失败静默（补抓轮处理）。
                            if (sp->artwork && sp->artwork->Peek(url))
                                PostMessageW(target, kMsgArtworkReady, 0, 0);
                });
        }
    }
}

void FillDiscoverList(BrowserState* state) {
    if (!state->sheet_list) return;
    if (IsVideoMode(state)) { FillVideoList(state); return; }
    state->discover_cols_applied = state->discover_cols;  // 行按当前列数铺，同步基准
    RebuildDiscoverRows(state);
    SendMessageW(state->sheet_list, WM_SETREDRAW, FALSE, 0);
    const int top_index = static_cast<int>(
        SendMessageW(state->sheet_list, LB_GETTOPINDEX, 0, 0));
    ListBox_ResetContent(state->sheet_list);
    for (size_t i = 0; i < state->discover_rows.size(); ++i)
        ListBox_AddString(state->sheet_list, L"");
    // LBS_OWNERDRAWVARIABLE：逐行指定高度（分组行矮、卡片行高）。
    for (size_t i = 0; i < state->discover_rows.size(); ++i) {
        const auto& row = state->discover_rows[i];
        const int h = (row.header && row.group < 0)
                          ? kCardRowHeight
                          : (row.header ? kGroupRowHeight : kCardRowHeight);
        SendMessageW(state->sheet_list, LB_SETITEMHEIGHT,
                     static_cast<WPARAM>(i), static_cast<LPARAM>(h));
    }
    SendMessageW(state->sheet_list, LB_SETTOPINDEX, top_index, 0);
    SendMessageW(state->sheet_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->sheet_list, nullptr, FALSE);
    PrefetchAllArtwork(state);
}

// 封面卡片自绘：分组标题行 / 一行多张竖向封面卡片（封面 + 标题 + 来源）。
void DrawDiscoverRow(BrowserState* state, const DRAWITEMSTRUCT& item) {
    if (IsVideoMode(state)) { DrawDiscoverRowVideo(state, item); return; }
    const int row = static_cast<int>(item.itemID);
    if (row < 0 || row >= static_cast<int>(state->discover_rows.size())) return;
    const BrowserState::DiscoverRow& r = state->discover_rows[row];
    HDC dc = item.hDC;
    RECT rc = item.rcItem;
    FillSolidRect(dc, rc, kPanel);

    // 骨架卡片：音源还在后台逐个挂载时，先把「发现」页的卡片网格画出来。
    if (r.header && r.group < 0) {
        const int thumb_w = std::min(kCardThumb, kCardWidth - 24);
        for (int c = 0; c < state->discover_cols; ++c) {
            RECT card = {rc.left + c * kCardWidth + 4, rc.top + 3,
                         rc.left + (c + 1) * kCardWidth - 4, rc.bottom - 3};
            FillSolidRect(dc, card, kPanelSoft);
            DrawRoundedBox(dc, card, CLR_INVALID, kOutline, 6);
            const RECT thumb = {
                card.left + (card.right - card.left - thumb_w) / 2, card.top + 6,
                card.left + (card.right - card.left + thumb_w) / 2,
                card.top + 6 + thumb_w};
            DrawRoundedBox(dc, thumb, kPanel, kOutline, 4);
            DrawTextIn(dc,
                       {card.left + 5, thumb.bottom + 6, card.right - 5, card.bottom - 6},
                       L"正在准备…", kSubtleText, state->small_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        return;
    }
    if (r.group < 0 || r.group >= static_cast<int>(state->groups.size())) return;

    if (r.header) {
        const auto& group = state->groups[r.group];
        FillSolidRect(dc, {rc.left + 4, rc.top + 6, rc.left + 7, rc.bottom - 6},
                      kText);
        std::wstring title = Utf8ToWideString(group.platform);
        if (title.empty()) title = L"未命名音源";
        title += L"   ·   " + std::to_wstring(group.sheets.size()) + L" 个榜单 / 歌单";
        DrawTextIn(dc, {rc.left + 14, rc.top, rc.right - 6, rc.bottom},
                   title.c_str(), kText, state->bold_font,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }

    const auto& sheets = state->groups[r.group].sheets;
    for (int c = 0; c < r.count; ++c) {
        const int index = r.first + c;
        if (index >= static_cast<int>(sheets.size())) break;
        const MusicSheet& s = sheets[index];
        RECT card = {rc.left + c * kCardWidth + 4, rc.top + 3,
                     rc.left + (c + 1) * kCardWidth - 4, rc.bottom - 3};
        const bool selected = state->selected_group == r.group &&
                              state->selected_sheet == index;
        FillSolidRect(dc, card, selected ? kRowSelected : kPanelSoft);
        DrawRoundedBox(dc, card, CLR_INVALID, selected ? kText : kOutline, 6);

        const int thumb_w = std::min(kCardThumb,
                                     static_cast<int>(card.right - card.left) - 12);
        const RECT thumb = {
            card.left + (card.right - card.left - thumb_w) / 2, card.top + 6,
            card.left + (card.right - card.left + thumb_w) / 2,
            card.top + 6 + thumb_w};
        HBITMAP bmp = state->artwork ? state->artwork->Peek(s.artwork) : nullptr;
        if (bmp) {
            BITMAP bm{};
            GetObjectW(bmp, sizeof(bm), &bm);
            HDC src = CreateCompatibleDC(dc);
            const HGDIOBJ old = SelectObject(src, bmp);
            StretchBlt(dc, thumb.left, thumb.top, thumb.right - thumb.left,
                       thumb.bottom - thumb.top, src, 0, 0, bm.bmWidth,
                       bm.bmHeight, SRCCOPY);
            SelectObject(src, old);
            DeleteDC(src);
            DrawRoundedBox(dc, thumb, CLR_INVALID, kOutline, 4);
        } else {
            // 封面未就绪（仍在下载 / URL 为空 / WebP 等格式解码失败）时画一张
            // 占位封面：按标题生成稳定配色 + 首字，避免出现一排空白方框。
            unsigned int hash = 2166136261u;
            for (unsigned char c : s.title) { hash ^= c; hash *= 16777619u; }
            static const COLORREF kPlaceholderColors[] = {
                RGB(0x6F, 0x62, 0xB6), RGB(0xC0, 0x64, 0x82),
                RGB(0x3F, 0x81, 0xB0), RGB(0x4E, 0x96, 0x7C),
                RGB(0xB8, 0x7C, 0x43), RGB(0x7A, 0x64, 0xA8),
                RGB(0x35, 0x80, 0x96), RGB(0xA0, 0x5E, 0x54),
            };
            const COLORREF bg = kPlaceholderColors[
                hash % (sizeof(kPlaceholderColors) / sizeof(kPlaceholderColors[0]))];
            DrawRoundedBox(dc, thumb, bg, kOutline, 4);
            const std::wstring wtitle = Utf8ToWideString(s.title);
            const std::wstring initial =
                wtitle.empty() ? L"\u266A" : wtitle.substr(0, 1);
            DrawTextIn(dc, thumb, initial.c_str(), RGB(255, 255, 255),
                       state->bold_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            // 封面请求已在 FillDiscoverList -> PrefetchAllArtwork 里全量发起，
            // 这里只负责在下载完成前画占位图，绘制路径不再触发任何网络请求。
        }

        // 标题与来源各占一整行：矩形高度必须不小于字体行高（ui_font -12 约
        // 16px、small_font -11 约 15px）。旧实现标题只有 8px 高，DT_VCENTER
        // 会把中文字形的上下笔画裁掉，看起来就是「文本显示不全」。
        const int title_top = thumb.bottom + 3;
        const int title_bottom = card.bottom - 23;
        DrawTextIn(dc,
                   {card.left + 5, title_top, card.right - 5, title_bottom},
                   Utf8ToWideString(s.title).c_str(), kText, state->ui_font,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        const std::wstring sub = Utf8ToWideString(
            s.group.empty() ? s.platform : s.group);
        DrawTextIn(dc,
                   {card.left + 5, title_bottom + 1, card.right - 5, card.bottom - 3},
                   sub.c_str(), kSubtleText, state->small_font,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

// 鼠标位置 → 卡片（group 下标 + sheet 下标）。点在分组行/空白处返回 false。
bool HitDiscoverCard(BrowserState* state, int* group, int* sheet) {
    if (!state || !state->sheet_list) return false;
    if (IsVideoMode(state)) return HitVideoCard(state, group, sheet);
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(state->sheet_list, &pt);
    const LRESULT hit = SendMessageW(state->sheet_list, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(pt.x, pt.y));
    const int row = LOWORD(hit);
    if (HIWORD(hit) || row < 0 ||
        row >= static_cast<int>(state->discover_rows.size())) return false;
    const BrowserState::DiscoverRow& r = state->discover_rows[row];
    if (r.header || r.group < 0 ||
        r.group >= static_cast<int>(state->groups.size())) return false;
    const int col = std::max(0, static_cast<int>(pt.x) / kCardWidth);
    const int index = r.first + col;
    if (col >= r.count ||
        index >= static_cast<int>(state->groups[r.group].sheets.size())) return false;
    *group = r.group;
    *sheet = index;
    return true;
}

// 把 MusicItem 列表转成 OnlineSearchHit 并灌进现有结果列表（沿用 raw 播放链路）。
void FillHitsFromMusicItems(BrowserState* state,
                            const std::vector<MusicItem>& items) {
    state->hits.clear();
    for (const auto& m : items) {
        OnlineSearchHit hit;
        hit.platform = m.platform;
        hit.id = m.id;
        hit.title = m.title;
        hit.artist = m.artist;
        hit.album = m.album;
        hit.artwork = m.artwork;
        hit.duration_ms = m.duration_ms;
        hit.raw = m.raw;
        state->hits.push_back(std::move(hit));
    }
    FillResultList(state);
}

// 发现页加载：窗口显示后才由 kMsgDiscoverStart 触发。后台线程先等插件挂载
// 完成，再逐音源取榜单 + 推荐；每完成一个就投递一批，UI 立刻追加卡片。
// 不占用 state->busy，因此加载期间「搜索」页依然可以正常用。
void StartLoadDiscover(BrowserState* state, bool force) {
    if (IsMusicFreeShuttingDown()) return;
    if (!state || state->discover_busy.exchange(true)) return;
    if (state->discover_loaded && !force) {
        state->discover_busy.store(false);
        return;
    }
    // 有缓存时不清空：旧卡片一直可见，后台新数据按音源原地替换，避免闪烁。
    const bool keep_cached = !state->groups.empty();
    if (!keep_cached) {
        state->groups.clear();
        state->discover_rows.clear();
    }
    state->selected_group = -1;
    state->selected_sheet = -1;
    state->skipped_sources = 0;
    state->finished_sources = 0;
    state->updated_sources = 0;
    // 强制刷新会重取榜单；清空后紧随其后的 FillDiscoverList 会重新全量预取，
    // 已解码 / 已落盘的封面命中缓存，几乎没有额外开销。
    state->art_requested.clear();
    state->discover_loaded = false;
    state->discover_placeholder = !keep_cached;
    FillDiscoverList(state);
    SetStatusFor(state, false, keep_cached
        ? L"已显示上次的榜单与推荐，正在后台静默更新…窗口现在即可操作"
        : L"正在后台加载各音源的榜单与推荐…窗口现在即可操作");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak] {
        BgThreadGuard guard;
        if (IsMusicFreeShuttingDown()) return;
        std::vector<std::string> platforms;
        try {
            platforms = ListDiscoverPlatforms();  // 内部等待后台挂载，不阻塞 UI
        } catch (...) {}
        for (const auto& platform : platforms) {
            auto* batch = new DiscoverPayload();
            batch->platform = platform;
            try {
                std::lock_guard<std::mutex> lock(g_plugin_io);
                DiscoverOutcome outcome = GetDiscoverSheets(platform);
                batch->sheets = std::move(outcome.sheets);
                batch->error = std::move(outcome.error);
            } catch (const std::exception& e) {
                batch->error = e.what();
            } catch (...) {
                batch->error = "未知错误";
            }
            auto sp = weak.lock();
            if (!sp) { delete batch; return; }
            if (!PostMessageW(sp->hwnd, kMsgSheetsReady, 0,
                              reinterpret_cast<LPARAM>(batch))) {
                delete batch;
                return;
            }
        }
        auto* done = new DiscoverPayload();
        done->finished = true;
        done->platforms = std::move(platforms);
        auto sp = weak.lock();
        if (!sp) { delete done; return; }
        if (!PostMessageW(sp->hwnd, kMsgSheetsReady, 0,
                          reinterpret_cast<LPARAM>(done)))
            delete done;
    }).detach();
}

// 打开选中的榜单 / 推荐歌单，把曲目灌进下方结果列表（后台 → kMsgSheetTracksReady）。
void StartOpenSheet(BrowserState* state) {
    if (IsMusicFreeShuttingDown()) return;
    if (!state || state->selected_group < 0 || state->selected_sheet < 0 ||
        state->selected_group >= static_cast<int>(state->groups.size())) {
        SetSheetProgress(state, L"请先点击一张封面卡片查看曲目",
                         L"请先点击一张榜单 / 歌单卡片");
        return;
    }
    const auto& sheets = state->groups[state->selected_group].sheets;
    if (state->selected_sheet >= static_cast<int>(sheets.size())) return;
    const MusicSheet sheet = sheets[state->selected_sheet];
    // 第一次点开榜单时才展开下方曲目区，并立即重排，让「正在加载」提示和
    // 随后的曲目有地方显示；之后保持展开，切页 / 刷新都不收起。
    if (!state->tracks_visible) {
        state->tracks_visible = true;
        LayoutChildren(state);
    }
    // group 由 GetDiscoverSheets 打了「榜单」/「推荐」前缀，据此选详情方法。
    const bool is_top = sheet.group.compare(0, 6, "榜单") == 0;
    const SheetKind kind = is_top ? SheetKind::kTopList : SheetKind::kSheet;
    if (state->busy.exchange(true)) {
        // 双击会带来第二次通知；静默忽略，别把「正在加载」的提示覆盖掉。
        return;
    }
    state->open_sheet_title = Utf8ToWideString(sheet.title);
    const std::wstring title = state->open_sheet_title;
    // 清掉上一张歌单的曲目，避免用户看到旧数据以为「点了没变化」。
    state->hits.clear();
    FillResultList(state);
    SetBusyUi(state, true);
    SetSheetProgress(state,
        L"歌单「" + title + L"」· 正在准备音源并加载曲目（首次启动约需 10 秒）…",
        L"正在打开「" + title + L"」：音源准备中，稍候即可看到曲目…");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, sheet, kind] {
        BgThreadGuard guard;
        if (IsMusicFreeShuttingDown()) return;
        auto* payload = new SearchPayload();
        payload->for_search = false;  // 歌单曲目只属于发现页
        try {
            SheetTracksOutcome out;
            {
                std::lock_guard<std::mutex> lock(g_plugin_io);
                out = GetSheetTracks(sheet.platform, sheet.raw, kind, 1);
            }
            if (!out.error.empty() && out.items.empty())
                payload->diagnostics.errors.push_back(out.error);
            for (const auto& m : out.items) {
                OnlineSearchHit hit;
                hit.platform = m.platform;
                hit.id = m.id;
                hit.title = m.title;
                hit.artist = m.artist;
                hit.album = m.album;
                hit.artwork = m.artwork;
                hit.duration_ms = m.duration_ms;
                hit.raw = m.raw;
                payload->hits.push_back(std::move(hit));
            }
        } catch (const std::exception& e) {
            payload->diagnostics.errors.push_back(e.what());
        } catch (...) {
            payload->diagnostics.errors.push_back("歌单详情读取异常");
        }
        auto sp = weak.lock();
        if (!sp) { delete payload; return; }
        if (!PostMessageW(sp->hwnd, kMsgSheetTracksReady, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 根据页签下标推导模式：0=音乐（原发现）/ 1=视频 / 2=搜索。
BrowserMode TabIndexToMode(int tab_index) {
    if (tab_index == 2) return BrowserMode::kSearch;
    if (tab_index == 1) return BrowserMode::kVideo;
    return BrowserMode::kMusic;
}

bool IsSearchMode(BrowserState* state) {
    return state && state->mode == BrowserMode::kSearch;
}
bool IsVideoMode(BrowserState* state) {
    return state && state->mode == BrowserMode::kVideo;
}

// 把当前正在显示的曲目归到对应页签的存档桶；切页时调用。
void ArchiveActiveHits(BrowserState* state) {
    if (!state) return;
    if (state->mode == BrowserMode::kSearch) state->search_hits = std::move(state->hits);
    else if (state->mode == BrowserMode::kVideo) state->video_hits = std::move(state->hits);
    else state->discover_hits = std::move(state->hits);
}
void RestoreActiveHits(BrowserState* state) {
    if (!state) return;
    if (state->mode == BrowserMode::kSearch) state->hits = std::move(state->search_hits);
    else if (state->mode == BrowserMode::kVideo) state->hits = std::move(state->video_hits);
    else state->hits = std::move(state->discover_hits);
}

// 切换页签：只做显隐与布局，不做任何同步插件 / 网络调用。
void OnTabChanged(BrowserState* state, int tab_index) {
    if (!state) return;
    const BrowserMode old_mode = state->mode;
    const BrowserMode new_mode = TabIndexToMode(tab_index);
    state->mode = new_mode;
    const bool search = new_mode == BrowserMode::kSearch;
    const bool video = new_mode == BrowserMode::kVideo;
    if (old_mode != new_mode) {
        // 三个页签各有一份曲目：切走时存档，切回时取回。后台任务按
        // 各自的 for_* 路由，即使晚到也不会串页。
        ArchiveActiveHits(state);
        RestoreActiveHits(state);
        FillResultList(state);
        // 这里直接写控件：SetStatus 会把文本归到当前页，反而覆盖存档。
        if (state->status) {
            std::wstring text;
            if (search) text = state->search_status;
            else if (video) text = state->video_status;
            else text = state->discover_status;
            SetWindowTextW(state->status, text.c_str());
        }
        if (state->tracks_caption) {
            std::wstring caption;
            if (search) caption = L"";
            else if (video) caption = state->video_caption;
            else caption = state->discover_caption;
            SetWindowTextW(state->tracks_caption, caption.c_str());
        }
    }
    ShowWindow(state->platform, search ? SW_SHOW : SW_HIDE);
    // ⚠ 输入框与搜索按钮的可见性**必须交给 LayoutChildren 统一管**（它负责
    // 「先 MoveWindow 定位、再 ShowWindow」）。在这里直接 ShowWindow 会让它们
    // 停在占位创建位置 (0,0,10,10)：视频页没有顶部工具栏，search 分支的
    // MoveWindow 不会执行，于是 10x10 的搜索按钮顶在窗口左上角，按钮里的
    // 「搜」字被裁成一小团笔画 —— 用户看到的就是一个「柒」。
    // 视频页要搜索请用图标功能栏里的「搜索」入口（会跳到搜索页）。
    ShowWindow(state->sheet_list, search ? SW_HIDE : SW_SHOW);
    ShowWindow(state->tracks_caption, search ? SW_HIDE : SW_SHOW);
    ShowWindow(state->tag_combo, SW_HIDE);
    ShowWindow(state->import_button, SW_SHOW);
    if (search) RefreshSourceComboForMode(state);
    ApplyFrameMetrics(state, false);
    // 切页首帧整窗重绘一次：只刷 sheet_list 时，tab 左缘 / 标题栏下沿这些
    // 皮肤区域可能残留上一页像素（用户反馈「第一次切到视频左上角有问题」）。
    if (state->hwnd) InvalidateRect(state->hwnd, nullptr, TRUE);
    // 卡片网格（sheet_list）是音乐/视频页共用的 ListBox：切走时会被另一页重填，
    // 切回来必须按当前页重建，否则切到视频再切回音乐会变成空白，只能靠「刷新」
    // 重新拉取。groups / video_groups 一直留在 state 里，这里只是重新铺一遍卡片
    // 行，不触发任何网络请求。放在 ApplyFrameMetrics 之后，列数已按新页算好。
    if (!search) FillDiscoverList(state);
    // 每次切进视频页都发一次，由 kMsgVideoStart 判断是否需要重新聚合：
    // 以前这里要求 !video_loaded，一旦早期用空站点表跑完一轮把
    // video_loaded 置 true，之后就永远不再加载，视频页一直停在占位卡片。
    if (video)
        PostMessageW(state->hwnd, kMsgVideoStart, 0, 0);
    if (!search && old_mode != BrowserMode::kVideo &&
        !state->discover_loaded && !state->discover_busy.load())
        PostMessageW(state->hwnd, kMsgDiscoverStart, 0, 0);
}

// 初始进入窗口时布置为发现页（第 0 个页签）。
void ApplyMode(BrowserState* state) {
    if (state) OnTabChanged(state, 0);
}

// ============================================================ 视频（影视仓 / TVBox）
// 视频站点 = 分组、视频条目 = 卡片、选集 = 曲目，复用发现页的卡片网格与列表。
void SetVideoStatus(BrowserState* state, const std::wstring& text) {
    if (!state) return;
    state->video_status = text;
    if (state->status && IsVideoMode(state))
        SetWindowTextW(state->status, text.c_str());
}
void SetVideoSheetCaption(BrowserState* state, const std::wstring& text) {
    if (!state) return;
    state->video_caption = text;
    if (state->tracks_caption && IsVideoMode(state))
        SetWindowTextW(state->tracks_caption, text.c_str());
}

// 直接播放视频页结果列表里选中的一行（选集 / 频道）。
// 双击列表行和「播放所选」按钮共用这条路：播放地址在 raw["url"] 里，
// 不再绕 ResolvePlayUrl（那是给音乐源插件用的）。
void PlaySelectedVideoHit(BrowserState* state) {
    if (!state) return;
    int sel = static_cast<int>(
        ListView_GetNextItem(state->list, -1, LVNI_SELECTED));
    // 未选中时默认播第一集（影视仓式习惯：点「播放所选」通常就是想接着看）。
    if (sel < 0 && !state->hits.empty()) sel = 0;
    TraceImport("play_hit sel=%d hits=%zu video=%d list=%p", sel,
                state->hits.size(), IsVideoMode(state) ? 1 : 0,
                reinterpret_cast<void*>(state->list));
    if (sel < 0 || sel >= static_cast<int>(state->hits.size())) {
        SetVideoSheetCaption(state, L"请先在下方列表中选择一行");
        return;
    }
    const OnlineSearchHit& hit = state->hits[static_cast<std::size_t>(sel)];
    std::string url;
    std::map<std::string, std::string> headers;
    if (hit.raw.is_object()) {
        if (hit.raw.contains("url") && hit.raw["url"].is_string())
            url = hit.raw["url"].get<std::string>();
        if (hit.raw.contains("headers") && hit.raw["headers"].is_object())
            for (auto it = hit.raw["headers"].begin();
                 it != hit.raw["headers"].end(); ++it)
                if (it.value().is_string())
                    headers[it.key()] = it.value().get<std::string>();
    }
    if (url.empty()) {
        TraceImport("play_hit url empty sel=%d", sel);
        SetVideoSheetCaption(state, L"该选集没有可播放地址");
        return;
    }
    TraceImport("play_hit opening url=%s", url.substr(0, 80).c_str());
    // 同集的其它线路（切换源按钮的数据）：raw["sources"] = [{url,name},...]
    std::vector<ttplayer::video::VideoPlayerSource> sources;
    if (hit.raw.is_object() && hit.raw.contains("sources") &&
        hit.raw["sources"].is_array()) {
        for (const auto& s : hit.raw["sources"]) {
            if (!s.is_object() || !s.contains("url") || !s["url"].is_string())
                continue;
            ttplayer::video::VideoPlayerSource src;
            src.url = Utf8ToWideString(s["url"].get<std::string>());
            if (s.contains("name") && s["name"].is_string())
                src.name = Utf8ToWideString(s["name"].get<std::string>());
            sources.push_back(std::move(src));
        }
    }
    // 线路名要传当前 hit 所属线路（raw["line"]），不是站点名 hit.platform；
    // 否则播放窗标题栏把「站点」当「线路」显示，切源后更易混淆。
    ttplayer::video::ShowVideoPlayer(
        state->hwnd, Utf8ToWideString(url),
        Utf8ToWideString(hit.title),
        Utf8ToWideString(hit.raw.value("line", hit.platform)),
        ActiveSkin(state), headers, sources,
        state->video_mgr ? state->video_mgr->AllParses()
                         : std::vector<ttplayer::video::VideoParse>());
}

// （FriendlySectionTitle / SectionPriority 已移出本匿名命名空间 —— 见文件
//  下方 } // namespace 之后的「跨编译单元导出符号」区，声明见
//  musicfree_browser_window.h；此处的调用点（BuildVideoHomeSections）经该
//  头文件声明仍可正常解析。）

// 把「推荐」聚合流按分类名（type_name）切成多个影视仓式分区：热播电影 / 热播剧集 /
// 动漫 … 每个分区一段海报墙。结果写入 state->video_home_items（铺平）+ sections。
// ⚠ 必须先按 key 分桶再整桶铺平：聚合流是按站点轮询交错的，同一分区的 items
// 散布在整条流里；以前「首次出现下标 + 累计总数」当区间用，各分区区间互相
// 重叠错位（trace: section[0] start=0 count=14 / section[1] start=4）——不仅
// 海报张冠李戴，行结构与数据也对不上。
void BuildVideoHomeSections(BrowserState* state) {
    if (!state) return;
    state->video_home_items.clear();
    state->video_home_sections.clear();
    BrowserState::VideoGroup* agg = nullptr;
    for (auto& g : state->video_groups)
        if (g.site_key == "agg:") { agg = &g; break; }
    if (!agg) { state->video_in_sections = false; return; }
    const auto& items = agg->items;
    std::vector<std::wstring> order;             // 首次出现顺序（按友好标题去重）
    std::map<std::wstring, std::vector<const ttplayer::video::VideoItem*>> buckets;
    for (const auto& it : items) {
        // ⚠ 按「友好标题」分桶，而不是原始 type_name：不同源对同一直观分类用了
        // 不同原始名（电影 / 最新电影 / 热播电影 / 电影推荐 …），但它们经
        // FriendlySectionTitle 都归一成「热播电影」。以前按原始名分桶，导致同一个
        // 「热播电影」标题在页面上被拆成好几段、重复出现（用户反馈「分类明显重复」）。
        const std::wstring title = FriendlySectionTitle(
            it.type_name.empty() ? std::string("推荐") : it.type_name);
        if (buckets.find(title) == buckets.end()) order.push_back(title);
        buckets[title].push_back(&it);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](const std::wstring& a, const std::wstring& b) {
                         return SectionPriority(a) < SectionPriority(b);
                     });
    for (const auto& title : order) {
        VideoHomeSection sec;
        sec.title = title;
        sec.start = static_cast<int>(state->video_home_items.size());
        sec.count = static_cast<int>(buckets[title].size());
        for (const auto* p : buckets[title])
            state->video_home_items.push_back(*p);
        state->video_home_sections.push_back(sec);
    }
    state->video_in_sections = !state->video_home_sections.empty();
}

// video_groups → 绘制行（复刻 RebuildDiscoverRows）。
void RebuildVideoRows(BrowserState* state) {
    state->video_rows.clear();
    const int cols = std::max(1, state->video_cols);
    // —— 影视仓式分区首页（「推荐」页签）：每个分区 = 标题行 + 若干卡片行 ——
    if (state->video_in_sections && !state->video_home_sections.empty()) {
        for (size_t si = 0; si < state->video_home_sections.size(); ++si) {
            const auto& sec = state->video_home_sections[si];
            BrowserState::DiscoverRow hdr;
            hdr.header = true; hdr.group = -3; hdr.section_idx = static_cast<int>(si);
            state->video_rows.push_back(hdr);
            for (int first = 0; first < sec.count; first += cols) {
                BrowserState::DiscoverRow cards;
                cards.group = -2; cards.first = sec.start + first;
                cards.count = std::min(cols, sec.count - first);
                state->video_rows.push_back(cards);
            }
        }
        // 直播等其余分组照旧垫底（聚合组已用分区呈现，跳过）。
        for (int g = 0; g < static_cast<int>(state->video_groups.size()); ++g) {
            const auto& group = state->video_groups[g];
            if (group.site_key == "agg:") continue;
            if (group.items.empty()) continue;
            BrowserState::DiscoverRow hdr;
            hdr.header = true; hdr.group = g;
            state->video_rows.push_back(hdr);
            const int total = static_cast<int>(group.items.size());
            for (int first = 0; first < total; first += cols) {
                BrowserState::DiscoverRow cards;
                cards.group = g; cards.first = first;
                cards.count = std::min(cols, total - first);
                state->video_rows.push_back(cards);
            }
        }
        return;
    }
    // —— 原有扁平 / 分类视图（video_groups）——
    // 聚合组永远存在（哪怕 items 为空），骨架占位改成「一张卡片都没有」判断。
    bool any_cards = false;
    for (const auto& group : state->video_groups)
        if (!group.items.empty()) { any_cards = true; break; }
    // 加载中 / 全部源失败时也必须留一行占位卡片：否则 ListBox 一行都没有，
    // 双缓冲只铺 kVideoPageBg 纯黑底，用户看到的就是「内容图片全都没显示」。
    // 以前这里还要求 video_placeholder，而 StartLoadVideo 一开头就把它设成
    // false，于是首页聚合期间（几十秒）整块内容区恒为纯黑。
    if (!any_cards) {
        BrowserState::DiscoverRow skeleton;
        skeleton.header = true; skeleton.group = -1; skeleton.count = 1;
        state->video_rows.push_back(skeleton);
        return;
    }
    for (int g = 0; g < static_cast<int>(state->video_groups.size()); ++g) {
        const auto& group = state->video_groups[g];
        if (group.items.empty()) continue;
        BrowserState::DiscoverRow header; header.header = true; header.group = g;
        state->video_rows.push_back(header);
        const int total = static_cast<int>(group.items.size());
        for (int first = 0; first < total; first += cols) {
            BrowserState::DiscoverRow cards;
            cards.group = g; cards.first = first;
            cards.count = std::min(cols, total - first);
            state->video_rows.push_back(cards);
        }
    }
}

// 统计「已请求过但缓存里仍无图」的视频封面数（下载失败批次的特征）。
int CountMissingVideoArtwork(BrowserState* state) {
    if (!state || !state->artwork) return 0;
    int missing = 0;
    const auto check = [&](const std::string& url) {
        if (url.empty() || !state->video_art_requested.count(url)) return;
        const auto it = state->video_art_retries.find(url);
        if (it != state->video_art_retries.end() &&
            it->second >= kVideoArtMaxRetries) return;
        if (!state->artwork->Peek(url)) ++missing;
    };
    for (const auto& item : state->video_home_items) check(item.pic);
    for (const auto& group : state->video_groups)
        for (const auto& item : group.items) check(item.pic);
    return missing;
}

// 把失败封面的请求重新入队（video_art_requested erase 后 Request 可重入），
// 次数记进 video_art_retries 封顶，避免图床真挂时无限风暴。
void RetryMissingVideoArtwork(BrowserState* state) {
    if (!state || !state->artwork) return;
    const HWND target = state->hwnd;
    std::weak_ptr<BrowserState> weak = g_browser;
    const auto retry = [&](const std::string& url) {
        if (url.empty() || !state->video_art_requested.count(url)) return;
        int& tries = state->video_art_retries[url];
        if (tries >= kVideoArtMaxRetries) return;
        if (state->artwork->Peek(url)) return;  // 已到图
        ++tries;
        state->video_art_requested.erase(url);  // 允许重新入队
        state->artwork->Request(url, kVideoPosterH,
            [weak, target](const std::string& url) {
                if (auto sp = weak.lock())
                    if (target && IsWindow(target))
                        // 只在真的拿到图时才通知重绘；失败静默（由 3 秒节流的
                        // 补抓轮处理），否则失败风暴会高频重绘海报墙。
                        if (sp->artwork && sp->artwork->Peek(url))
                            PostMessageW(target, kMsgArtworkReady, 0, 0);
            });
    };
    for (const auto& item : state->video_home_items) retry(item.pic);
    for (const auto& group : state->video_groups)
        for (const auto& item : group.items) retry(item.pic);
}

void PrefetchVideoArtwork(BrowserState* state) {
    if (!state || !state->artwork) return;
    HWND target = state->hwnd;
    std::weak_ptr<BrowserState> weak = g_browser;
    for (const auto& group : state->video_groups) {
        for (const auto& item : group.items) {
            const std::string& url = item.pic;
            if (url.empty() ||
                state->video_art_requested.find(url) != state->video_art_requested.end())
                continue;
            state->video_art_requested.insert(url);
            // 海报是竖版 2:3，请求高度作为 max_edge，避免缩略图模糊。
            state->artwork->Request(url, kVideoPosterH,
                [weak, target](const std::string& url) {
                    if (auto sp = weak.lock())
                        if (target && IsWindow(target))
                            // 只在真的拿到图时才通知重绘；失败静默（补抓轮处理）。
                            if (sp->artwork && sp->artwork->Peek(url))
                                PostMessageW(target, kMsgArtworkReady, 0, 0);
                });
        }
    }
    // 影视仓式分区视图的卡片封面（与 video_groups 同源，去重集合会挡掉重复）。
    for (const auto& item : state->video_home_items) {
        const std::string& url = item.pic;
        if (url.empty() ||
            state->video_art_requested.find(url) != state->video_art_requested.end())
            continue;
        state->video_art_requested.insert(url);
        state->artwork->Request(url, kVideoPosterH,
            [weak, target](const std::string& url) {
                if (auto sp = weak.lock())
                    if (target && IsWindow(target))
                        // 只在真的拿到图时才通知重绘；失败静默（由 3 秒节流的
                        // 补抓轮处理），否则失败风暴会高频重绘海报墙。
                        if (sp->artwork && sp->artwork->Peek(url))
                            PostMessageW(target, kMsgArtworkReady, 0, 0);
            });
    }
}

void FillVideoList(BrowserState* state) {
    if (!state->sheet_list) return;
    state->video_cols_applied = state->video_cols;  // 行按当前列数铺，同步基准
    // 影视仓式分区首页：仅在「推荐」页签且非搜索时，把聚合流切成分类分区。
    if (state->video_active_cat == -1 && !IsSearchMode(state))
        BuildVideoHomeSections(state);
    else
        state->video_in_sections = false;
    RebuildVideoRows(state);
    SendMessageW(state->sheet_list, WM_SETREDRAW, FALSE, 0);
    const int top_index = static_cast<int>(
        SendMessageW(state->sheet_list, LB_GETTOPINDEX, 0, 0));
    ListBox_ResetContent(state->sheet_list);
    for (size_t i = 0; i < state->video_rows.size(); ++i)
        ListBox_AddString(state->sheet_list, L"");
    for (size_t i = 0; i < state->video_rows.size(); ++i) {
        const auto& row = state->video_rows[i];
        // 骨架占位（group==-1）用整行高度；标题行（站点/分区，group>=0 或 -3）用分组高；
        // 卡片行用卡片高度。
        const int h = (row.header && row.group == -1)
                          ? kVideoRowHeight
                          : (row.header ? kGroupRowHeight : kVideoRowHeight);
        SendMessageW(state->sheet_list, LB_SETITEMHEIGHT,
                     static_cast<WPARAM>(i), static_cast<LPARAM>(h));
    }
    SendMessageW(state->sheet_list, LB_SETTOPINDEX, top_index, 0);
    SendMessageW(state->sheet_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->sheet_list, nullptr, FALSE);
    PrefetchVideoArtwork(state);
}

// ---- 视频页配色在文件头部定义（sheet_list 自绘底色也要用）--------------

// 分类页签：cat_index = -1 表示「全部」（恢复首页推荐）。
struct VideoChip {
    RECT rect;
    int cat_index;
};

// 计算一个分组标题行里的分类 chips（跟在标题文字后面从左往右排，放不下丢弃）。
// 绘制与命中测试共用同一份几何，必须完全确定 —— 只依赖字体和文本。
std::vector<VideoChip> LayoutVideoChips(HDC dc, BrowserState* state,
                                        const RECT& rc, int group_index) {
    std::vector<VideoChip> out;
    if (!state || group_index < 0 ||
        group_index >= static_cast<int>(state->video_groups.size()))
        return out;
    const auto& group = state->video_groups[group_index];
    if (group.site_key == "live:") return out;  // 直播组没有分类
    // 标题宽（决定 chips 起点）
    std::wstring title = Utf8ToWideString(group.site_name);
    const bool is_agg = group.site_key == "agg:";
    if (is_agg) {
        // 聚合推荐组：一条统一推荐流的统计，而不是站点名。
        const auto& batches = state->video_cat_batches.empty()
                                  ? state->video_agg_batches
                                  : state->video_cat_batches;
        title = group.active_category_name.empty()
                    ? L"推荐"
                    : group.active_category_name;
        title += L"   ·   " + std::to_wstring(group.items.size()) +
                 L" 部影片   ·   " + std::to_wstring(batches.size()) +
                 L" 个片源";
    }
    if (title.empty()) title = L"未命名视频源";
    if (!is_agg)
        title += L"   ·   " + std::to_wstring(group.items.size()) + L" 个内容";
    SIZE tsz{};
    const HGDIOBJ old_font = SelectObject(dc, state->bold_font);
    GetTextExtentPoint32W(dc, title.c_str(),
                          static_cast<int>(title.size()), &tsz);
    SelectObject(dc, old_font);
    int x = rc.left + 14 + tsz.cx + 14;
    if (group.category_loading) x += 64;  // 给「加载中…」留位
    const int ch = 20;
    const int y = rc.top + (kGroupRowHeight - ch) / 2;
    SelectObject(dc, state->small_font);
    const auto place = [&](int cat_index, const std::wstring& label) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, label.c_str(),
                              static_cast<int>(label.size()), &sz);
        const int w = sz.cx + 20;
        if (x + w > rc.right - 6) return;
        out.push_back({{x, y, x + w, y + ch}, cat_index});
        x += w + 6;
    };
    place(-1, is_agg ? L"推荐" : L"全部");
    if (is_agg) {
        // 聚合页签（跨站点同名分类合并，按共有站点数排序）。
        const int max_cats =
            std::min<int>(state->video_agg_categories.size(), 10);
        for (int i = 0; i < max_cats; ++i)
            place(i, Utf8ToWideString(state->video_agg_categories[i].name));
    } else {
        const int max_cats = std::min<int>(group.categories.size(), 14);
        for (int i = 0; i < max_cats; ++i)
            place(i, Utf8ToWideString(group.categories[i].name));
    }
    SelectObject(dc, old_font);
    return out;
}

std::wstring VideoChipLabel(BrowserState* state, int group_index,
                            const VideoChip& chip) {
    if (chip.cat_index < 0) {
        const bool is_agg =
            group_index >= 0 &&
            group_index < static_cast<int>(state->video_groups.size()) &&
            state->video_groups[group_index].site_key == "agg:";
        return is_agg ? L"推荐" : L"全部";
    }
    if (group_index < 0 ||
        group_index >= static_cast<int>(state->video_groups.size()))
        return L"?";
    const auto& group = state->video_groups[group_index];
    if (group.site_key == "agg:") {
        if (chip.cat_index >=
            static_cast<int>(state->video_agg_categories.size()))
            return L"?";
        return Utf8ToWideString(
            state->video_agg_categories[chip.cat_index].name);
    }
    const auto& cats = group.categories;
    if (chip.cat_index >= static_cast<int>(cats.size())) return L"?";
    return Utf8ToWideString(cats[chip.cat_index].name);
}

void DrawDiscoverRowVideo(BrowserState* state, const DRAWITEMSTRUCT& item) {
    const int row = static_cast<int>(item.itemID);
    if (row < 0 || row >= static_cast<int>(state->video_rows.size())) return;
    const BrowserState::DiscoverRow& r = state->video_rows[row];
    HDC dc = item.hDC;
    RECT rc = item.rcItem;
    FillSolidRect(dc, rc, kVideoPageBg);
    if (r.header && r.group == -1) {
        // 骨架占位：深色海报框 + 引导文字（与音乐页同职责，配色独立）。
        for (int c = 0; c < state->video_cols; ++c) {
            RECT card = {rc.left + c * kVideoCardWidth + 4, rc.top + 3,
                         rc.left + (c + 1) * kVideoCardWidth - 4, rc.bottom - 3};
            const RECT poster = {card.left + 10, card.top + 4,
                                 card.right - 10, card.top + 4 + kVideoPosterH};
            DrawRoundedBox(dc, poster, kVideoCardBg, kVideoCardLine, 6);
            DrawTextIn(dc,
                       {card.left, poster.bottom + 6, card.right,
                        poster.bottom + 26},
                       state->video_placeholder_text.c_str(), kVideoSubTx,
                       state->small_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        return;
    }
    if (r.group == -3) {
        // 影视仓式分区标题行（热播电影 / 热播剧集 / 动漫 …）。
        const auto& sec = state->video_home_sections[r.section_idx];
        FillSolidRect(dc, {rc.left + 4, rc.top + 6, rc.left + 7, rc.bottom - 6},
                      kVideoSelLine);
        DrawTextIn(dc, {rc.left + 14, rc.top, rc.right - 60, rc.bottom},
                   sec.title.c_str(), kVideoTitleTx, state->bold_font,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        const std::wstring more = L"更多 ›";
        DrawTextIn(dc, {rc.right - 56, rc.top, rc.right - 6, rc.bottom},
                   more.c_str(), kVideoSubTx, state->small_font,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    // group == -2（分区卡片行）是合法值，必须放行到下面的 section_card 分支。
    // 以前这里写成 `r.group < 0 return`，-2 在进入分区绘制前就被吞掉——
    // 分区标题行能画（-3 在前面处理），卡片行却整块黑屏（死代码）。
    if (r.group < -2 || r.group >= static_cast<int>(state->video_groups.size())) return;
    if (r.header) {
        // 分组标题行：聚合统计（或站点名）+ 分类页签（影视仓式）。
        const auto& group = state->video_groups[r.group];
        const bool is_agg = group.site_key == "agg:";
        FillSolidRect(dc, {rc.left + 4, rc.top + 6, rc.left + 7, rc.bottom - 6},
                      kVideoSelLine);
        std::wstring title = Utf8ToWideString(group.site_name);
        if (is_agg) {
            const auto& batches = state->video_cat_batches.empty()
                                      ? state->video_agg_batches
                                      : state->video_cat_batches;
            title = group.active_category_name.empty()
                        ? L"推荐"
                        : group.active_category_name;
            title += L"   ·   " + std::to_wstring(group.items.size()) +
                     L" 部影片   ·   " + std::to_wstring(batches.size()) +
                     L" 个片源";
        }
        if (title.empty()) title = L"未命名视频源";
        if (!is_agg)
            title += L"   ·   " + std::to_wstring(group.items.size()) +
                     L" 个内容";
        DrawTextIn(dc, {rc.left + 14, rc.top, rc.right - 6, rc.bottom},
                   title.c_str(), kVideoTitleTx, state->bold_font,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (group.category_loading) {
            SIZE tsz{};
            const HGDIOBJ old_font = SelectObject(dc, state->bold_font);
            GetTextExtentPoint32W(dc, title.c_str(),
                                  static_cast<int>(title.size()), &tsz);
            SelectObject(dc, old_font);
            DrawTextIn(dc, {rc.left + 14 + tsz.cx + 14, rc.top,
                            rc.right - 6, rc.bottom},
                       L"加载中…", kVideoSubTx, state->small_font,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        // ⚠ 不再在分组标题行里画第二排分类 chips：顶部导航行（video_navbar）
        // 已经有同一组分类，两处重复既挤又容易误点（用户反馈「下方会出现第
        // 二条分类」）。分类入口只保留顶部导航行。
        return;
    }
    const bool section_card = (r.group == -2);
    for (int c = 0; c < r.count; ++c) {
        const int index = r.first + c;
        if (section_card) {
            if (index >= static_cast<int>(state->video_home_items.size())) break;
        } else {
            if (r.group < 0 ||
                r.group >= static_cast<int>(state->video_groups.size())) break;
            if (index >= static_cast<int>(state->video_groups[r.group].items.size()))
                break;
        }
        const ttplayer::video::VideoItem& s = section_card
            ? state->video_home_items[index]
            : state->video_groups[r.group].items[index];
        RECT card = {rc.left + c * kVideoCardWidth + 4, rc.top + 3,
                     rc.left + (c + 1) * kVideoCardWidth - 4, rc.bottom - 3};
        const bool selected = section_card
            ? (state->selected_video_group == -2 &&
               state->selected_video_item == index)
            : (state->selected_video_group == r.group &&
               state->selected_video_item == index);
        const RECT poster = {card.left + 10, card.top + 4,
                             card.right - 10, card.top + 4 + kVideoPosterH};
        HBITMAP bmp = state->artwork ? state->artwork->Peek(s.pic) : nullptr;
        if (bmp) {
            BITMAP bm{};
            GetObjectW(bmp, sizeof(bm), &bm);
            HDC src = CreateCompatibleDC(dc);
            const HGDIOBJ old = SelectObject(src, bmp);
            // 海报缩放明显（原图 ~400px → 196px），HALFTONE 比 COLORONCOLOR 平滑。
            const int prev_mode = SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, poster.left, poster.top,
                       poster.right - poster.left,
                       poster.bottom - poster.top, src, 0, 0, bm.bmWidth,
                       bm.bmHeight, SRCCOPY);
            SetStretchBltMode(dc, prev_mode);
            SelectObject(src, old);
            DeleteDC(src);
            DrawRoundedBox(dc, poster, CLR_INVALID,
                           selected ? kVideoSelLine : kVideoCardLine, 6);
        } else {
            unsigned int hash = 2166136261u;
            for (unsigned char c : s.title) { hash ^= c; hash *= 16777619u; }
            static const COLORREF kPlaceholderColors[] = {
                RGB(0x3E, 0x44, 0x66), RGB(0x5A, 0x3E, 0x5C),
                RGB(0x2E, 0x54, 0x66), RGB(0x35, 0x5C, 0x50),
                RGB(0x66, 0x50, 0x36), RGB(0x46, 0x3E, 0x66),
                RGB(0x2C, 0x56, 0x5E), RGB(0x60, 0x3C, 0x3C),
            };
            const COLORREF bg = kPlaceholderColors[
                hash % (sizeof(kPlaceholderColors) / sizeof(kPlaceholderColors[0]))];
            DrawRoundedBox(dc, poster, bg, kVideoCardLine, 6);
            const std::wstring wtitle = Utf8ToWideString(s.title);
            const std::wstring initial =
                wtitle.empty() ? L"\u25B6" : wtitle.substr(0, 1);
            DrawTextIn(dc, poster, initial.c_str(), RGB(235, 240, 246),
                       state->bold_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        // 海报右下角备注角标（「更新至12集」「HD」…），影视仓同款位置。
        if (!s.remarks.empty()) {
            std::wstring rem = Utf8ToWideString(s.remarks);
            if (rem.size() > 12) rem = rem.substr(0, 12) + L"…";
            SIZE sz{};
            SelectObject(dc, state->small_font);
            GetTextExtentPoint32W(dc, rem.c_str(),
                                  static_cast<int>(rem.size()), &sz);
            const RECT badge = {poster.right - sz.cx - 14, poster.bottom - 22,
                                poster.right - 4, poster.bottom - 6};
            if (badge.left > poster.left) {
                DrawRoundedBox(dc, badge, RGB(8, 10, 14), CLR_INVALID, 4);
                DrawTextIn(dc, badge, rem.c_str(), RGB(196, 210, 224),
                           state->small_font,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        // 来源标签（左上角）+ 评分（右上角）：影视仓同款角标。来源取站点名
        // （最多 5 字）；评分仅当 CMS 返回了 score 字段才画（金色）。
        {
            SelectObject(dc, state->small_font);
            std::wstring src = Utf8ToWideString(s.site_name);
            if (src.size() > 5) src = src.substr(0, 5);
            if (!src.empty()) {
                SIZE sz{};
                GetTextExtentPoint32W(dc, src.c_str(),
                                      static_cast<int>(src.size()), &sz);
                const int bw = sz.cx + 12;
                const RECT badge = {poster.left + 4, poster.top + 4,
                                    poster.left + 4 + bw, poster.top + 4 + 18};
                DrawRoundedBox(dc, badge, RGB(8, 10, 14), CLR_INVALID, 4);
                DrawTextIn(dc, badge, src.c_str(), RGB(196, 210, 224),
                           state->small_font,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            if (!s.score.empty()) {
                std::wstring sc = Utf8ToWideString(s.score);
                if (sc.size() > 4) sc = sc.substr(0, 4);
                SIZE sz{};
                GetTextExtentPoint32W(dc, sc.c_str(),
                                      static_cast<int>(sc.size()), &sz);
                const int bw = sz.cx + 12;
                const RECT badge = {poster.right - 4 - bw, poster.top + 4,
                                    poster.right - 4, poster.top + 4 + 18};
                DrawRoundedBox(dc, badge, RGB(40, 30, 6), CLR_INVALID, 4);
                DrawTextIn(dc, badge, sc.c_str(), RGB(245, 196, 80),
                           state->small_font,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        // 海报下方：片名（白）+ 类型/年份（灰）
        const int title_top = poster.bottom + 3;
        DrawTextIn(dc,
                   {card.left + 6, title_top, card.right - 6, title_top + 19},
                   Utf8ToWideString(s.title).c_str(), kVideoTitleTx,
                   state->ui_font,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                       DT_NOPREFIX);
        std::string sub_raw = s.year;
        if (!s.type_name.empty()) {
            if (!sub_raw.empty()) sub_raw += " ";
            sub_raw += s.type_name;
        }
        if (sub_raw.empty()) sub_raw = s.site_name;
        const std::wstring sub = Utf8ToWideString(sub_raw);
        DrawTextIn(dc,
                   {card.left + 6, title_top + 20, card.right - 6,
                    title_top + 36},
                   sub.c_str(), kVideoSubTx, state->small_font,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                       DT_NOPREFIX);
    }
}

bool HitVideoCard(BrowserState* state, int* group, int* item) {
    if (!state || !state->sheet_list) return false;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(state->sheet_list, &pt);
    const LRESULT hit = SendMessageW(state->sheet_list, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(pt.x, pt.y));
    const int row = LOWORD(hit);
    if (HIWORD(hit) || row < 0 ||
        row >= static_cast<int>(state->video_rows.size())) return false;
    const BrowserState::DiscoverRow& r = state->video_rows[row];
    if (r.header) return false;  // 标题行（站点 / 分区）不可点
    const int col = std::max(0, static_cast<int>(pt.x) / kVideoCardWidth);
    const int index = r.first + col;
    if (r.group == -2) {  // 影视仓式分区卡片：下标落在 video_home_items
        if (col >= r.count ||
            index >= static_cast<int>(state->video_home_items.size()))
            return false;
        *group = -2;
        *item = index;
        return true;
    }
    if (r.group < 0 ||
        r.group >= static_cast<int>(state->video_groups.size())) return false;
    if (col >= r.count ||
        index >= static_cast<int>(state->video_groups[r.group].items.size()))
        return false;
    *group = r.group;
    *item = index;
    return true;
}

// 点击落在分组标题行的分类页签上：返回 true 并回传组下标与页签下标
// （-1 = 「全部」）。与绘制共用 LayoutVideoChips，几何完全一致。
bool HitVideoChip(BrowserState* state, int* group, int* cat_index) {
    if (!state || !state->sheet_list) return false;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(state->sheet_list, &pt);
    const LRESULT hit = SendMessageW(state->sheet_list, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(pt.x, pt.y));
    const int row = LOWORD(hit);
    if (HIWORD(hit) || row < 0 ||
        row >= static_cast<int>(state->video_rows.size())) return false;
    const BrowserState::DiscoverRow& r = state->video_rows[row];
    if (!r.header || r.group < 0 ||
        r.group >= static_cast<int>(state->video_groups.size())) return false;
    RECT rc{};
    if (!SendMessageW(state->sheet_list, LB_GETITEMRECT, row,
                      reinterpret_cast<LPARAM>(&rc)))
        return false;
    HDC dc = GetDC(state->sheet_list);
    const std::vector<VideoChip> chips =
        LayoutVideoChips(dc, state, rc, r.group);
    ReleaseDC(state->sheet_list, dc);
    for (const VideoChip& chip : chips) {
        if (PtInRect(&chip.rect, pt)) {
            *group = r.group;
            *cat_index = chip.cat_index;
            return true;
        }
    }
    return false;
}

// ---- 影视仓式外壳：顶部分类导航行 + 图标功能栏 ------------------------------
//
// 分类导航行：推荐 + video_agg_categories（跨站合并，≤10）。图标功能栏：历史/直播/
// 搜索/配置/线路/网盘/收藏/推送/设置；直播→导入源对话框、搜索→切到搜索页，其余先占位。
// 几何只依赖字体与文本，绘制与命中测试共用同一份 Layout，保证完全一致。

std::vector<VideoNavTab> LayoutVideoNavTabs(HDC dc, BrowserState* state,
                                           const RECT& rc) {
    std::vector<VideoNavTab> out;
    if (!state) return out;
    const int ch = kVideoNavH - 8;
    const int y = rc.top + (kVideoNavH - ch) / 2;
    int x = rc.left + 8;
    SelectObject(dc, state->small_font);
    auto place = [&](int cat_index, const std::wstring& label) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, label.c_str(),
                              static_cast<int>(label.size()), &sz);
        const int w = sz.cx + 20;
        if (x + w > rc.right - 4) return;
        out.push_back({{x, y, x + w, y + ch}, cat_index});
        x += w + 6;
    };
    place(-1, L"推荐");
    const int max_cats = std::min<int>(state->video_agg_categories.size(), 10);
    for (int i = 0; i < max_cats; ++i)
        place(i, Utf8ToWideString(state->video_agg_categories[i].name));
    return out;
}

// 自绘 STATIC（分类导航行 / 图标功能栏）的双缓冲包装：整幅画进兼容位图后
// 一次 BitBlt 上屏。以前直接画到屏幕，拖动缩放时每帧先露底色再画内容，
// 两条自绘条是肉眼可见的闪烁源。
template <typename Fn>
void PaintBuffered(HWND control, Fn&& paint) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(control, &ps);
    if (!dc) return;
    RECT rc{};
    GetClientRect(control, &rc);
    const int width = std::max<LONG>(1, rc.right - rc.left);
    const int height = std::max<LONG>(1, rc.bottom - rc.top);
    const HDC buffer = CreateCompatibleDC(dc);
    const HBITMAP canvas =
        buffer ? CreateCompatibleBitmap(dc, width, height) : nullptr;
    if (buffer && canvas) {
        const HGDIOBJ old = SelectObject(buffer, canvas);
        paint(buffer, rc);
        BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old);
    }
    if (canvas) DeleteObject(canvas);
    if (buffer) DeleteDC(buffer);
    EndPaint(control, &ps);
}

void PaintVideoNavbar(BrowserState* state, HWND control) {
    PaintBuffered(control, [state](HDC dc, const RECT& rc) {
        FillSolidRect(dc, rc, kVideoPageBg);
        // 底部分隔线
        FillSolidRect(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom},
                      kVideoCardLine);
        const auto tabs = LayoutVideoNavTabs(dc, state, rc);
        for (const auto& tab : tabs) {
            const bool active = tab.cat_index == state->video_active_cat;
            if (active)
                DrawRoundedBox(dc, tab.rect, kVideoSelLine, CLR_INVALID, 9);
            else
                DrawRoundedBox(dc, tab.rect, kVideoChipBg, kVideoChipLine, 9);
            const std::wstring label = tab.cat_index < 0 ? L"推荐"
                : Utf8ToWideString(state->video_agg_categories[tab.cat_index].name);
            DrawTextIn(dc, tab.rect, label.c_str(),
                       active ? RGB(255, 255, 255) : kVideoSubTx,
                       state->small_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    });
}

void HandleVideoNavClick(BrowserState* state, HWND control) {
    if (!state) return;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(control, &pt);
    RECT rc{};
    GetClientRect(control, &rc);
    HDC dc = GetDC(control);
    const auto tabs = LayoutVideoNavTabs(dc, state, rc);
    ReleaseDC(control, dc);
    for (const auto& tab : tabs) {
        if (PtInRect(&tab.rect, pt)) {
            StartLoadAggVideoCategory(state, tab.cat_index);
            InvalidateRect(control, nullptr, FALSE);
            return;
        }
    }
}

std::vector<VideoIconBtn> LayoutVideoIconBtns(HDC dc, BrowserState* state,
                                              const RECT& rc) {
    std::vector<VideoIconBtn> out;
    const int n = static_cast<int>(sizeof(kVideoIconLabels) /
                                   sizeof(kVideoIconLabels[0]));
    const int bw = (rc.right > rc.left) ? (rc.right - rc.left) / n : 40;
    const int bh = kVideoIconH - 12;
    const int y = rc.top + 6;
    for (int i = 0; i < n; ++i) {
        const int x = rc.left + i * bw + 4;
        out.push_back({{x, y, x + bw - 8, y + bh}, i});
    }
    return out;
}

void PaintVideoIconbar(BrowserState* state, HWND control) {
    PaintBuffered(control, [state](HDC dc, const RECT& rc) {
        FillSolidRect(dc, rc, kVideoPageBg);
        // 顶部分隔线
        FillSolidRect(dc, {rc.left, rc.top, rc.right, rc.top + 1},
                      kVideoCardLine);
        SelectObject(dc, state->small_font);
        const auto btns = LayoutVideoIconBtns(dc, state, rc);
        for (size_t i = 0; i < btns.size(); ++i) {
            const auto& b = btns[i];
            const bool hot = state->hot_video_icon == static_cast<int>(i);
            DrawRoundedBox(dc, b.rect,
                           hot ? kVideoSelLine : kVideoChipBg,
                           hot ? CLR_INVALID : kVideoChipLine, 8);
            DrawTextIn(dc, b.rect, kVideoIconLabels[i],
                       hot ? RGB(255, 255, 255) : kVideoTitleTx,
                       state->small_font,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    });
}

void HandleVideoIconClick(BrowserState* state, HWND control) {
    if (!state) return;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(control, &pt);
    RECT rc{};
    GetClientRect(control, &rc);
    HDC dc = GetDC(control);
    const auto btns = LayoutVideoIconBtns(dc, state, rc);
    ReleaseDC(control, dc);
    for (size_t i = 0; i < btns.size(); ++i) {
        if (PtInRect(&btns[i].rect, pt)) {
            switch (static_cast<VideoIconId>(i)) {
                case VideoIconId::kLive:
                    OpenVideoSourceDialog(state);
                    break;
                case VideoIconId::kSearch:
                    TabCtrl_SetCurSel(state->tab, 2);
                    OnTabChanged(state, 2);
                    break;
                default: {
                    const std::wstring name = kVideoIconLabels[i];
                    SetVideoStatus(state, L"「" + name +
                                           L"」功能即将上线，敬请期待");
                    break;
                }
            }
            InvalidateRect(control, nullptr, FALSE);
            return;
        }
    }
}

// ---- 多源聚合推荐（影视仓式）----------------------------------------------
//
// 影视仓首页是「一条统一的推荐流」：分类页签 + 海报网格，从不按源分段。
// 这里把所有 CMS 站点的内容合并成一条流：各站点轮询交错（每轮每站取一条，
// 保证每个源均匀出现而不是「源 A 全部 + 源 B 全部」），同名同年影片只留
// 第一份（跨源重复片在 CMS 生态里极其常见）。卡片自带 site_key，点击详情
// 时各回各的源，聚合只影响展示层。

// （NormalizeVideoTitle 已移出本匿名命名空间 —— 见文件下方 } // namespace
//  之后的「跨编译单元导出符号」区，声明见 musicfree_browser_window.h。）

// 用当前批次表重建聚合组（video_groups[0]）的混排列表。
// cat_batches 非空（分类页签激活）时优先用它；否则用首页批次。
void RebuildAggregatedVideoItems(BrowserState* state) {
    if (!state) return;
    BrowserState::VideoGroup* agg = nullptr;
    for (auto& g : state->video_groups)
        if (g.site_key == "agg:") { agg = &g; break; }
    if (!agg) return;
    const auto& batches = state->video_cat_batches.empty()
                              ? state->video_agg_batches
                              : state->video_cat_batches;
    std::set<std::string> seen;
    std::vector<ttplayer::video::VideoItem> out;
    std::vector<size_t> cur(batches.size(), 0);
    bool added = true;
    while (added) {
        added = false;
        for (size_t i = 0; i < batches.size(); ++i) {
            if (cur[i] >= batches[i].items.size()) continue;
            added = true;
            const ttplayer::video::VideoItem& it = batches[i].items[cur[i]++];
            if (it.title.empty()) continue;
            const std::string key = NormalizeVideoTitle(it.title, it.year);
            if (!key.empty() && !seen.insert(key).second) continue;
            out.push_back(it);
        }
    }
    agg->items = std::move(out);
}

// 分类名归一化：去首尾空白、转小写，用于跨源同名去重。不同源常把同一分类
// 写成 "电影" / "电影 " / "MOVIE"，不归一就会在顶部分类页签里出现"同名两条"。
static std::string NormalizeCategoryName(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        const unsigned char c = static_cast<unsigned char>(s[b]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            ++b;
        else
            break;
    }
    while (e > b) {
        const unsigned char c = static_cast<unsigned char>(s[e - 1]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            --e;
        else
            break;
    }
    std::string out = s.substr(b, e - b);
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return out;
}

// 把一个站点刚取回的分类表并进聚合页签：同名分类合并 per_site（替换式，
// 站点重复刷新不会累积旧条目）。本函数只做"累积 / 去重"，不排序、不裁剪——
// 排序与裁剪统一在 FinalizeAggVideoCategories 里、所有站点到齐后做一次，
// 否则每到一个站点就重排，顶部分类页签会实时跳动、还把后到的挤掉（用户反馈
// "顺序一直变 + 同名分类重复出现"）。
void MergeAggVideoCategories(BrowserState* state, const std::string& site_key,
                             const std::vector<ttplayer::video::VideoCategory>& cats) {
    for (const auto& cat : cats) {
        if (cat.id.empty() || cat.name.empty()) continue;
        const std::string key = NormalizeCategoryName(cat.name);
        if (key.empty()) continue;
        BrowserState::VideoAggCategory* found = nullptr;
        for (auto& agg : state->video_agg_categories)
            if (agg.norm_name == key) { found = &agg; break; }
        if (!found) {
            BrowserState::VideoAggCategory fresh;
            fresh.name = cat.name;
            fresh.norm_name = key;
            state->video_agg_categories.push_back(std::move(fresh));
            found = &state->video_agg_categories.back();
        }
        auto& per = found->per_site;
        per.erase(std::remove_if(per.begin(), per.end(),
                                 [&](const std::pair<std::string, std::string>& p) {
                                     return p.first == site_key;
                                 }),
                  per.end());
        per.emplace_back(site_key, cat.id);
    }
}

// 所有站点首页到齐后，按"共有站点数"稳定排序并把页签裁剪到 ≤10。
// 只在此处做一次，顶部分类页签不会再中途乱跳 / 反复出现同名。
void FinalizeAggVideoCategories(BrowserState* state) {
    std::stable_sort(state->video_agg_categories.begin(),
                     state->video_agg_categories.end(),
                     [](const BrowserState::VideoAggCategory& a,
                        const BrowserState::VideoAggCategory& b) {
                         return a.per_site.size() > b.per_site.size();
                     });
    if (state->video_agg_categories.size() > 10)
        state->video_agg_categories.resize(10);
}

// 批次并入/替换（静默刷新时同站点新数据直接覆盖旧批次，界面无缝更新）。
void UpsertAggBatch(std::vector<BrowserState::VideoAggBatch>& batches,
                    VideoHomePayload* payload) {
    BrowserState::VideoAggBatch batch;
    batch.site_key = payload->site_key;
    batch.site_name = payload->site_name;
    batch.items = std::move(payload->items);
    for (auto& b : batches) {
        if (b.site_key == batch.site_key) {
            b.site_name = batch.site_name;
            b.items = std::move(batch.items);
            return;
        }
    }
    batches.push_back(std::move(batch));
}

void StartLoadAggVideoCategory(BrowserState* state, int cat_index);

// 视频首页聚合：并发抓取所有 CMS 站点的首页内容，UI 端合并成一条
// 「多源推荐流」（影视仓式统一推荐网格），批次到达即渐进填充，不按站点分段。
// 软件启动后自动刷新一次已导入的 TVBox 源。配置源随时会换内容、换加密方式，
// 这里走的是与「首次导入」完全相同的下载 + 自动解密链，成功才写回缓存。
//
// 线程安全要点：后台线程不碰窗口状态（窗口可能随时关闭），而是用复制出来的
// 目录与网络配置另建一个独立管理器；写盘完成后只 PostMessage 一个 HWND。
// 进程内只触发一次，避免每次打开音乐窗都重新联网。
void MaybeRefreshVideoSources(BrowserState* state) {
    static std::atomic<bool> s_started{false};
    if (!state || !state->video_mgr) return;
    if (s_started.exchange(true)) return;
    const HWND hwnd = state->hwnd;
    const std::filesystem::path dir = state->video_mgr->cache_dir();
    const auto net = state->video_mgr->network();
    std::thread([hwnd, dir, net]() {
        try {
            ttplayer::video::VideoSourceManager mgr(dir);
            mgr.SetNetworkConfig(net);
            mgr.LoadCached();
            const auto rep = mgr.RefreshAll();
            if (rep.updated > 0)
                PostMessageW(hwnd, kMsgVideoSourcesRefreshed, 0, 0);
        } catch (...) {
            // 网络异常 / 源失效：保留旧缓存，下一轮启动再试。
        }
    }).detach();
}

void StartLoadVideo(BrowserState* state, bool force) {
    if (!state) return;
    // 懒创建兜底：窗口创建期的 video_mgr 初始化可能被更早的启动消息抢先，
    // 也可能因宿主状态没就绪而整段跳过。视频页真正要取数据时再补一次，
    // 保证 StartLoadVideo 不会因为 mgr 为空而静默 return（表现为永远占位）。
    if (!state->video_mgr) {
        std::wstring exe_w(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, exe_w.data(),
                                           static_cast<DWORD>(exe_w.size()));
        if (n > 0) {
            exe_w.resize(n);
            const std::filesystem::path exe_dir =
                std::filesystem::path(exe_w).parent_path();
            state->video_mgr =
                std::make_unique<ttplayer::video::VideoSourceManager>(
                    exe_dir / "video");
            ttplayer::musicfree::NetworkConfig net;
            if (state->mgr) net = state->mgr->network();
            if (net.user_agent.empty())
                net.user_agent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                 L"AppleWebKit/537.36 (KHTML, like Gecko) "
                                 L"Chrome/124.0.0.0 Safari/537.36";
            state->video_mgr->SetNetworkConfig(std::move(net));
            state->video_mgr->LoadCached();
            state->video_sources_ready = true;
            TraceImport("video_mgr lazy-created");
        }
    }
    TraceImport("StartLoadVideo mgr=%d busy=%d force=%d",
                state->video_mgr ? 1 : 0,
                state->video_busy.load() ? 1 : 0, force ? 1 : 0);
    if (!state->video_mgr) return;
    if (state->video_busy.exchange(true)) return;
    if (state->video_loaded && !force) {
        ReleaseVideoBusy(state);
        return;
    }
    state->selected_video_group = -1;
    state->selected_video_item = -1;
    state->video_art_requested.clear();
    state->video_loaded = false;
    state->video_agg_batches.clear();
    state->video_cat_batches.clear();
    state->video_agg_categories.clear();
    state->video_active_cat = -1;
    state->video_cat_expected = 0;
    // 聚合推荐组永远在最前（影视仓首页 = 推荐流优先），直播卡片垫底。
    state->video_groups.clear();
    state->video_rows.clear();
    BrowserState::VideoGroup agg;
    agg.site_key = "agg:";
    agg.site_name = "推荐";
    state->video_groups.push_back(std::move(agg));
    // 直播源卡片先铺上（纯本地数据，不发请求）。就算所有 CMS 站点都取不到
    // 内容，页面上至少有「直播源」卡片可点，不再是一排空占位。
    AddLiveSourceCards(state);
    state->video_placeholder = false;
    state->video_placeholder_text = L"正在准备…";
    FillVideoList(state);
    SetVideoStatus(state, L"正在聚合各视频源的推荐内容…窗口现在即可操作");

    std::weak_ptr<BrowserState> weak = g_browser;
    // 视频后台线程不得持有裸 video_mgr：窗口销毁时 BrowserState（连同
    // video_mgr）立即析构，线程再访问就是 use-after-free（PageHeap 实测在
    // AllSites/network() 的内部锁上触发 AppVerifier stop，随后保存配置时
    // abort）。改为每个访问点 weak.lock() 保活 BrowserState，并纳入退出
    // 协调计数，程序关闭时随 ShutdownMusicFree 一起被等待。
    std::thread([weak] {
        BgThreadGuard guard;
        auto sp = weak.lock();
        if (!sp || !sp->video_mgr || IsMusicFreeShuttingDown()) return;
        auto* mgr = sp->video_mgr.get();
        std::vector<ttplayer::video::VideoSite> sites;
        try { sites = mgr->AllSites(); } catch (...) { return; }
        // 每站点独立线程并发抓取（影视仓的爬虫也是 spThreadPool 并发跑）：
        // 串行 4 站要 4 倍时间，聚合流会被最慢的源拖住。
        std::vector<std::thread> workers;
        for (const auto& site : sites) {
            workers.emplace_back([weak, site] {
                BgThreadGuard worker_guard;
                auto* batch = new VideoHomePayload();
                batch->site_key = site.key.empty() ? site.name : site.key;
                batch->site_name = site.name;
                try {
                    auto alive = weak.lock();
                    if (!alive || !alive->video_mgr ||
                        IsMusicFreeShuttingDown()) {
                        batch->error = "正在退出";
                    } else {
                        ttplayer::video::VideoListing listing =
                            ttplayer::video::FetchHome(
                                site, alive->video_mgr->network());
                        batch->items = std::move(listing.items);
                        batch->categories = std::move(listing.categories);
                        batch->error = listing.error;
                    }
                } catch (const std::exception& e) {
                    batch->error = e.what();
                } catch (...) {
                    batch->error = "未知错误";
                }
                TraceImport("home site=%s items=%zu err=%s", batch->site_key.c_str(),
                            batch->items.size(), batch->error.c_str());
                auto poster = weak.lock();
                if (!poster) { delete batch; return; }
                if (!PostMessageW(poster->hwnd, kMsgVideoHomeReady, 0,
                                  reinterpret_cast<LPARAM>(batch)))
                    delete batch;
            });
        }
        for (auto& t : workers) t.join();
        auto* done = new VideoHomePayload();
        done->finished = true;
        done->total = static_cast<int>(sites.size());
        auto done_poster = weak.lock();
        if (!done_poster) { delete done; return; }
        if (!PostMessageW(done_poster->hwnd, kMsgVideoHomeReady, 0,
                          reinterpret_cast<LPARAM>(done)))
            delete done;
    }).detach();
}

// 分类页签点击（聚合版）：cat_index = -1 是「推荐」，本地重算混排秒回首页；
// 其它页签对每个有该分类的站点并发拉第一页，批次到齐后重排聚合流。
void StartLoadAggVideoCategory(BrowserState* state, int cat_index) {
    if (!state || !state->video_mgr) return;
    if (cat_index < -1 ||
        cat_index >= static_cast<int>(state->video_agg_categories.size()))
        return;
    if (cat_index == state->video_active_cat) return;  // 重复点击当前页签
    if (state->video_busy.exchange(true)) {
        // 不再静默丢弃：记下用户点的分类，当前任务结束（ReleaseVideoBusy）
        // 后自动执行。以前首页聚合动辄几十秒，期间所有分类点了都没反应。
        state->video_pending_cat.store(cat_index);
        // 立即给可见反馈：清掉首页渐进数据、整块网格换成「加载中」骨架——
        // 用户点了分类立刻看到切换动作，而不是对着旧内容干等几十秒。
        // 首页聚合剩余批次到达时会照常走 finished 分支复位 busy 并执行本分类。
        for (auto& g : state->video_groups) {
            if (g.site_key != "agg:") continue;
            g.items.clear();
            g.active_category_name.clear();
            break;
        }
        state->video_agg_batches.clear();
        state->video_active_cat = cat_index;
        state->video_placeholder = true;
        state->video_placeholder_text =
            cat_index < 0 ? L"正在返回推荐流…" : L"正在加载所选分类…";
        state->video_loaded = false;
        FillVideoList(state);
        SetVideoStatus(state, L"正在加载，已记下分类切换，完成后自动执行…");
        return;
    }
    BrowserState::VideoGroup* agg = nullptr;
    for (auto& g : state->video_groups)
        if (g.site_key == "agg:") { agg = &g; break; }
    if (!agg) { ReleaseVideoBusy(state); return; }  // 搜索中聚合组被暂替
    state->video_active_cat = cat_index;
    state->video_cat_batches.clear();
    if (cat_index < 0) {
        // 「推荐」：首页批次还在内存里，直接重排，不联网。
        agg->category_loading = false;
        agg->active_category.clear();
        agg->active_category_name = L"推荐";
        RebuildAggregatedVideoItems(state);
        FillVideoList(state);
        SetVideoStatus(state, L"已恢复多源推荐流");
        ReleaseVideoBusy(state);
        return;
    }
    const BrowserState::VideoAggCategory& target =
        state->video_agg_categories[cat_index];
    agg->category_loading = true;
    agg->active_category = target.name;
    agg->active_category_name = Utf8ToWideString(target.name);
    RebuildAggregatedVideoItems(state);   // 批次清空 → 先展示空网格
    FillVideoList(state);
    if (state->video_navbar)
        InvalidateRect(state->video_navbar, nullptr, FALSE);  // 高亮当前页签
    SetVideoStatus(state, L"正在聚合「" + agg->active_category_name +
                              L"」分类（" +
                              std::to_wstring(target.per_site.size()) +
                              L" 个片源）…");

    // 复制站点表快照：后台线程各取各的 id，互不依赖。
    std::vector<std::pair<ttplayer::video::VideoSite, std::string>> jobs;
    try {
        for (const auto& s : state->video_mgr->AllSites()) {
            const std::string key = s.key.empty() ? s.name : s.key;
            for (const auto& [site_key, cat_id] : target.per_site)
                if (site_key == key) { jobs.emplace_back(s, cat_id); break; }
        }
    } catch (...) {}
    state->video_cat_expected = static_cast<int>(jobs.size());
    if (jobs.empty()) {
        agg->category_loading = false;
        SetVideoStatus(state, L"该分类没有可用的片源");
        ReleaseVideoBusy(state);
        return;
    }
    std::weak_ptr<BrowserState> weak = g_browser;
    std::vector<std::thread> workers;
    for (const auto& [site, cat_id] : jobs) {
        workers.emplace_back([weak, site, cat_id] {
            BgThreadGuard guard;
            auto* batch = new VideoHomePayload();
            batch->category_load = true;
            batch->site_key = site.key.empty() ? site.name : site.key;
            batch->site_name = site.name;
            try {
                auto alive = weak.lock();
                if (!alive || !alive->video_mgr || IsMusicFreeShuttingDown()) {
                    batch->error = "正在退出";
                } else {
                    ttplayer::video::VideoListing listing =
                        ttplayer::video::FetchCategory(
                            site, cat_id, 1, alive->video_mgr->network());
                    batch->items = std::move(listing.items);
                    batch->error = listing.error;
                }
            } catch (const std::exception& e) {
                batch->error = e.what();
            } catch (...) {
                batch->error = "未知错误";
            }
            TraceImport("category site=%s cat=%s items=%zu err=%s",
                        batch->site_key.c_str(), cat_id.c_str(),
                        batch->items.size(), batch->error.c_str());
            auto poster = weak.lock();
            if (!poster) { delete batch; return; }
            if (!PostMessageW(poster->hwnd, kMsgVideoHomeReady, 0,
                              reinterpret_cast<LPARAM>(batch)))
                delete batch;
        });
    }
    for (auto& t : workers) t.detach();   // 批次各自回帖，最后一个到时复位 busy
}

// 直播源卡片：一个 live 源一张卡，全部是本地数据、不联网，点开才去拉频道表。
//
// 为什么必须放进首页：现在大量 TVBox 仓库整站都是 SPIDER（api 写成
// csp_XxxGuard 这种类名，要 TVBox 的 JS/Java 运行库才能跑），本程序一个都取不到
// 内容；这类配置里唯一真能播的就是 lives（m3u / txt）。以前首页只剩四张
// 「点击「导入…」」的占位卡，导入明明成功了却像什么都没发生。
void AddLiveSourceCards(BrowserState* state) {
    if (!state || !state->video_mgr) return;
    for (const auto& group : state->video_groups)
        if (group.site_key == "live:") return;   // 已经铺过
    std::vector<ttplayer::video::VideoLive> lives;
    try { lives = state->video_mgr->AllLives(); } catch (...) { return; }
    if (lives.empty()) return;
    BrowserState::VideoGroup group;
    group.site_key = "live:";
    group.site_name = "直播源";
    for (std::size_t i = 0; i < lives.size(); ++i) {
        ttplayer::video::VideoItem item;
        item.id = std::to_string(i);
        item.site_key = "live:" + lives[i].url;   // 前缀标记：点击走直播分支
        item.site_name = lives[i].name;
        item.title = lives[i].name;
        item.remarks = lives[i].group.empty() ? "直播源" : lives[i].group;
        group.items.push_back(std::move(item));
    }
    state->video_groups.push_back(std::move(group));
}

// 打开直播源卡片：拉频道表（m3u / txt），把频道当成「选集」灌进下方列表，
// 双击任意频道由视频窗口直接播放（地址本身就是可播的 m3u8 / flv / ts）。
void StartOpenVideoLive(BrowserState* state, const ttplayer::video::VideoItem& item) {
    if (!state || !state->video_mgr) return;
    const std::string want = item.site_key.size() > 5 ? item.site_key.substr(5)
                                                      : std::string();
    ttplayer::video::VideoLive live;
    try {
        for (const auto& l : state->video_mgr->AllLives())
            if (l.url == want) { live = l; break; }
    } catch (...) {}
    if (live.url.empty()) {
        SetVideoSheetCaption(state, L"找不到这个直播源，可能已被删除；点「导入…」可重新添加");
        return;
    }
    if (!state->video_tracks_visible) {
        state->video_tracks_visible = true;
        LayoutChildren(state);
    }
    if (state->video_detail_busy.exchange(true)) return;
    state->video_hits.clear();
    FillResultList(state);
    SetBusyUi(state, true);
    SetVideoSheetCaption(state, L"直播源「" + Utf8ToWideString(live.name) +
                                   L"」· 正在读取频道表…");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, live] {
        BgThreadGuard guard;
        auto* payload = new VideoDetailPayload();
        payload->title = live.name;
        payload->site_name = live.name;
        payload->live = true;
        try {
            auto alive = weak.lock();
            if (!alive || !alive->video_mgr || IsMusicFreeShuttingDown()) {
                payload->error = "正在退出";
            } else {
                ttplayer::video::LiveListing listing =
                    ttplayer::video::FetchLive(
                        live, alive->video_mgr->network());
                payload->error = listing.error;
                ttplayer::video::VideoLine line;
                line.name = live.group.empty() ? "直播" : live.group;
                for (auto& channel : listing.channels) {
                    ttplayer::video::VideoEpisode ep;
                    ep.name = channel.name.empty() ? channel.url : channel.name;
                    ep.url = channel.url;
                    line.episodes.push_back(std::move(ep));
                }
                if (!line.episodes.empty()) payload->lines.push_back(std::move(line));
            }
        } catch (const std::exception& e) {
            payload->error = e.what();
        } catch (...) {
            payload->error = "直播频道读取异常";
        }
        auto poster = weak.lock();
        if (!poster) { delete payload; return; }
        if (!PostMessageW(poster->hwnd, kMsgVideoDetailReady, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 打开选中的视频卡片：取详情（多路线路选集）→ 灌进下方结果列表。
void StartOpenVideoItem(BrowserState* state) {
    if (!state || !state->video_mgr) return;
    if (state->selected_video_item < 0) {
        SetVideoSheetCaption(state, L"请先点击一张视频卡片查看选集");
        return;
    }
    const ttplayer::video::VideoItem* pitem = nullptr;
    if (state->selected_video_group == -2) {  // 影视仓式分区卡片
        if (state->selected_video_item <
            static_cast<int>(state->video_home_items.size()))
            pitem = &state->video_home_items[state->selected_video_item];
    } else if (state->selected_video_group >= 0 &&
               state->selected_video_group <
                   static_cast<int>(state->video_groups.size())) {
        const auto& items = state->video_groups[state->selected_video_group].items;
        if (state->selected_video_item < static_cast<int>(items.size()))
            pitem = &items[state->selected_video_item];
    }
    if (!pitem) {
        SetVideoSheetCaption(state, L"请先点击一张视频卡片查看选集");
        return;
    }
    ttplayer::video::VideoItem item = *pitem;
    if (item.site_key.rfind("live:", 0) == 0) {
        StartOpenVideoLive(state, item);
        return;
    }
    ttplayer::video::VideoSite site;
    for (const auto& s : state->video_mgr->AllSites())
        if ((s.key.empty() ? s.name : s.key) == item.site_key) { site = s; break; }
    if (!state->video_tracks_visible) {
        state->video_tracks_visible = true;
        LayoutChildren(state);
    }
    if (state->video_detail_busy.exchange(true)) return;
    state->video_hits.clear();
    FillResultList(state);
    SetBusyUi(state, true);
    const std::wstring title = Utf8ToWideString(item.title);
    SetVideoSheetCaption(state, L"视频「" + title + L"」· 正在加载选集（多路线路）…");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, item, site] {
        BgThreadGuard guard;
        auto* payload = new VideoDetailPayload();
        payload->title = item.title;
        payload->site_name = site.name;
        payload->play_url = site.play_url;
        try {
            auto alive = weak.lock();
            if (!alive || !alive->video_mgr || IsMusicFreeShuttingDown()) {
                payload->error = "正在退出";
            } else {
                ttplayer::video::VideoDetail detail =
                    ttplayer::video::FetchDetail(
                        site, item, alive->video_mgr->network());
                payload->title = detail.title;
                payload->lines = std::move(detail.lines);
            }
        } catch (const std::exception& e) {
            payload->error = e.what();
        } catch (...) {
            payload->error = "详情读取异常";
        }
        auto poster = weak.lock();
        if (!poster) { delete payload; return; }
        if (!PostMessageW(poster->hwnd, kMsgVideoDetailReady, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 视频搜索：跨站点 FetchSearch 聚合，结果直接进卡片网格。
void StartVideoSearch(BrowserState* state) {
    if (!state || !state->video_mgr) return;
    const std::wstring q = WindowText(state->query);
    if (q.empty()) { SetVideoStatus(state, L"请输入要搜索的影视名称"); return; }
    const std::string keyword = core::WideToUtf8(q);
    if (state->video_busy.exchange(true)) {
        SetVideoStatus(state, L"视频搜索进行中，请稍候");
        return;
    }
    state->video_groups.clear();
    state->video_rows.clear();
    state->video_active_cat = -1;
    state->video_cat_batches.clear();
    state->video_placeholder = false;
    FillVideoList(state);
    SetBusyUi(state, true);
    SetVideoStatus(state, L"正在跨视频源搜索：「" + q + L"」…");

    std::weak_ptr<BrowserState> weak = g_browser;
    std::thread([weak, keyword] {
        BgThreadGuard guard;
        auto* payload = new VideoSearchPayload();
        std::vector<ttplayer::video::VideoSite> sites;
        try {
            auto alive = weak.lock();
            if (alive && alive->video_mgr && !IsMusicFreeShuttingDown())
                sites = alive->video_mgr->AllSites();
        } catch (...) {}
        payload->sites_total = static_cast<int>(sites.size());
        for (const auto& site : sites) {
            try {
                auto alive = weak.lock();
                if (!alive || !alive->video_mgr || IsMusicFreeShuttingDown())
                    break;
                ttplayer::video::VideoListing listing =
                    ttplayer::video::FetchSearch(site, keyword, 1,
                                                 alive->video_mgr->network());
                for (auto& it : listing.items) {
                    it.site_key = site.key.empty() ? site.name : site.key;
                    it.site_name = site.name;
                    payload->items.push_back(std::move(it));
                }
                if (listing.items.empty() && listing.error.empty())
                    payload->sites_ok++;
                else
                    payload->sites_ok++;
            } catch (...) {}
        }
        auto poster = weak.lock();
        if (!poster) { delete payload; return; }
        if (!PostMessageW(poster->hwnd, kMsgVideoSearchReady, 0,
                          reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

// 紧凑模态输入框（无需 RC 资源）：用于粘贴影视仓 / TVBox 配置地址。
//
// ⚠ 已停用（2026-11-07）：这个自建窗口类在本进程里 CreateWindowExW 恒失败，
// 返回 NULL + GetLastError=1400（ERROR_INVALID_WINDOW_HANDLE）—— 已确认类注册
// 成功（GetClassInfoEx 能查到）、同一时刻建系统类 STATIC 窗口正常、换样式/换
// owner/owner=nullptr 都一样失败。表现就是点了「导入…」毫无反应。视频源导入
// 因此改成**不新建窗口**的方案：把底部状态行临时变成地址输入栏。
#if 0
namespace {
struct InputBoxCtx {
    HWND edit = nullptr;
    std::wstring value;
    std::wstring prompt;
    bool confirmed = false;
};
LRESULT CALLBACK InputBoxProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* ctx = reinterpret_cast<InputBoxCtx*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            ctx = reinterpret_cast<InputBoxCtx*>(
                reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            return DefWindowProcW(hwnd, msg, w, l);
        }
        case WM_CREATE: {
            HWND label = CreateWindowW(L"STATIC", ctx->prompt.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 12, 396, 24, hwnd, nullptr,
                nullptr, nullptr);
            if (label)
                SendMessageW(label, WM_SETFONT,
                             reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            ctx->edit = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                12, 40, 396, 26, hwnd, reinterpret_cast<HMENU>(1), nullptr, nullptr);
            if (ctx->edit) {
                SendMessageW(ctx->edit, WM_SETFONT,
                             reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
                SetWindowTextW(ctx->edit, ctx->value.c_str());
                SetFocus(ctx->edit);
            }
            CreateWindowW(L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 232, 92, 86, 28, hwnd,
                reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE, 326, 92, 86, 28, hwnd,
                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDOK) {
                wchar_t b[4096]{};
                if (ctx->edit) GetWindowTextW(ctx->edit, b, 4096);
                ctx->value = b;
                ctx->confirmed = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(w) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
} // namespace

bool GetInputText(HWND owner, const std::wstring& caption,
                 const std::wstring& prompt, wchar_t* out, size_t out_len) {
    InputBoxCtx ctx;
    ctx.prompt = prompt;
    static const wchar_t* cls = L"TTPlayerInputBox";
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = InputBoxProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (RegisterClassExW(&wc) ||
            GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            reg = true;
        else {
            TraceImport("GetInputText register class failed err=%lu", GetLastError());
            return false;
        }
    }
    if (owner) EnableWindow(owner, FALSE);

    // 居中到 owner。原来固定放在 (0,0)：窗口会跑到屏幕左上角，离音乐窗很远，
    // 用户很容易以为「没反应」。
    RECT anchor{0, 0, 0, 0};
    if (owner) GetWindowRect(owner, &anchor);
    const int w = 420, h = 170;
    const int x = anchor.left + ((anchor.right - anchor.left) - w) / 2;
    const int y = anchor.top + ((anchor.bottom - anchor.top) - h) / 2;

    // 先试「带标题栏的对话框外观」。★ 这里**不能**用 WS_EX_TOPMOST：
    // CreateWindowEx 的扩展样式里没有这一项（置顶要用 SetWindowPos），带上它
    // 会让窗口创建直接失败（GetLastError 返回 1400），于是点了「导入」毫无反应。
    // 万一某些系统上带标题栏的 popup 也建不出来，再退回无边框样式，保证可用。
    const auto create_with = [&](DWORD style, DWORD ex_style, HWND parent) {
        ctx.edit = nullptr;
        SetLastError(0);
        HWND handle = CreateWindowExW(ex_style, cls, caption.c_str(), style,
                                      x, y, w, h, parent, nullptr,
                                      GetModuleHandleW(nullptr), &ctx);
        TraceImport("GetInputText create style=%08lx ex=%08lx parent=%p -> %p err=%lu",
                    style, ex_style, parent, handle, GetLastError());
        return handle;
    };
    TraceImport("GetInputText owner=%p iswindow=%d hinst=%p", owner,
                (owner && IsWindow(owner)) ? 1 : 0, GetModuleHandleW(nullptr));
    {
        WNDCLASSEXW info{sizeof(info)};
        const BOOL have = GetClassInfoExW(GetModuleHandleW(nullptr), cls, &info);
        SetLastError(0);
        HWND probe = CreateWindowExW(0, L"STATIC", L"probe",
                                     WS_POPUP | WS_VISIBLE, 0, 0, 20, 20,
                                     nullptr, nullptr, GetModuleHandleW(nullptr),
                                     nullptr);
        TraceImport("probe: class_registered=%d err_info=%lu system_class_hwnd=%p err=%lu",
                    have ? 1 : 0, GetLastError(), probe, GetLastError());
        if (probe) DestroyWindow(probe);
    }
    HWND hwnd = create_with(WS_POPUP | WS_CLIPCHILDREN | WS_CAPTION | WS_SYSMENU,
                            WS_EX_TOOLWINDOW, owner);
    if (!hwnd)
        hwnd = create_with(WS_POPUP | WS_CLIPCHILDREN, WS_EX_TOOLWINDOW, owner);
    if (!hwnd)
        hwnd = create_with(WS_POPUP | WS_CLIPCHILDREN | WS_CAPTION | WS_SYSMENU,
                           WS_EX_TOOLWINDOW, nullptr);
    if (!hwnd) { if (owner) EnableWindow(owner, TRUE); return false; }
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    MSG m;
    while (IsWindow(hwnd) && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (owner && IsWindow(owner)) EnableWindow(owner, TRUE);
    if (ctx.confirmed && out && out_len > 0) {
        wcsncpy_s(out, out_len, ctx.value.c_str(), _TRUNCATE);
        return true;
    }
    return false;
}
#endif  // 停用的自建模态输入框

// ------------------------------------------------- 视频源导入（模态对话框）
//
// 与音乐侧「导入音源」完全同款：点「导入…」弹出一个和音乐窗同一个皮肤外框的
// 模态窗口（ttplayer::video::ShowVideoSourceDialog），里面既能粘贴影视仓 /
// TVBox 地址导入，也能看到**已经导入过的**每一份配置及其站点清单（点一行即
// 显示详情），还能复选删除。
//
// 早前为了绕开自建窗口创建失败（GetLastError=1400）改成过「把底部状态行临时
// 变成输入框」的页面内方案，那样既看不到已导入的源、也没法删。视频源对话框
// 复用音乐导入窗那套已在真机验证过的建窗方式（WS_POPUP + WS_EX_TOOLWINDOW），
// 因此回到对话框形态。

void OpenVideoSourceDialog(BrowserState* state) {
    if (!state || !state->video_mgr) return;
    // ★ 这里**不再**因为首页 / 详情正在加载就拒绝打开：管理器内部用互斥锁保护
    // imported_，AllSites() 返回的是值拷贝，并发读写是安全的；而「点了导入却没
    // 反应」正是这类静默早退造成的。真正的并发写（video_groups）用
    // video_reload_pending 推迟到当前聚合结束后再做。
    TraceImport("ShowVideoSourceDialog enter busy=%d detail=%d",
                state->video_busy.load() ? 1 : 0,
                state->video_detail_busy.load() ? 1 : 0);
    const bool changed = ttplayer::video::ShowVideoSourceDialog(
        state->hwnd, state->video_mgr.get(), state->host.skin);
    TraceImport("ShowVideoSourceDialog leave changed=%d", changed ? 1 : 0);
    if (changed) {
        // 站点集合变了：首页要按新表重新聚合。
        state->video_loaded = false;
        if (state->video_busy.load()) {
            state->video_reload_pending = true;
            SetVideoStatus(state, L"已更新视频源，正在按新源重新加载首页…");
        } else {
            StartLoadVideo(state, true);
        }
        return;
    }
    // 没改动也把源情况写回状态行：用户可能只是打开看了一眼已导入的源。
    // 按「真能取到内容的东西」分情况说清楚 —— 全 SPIDER 的仓库说「点卡片查看
    // 剧集」就是在误导，那几张卡片是直播源。
    const auto sources = state->video_mgr->ListSources();
    const size_t count = sources.size();
    if (!count) {
        SetVideoStatus(state, L"尚未导入视频源，点「导入…」添加影视仓 / TVBox 地址");
        return;
    }
    int cms = 0, native = 0, spider = 0, lives = 0;
    for (const auto& src : sources) {
        lives += static_cast<int>(src.lives.size());
        for (const auto& s : src.sites) {
            if (s.kind == ttplayer::video::VideoSourceKind::kCms) ++cms;
            else if (s.kind == ttplayer::video::VideoSourceKind::kNativeSpider) ++native;
            else if (s.kind == ttplayer::video::VideoSourceKind::kSpider) ++spider;
        }
    }
    if (cms > 0 || native > 0) {
        SetVideoStatus(state, L"已导入 " + std::to_wstring(count) +
            L" 个视频源，点卡片查看剧集");
    } else if (lives > 0) {
        SetVideoStatus(state, L"已导入 " + std::to_wstring(count) + L" 个视频源：" +
            std::to_wstring(lives) + L" 个直播源可点开观看" +
            (spider > 0 ? L"（SPIDER 站点需 TVBox 运行库，暂不支持）" : L""));
    } else if (spider > 0) {
        SetVideoStatus(state, L"已导入 " + std::to_wstring(count) + L" 个视频源，但 " +
            std::to_wstring(spider) +
            L" 个站点全是 SPIDER 类型，本程序取不到内容；请换含 CMS 接口（provide/vod）的源");
    } else {
        SetVideoStatus(state, L"已导入 " + std::to_wstring(count) +
            L" 个视频源，但未识别到可播放站点；请检查配置里的 api 是否有效");
    }
}


// ------------------------------------------- WebView2 页面承载 / 运行库自动安装
//
// 以前「系统没装 Runtime」就静默回退旧 Win32 控件，用户永远看不到 WebUI。
// 现在按系统版本选 Runtime（见 webview2_runtime.h），缺失时后台下载官方
// Bootstrapper 静默安装：Win10+ 装最新版，Win7/8/8.1 自动装 109.0.1518.78；
// 装完 PostMessage(kMsgWebView2Installed) 回来重试，本次会话就能切到 WebUI。
void TryAttachWebview(HWND hwnd, BrowserState* state) {
    if (!state) return;
    static const bool kNoWv =
        GetEnvironmentVariableW(L"TTPLAYER_NO_WEBVIEW", nullptr, 0) > 0;
    if (kNoWv || state->webview_failed) return;

    if (!ttplayer::musicfree::WebviewHost::RuntimeAvailable()) {
        // 进程内只启动一次；XP 上 Plan 判为不支持，完全不会下载。
        ttplayer::musicfree::EnsureWebView2RuntimeAsync(hwnd,
                                                        kMsgWebView2Installed);
        return;
    }

    const std::wstring root = ttplayer::musicfree::PrepareWebuiRoot();
    if (root.empty()) return;
    state->bridge = std::make_unique<ttplayer::musicfree::MusicFreeBridge>();
    state->bridge->Register(
        "tab.switch",
        [st = state](const nlohmann::json& payload,
                     ttplayer::musicfree::MusicFreeBridge::ReplyFn reply) {
            if (payload.is_object()) {
                if (const auto it = payload.find("tab");
                    it != payload.end() && it->is_string())
                    st->page_tab = it->get<std::string>();
            }
            reply(true, nlohmann::json::object(), std::string());
        });
    ttplayer::musicfree::RegisterMusicCommands(*state->bridge);
    ttplayer::musicfree::RegisterVideoCommands(*state->bridge);
    ttplayer::musicfree::RegisterMiscCommands(*state->bridge);
    state->webview = std::make_unique<ttplayer::musicfree::WebviewHost>();
    if (state->webview->Create(hwnd, state->layout.content, root.c_str(),
                               state->bridge.get())) {
        HideLegacyContentControls(state, true);  // 页面接管视觉，旧控件只藏不删
    } else {
        state->webview_failed = true;
        state->webview.reset();
        state->bridge.reset();
    }
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK BrowserProc(HWND hwnd, UINT message, WPARAM wparam,
                            LPARAM lparam) {
    BrowserState* state = reinterpret_cast<BrowserState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case kMsgWebView2Installed: {
            // WebView2 运行库后台装好：重新按系统版本选一次并挂载 WebUI，
            // 无需用户重启程序。旧 Win32 控件仍在，失败时原样保留。
            if (state && !state->webview_failed) TryAttachWebview(hwnd, state);
            return 0;
        }
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            state = static_cast<BrowserState*>(create->lpCreateParams);
            if (state) {
                state->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(state));
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        case WM_CREATE: {
            if (!state) return -1;
            state->panel_brush = CreateSolidBrush(kPanel);
            state->video_panel_brush = CreateSolidBrush(kVideoPageBg);
            // 底部提示行：告诉用户在线结果可以直接下载（右键菜单与「下载」按钮
            // 走的是同一条路径）。提示文字不用符号字符，界面字体没有那些字形。
            state->link_text = L"右键结果可下载";

            state->platform = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdPlatform), nullptr,
                nullptr);
            // 不要 WS_EX_CLIENTEDGE：系统凹陷边盖不住也换不掉底色，这里改用
            // PaintFrame 自绘的一像素描边。
            state->query = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdQuery), nullptr,
                nullptr);
            // ⚠ 搜索按钮**故意不带 WS_VISIBLE**：本文件所有子控件的占位创建
            // 位置都是 (0,0,10,10)，靠后续 MoveWindow 定位后再显示。搜索按钮
            // 只在搜索页出现，若带着 WS_VISIBLE 又在别页被 ShowWindow，就没人
            // 定位它 —— 它会以 10x10 的占位尺寸停在窗口左上角，「搜」字被裁成
            // 一小团笔画，看着就是一个「柒」（用户反馈的左上角残影）。
            // 改成创建即隐藏，可见性统一由 LayoutChildren 负责（先定位后显示）。
            state->search_button = CreateWindowW(L"BUTTON", L"搜索",
                WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdSearch), nullptr,
                nullptr);
            state->import_button = CreateWindowW(L"BUTTON", L"导入…",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdImport), nullptr,
                nullptr);
            state->play_button = CreateWindowW(L"BUTTON", L"播放所选",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdPlay), nullptr,
                nullptr);
            state->enqueue_button = CreateWindowW(L"BUTTON", L"添加到列表",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdEnqueue), nullptr,
                nullptr);
            state->download_button = CreateWindowW(L"BUTTON", L"下载",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdDownload), nullptr,
                nullptr);
            // 状态行宽度受「下载 / 导入」按钮挤压，长文案会正好被矩形硬裁在
            // 半个字上（没有省略号，看起来像渲染坏了）。加 SS_ENDELLIPSIS 让它
            // 自己收尾成「…」。
            state->status = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdStatus), nullptr,
                nullptr);
            state->tracks_caption = CreateWindowW(
                L"STATIC", kSheetHint,
                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(kIdTracksCaption), nullptr, nullptr);
            state->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                    LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_OWNERDRAWFIXED,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdList), nullptr,
                nullptr);
            if (!state->list || !state->platform || !state->query) return -1;

            InitializeColumns(state);
            ListView_SetExtendedListViewStyle(state->list,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetBkColor(state->list, kPanel);
            ListView_SetTextBkColor(state->list, kPanel);
            ListView_SetTextColor(state->list, kText);
            EnsureFonts(state);
            for (HWND control : {state->platform, state->query, state->status,
                                 state->list, state->tracks_caption}) {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state->ui_font), TRUE);
            }
            // 曲目区状态条用粗体，加载中 / 失败原因一眼就能看到。
            if (state->tracks_caption)
                SendMessageW(state->tracks_caption, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state->bold_font), TRUE);
            if (HWND header = ListView_GetHeader(state->list)) {
                SendMessageW(header, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state->small_font), TRUE);
            }

            TrackControl(state, state->search_button, kIdSearch, false);
            TrackControl(state, state->import_button, kIdImport, false);
            TrackControl(state, state->play_button, kIdPlay, false);
            TrackControl(state, state->enqueue_button, kIdEnqueue, false);
            TrackControl(state, state->download_button, kIdDownload, false);
            TrackControl(state, state->query, kIdQuery, false);
            TrackControl(state, state->list, kIdList, true);

            // 页签 + 收藏列表 + 标签 + 加载/打开 按钮（排行榜 / 歌单页用）
            state->tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | TCS_TABS | TCS_SINGLELINE |
                    TCS_RAGGEDRIGHT | TCS_FOCUSNEVER,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdTab), nullptr, nullptr);
            state->sheet_list = CreateWindowExW(0, L"ListBox", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWVARIABLE |
                    LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdSheetList), nullptr, nullptr);
            state->tag_combo = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP,
                0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(kIdTagCombo), nullptr, nullptr);
            if (!state->tab || !state->sheet_list) return -1;

            // 三个页签：音乐（原发现，默认）/ 视频 / 搜索
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            const wchar_t* tab_titles[] = {L"音乐", L"视频", L"搜索"};
            for (int i = 0; i < 3; ++i) {
                ti.pszText = const_cast<wchar_t*>(tab_titles[i]);
                TabCtrl_InsertItem(state->tab, i, &ti);
            }
            SendMessageW(state->tab, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state->ui_font), TRUE);
            TabCtrl_SetCurSel(state->tab, 0);

            TrackControl(state, state->sheet_list, kIdSheetList, false);
            TrackControl(state, state->tag_combo, kIdTagCombo, false);
            TrackControl(state, state->load_sheets_button, kIdLoadSheets, false);
            TrackControl(state, state->open_sheet_button, kIdOpenSheet, false);
            TrackControl(state, state->tab, kIdTab, false);

            // 影视仓式外壳：视频页顶部的分类导航行 + 图标功能栏（仅视频页显示，
            // 音乐/搜索页由 LayoutChildren 隐藏）。两者都是自绘 STATIC，走 ChildProc。
            // ⚠ 必须带 SS_NOTIFY：无此样式的 STATIC 对 WM_NCHITTEST 返回
            // HTTRANSPARENT，真实鼠标点击会穿透到父窗口，HandleVideoNavClick
            // 永远收不到 WM_LBUTTONDOWN（表现为「分类无法点击」）。
            state->video_navbar = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_NOTIFY, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(kIdVideoNav), nullptr, nullptr);
            state->video_iconbar = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_NOTIFY, 0, 0, 10, 10, hwnd,
                reinterpret_cast<HMENU>(kIdVideoIcons), nullptr, nullptr);
            if (state->video_navbar)
                TrackControl(state, state->video_navbar, kIdVideoNav, false);
            if (state->video_iconbar)
                TrackControl(state, state->video_iconbar, kIdVideoIcons, false);
            ShowWindow(state->video_navbar, SW_HIDE);
            ShowWindow(state->video_iconbar, SW_HIDE);

            // 封面异步下载 / 解码助手（缓存目录 <exe>/musicfree/artcache）
            if (state->mgr) {
                std::wstring exe(32768, L'\0');
                const DWORD n = GetModuleFileNameW(nullptr, exe.data(),
                                                  static_cast<DWORD>(exe.size()));
                if (n > 0) {
                    exe.resize(n);
                    const std::filesystem::path dir =
                        std::filesystem::path(exe).parent_path() / "musicfree" / "artcache";
                    state->artwork = std::make_unique<ArtworkCache>(dir, state->mgr->network());
                }
            }

            // 视频源管理器（影视仓 / TVBox）：与音乐源完全独立，用户手动导入，
            // 不内置任何视频源。
            //
            // ⚠ 不能依赖 state->mgr：MusicFree 管理器由后台线程挂载，窗口创建
            // 早期可能还没就绪。以前整个创建被 if (state->mgr) 包住，结果
            // video_mgr 为空，StartLoadVideo 直接 return，视频页永远只剩
            // 「正在准备…」占位卡片（trace: video_start ... groups=0）。
            {
                std::wstring exe_w(32768, L'\0');
                const DWORD n2 = GetModuleFileNameW(nullptr, exe_w.data(),
                                                   static_cast<DWORD>(exe_w.size()));
                if (n2 > 0) {
                    exe_w.resize(n2);
                    const std::filesystem::path exe_dir =
                        std::filesystem::path(exe_w).parent_path();
                    state->video_mgr =
                        std::make_unique<ttplayer::video::VideoSourceManager>(
                            exe_dir / "video");
                    // 视频源请求换浏览器 UA：默认的
                    // "Mozilla/5.0 (compatible; TTPlayerMusicFree/0.1)" 会被不少
                    // 直播源 / CMS 站直接 403（实测 gh.927223.xyz）。
                    {
                        ttplayer::musicfree::NetworkConfig net;
                        if (state->mgr) net = state->mgr->network();
                        if (net.user_agent.empty())
                            net.user_agent = L"Mozilla/5.0 (Windows NT 10.0; "
                                             L"Win64; x64) AppleWebKit/537.36 "
                                             L"(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
                        state->video_mgr->SetNetworkConfig(std::move(net));
                    }
                    state->video_mgr->LoadCached();
                    state->video_sources_ready = true;
                }
            }

            // 发现页内容缓存：先读进程内缓存，没有再读磁盘。纯本地文件 IO
            // （几十~几百 KB），窗口创建时同步做完，首帧就是真实卡片。
            state->discover_cache_path = DiscoverCachePath();
            {
                DiscoverCacheData cached;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(g_discover_cache_mutex);
                    if (!g_discover_cache.groups.empty() &&
                        g_discover_cache.saved_at > 0) {
                        cached = g_discover_cache;
                        found = true;
                    }
                }
                if (!found)
                    found = ReadDiscoverCacheFile(state->discover_cache_path, &cached);
                if (found && !cached.groups.empty()) {
                    const std::int64_t now = NowUnixSeconds();
                    state->groups = cached.groups;  // 复制：cached 还要回填进程内缓存
                    state->discover_cache_saved_at = cached.saved_at;
                    state->discover_cache_fresh =
                        cached.saved_at <= now &&
                        (now - cached.saved_at) < kDiscoverCacheTtlSeconds;
                    state->discover_placeholder = false;
                    FillDiscoverList(state);
                    {
                        std::lock_guard<std::mutex> lock(g_discover_cache_mutex);
                        if (g_discover_cache.groups.empty())
                            g_discover_cache = std::move(cached);
                    }
                }
            }

            // 只布置控件，不做任何插件 / 网络调用：窗口先显示出来。
            // 缓存未过期时 kMsgDiscoverStart 直接沿用上面的卡片，完全不联网。
            ApplyMode(state);
            // 保留创建时给定的尺寸（可能是用户上次调好的大小），只做最小尺寸夹取。
            ApplyFrameMetrics(state, false);
            // 列数随窗口宽度重算，重载一次绘制行让缓存卡片按实际列数排布。
            if (!state->groups.empty()) FillDiscoverList(state);
            SetStatusFor(state, false, state->groups.empty()
                ? L"正在后台准备各音源的榜单与推荐…窗口现在即可操作"
                : L"已载入本地缓存的榜单与推荐，正在检查是否需要更新…");
            WatchSourcesReady(state);
            // 「在线搜索当前曲目」这类入口带关键词进来时切到搜索页，
            // 搜索本身仍在工作线程，不会卡住窗口。
            if (!state->initial_query.empty()) {
                TabCtrl_SetCurSel(state->tab, 1);
                OnTabChanged(state, 1);
                SetWindowTextW(state->query, state->initial_query.c_str());
                StartSearch(state);
            } else {
                SetFocus(state->sheet_list);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure && measure->CtlType == ODT_LISTVIEW) {
                measure->itemHeight = kRowHeight;
                return TRUE;
            }
            if (measure && measure->CtlType == ODT_LISTBOX &&
                measure->CtlID == kIdSheetList) {
                measure->itemHeight = kCardRowHeight;  // 逐行高度由 LB_SETITEMHEIGHT 指定
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (!item || !state) break;
            if (item->CtlType == ODT_LISTVIEW) {
                DrawResultRow(state, *item);
                return TRUE;
            }
            if (item->CtlType == ODT_BUTTON) {
                DrawOwnerButton(state, *item);
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == kIdSheetList) {
                DrawDiscoverRow(state, *item);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            if (!state || !state->panel_brush) break;
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, kPanel);
            SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(state->panel_brush);
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (dc && state) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const int width = std::max(1L, client.right - client.left);
                const int height = std::max(1L, client.bottom - client.top);
                // 框体是一整幅位图 + 多个按钮帧，逐次直接画会闪；先画进内存位图再
                // 一次性 BitBlt。
                const HDC buffer = CreateCompatibleDC(dc);
                const HBITMAP canvas = buffer
                    ? CreateCompatibleBitmap(dc, width, height) : nullptr;
                if (buffer && canvas) {
                    const HGDIOBJ old_bitmap = SelectObject(buffer, canvas);
                    PaintFrame(state, buffer);
                    BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
                    SelectObject(buffer, old_bitmap);
                }
                if (canvas) DeleteObject(canvas);
                if (buffer) DeleteDC(buffer);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {
            if (!state) return HTCLIENT;
            // WM_NCHITTEST 的坐标是屏幕坐标，必须先转成客户区。
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            // 框体按钮优先于缩放带（关闭按钮离右边缘只有几个像素）。
            if (FrameHitTest(state, point) != kFrameHitNone) return HTCLIENT;
            return ResizeHitTest(state, point);
        }
        case WM_SIZING: {
            // 拖动缩放中：拖下边时把底部吸附到其它 TTPlayer 窗口的底部，
            // 与播放列表 / 主窗拼齐（用户反馈「下方缩放要跟其他窗口底部对齐」）。
            // 返回 TRUE 表示已处理（lparam 指向的 RECT 即新窗口矩形）。
            if (state && lparam) {
                auto* rc = reinterpret_cast<RECT*>(lparam);
                const int edge = static_cast<int>(wparam);
                if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT ||
                    edge == WMSZ_BOTTOMRIGHT)
                    SnapBrowserBottomEdge(state, *rc);
                return TRUE;
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            if (!state) break;
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const SIZE minimum = MinFrameSize(state);
            info->ptMinTrackSize.x = minimum.cx;   // 皮肤自然宽度：只允许拉宽
            info->ptMinTrackSize.y = minimum.cy;
            // 不设 ptMaxTrackSize.x：用户可自由拖宽 / 拖高窗口。
            // 宽度下限保留在自然宽度，避免把标题栏「千千音乐窗」压变形；
            // 拉宽时的文字变形由 PaintFrame 的 1:1 标题区重绘消除。
            return 0;
        }
        case WM_MOVE:
            // 位置/大小变化就回写宿主（主窗侧有 800ms 合并落盘）：这样即使用
            // 「最大化/还原」或程序化移动，下次启动也能回到用户最后看到的样子。
            if (state && state->host.store_bounds) {
                RECT bounds{};
                GetWindowRect(hwnd, &bounds);
                if (bounds.right > bounds.left && bounds.bottom > bounds.top)
                    state->host.store_bounds(bounds);
            }
            break;
        case WM_SIZE: {
            // 缩放时跟着重排：内容区、右侧锚定的框体按钮、底部行都要按新客户区
            // 重新算一遍，窗口区域也要同步（否则拉伸后的边角会被旧掩码裁掉）。
            if (wparam != SIZE_MINIMIZED) {
                ApplyFrameMetrics(state, false);
                // 记住「用户调过大小」——换肤时就不再强行拉回新皮肤的自然尺寸。
                if (state && !state->applying_frame) {
                    const SIZE natural = NaturalFrameSize(state);
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    if (client.right - client.left != natural.cx ||
                        client.bottom - client.top != natural.cy)
                        state->user_resized = true;
                }
                if (state && state->host.store_bounds) {
                    RECT bounds{};
                    GetWindowRect(hwnd, &bounds);
                    if (bounds.right > bounds.left && bounds.bottom > bounds.top)
                        state->host.store_bounds(bounds);
                }
            }
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            // 拖动缩放结束：再对齐一次并把新的宽高一并记进设置，下次打开就是
            // 用户选的大小（不再被位图尺寸写死）。
            ApplyFrameMetrics(state, false);
            if (state && state->host.store_bounds) {
                RECT bounds{};
                GetWindowRect(hwnd, &bounds);
                if (bounds.right > bounds.left && bounds.bottom > bounds.top)
                    state->host.store_bounds(bounds);
            }
            // 缩放带跟着新客户区走，顺便把四条边的命中结果留一份证据。
            TraceResizeBands(state);
            return 0;
        }
        case WM_SETCURSOR: {
            if (reinterpret_cast<HWND>(wparam) != hwnd || LOWORD(lparam) != HTCLIENT)
                break;
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            const bool hand = state && FrameHitTest(state, point) != kFrameHitNone;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_MOUSEMOVE: {
            if (!state) break;
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (state->dragging) {
                ContinueDrag(state, point);
                return 0;
            }
            const int hit = FrameHitTest(state, point);
            if (hit != state->frame_hover) {
                state->frame_hover = hit;
                InvalidateFrame(state);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (state && state->frame_hover != kFrameHitNone) {
                state->frame_hover = kFrameHitNone;
                InvalidateFrame(state);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!state) break;
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int hit = FrameHitTest(state, point);
            if (hit != kFrameHitNone) {
                state->frame_pressed = hit;
                InvalidateFrame(state);
                return 0;
            }
            BeginDrag(state, point);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!state) break;
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (state->frame_pressed != kFrameHitNone) {
                const int pressed = state->frame_pressed;
                state->frame_pressed = kFrameHitNone;
                InvalidateFrame(state);
                if (pressed == FrameHitTest(state, point))
                    InvokeFrameHit(state, pressed);
                return 0;
            }
            EndDrag(state);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (state && state->dragging) EndDrag(state);
            return 0;
        case WM_KEYDOWN:
            if (state && wparam == VK_ESCAPE) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_COMMAND: {
            if (!state) break;
            switch (LOWORD(wparam)) {
                case kIdSearch:
                    if (IsVideoMode(state)) StartVideoSearch(state);
                    else StartSearch(state);
                    return 0;
                case kIdPlay:
                    if (IsVideoMode(state)) {
                        // 视频页的播放地址在 raw["url"]，不走音乐源插件解析。
                        PlaySelectedVideoHit(state);
                        return 0;
                    }
                    StartResolve(state, OnlineTrackAction::play);
                    return 0;
                case kIdEnqueue:
                    StartResolve(state, OnlineTrackAction::enqueue);
                    return 0;
                case kIdDownload:
                    StartDownload(state);
                    return 0;
                case kIdImport: {
                    TraceImport("kIdImport video=%d video_busy=%d",
                                IsVideoMode(state) ? 1 : 0,
                                state->video_busy.load() ? 1 : 0);
                    if (IsVideoMode(state)) {
                        OpenVideoSourceDialog(state);
                        return 0;
                    }
                    // 音源集合在导入时会被改写，而搜索线程正在遍历插件表；
                    // 两者不能并发，否则可能出现悬垂指针。
                    if (state->busy.load() || state->discover_busy.load()) {
                        SetStatus(state, L"正在读取音源数据，请稍候再导入音源");
                        return 0;
                    }
                    // 导入对话框自带模态消息循环；导入完成后音源集合可能变化，
                    // 因此回到这里要重建音源下拉框。
                    // 皮肤来源直接转交音乐窗自己的 host.skin：导入窗因此和音乐窗
                    // 用同一个框体（同尺寸、同背景位图、同关闭按钮）。
                    ShowMusicFreeImportDialog(hwnd, state->mgr, state->host.skin);
                    RefreshSourceComboForMode(state);
                    if (state->mgr) {
                        const size_t count = state->mgr->ListPlugins().size();
                        SetStatus(state, count
                            ? L"已导入音源：" + std::to_wstring(count) +
                                  L" 个，可以开始搜索"
                            : L"尚未导入任何音源，请再次点击「导入…」");
                    }
                    // 音源集合可能已变化：缓存立刻失效，后台强制重建并重写缓存。
                    // 已有卡片不清空，慢音源不会让界面闪回骨架。
                    state->discover_cache_fresh = false;
                    StartLoadDiscover(state, true);
                    return 0;
                }
                case kIdLoadSheets:
                    if (IsVideoMode(state)) StartLoadVideo(state, true);
                    else StartLoadDiscover(state, true);
                    return 0;
                case kIdOpenSheet:
                    if (IsVideoMode(state)) StartOpenVideoItem(state);
                    else StartOpenSheet(state);
                    return 0;
                case kIdSheetList: {
                    const int code = HIWORD(wparam);
                    if (code == LBN_SELCHANGE || code == LBN_DBLCLK) {
                        // 视频页：先看是否点在分组标题行的分类页签上（页签点击
                        // 也会触发列表选中变化，所以必须在卡片命中之前判断）。
                        if (IsVideoMode(state)) {
                            int cg = -1, cc = -2;
                            if (HitVideoChip(state, &cg, &cc) && cc >= -1) {
                                // 现在只有聚合组有页签（直播组/搜索组不画 chips）。
                                if (state->video_groups[cg].site_key == "agg:")
                                    StartLoadAggVideoCategory(state, cc);
                                return 0;
                            }
                        }
                        int g = -1, s = -1;
                        if (HitDiscoverCard(state, &g, &s)) {
                            if (IsVideoMode(state)) {
                                state->selected_video_group = g;
                                state->selected_video_item = s;
                            } else {
                                state->selected_group = g;
                                state->selected_sheet = s;
                            }
                            InvalidateRect(state->sheet_list, nullptr, FALSE);
                            // 单击即打开：点了封面卡片就该看到曲目，不再要求双击。
                            if (IsVideoMode(state)) {
                                if (!state->video_detail_busy.load())
                                    StartOpenVideoItem(state);
                                else
                                    SetVideoSheetCaption(state,
                                        L"上一个视频仍在加载选集，请稍候");
                            } else if (!state->busy.load()) {
                                StartOpenSheet(state);
                            } else {
                                SetSheetCaption(state,
                                    L"上一张歌单仍在加载，请等加载完成后再点一次卡片");
                            }
                        }
                    }
                    return 0;
                }
                case kIdTagCombo:
                    return 0;  // 推荐标签已并入发现页自动聚合
                default: break;
            }
            break;
        }
        case WM_NOTIFY: {
            if (!state) break;
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (!header) break;
            if (header->hwndFrom == state->list && header->code == NM_DBLCLK) {
                if (IsVideoMode(state)) {
                    PlaySelectedVideoHit(state);
                    return 0;
                }
                StartResolve(state, OnlineTrackAction::play);
                return 0;
            }
            if (header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
                OnTabChanged(state, static_cast<int>(
                    TabCtrl_GetCurSel(state->tab)));
                return 0;
            }
            const HWND list_header = state->list ? ListView_GetHeader(state->list)
                                                 : nullptr;
            if (list_header && header->hwndFrom == list_header &&
                header->code == NM_CUSTOMDRAW) {
                return HandleHeaderCustomDraw(
                    state, reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
            }
            break;
        }
        case kMsgSearchDone: {
            auto* payload = reinterpret_cast<SearchPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const bool for_search = payload->for_search;
            const size_t result_count = payload->hits.size();
            const bool on_page = for_search ==
                (state->mode == BrowserMode::kSearch);
            if (on_page) state->hits = std::move(payload->hits);
            else (for_search ? state->search_hits : state->discover_hits) =
                std::move(payload->hits);
            const OnlineSearchDiagnostics diagnostics = std::move(payload->diagnostics);
            delete payload;
            state->busy.store(false);
            SetBusyUi(state, false);
            if (on_page) FillResultList(state);

            const std::wstring failed = diagnostics.errors.empty()
                ? std::wstring()
                : Utf8ToWideString(diagnostics.errors.front());
            if (result_count == 0) {
                std::wstring text = L"没有结果（已查询 " +
                    std::to_wstring(diagnostics.sources) + L" 个音源";
                if (!diagnostics.errors.empty())
                    text += L"，其中 " + std::to_wstring(diagnostics.errors.size()) +
                            L" 个报错";
                text += L"）";
                if (!failed.empty()) text += L"：" + failed;
                else if (diagnostics.sources == 0)
                    text += L"，请先用「导入…」添加音源";
                SetStatusFor(state, for_search, text);
            } else {
                std::wstring text = L"找到 " + std::to_wstring(result_count) +
                    L" 条结果（" + std::to_wstring(diagnostics.sources_with_hits) +
                    L"/" + std::to_wstring(diagnostics.sources) +
                    L" 个音源有结果），双击任意一行即可播放";
                if (!failed.empty()) text += L"；部分音源失败：" + failed;
                SetStatusFor(state, for_search, text);
            }
            return 0;
        }
        case kMsgResolveDone: {
            auto* payload = reinterpret_cast<ResolvePayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const OnlineTrackAction action = payload->action;
            const OnlineSearchHit hit = std::move(payload->hit);
            const std::wstring resolved = std::move(payload->url);
            const std::wstring reason = std::move(payload->error);
            delete payload;
            state->busy.store(false);
            SetBusyUi(state, false);
            if (resolved.empty()) {
                // 把音源自己给出的失败原因显示出来，用户才知道该换音源、
                // 换歌曲，还是该去关心会员/地区限制。
                std::wstring text = L"解析失败：该音源没有返回可播放地址";
                if (!reason.empty()) text += L"（" + reason + L"）";
                SetStatus(state, text);
                std::wstring detail =
                    L"无法解析播放地址。\n\n常见原因：音源已失效、需要会员、或该曲目被限制。\n"
                    L"可尝试换一个音源或换一首歌。";
                if (!reason.empty()) detail += L"\n\n音源返回：\n" + reason;
                MessageBoxW(hwnd, detail.c_str(), L"MusicFree 音乐窗",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            // 元数据本身已是 UTF-8；只有解析出的地址需要转码，且转码可能抛，
            // 不能让它逃出窗口过程。
            try {
                TraceDiagnostic("RESOLVED",
                    "action=" + std::string(action == OnlineTrackAction::enqueue
                                   ? "enqueue" : "play") +
                    "\nplatform=" + hit.platform +
                    "\nid=" + hit.id +
                    "\ntitle=" + hit.title +
                    "\nartist=" + hit.artist +
                    "\nalbum=" + hit.album +
                    "\nduration_ms=" + std::to_string(hit.duration_ms) +
                    "\nurl=" + core::WideToUtf8(resolved));
            } catch (const std::exception&) {
            }
            if (!state->on_track) {
                SetStatus(state, L"播放地址已解析，但播放器未接入");
                return 0;
            }
            // 元数据与地址一起交给播放器：列表/标题栏用 hit 里的歌曲名与歌手，
            // 不要用解析出来的 CDN 地址反推文件名。
            const std::wstring label = Utf8ToWideString(hit.title);
            // 打开在线流可能耗时：先把提示推到界面，别让用户以为卡在解析。
            if (action == OnlineTrackAction::play)
                SetStatus(state, L"正在启动播放：" + label + L" …");
            const bool accepted = state->on_track(action, hit, resolved);
            if (!accepted) {
                SetStatus(state, action == OnlineTrackAction::enqueue
                    ? L"加入播放列表失败：" + label
                    : L"播放请求未被接受：" + label);
                return 0;
            }
            SetStatus(state, action == OnlineTrackAction::enqueue
                ? L"已加入播放列表：" + label : L"正在播放…");
            return 0;
        }
        case kMsgDownloadReady: {
            auto* payload = reinterpret_cast<ResolvePayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const OnlineSearchHit hit = std::move(payload->hit);
            const std::wstring url = std::move(payload->url);
            const std::wstring reason = std::move(payload->error);
            delete payload;
            if (url.empty()) {
                state->busy.store(false);
                SetBusyUi(state, false);
                std::wstring text = L"下载失败：该音源没有返回可播放地址";
                if (!reason.empty()) text += L"（" + reason + L"）";
                SetStatus(state, text);
                return 0;
            }
            // 解析出来的地址决定文件扩展名，所以「另存为」必须放在这一步之后。
            const std::wstring name = DefaultDownloadName(hit.title, hit.artist, url);
            const auto directory = DefaultDownloadDirectory();
            std::wstring initial = directory.empty()
                ? name : (directory / name).wstring();
            std::wstring buffer(initial.size() + 64, L'\0');
            std::copy(initial.begin(), initial.end(), buffer.begin());
            const std::wstring directory_text = directory.wstring();

            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = state->hwnd;
            ofn.lpstrFilter =
                L"音频文件\0*.mp3;*.flac;*.m4a;*.aac;*.wav;*.ogg;*.wma\0"
                L"所有文件\0*.*\0";
            ofn.lpstrFile = buffer.data();
            ofn.nMaxFile = static_cast<DWORD>(buffer.size());
            ofn.lpstrInitialDir = directory_text.empty()
                ? nullptr : directory_text.c_str();
            // OFN_NOCHANGEDIR 很关键：不设的话公共对话框会把进程当前目录改掉，
            // 而播放器是按进程当前目录找资源的。
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&ofn)) {
                state->busy.store(false);
                SetBusyUi(state, false);
                SetStatus(state, L"已取消下载");
                return 0;
            }
            BeginDownloadToFile(state, hit, url, buffer.c_str());
            return 0;
        }
        case kMsgDownloadDone: {
            auto* payload = reinterpret_cast<DownloadPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const DownloadPayload result = std::move(*payload);
            delete payload;
            state->busy.store(false);
            SetBusyUi(state, false);
            const std::wstring title = Utf8ToWideString(result.hit.title);
            if (!result.ok) {
                std::wstring text = L"下载失败：" + title;
                if (!result.error.empty()) text += L"（" + result.error + L"）";
                SetStatus(state, text);
                return 0;
            }
            // 保留一位小数的 MB，不引 printf：字节 / 1MiB 的十分之一。
            const std::int64_t tenths = (result.bytes * 10 + 524288) / 1048576;
            const std::wstring size = std::to_wstring(tenths / 10) + L"." +
                std::to_wstring(tenths % 10) + L" MB";
            SetStatus(state, L"已下载 " + title + L"（" + size + L"）→ " +
                             result.path);
            try {
                TraceDiagnostic("DOWNLOADED",
                    "platform=" + result.hit.platform +
                    "\ntitle=" + result.hit.title +
                    "\nartist=" + result.hit.artist +
                    "\nbytes=" + std::to_string(result.bytes) +
                    "\npath=" + core::WideToUtf8(result.path));
            } catch (const std::exception&) {
            }
            return 0;
        }
        case kMsgDiscoverStart:
            // 缓存未过期：这次开窗完全不联网，沿用 WM_CREATE 里已经渲染好的卡片。
            if (state && state->discover_cache_fresh && !state->groups.empty()) {
                state->discover_loaded = true;
                const std::int64_t age_min = std::max<std::int64_t>(0,
                    (NowUnixSeconds() - state->discover_cache_saved_at) / 60);
                SetStatusFor(state, false, L"已从本地缓存显示 " +
                    std::to_wstring(state->groups.size()) +
                    L" 个音源的榜单与推荐（缓存于 " +
                    std::to_wstring(age_min) +
                    L" 分钟前，本次无需重新加载）。点「刷新」可立即更新");
            } else {
                StartLoadDiscover(state, false);
            }
            return 0;
        case kMsgSourcesReady:
            // 后台插件挂载完成：缓存路径不跑网络刷新，靠这条消息把搜索页
            // 的音源下拉框从「缓存名单」换成实时权威名单。
            if (state) {
                state->sources_ready = true;
                RefreshSourceComboForMode(state);
                state->video_sources_ready = true;
                // 若正处于视频页且尚未加载，后台挂载完成后才开始聚合首页。
                if (IsVideoMode(state) && !state->video_loaded &&
                    !state->video_busy.load())
                    PostMessageW(state->hwnd, kMsgVideoStart, 0, 0);
            }
            return 0;
        case kMsgSheetsReady: {
            auto* payload = reinterpret_cast<DiscoverPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            if (!payload->finished) {
                ++state->finished_sources;
                if (!payload->sheets.empty()) {
                    state->discover_placeholder = false;
                    UpsertDiscoverGroup(state, payload->platform,
                                        std::move(payload->sheets));
                    ++state->updated_sources;
                    FillDiscoverList(state);
                } else {
                    // 本轮无数据 / 报错：不进新卡片，但保留缓存里的旧卡片，
                    // 免得一次临时网络故障就把整个发现页清空。
                    ++state->skipped_sources;
                }
                delete payload;
                std::wstring text = L"已展示 " +
                    std::to_wstring(state->groups.size()) + L" 个音源";
                if (state->skipped_sources)
                    text += L"，本轮跳过 " +
                            std::to_wstring(state->skipped_sources) + L" 个无数据音源";
                text += L"，其余仍在后台加载…";
                SetStatusFor(state, false, text);
                return 0;
            }
            // 剪掉缓存里已经被删除 / 卸载的音源分组。
            const std::vector<std::string> platforms = std::move(payload->platforms);
            delete payload;
            if (!platforms.empty() && !state->groups.empty()) {
                const size_t before = state->groups.size();
                state->groups.erase(
                    std::remove_if(state->groups.begin(), state->groups.end(),
                        [&platforms](const BrowserState::DiscoverGroup& g) {
                            return std::find(platforms.begin(), platforms.end(),
                                             g.platform) == platforms.end();
                        }),
                    state->groups.end());
                if (state->groups.size() != before) FillDiscoverList(state);
            }
            state->discover_busy.store(false);
            state->discover_loaded = true;
            if (state->discover_placeholder) {
                state->discover_placeholder = false;
                FillDiscoverList(state);
            }
            // 本轮拿到内容才写缓存；全部失败时保留上次的好缓存，不把它冲成空。
            if (!state->groups.empty() && state->updated_sources > 0) {
                DiscoverCacheData snapshot;
                snapshot.saved_at = NowUnixSeconds();
                snapshot.groups = state->groups;
                {
                    std::lock_guard<std::mutex> lock(g_discover_cache_mutex);
                    g_discover_cache = snapshot;
                }
                state->discover_cache_saved_at = snapshot.saved_at;
                state->discover_cache_fresh = true;
                WriteDiscoverCacheFile(state->discover_cache_path, snapshot);
            }
            // 后台挂载此时必定完成，搜索页的音源下拉框终于可以填全。
            state->sources_ready = true;
            RefreshSourceComboForMode(state);
            std::wstring text = state->groups.empty()
                ? L"没有音源返回榜单 / 推荐数据，可切到「搜索」或点「导入…」添加音源"
                : L"已展示 " + std::to_wstring(state->groups.size()) +
                      L" 个音源的榜单与推荐（已缓存，下次开窗秒开）";
            if (state->skipped_sources)
                text += L"，跳过 " + std::to_wstring(state->skipped_sources) +
                        L" 个无数据 / 异常音源";
            text += L"。点任意封面卡片即可展开曲目列表";
            SetStatusFor(state, false, text);
            return 0;
        }

        // ---- 视频：首页/分类聚合批次到达 ----
        case kMsgVideoHomeReady: {
            auto* payload = reinterpret_cast<VideoHomePayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            auto find_agg = [state]() -> BrowserState::VideoGroup* {
                for (auto& g : state->video_groups)
                    if (g.site_key == "agg:") return &g;
                return nullptr;
            };
            if (payload->finished) {
                TraceImport("video finished batches=%zu busy_before=1",
                            state->video_agg_batches.size());
                state->video_loaded = true;
                ReleaseVideoBusy(state);
                // 所有站点到齐：聚合分类页签只在这里统一排序 + 裁剪一次，
                // 顶部分类页签不再中途乱跳 / 反复出现同名（用户反馈的体验问题）。
                FinalizeAggVideoCategories(state);
                if (state->video_navbar)
                    InvalidateRect(state->video_navbar, nullptr, FALSE);
                state->video_cat_expected = 0;
                if (BrowserState::VideoGroup* agg = find_agg())
                    agg->category_loading = false;
                if (state->video_reload_pending) {
                    // 这一轮聚合开始时站点表还是旧的，期间用户在「视频源」对话框里
                    // 导入 / 删除了源：本轮结果作废，立刻按新表重来一次。
                    state->video_reload_pending = false;
                    state->video_loaded = false;
                    delete payload;
                    StartLoadVideo(state, true);
                    return 0;
                }
                if (state->video_pending_cat.load() != -2) {
                    // 用户已排队了分类切换（ReleaseVideoBusy 刚把它排进消息队列），
                    // 马上由 pending 执行接管并自己铺「加载中」网格；这里不要再
                    // FillVideoList，否则「点击导入…」占位会闪一下又换内容。
                    delete payload;
                    return 0;
                }
                BrowserState::VideoGroup* agg = find_agg();
                // 一个源都没取到内容时，占位卡片改成「去导入」的指引；否则空窗口
                // 挂着四张「正在准备…」会让人误以为还在加载、一直干等。
                const bool has_cards = agg && !agg->items.empty();
                state->video_placeholder = !has_cards;
                state->video_placeholder_text = L"点击「导入…」";
                FillVideoList(state);
                if (!state->video_import_note.empty()) {
                    // 刚导入完：优先显示导入结果（成功/失败的站点统计），否则
                    // 这条有用的信息会被下面的统计文案直接盖掉。
                    SetVideoStatus(state, state->video_import_note);
                    state->video_import_note.clear();
                } else if (!agg || state->video_agg_batches.empty()) {
                    SetVideoStatus(state, L"尚未导入视频源，点「导入…」添加影视仓 / TVBox 配置");
                } else if (!has_cards) {
                    // 有直播卡但没有 CMS 内容：把原因讲清楚，别让用户对着占位卡猜。
                    int cms = 0, native = 0, spider = 0;
                    try {
                        for (const auto& src : state->video_mgr->ListSources())
                            for (const auto& s : src.sites) {
                                if (s.kind == ttplayer::video::VideoSourceKind::kCms) ++cms;
                                else if (s.kind == ttplayer::video::VideoSourceKind::kNativeSpider) ++native;
                                else if (s.kind == ttplayer::video::VideoSourceKind::kSpider) ++spider;
                            }
                    } catch (...) {}
                    std::wstring msg = L"视频页：";
                    if (cms == 0 && native == 0 && spider > 0)
                        msg += L"已导入的 " + std::to_wstring(spider) +
                               L" 个站点全是 SPIDER 类型（需 TVBox 运行库），取不到影视内容；";
                    else
                        msg += L"已查询 " + std::to_wstring(cms + native) +
                               L" 个站点（CMS " + std::to_wstring(cms) +
                               L"，原生 " + std::to_wstring(native) +
                               L"），均未取到内容（可能已失效）；";
                    msg += L"直播源卡片可以点开观看";
                    SetVideoStatus(state, msg);
                } else {
                    SetVideoStatus(state,
                        L"已聚合 " + std::to_wstring(agg->items.size()) +
                        L" 部影片（来自 " +
                        std::to_wstring(state->video_agg_batches.size()) +
                        L" 个片源，重复片名已合并），点击卡片查看选集");
                }
                delete payload;
                return 0;
            }
            // 分类页签的返回：并入分类批次表，批次到齐后重排聚合流。
            if (payload->category_load) {
                UpsertAggBatch(state->video_cat_batches, payload);
                RebuildAggregatedVideoItems(state);
                FillVideoList(state);
                BrowserState::VideoGroup* agg = find_agg();
                if (state->video_cat_expected > 0 &&
                    --state->video_cat_expected == 0) {
                    if (agg) agg->category_loading = false;
                    ReleaseVideoBusy(state);
                    if (agg && !agg->items.empty())
                        SetVideoStatus(state,
                            L"已聚合「" + agg->active_category_name +
                            L"」分类 " + std::to_wstring(agg->items.size()) +
                            L" 部影片（来自 " +
                            std::to_wstring(state->video_cat_batches.size()) +
                            L" 个片源），点击卡片查看选集");
                    else if (agg)
                        SetVideoStatus(state, L"分类「" +
                            agg->active_category_name + L"」暂无内容，点「推荐」返回");
                }
                delete payload;
                return 0;
            }
            // 首页批次：并入批次表 + 合并分类页签，重排聚合流（渐进填充）。
            if (payload->error.empty() && !payload->items.empty()) {
                UpsertAggBatch(state->video_agg_batches, payload);
                MergeAggVideoCategories(state, payload->site_key,
                                        payload->categories);
                // 分类页签随各站点首页数据逐步到达，到了就刷新顶部导航行。
                if (state->video_navbar)
                    InvalidateRect(state->video_navbar, nullptr, FALSE);
            }
            RebuildAggregatedVideoItems(state);
            FillVideoList(state);
            delete payload;
            return 0;
        }
        // ---- 视频：详情选集加载完成 ----
        case kMsgVideoDetailReady: {
            auto* payload = reinterpret_cast<VideoDetailPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            std::vector<OnlineSearchHit> hits;
            for (size_t li = 0; li < payload->lines.size(); ++li) {
                const auto& line = payload->lines[li];
                for (size_t ei = 0; ei < line.episodes.size(); ++ei) {
                    const auto& ep = line.episodes[ei];
                    OnlineSearchHit hit;
                    hit.platform = payload->site_name;
                    hit.title = ep.name;
                    std::string final_url = ep.url;
                    if (!payload->play_url.empty() &&
                        final_url.find("://") == std::string::npos)
                        final_url = payload->play_url + final_url;
                    hit.raw = nlohmann::json::object(
                        {{"url", final_url}, {"line", line.name},
                         {"line_idx", static_cast<int>(li)},
                         {"ep_idx", static_cast<int>(ei)}});
                    hits.push_back(std::move(hit));
                }
            }
            // 影视仓式多线路：把同一集在各线路的对应地址打包进每个 hit 的
            // raw["sources"]，播放窗口的「切换源」按钮据此循环切换线路。
            for (auto& h : hits) {
                if (!h.raw.is_object()) continue;
                const int li = h.raw.value("line_idx", -1);
                const int ei = h.raw.value("ep_idx", -1);
                if (li < 0 || ei < 0) continue;
                // ⚠ 必须包含**当前线路**：以前只装「其它线路」，播放窗在
                // sources 里找不到当前 hit 的 url 时会把第 0 项的 url 覆盖成当前
                // 地址，造成「线路名 ffm3u8 却播着 feifan 的网页地址」的错位
                // （用户截图里网页线路播不了的真根因）。
                nlohmann::json arr = nlohmann::json::array();
                for (size_t li2 = 0; li2 < payload->lines.size(); ++li2) {
                    const auto& ln = payload->lines[li2];
                    if (ei >= static_cast<int>(ln.episodes.size())) continue;
                    std::string u = ln.episodes[ei].url;
                    if (!payload->play_url.empty() &&
                        u.find("://") == std::string::npos)
                        u = payload->play_url + u;
                    arr.push_back(nlohmann::json::object(
                        {{"url", u}, {"name", ln.name}}));
                }
                if (!arr.empty()) h.raw["sources"] = std::move(arr);
            }
            state->video_hits = std::move(hits);
            state->video_detail_busy.store(false);
            SetBusyUi(state, false);
            if (IsVideoMode(state)) {
                state->hits = state->video_hits;
                FillResultList(state);
            }
            const int n = static_cast<int>(state->video_hits.size());
            const wchar_t* unit = payload->live ? L"个频道" : L"个选集";
            if (!payload->error.empty()) {
                SetVideoSheetCaption(state, L"加载选集失败：" +
                    Utf8ToWideString(payload->error));
            } else {
                SetVideoStatus(state, (payload->live ? L"已加载频道：" : L"已加载选集：") +
                    std::to_wstring(n) + L" " + unit + L"，双击任意一行即可播放");
                if (n > 0)
                    SetVideoSheetCaption(state,
                        (payload->live ? L"直播「" : L"视频「") +
                        Utf8ToWideString(payload->title) + L"」· " +
                        std::to_wstring(n) + L" " + unit + L"（双击播放）");
                else
                    SetVideoSheetCaption(state, payload->live
                        ? L"这个直播源暂时没有可用频道"
                        : L"该视频暂无可播放选集");
            }
            delete payload;
            return 0;
        }
        // ---- 视频：搜索结果到达 ----
        case kMsgVideoSearchReady: {
            auto* payload = reinterpret_cast<VideoSearchPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            ReleaseVideoBusy(state);
            SetBusyUi(state, false);
            state->video_groups.clear();
            if (!payload->items.empty()) {
                BrowserState::VideoGroup g;
                g.site_key = "";
                g.site_name = "搜索结果";
                g.items = std::move(payload->items);
                state->video_groups.push_back(std::move(g));
            }
            state->video_rows.clear();
            FillVideoList(state);
            const int n = state->video_groups.empty()
                ? 0 : static_cast<int>(state->video_groups[0].items.size());
            if (n > 0)
                SetVideoStatus(state, L"找到 " + std::to_wstring(n) +
                    L" 个视频，点击卡片查看选集");
            else
                SetVideoStatus(state, L"没有找到相关视频（已查询 " +
                    std::to_wstring(payload->sites_total) + L" 个视频源）");
            delete payload;
            return 0;
        }
        // ---- 视频：窗口显示后再启动首页加载 ----
        case kMsgVideoStart: {
            if (state && IsVideoMode(state)) {
                bool empty = true;
                for (const auto& g : state->video_groups)
                    if (!g.items.empty()) { empty = false; break; }
                TraceImport("video_start busy=%d loaded=%d empty=%d groups=%zu",
                            state->video_busy.load() ? 1 : 0,
                            state->video_loaded ? 1 : 0, empty ? 1 : 0,
                            state->video_groups.size());
                // 聚合组一张卡片都没有就重跑。若 busy 因异常路径卡在 true，
                // 先复位再启动 —— 否则视频页会永远停在「正在准备…」占位卡片。
                if (empty) {
                    if (state->video_busy.load()) state->video_busy = false;
                    StartLoadVideo(state, true);
                }
            }
            MaybeRefreshVideoSources(state);
            return 0;
        }
        case kMsgVideoSourcesRefreshed:
            // 后台已把最新配置（含自动解密结果）写进 index.json，UI 线程重新
            // 载入并强制重跑首页聚合，用户看到的就是最新站点内容。
            if (state && state->video_mgr) {
                state->video_mgr->Reload();
                StartLoadVideo(state, true);
            }
            return 0;
        case kMsgVideoPendingCat:
            // 首页 / 分类加载期间被记下的分类切换，busy 释放后在这里补执行。
            StartLoadAggVideoCategory(state, static_cast<int>(lparam));
            return 0;
        case kMsgSheetTracksReady: {
            auto* payload = reinterpret_cast<SearchPayload*>(lparam);
            if (!state || !payload) { delete payload; return 0; }
            const bool for_search = payload->for_search;
            const size_t result_count = payload->hits.size();
            const bool on_page = for_search ==
                (state->mode == BrowserMode::kSearch);
            if (on_page) state->hits = std::move(payload->hits);
            else (for_search ? state->search_hits : state->discover_hits) =
                std::move(payload->hits);
            const OnlineSearchDiagnostics diagnostics = std::move(payload->diagnostics);
            delete payload;
            state->busy.store(false);
            SetBusyUi(state, false);
            // 兜底：即使展开标志意外丢失，榜单曲目到达时也要能看到列表。
            if (!for_search && !state->tracks_visible) {
                state->tracks_visible = true;
                LayoutChildren(state);
            }
            if (on_page) FillResultList(state);
            const std::wstring title = state->open_sheet_title.empty()
                ? std::wstring(L"歌单") : state->open_sheet_title;
            const std::wstring reason = diagnostics.errors.empty()
                ? std::wstring()
                : Utf8ToWideString(diagnostics.errors.front());
            if (result_count == 0) {
                std::wstring text = L"该歌单/排行榜没有返回曲目";
                if (!reason.empty()) text += L"：" + reason;
                SetStatusFor(state, for_search, text);
                SetSheetCaption(state, L"歌单「" + title + L"」· 加载失败：" +
                    (reason.empty() ? std::wstring(L"插件未返回数据") : reason) +
                    L"｜再点一次卡片可重试");
            } else {
                const std::wstring ok = L"已载入 " +
                    std::to_wstring(result_count) +
                    L" 首曲目，双击任意一行即可播放";
                SetStatusFor(state, for_search, ok);
                SetSheetCaption(state, L"歌单「" + title + L"」· " + ok);
            }
            return 0;
        }
        case kMsgArtworkReady: {
            // 海报到位：只置脏 + 250ms 合并重绘。逐张 InvalidateRect 会在
            // 并发下载时把海报墙刷成每秒十几次的全量重绘（用户看到的
            // 「加载图片时疯狂闪烁」）。
            if (state && state->sheet_list && !state->artwork_dirty) {
                state->artwork_dirty = true;
                SetTimer(hwnd, kArtworkRepaintTimerId, 250, nullptr);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wparam == kArtworkRepaintTimerId) {
                KillTimer(hwnd, kArtworkRepaintTimerId);
                if (state) {
                    state->artwork_dirty = false;
                    if (state->sheet_list)
                        InvalidateRect(state->sheet_list, nullptr, FALSE);
                    // 下载失败（彩色骨架常驻）的封面：3 秒节流后统一补抓一轮。
                    const std::int64_t now = GetTickCount64();
                    if (CountMissingVideoArtwork(state) > 0 &&
                        now - state->video_art_retry_at >= 3000) {
                        state->video_art_retry_at = now;
                        RetryMissingVideoArtwork(state);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU: {
            if (!state) break;
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // 键盘唤出（VK_APPS / Shift+F10）时坐标为 (-1,-1)，用列表左上角附近
            // 作为默认落点；命中测试拿不到行时不会弹菜单。
            if (point.x == -1 && point.y == -1) {
                if (!state->list) break;
                RECT bounds{};
                GetWindowRect(state->list, &bounds);
                point = {bounds.left + 40, bounds.top + 40};
            }
            ShowResultContextMenu(state, point);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            TraceImport("browser WM_DESTROY enter");
            // 子控件在这里之后才会被销毁，它们的子类化上下文必须留到那之后，
            // 因此只释放本窗口自己的 GDI 资源。
            if (state) {
                // WebView2 页面宿主：先 Destroy 宿主（排空在途回调）→ 再放桥 →
                // 最后放宿主实例，顺序不能颠倒（桥要活到宿主回调全部落地）。
                if (state->webview) {
                    state->webview->Destroy();
                    state->bridge.reset();
                    state->webview.reset();
                }
                if (state->host.store_bounds) {
                    RECT bounds{};
                    GetWindowRect(hwnd, &bounds);
                    if (bounds.right > bounds.left && bounds.bottom > bounds.top)
                        state->host.store_bounds(bounds);
                }
                if (state->host.on_closed) state->host.on_closed();
                DiscardFonts(state);
                if (state->panel_brush) {
                    DeleteObject(state->panel_brush);
                    state->panel_brush = nullptr;
                }
                state->on_track = nullptr;
            }
            // 这里不能 PostQuitMessage：本窗口不是进程的主窗口。
            TraceImport("browser WM_DESTROY leave");
            return 0;
        case WM_NCDESTROY: {
            TraceImport("browser WM_NCDESTROY enter");
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            const bool owns = g_browser && g_browser->hwnd == hwnd;
            const LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
            TraceImport("browser WM_NCDESTROY defproc done owns=%d", owns ? 1 : 0);
            if (owns) g_browser.reset();
            TraceImport("browser WM_NCDESTROY reset done");
            return result;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

constexpr wchar_t kBrowserClass[] = L"TTPlayerMusicFreeBrowser";

bool EnsureWindowClass() {
    static int state = 0;  // 0 未注册 / 1 成功 / -1 失败
    if (state != 0) return state > 0;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = BrowserProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kBrowserClass;
    // 框体（含圆角透明区）全部自绘，所以不能挂系统背景刷，否则透明角会被刷成
    // 系统色。区域由 ApplyFrameRegion 设置。
    window_class.hbrBackground = nullptr;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

// 初始位置：优先用保存过的位置，否则贴在主窗口右侧，最后夹到工作区内。
RECT InitialBrowserBounds(HWND owner, int width, int height) {
    RECT bounds{0, 0, width, height};
    RECT owner_rect{};
    if (owner && GetWindowRect(owner, &owner_rect)) {
        bounds.left = owner_rect.right + 8;
        bounds.top = owner_rect.top;
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &info)) {
        const int work_width = info.rcWork.right - info.rcWork.left;
        const int work_height = info.rcWork.bottom - info.rcWork.top;
        bounds.left = std::clamp(bounds.left, info.rcWork.left,
                                 std::max(info.rcWork.left,
                                          info.rcWork.right - width));
        bounds.top = std::clamp(bounds.top, info.rcWork.top,
                                std::max(info.rcWork.top,
                                         info.rcWork.bottom - height));
        bounds.right = bounds.left + std::min(width, work_width);
        bounds.bottom = bounds.top + std::min(height, work_height);
    }
    return bounds;
}

} // namespace

// ─────────────────── 跨编译单元导出符号（bridge_cmds_* 复用）──────────────────
// 下列符号原在上方匿名命名空间内（内部链接），bridge_cmds_video.cpp /
// bridge_cmds_misc.cpp 需要**同源**复用（分区归一 / 排序 / 跨源去重 / 插件
// 串行锁 / 当前皮肤），故移出匿名命名空间改为外部链接 —— 实现原样未改。
// 声明见 include/ttplayer/musicfree/musicfree_browser_window.h。
// 位置说明：必须在匿名命名空间闭合之后定义，才能访问其中的 g_browser /
// ActiveLegacySkin / Utf8ToWideString（匿名命名空间成员对外层可见）。

// 所有插件调用（搜索 / 解析 / 发现页加载）串行化：每个 MusicFree 插件持有
// 独立的 QuickJS runtime，不能同时被两个线程重入。按「一次插件调用」粒度持锁，
// 发现页逐音源加载时搜索线程仍有机会插队，不会整段被卡住。
std::mutex g_plugin_io;

// 把 CMS 分类名美化成影视仓式分区标题（常见分类加「热播」前缀）。
std::wstring FriendlySectionTitle(const std::string& type_name) {
    std::string t = type_name;
    const size_t a = t.find_first_not_of(" \t\r\n");
    const size_t b = t.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return L"推荐";
    t = t.substr(a, b - a + 1);
    if (t.empty()) return L"推荐";
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    auto ends_with = [&](const char* suffix) {
        const std::string s(suffix);
        return t.size() >= s.size() &&
               t.compare(t.size() - s.size(), s.size(), s) == 0;
    };
    if (has("纪录片")) return L"热播纪录片";
    if (has("动漫") || has("动画")) return L"热播动漫";
    if (has("短剧")) return L"热播短剧";
    if (has("电影")) return L"热播电影";
    if (has("综艺")) return L"热播综艺";
    if (has("剧"))   return L"热播剧集";
    // 动作片 / 喜剧片 / 爱情片 / 科幻片 / 战争片 … 都是电影分类，统一并入
    // 「热播电影」（以前只认「电影」二字，这些类型会各自成一个零散分区）。
    if (ends_with("片")) return L"热播电影";
    return Utf8ToWideString(t);  // 少儿 / 体育 / 其他 等原样
}

// 分区优先级：电影/剧集/动漫/综艺/纪录片/短剧 排前面，其余靠后（同优先级保持出现顺序）。
// 入参是「友好标题」（FriendlySectionTitle 的归一名），与分桶 key 保持一致。
int SectionPriority(const std::wstring& t) {
    if (t.find(L"电影") != std::wstring::npos) return 0;
    if (t.find(L"剧")   != std::wstring::npos) return 1;
    if (t.find(L"动漫") != std::wstring::npos || t.find(L"动画") != std::wstring::npos) return 2;
    if (t.find(L"综艺") != std::wstring::npos) return 3;
    if (t.find(L"纪录片") != std::wstring::npos) return 4;
    if (t.find(L"短剧") != std::wstring::npos) return 5;
    return 9;
}

// 跨源同名同年去重键：去空白 + 转小写，同名不同年不算重复。
std::string NormalizeVideoTitle(const std::string& title, const std::string& year) {
    std::string key;
    key.reserve(title.size() + 8);
    for (char c : title) {
        if (static_cast<unsigned char>(c) <= ' ') continue;  // 空白全去掉
        key += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    if (!year.empty()) { key += "@"; key += year; }  // 同名不同年不算重复
    return key;
}

// 当前音乐窗的皮肤薄封装：等价 ActiveLegacySkin(g_browser.get())，供视频命令域
//（ShowVideoPlayer / ShowVideoSourceDialog）取与音乐窗 state->host.skin 同口径
// 的外框；音乐窗未创建时 g_browser 为空 → 返回 nullptr（按无皮肤形态绘制）。
const skin::LegacySkin* MusicFreeBrowserLegacySkin() {
    return ActiveLegacySkin(g_browser.get());
}

bool IsMusicFreeBrowserVisible() {
    return g_browser && g_browser->hwnd && IsWindowVisible(g_browser->hwnd);
}

HWND MusicFreeBrowserWindow() {
    return g_browser ? g_browser->hwnd : nullptr;
}

void ReloadMusicFreeBrowser() {
    if (!g_browser || !g_browser->hwnd || !IsWindow(g_browser->hwnd)) return;
    if (g_browser->webview && g_browser->webview->Ready())
        g_browser->webview->Reload();
}

// 打开「导入音源」对话框 —— 与 Win32 界面「导入…」按钮（kIdImport 音乐分支）
// 走完全相同的实现：同一个模态对话框、同一套导入后刷新（重建音源下拉框、
// 失效发现页缓存并重新加载）。WebView 页面经 bridge 的 music.import 调用它。
// 返回 false 表示音乐窗未创建或正在加载（与按钮禁用态同口径）。
bool MusicFreeOpenImportDialog() {
    if (!g_browser || !g_browser->hwnd) return false;
    BrowserState* state = g_browser.get();
    // 音源集合在导入时会被改写，而搜索线程正在遍历插件表，两者不能并发。
    if (state->busy.load() || state->discover_busy.load()) return false;
    ShowMusicFreeImportDialog(state->hwnd, state->mgr, state->host.skin);
    RefreshSourceComboForMode(state);
    if (state->mgr) {
        const size_t count = state->mgr->ListPlugins().size();
        SetStatus(state, count
            ? L"已导入音源：" + std::to_wstring(count) +
                  L" 个，可以开始搜索"
            : L"尚未导入任何音源，请再次点击「导入音源」");
    }
    // 音源集合可能已变化：缓存立刻失效，后台强制重建并重写缓存。
    state->discover_cache_fresh = false;
    StartLoadDiscover(state, true);
    return true;
}

void CloseMusicFreeBrowser() {
    if (g_browser && g_browser->hwnd && IsWindow(g_browser->hwnd))
        PostMessageW(g_browser->hwnd, WM_CLOSE, 0, 0);
}

void RefreshMusicFreeBrowserFrame() {
    if (!g_browser || !g_browser->hwnd) return;
    // 换肤时只有「用户从没调过大小」才回到新皮肤的自然尺寸；用户手动拉大过就
    // 保留他的尺寸，否则每换一次皮肤窗口尺寸都会被重置。
    ApplyFrameMetrics(g_browser.get(), !g_browser->user_resized);
}

void ShowMusicFreeBrowser(HWND owner, MusicFreeSourceManager* mgr,
                          BrowserWindowHost host,
                          std::function<bool(OnlineTrackAction,
                                             const OnlineSearchHit&,
                                             const std::wstring&)> on_track,
                          const std::wstring& initial_query) {
    // 不同步等待音源挂载：窗口必须立刻显示，插件就绪由后台线程负责。
    // 已存在则只做前置、刷新音源与宿主，保持单一音乐窗。
    if (g_browser && g_browser->hwnd && IsWindow(g_browser->hwnd)) {
        if (on_track) g_browser->on_track = std::move(on_track);
        g_browser->host = std::move(host);
        g_browser->mgr = mgr ? mgr : AppMusicFreeManager();
        // 宿主的置顶状态可能在上次关闭后被「选项/主窗口」改过，这里同步一次。
        if (g_browser->top_most != g_browser->host.top_most)
            SetTopMost(g_browser.get(), g_browser->host.top_most);
        if (g_browser->mode == BrowserMode::kSearch)
            RefreshSourceComboForMode(g_browser.get());
        // 唤出已存在的窗口时**不再**把尺寸拉回皮肤位图尺寸：窗口现在可以缩放，
        // 用户上次调好的宽高要留着，这里只重排一次。
        ApplyFrameMetrics(g_browser.get(), false);
        ShowWindow(g_browser->hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_browser->hwnd);
        // 带着关键词再次唤出（例如在播放列表里对另一首歌再点一次「在线搜索」）：
        // 覆盖输入框并重新搜索，而不是让用户对着旧结果发呆。
        if (!initial_query.empty()) {
            TabCtrl_SetCurSel(g_browser->tab, 1);
            OnTabChanged(g_browser.get(), 1);
            SetWindowTextW(g_browser->query, initial_query.c_str());
            StartSearch(g_browser.get());
        }
        return;
    }

    if (!EnsureWindowClass()) return;

    auto state = std::make_shared<BrowserState>();
    state->owner = owner;
    state->mgr = mgr ? mgr : AppMusicFreeManager();
    state->host = std::move(host);
    state->on_track = std::move(on_track);
    state->top_most = state->host.top_most;
    state->initial_query = initial_query;
    g_browser = state;

    const SIZE natural = NaturalFrameSize(state.get());
    const SIZE minimum = MinFrameSize(state.get());
    int width = natural.cx;
    int height = natural.cy;
    RECT bounds = InitialBrowserBounds(owner, width, height);
    if (state->host.saved_bounds) {
        const RECT saved = state->host.saved_bounds();
        if (saved.right > saved.left && saved.bottom > saved.top) {
            bounds.left = saved.left;
            bounds.top = saved.top;
            // 上次的**尺寸**也一起恢复（只夹到最小尺寸）：窗口能缩放之后，
            // 每次唤出都回到皮肤位图尺寸就等于把用户调过的宽高丢了。
            width = std::max<int>(saved.right - saved.left, minimum.cx);
            height = std::max<int>(saved.bottom - saved.top, minimum.cy);
            bounds.right = saved.left + width;
            bounds.bottom = saved.top + height;
        }
    }
    // 上次的坐标可能整体落在屏幕外（换了显示器 / 分辨率变小 / 缩放变化）。
    // 那样窗口其实创建了却看不见，用户观感就是「音乐窗没自动打开」。
    // 恢复后若与所有显示器都不相交，就挪回主屏工作区左上角。
    if (!MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL)) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(
                MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY),
                &monitor)) {
            const int work_w = monitor.rcWork.right - monitor.rcWork.left;
            const int work_h = monitor.rcWork.bottom - monitor.rcWork.top;
            width = std::min(width, work_w - 48);
            height = std::min(height, work_h - 48);
            bounds.left = monitor.rcWork.left + 24;
            bounds.top = monitor.rcWork.top + 24;
            bounds.right = bounds.left + width;
            bounds.bottom = bounds.top + height;
        }
    }

    // 皮肤位图尺寸 = 想要的**客户区**尺寸。用 AdjustWindowRectEx 反算外框尺寸，
    // 而不是直接把位图尺寸当外框尺寸传：只要窗口样式里混进了任何非客户区边框
    // （例如 WS_EX_DLGMODALFRAME 会给 WS_POPUP 加 3px 边框），客户区就会比位图
    // 小，从而触发 DrawFrameBackground 的「整幅缩放」退化分支 —— 外观上正是
    // 「背景偏 / 被压扁」。当前用的是 WS_POPUP + WS_EX_TOOLWINDOW（delta=0），
    // 这层换算只是防止以后有人改样式时静默退化的保险。
    const DWORD frame_style = WS_POPUP;
    // ⚠ 不要加 WS_EX_COMPOSITED：它与 owner-draw ListView / 公共控件组合会
    // 触发全窗口软件合成，窗口越大越卡、列表区域反复失效重绘（用户实测
    // 「选集列表非常卡、闪烁明显」）。拖动闪烁已由各重绘路径自身的双缓冲
    // 解决（主窗 WM_PAINT、海报墙 LB、导航行/图标栏 PaintBuffered）。
    const DWORD frame_ex_style = WS_EX_TOOLWINDOW;
    RECT frame_rect{0, 0, width, height};
    AdjustWindowRectEx(&frame_rect, frame_style, FALSE, frame_ex_style);
    const HWND hwnd = CreateWindowExW(
        frame_ex_style, kBrowserClass, L"音乐窗 · MusicFree", frame_style,
        bounds.left + frame_rect.left, bounds.top + frame_rect.top,
        frame_rect.right - frame_rect.left, frame_rect.bottom - frame_rect.top,
        owner, nullptr, GetModuleHandleW(nullptr), state.get());
    if (!hwnd) { g_browser.reset(); return; }

    // 诊断：客户区必须正好等于皮肤背景位图尺寸，否则皮肤会被整幅压扁。
    // 这是「音乐窗和导入窗外框是否真的像素一致」的可核对证据。
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        TraceDiagnostic("BROWSER_FRAME_CLIENT",
                        "client=" + std::to_string(client.right - client.left) + "x" +
                            std::to_string(client.bottom - client.top) +
                            " natural=" + std::to_string(natural.cx) + "x" +
                            std::to_string(natural.cy) + " identical=" +
                            ((client.right - client.left == natural.cx &&
                              client.bottom - client.top == natural.cy)
                                 ? "1"
                                 : "0"));
    }
    TraceResizeBands(g_browser.get());

    // WebView2 页面承载：此处 CreateWindowExW 已返回（创建期 WM_SIZE 已跑过
    // ApplyFrameMetrics，state->layout.content 就是皮肤内容区），窗口尚未显示。
    // Runtime 缺失时 TryAttachWebview 会启动后台自动安装，装好后再重试一次；
    // 任何失败仍回退旧 Win32 控件，绝不阻塞启动。
    TryAttachWebview(hwnd, state.get());

    if (g_browser->top_most)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
}

} // namespace ttplayer::musicfree
