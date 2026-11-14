// 音乐窗 WebView2 JSON 桥 —— 视频能力命令实现（Task 7）
//
// 职责边界：只做「参数校验 → 复用既有业务函数 → 拼 JSON 回执 / 推事件」。
// 硬约束（对应 spec「功能不减」与 tasks.md Task 7）：
//   1. 首页聚合复用既有 VideoSourceManager + FetchHome 并发抓取（做法对齐
//      StartLoadVideo 的「每站点一线程」并发，musicfree_browser_window.cpp
//      L4116-4168），本模块自建独立管理器实例（目录 / UA 与 StartLoadVideo
//      的懒创建分支一致，见 BridgeVideoManager 注释）；
//   2. 分区标题归一 / 分区排序 / 跨源同名去重**必须与既有实现同源**——
//      FriendlySectionTitle（L3081）/ SectionPriority（L3100）/
//      NormalizeVideoTitle（L3889）三个函数 extern 复用，本文件绝不复刻；
//      分桶-铺平逻辑与 BuildVideoHomeSections（L3116-3151）逐行对齐；
//   3. 打开播放只调既有 ShowVideoPlayer（参数口径对齐 PlaySelectedVideoHit
//      L3071-3077），详情取选集只调既有 FetchDetail（前缀与跨线路 sources
//      口径对齐 kMsgVideoDetailReady L5742-5776）；
//   4. 视频源导入只调既有 ShowVideoSourceDialog（用法对齐 OpenVideoSourceDialog
//      L4679-4699），报告文案口径对齐其收尾状态行（L4705-4730）；
//   5. 不新造任何存储文件；handler 不抛异常，失败统一回 ok:false + 中文 error。
//
// 【直播入口结论】既有直播能力 = AddLiveSourceCards（L4293，本地铺直播卡）与
//   StartOpenVideoLive（L4317，FetchLive 拉频道表），二者都在
//   musicfree_browser_window.cpp 的匿名命名空间内且依赖 BrowserState*，
//   **没有可直接调用的公开入口**（也没有 LivePlay 之类的公开函数）；
//   数据层的公开 API（VideoSourceManager::AllLives / FetchLive）存在，但按任务
//   约定先不另写一份直播铺卡 / 开播逻辑：video.sources 带 {live:true} 统一回
//   ok:false「直播入口待接线」，等宿主导出无状态入口后再接（见文件尾注释）。
//
// 线程模型（见 musicfree_bridge.h 头注）：
//   · handler 在 UI 线程被调；首页聚合与详情取选集都可能超过页面 5s 超时，
//     故 video.home 先立即回执 accepted，数据经 evt video.home 推回；
//     video.open 的详情分支只能走一次回执（没有承载选集数据的事件名），
//     结果在工作线程就绪后回，网络慢时可能超过页面 SEND_TIMEOUT_MS=5000
//     （页面侧可加大 video.open 的超时或按需重试，C++ 侧不阻塞 UI）；
//   · 工作线程 PushEvent 线程安全（桥内有锁），但回推前用 BridgeAlive()
//     复核音乐窗还在（桥实例随窗口创建 / 销毁，见 L6175 / L6198）。

#include "bridge_cmds_video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ttplayer/core/text.h"
#include "ttplayer/musicfree/musicfree_app.h"
#include "ttplayer/musicfree/musicfree_browser_window.h"
#include "ttplayer/musicfree/musicfree_http.h"
#include "ttplayer/musicfree/musicfree_source_manager.h"
#include "ttplayer/skin/skin.h"
#include "ttplayer/video/video_cms.h"
#include "ttplayer/video/video_history.h"
#include "ttplayer/video/spider/spider_api.h"
#include "ttplayer/video/video_player_js.h"
#include "ttplayer/video/video_player_window.h"
#include "ttplayer/video/video_resolve.h"
#include "ttplayer/video/video_source_dialog.h"
#include "ttplayer/video/video_source_manager.h"
#include "ttplayer/video/video_types.h"

// ───────────────────────── 需宿主导出的既有符号 ─────────────────────────
//
// musicfree_browser_window.cpp 是宿主独占文件，本模块不改它：下列函数目前
// 位于该文件的匿名命名空间内（行号为本次实现时的快照），由宿主把它们移出
// 匿名命名空间（保留名字与签名即可，建议同时在
// include/ttplayer/musicfree/musicfree_browser_window.h 里补声明）后本文件
// 才能链接。分区标题归一 / 分区排序 / 跨源去重是页面与旧 Win32 海报墙
// 必须**同源**的逻辑，因此这里只声明、绝不复刻。

namespace ttplayer::skin {
class LegacySkin;
}  // namespace ttplayer::skin

namespace ttplayer::musicfree {

// musicfree_browser_window.cpp L3081：CMS 分类名 → 友好分区标题。
std::wstring FriendlySectionTitle(const std::string& type_name);
// musicfree_browser_window.cpp L3100（现为 static）：友好标题 → 分区优先级。
int SectionPriority(const std::wstring& t);
// musicfree_browser_window.cpp L3889：跨源同名同年去重键。
std::string NormalizeVideoTitle(const std::string& title, const std::string& year);
// 新增薄封装（建议放 ActiveLegacySkin，同文件 L863 旁）：
//   const skin::LegacySkin* MusicFreeBrowserLegacySkin() {
//       return ActiveLegacySkin(g_browser.get());
//   }
// 用于 ShowVideoPlayer / ShowVideoSourceDialog 的皮肤外框，口径与既有调用方
//（state->host.skin）一致。
const skin::LegacySkin* MusicFreeBrowserLegacySkin();

namespace {

using json = nlohmann::json;

// ───────────────────────── 参数取值（页面入参不可信，一律不抛） ──────────

std::string JsonStr(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

bool JsonBool(const json& j, const char* key, bool fallback) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) {
        const std::string& s = it->get_ref<const std::string&>();
        return s == "1" || s == "true" || s == "yes" || s == "on";
    }
    return fallback;
}

// ───────────────────────── 视频源管理器（桥侧独立实例） ─────────────────
//
// 与 StartLoadVideo 的懒创建分支（L4054-4075）同参数：<exe>/video 缓存目录、
// 进程级音源管理器的网络配置、同一个 UA 兜底、LoadCached() 读回已导入站点。
// 走独立实例而不是音乐窗的 video_mgr：后者住在 BrowserState（匿名命名空间）
// 里不可见；管理器内部 imported_ 有互斥锁、AllSites() 返回值拷贝，两个实例
// 同读同一份 index.json 是安全的（与 MaybeRefreshVideoSources L4035-4046
// 用独立实例刷写同一目录的做法一致）。导入对话框改动会经对话框直接落盘到
// 同一 index.json，本实例随后可见；音乐窗旧管线要等它自己的 Reload/重启。
std::mutex g_mgr_mtx;
std::unique_ptr<ttplayer::video::VideoSourceManager> g_mgr;

ttplayer::video::VideoSourceManager* BridgeVideoManager() {
    std::lock_guard<std::mutex> lk(g_mgr_mtx);
    if (g_mgr) return g_mgr.get();
    try {
        std::wstring exe_w(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, exe_w.data(),
                                            static_cast<DWORD>(exe_w.size()));
        if (n == 0 || n >= exe_w.size()) return nullptr;
        exe_w.resize(n);
        auto mgr = std::make_unique<ttplayer::video::VideoSourceManager>(
            std::filesystem::path(exe_w).parent_path() / L"video");
        ttplayer::musicfree::NetworkConfig net;
        if (auto* app = AppMusicFreeManager()) net = app->network();
        if (net.user_agent.empty())
            net.user_agent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                             L"AppleWebKit/537.36 (KHTML, like Gecko) "
                             L"Chrome/124.0.0.0 Safari/537.36";
        mgr->SetNetworkConfig(std::move(net));
        mgr->LoadCached();
        g_mgr = std::move(mgr);
        return g_mgr.get();
    } catch (...) {
        return nullptr;  // 建盘 / 读盘异常：由调用方回 ok:false 中文错误
    }
}

// ───────────────────────── 线程内回推前的存活复核 ─────────────────
//
// 桥实例随音乐窗创建 / 销毁；首页聚合可能跑几十秒，窗口关了再 PushEvent /
// reply 就是悬垂引用。残余的「检查通过与回推之间窗口被销毁」竞态由宿主按
// 桥头文件线程模型第 3 条（销毁前等待 / 作废在途回调）兜底。
bool BridgeAlive() {
    return !IsMusicFreeShuttingDown() && MusicFreeBrowserWindow() != nullptr;
}

// 离开作用域即复位某个 busy 标志：工作线程里的任何 return / 异常路径都必须
// 复位，否则 video.home 会永久卡在 already_running。
struct FlagReset {
    std::atomic<bool>& flag;
    ~FlagReset() { flag.store(false); }
};

// video.home 并发合并：同一时刻只跑一轮聚合；后到的请求回 already_running，
// 当前轮完成后照常推 evt，页面不会丢数据。
std::atomic<bool> g_home_busy{false};
// video.category 同一时刻只跑一轮分类聚合（与 g_home_busy 同理）。
std::atomic<bool> g_cat_busy{false};
// video.search 同理：一轮全源搜索最多跑 25 秒，期间重复点搜索直接回
// already_running（页面按 q 过滤迟到结果，不会串数据）。
std::atomic<bool> g_search_busy{false};
// 视频源对话框是模态窗口，不允许叠开（模态消息循环里可能再收到 web 消息）。
std::atomic<bool> g_dialog_open{false};

// ───────────────────────── 卡片 → 页面视图模型 ─────────────────
//
// 字段按任务约定：id/title/cover/rate/from 必带（有值才带），site 是回传
// video.open 做详情定位用的站点键，remarks/type/year 供卡片角标与筛选。
// 影视源常用第三方图片代理（wsrv.nl / wsrv.io）包一层豆瓣等图床；实测本机
// 到该代理经常 20s 超时，而原图直连 0.7s 就 200——表现为「有些封面空白」。
// 这里把代理前缀剥掉，直接给原始图片地址。
std::string UnwrapImageProxy(const std::string& url) {
    std::string low = url;
    for (auto& ch : low)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    if (low.find("wsrv.nl") == std::string::npos &&
        low.find("wsrv.io") == std::string::npos)
        return url;
    const std::size_t q = low.find("?url=");
    if (q == std::string::npos) return url;
    std::string inner = url.substr(q + 5);
    if (const std::size_t amp = inner.find('&'); amp != std::string::npos)
        inner.resize(amp);
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(inner.size());
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '%' && i + 2 < inner.size()) {
            const int h = hexval(inner[i + 1]), l = hexval(inner[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>(h * 16 + l));
                i += 2;
                continue;
            }
        }
        out.push_back(inner[i]);
    }
    if (out.size() > 8 &&
        (out.rfind("http://", 0) == 0 || out.rfind("https://", 0) == 0))
        return out;
    return url;
}

// ───────────────────────── 纯网盘源（学习型黑名单） ─────────────────
//
// 部分「搜搜」类源（优汐 / 抠抠 …）只有网盘分享链接（百度 / 夸克 / UC），
// 详情永远拿不到可播放直链，点开必然失败。首次检测到（详情有线路、但经过
// 网盘剔除后一行不剩）就把站点 key 记到 <exe>\video\cloud_sites.json，
// 之后首页聚合 / 搜索 / 跨源扫描都跳过它，用户不会再点到「注定播不了」的卡片。
// 想恢复这些源：删掉 cloud_sites.json 即可（下次点开还会重新学习）。
std::mutex g_cloud_sites_mtx;
std::set<std::string> g_cloud_sites;
bool g_cloud_sites_loaded = false;

std::filesystem::path CloudSitesPath() {
    std::wstring exe_w(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe_w.data(),
                                       static_cast<DWORD>(exe_w.size()));
    if (n == 0 || n >= exe_w.size()) return {};
    exe_w.resize(n);
    return std::filesystem::path(exe_w).parent_path() / L"video" /
           L"cloud_sites.json";
}

// 已学习的纯网盘源 key 集合（拷贝返回，调用方无需持锁）。
std::set<std::string> LoadedCloudSites() {
    std::lock_guard<std::mutex> lk(g_cloud_sites_mtx);
    if (!g_cloud_sites_loaded) {
        g_cloud_sites_loaded = true;
        try {
            std::ifstream in(CloudSitesPath(), std::ios::binary);
            if (in) {
                json root;
                in >> root;
                if (root.contains("sites") && root["sites"].is_array())
                    for (const auto& v : root["sites"])
                        if (v.is_string())
                            g_cloud_sites.insert(v.get<std::string>());
            }
        } catch (...) {
        }
    }
    return g_cloud_sites;
}

// 记一个纯网盘源（幂等；写盘失败不影响本次运行内的过滤）。
void MarkCloudSite(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lk(g_cloud_sites_mtx);
    if (!g_cloud_sites_loaded) {
        g_cloud_sites_loaded = true;
        try {
            std::ifstream in(CloudSitesPath(), std::ios::binary);
            if (in) {
                json root;
                in >> root;
                if (root.contains("sites") && root["sites"].is_array())
                    for (const auto& v : root["sites"])
                        if (v.is_string())
                            g_cloud_sites.insert(v.get<std::string>());
            }
        } catch (...) {
        }
    }
    if (!g_cloud_sites.insert(key).second) return;
    try {
        json root;
        root["sites"] = json::array();
        for (const auto& k : g_cloud_sites) root["sites"].push_back(k);
        std::ofstream out(CloudSitesPath(),
                          std::ios::binary | std::ios::trunc);
        if (out) out << root.dump(2);
    } catch (...) {
    }
}

// 从站点列表里剔除已学习的纯网盘源。
void DropCloudSites(std::vector<ttplayer::video::VideoSite>* sites) {
    if (!sites || sites->empty()) return;
    const std::set<std::string> cloud = LoadedCloudSites();
    if (cloud.empty()) return;
    sites->erase(std::remove_if(sites->begin(), sites->end(),
                                [&](const ttplayer::video::VideoSite& s) {
                                    const std::string k =
                                        s.key.empty() ? s.name : s.key;
                                    return cloud.count(k) > 0;
                                }),
                 sites->end());
}

json CardToJson(const ttplayer::video::VideoItem& it) {
    json c;
    c["id"] = it.id;
    if (!it.site_key.empty()) c["site"] = it.site_key;
    c["title"] = it.title;
    if (!it.pic.empty()) c["cover"] = UnwrapImageProxy(it.pic);
    if (!it.score.empty()) c["rate"] = it.score;
    if (!it.site_name.empty()) c["from"] = it.site_name;
    if (!it.remarks.empty()) c["remarks"] = it.remarks;
    if (!it.type_name.empty()) c["type"] = it.type_name;
    if (!it.year.empty()) c["year"] = it.year;
    return c;
}

// ───────────────────────── 首页聚合（video.home 的数据源） ─────────────────
//
// 三步，每一步都与音乐窗旧管线的既有实现同口径：
//   1) 并发抓取：每站点一线程 FetchHome（对齐 StartLoadVideo L4121-4159）；
//   2) 交错 + 去重：每轮每站取一条、NormalizeVideoTitle 同名同年只留首份
//      （对齐 RebuildAggregatedVideoItems L3903-3929）；
//   3) 分区分桶：按 FriendlySectionTitle 归一分桶、首现顺序出桶、
//      按 SectionPriority 稳定排序（对齐 BuildVideoHomeSections L3116-3151
//      —— 同名分区不重复的来源）。FinalizeAggVideoCategories 是「顶部分类
//      页签」的裁剪逻辑（L3990），与分区卡片无关，这里不涉及。
struct HomeBatch {
    std::string key;
    std::string name;
    std::vector<ttplayer::video::VideoItem> items;
    std::vector<ttplayer::video::VideoCategory> categories;  // 首页带分类（CMS class）
    std::string error;
};

// 把交错去重后的条目流按「友好分区标题」分桶成影视仓式 section 数组。
// 分区口径与桥管线首页/旧管线 BuildVideoHomeSections 一致：优先 CMS 分类名
// （type_name，FriendlySectionTitle 归一），空则退回来源站点，再空落「推荐」。
// 抽出供首页聚合与分类聚合（video.category 命令）共用。
json BuildSectionsFromFlow(const std::vector<ttplayer::video::VideoItem>& flow) {
    std::vector<std::wstring> order;
    std::map<std::wstring, std::vector<const ttplayer::video::VideoItem*>> buckets;
    for (const auto& it : flow) {
        std::wstring title;
        if (!it.type_name.empty())
            title = FriendlySectionTitle(it.type_name);
        if (title.empty() && !it.site_name.empty()) {
            const std::wstring site = core::Utf8ToWide(it.site_name);
            if (!site.empty()) {
                // 原生 spider 源不带 CMS 分类名（type_name），过去只能退回
                // 「源 · 站名」，首页就变成一堆按源罗列的块，看不到影视仓式
                // 推荐流。站点名通常带类型（「巴士┃动漫」「XX┃电影」），先按
                // 同一套 FriendlySectionTitle 归一；识别不出类型的并入「推荐」
                // 混合流（与影视仓「最近更新」同观感），不再每源占一块。
                const std::wstring guess =
                    FriendlySectionTitle(core::WideToUtf8(site));
                title = (guess.empty() || guess == site) ? L"推荐" : guess;
            }
        }
        if (title.empty()) title = FriendlySectionTitle(std::string("推荐"));
        if (buckets.find(title) == buckets.end()) order.push_back(title);
        buckets[title].push_back(&it);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const std::wstring& a, const std::wstring& b) {
                         return SectionPriority(a) < SectionPriority(b);
                     });
    json sections = json::array();
    for (const auto& title : order) {
        json sec;
        sec["title"] = core::WideToUtf8(title);
        json cards = json::array();
        for (const auto* p : buckets[title]) cards.push_back(CardToJson(*p));
        sec["cards"] = std::move(cards);
        sections.push_back(std::move(sec));
    }
    return sections;
}

// 分类页签只保留影视仓标准六类（电影/剧集/综艺/动漫/短剧/纪录片）。
// 各站 categories 里混着大量非内容分类（全部/其他/即将上映/最近更新/正在热映/
// 篮球/足球/高清预告/站点名…），全部丢弃；常见子类（国漫/日漫/纪录）并进标准类。
// 这样无论导入什么配置，页签始终是干净的一排。
std::wstring CanonicalCatTitle(const std::string& name) {
    if (name.empty()) return {};
    const std::wstring t = FriendlySectionTitle(name);
    if (t == L"热播电影" || t == L"热播剧集" || t == L"热播综艺" ||
        t == L"热播动漫" || t == L"热播短剧" || t == L"热播纪录片")
        return t;
    // FriendlySectionTitle 未覆盖的子类：国漫 / 日漫 / 纪录
    if (name.find("漫") != std::string::npos) return L"热播动漫";
    if (name.find("纪录") != std::string::npos) return L"热播纪录片";
    return {};  // 其余（全部/其他/时间/体育/站点名…）不是内容分类，丢弃
}

json AggregateVideoHome(ttplayer::video::VideoSourceManager* mgr) {
    json out = json::object();
    out["sections"] = json::array();

    // 吸收后台 MaybeRefreshVideoSources 写盘的最新站点表（只读盘不下载）。
    mgr->Reload();

    std::vector<ttplayer::video::VideoSite> sites;
    try {
        sites = mgr->AllSites();
    } catch (const std::exception& e) {
        out["error"] = std::string("读取视频源失败: ") + e.what();
        return out;
    }
    const bool had_sites = !sites.empty();
    DropCloudSites(&sites);   // 已学习的纯网盘源不参与首页推荐
    if (sites.empty()) {
        out["error"] = had_sites
                           ? "可用的视频源都只有网盘资源（已全部隐藏），"
                             "请导入带直链的影视仓配置"
                           : "尚未导入视频源：请先点「导入」添加影视仓 / TVBox 配置地址";
        return out;
    }

    // —— 1) 每站点一线程并发抓首页（失败站点跳过，不阻断其它源）——
    std::vector<HomeBatch> batches(sites.size());
    for (std::size_t i = 0; i < sites.size(); ++i) {
        batches[i].key = sites[i].key.empty() ? sites[i].name : sites[i].key;
        batches[i].name = sites[i].name;
    }
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        workers.emplace_back([mgr, &batches, i, site = sites[i]] {
            BgThreadGuard guard;  // 退出时由 ShutdownMusicFree 等待
            if (IsMusicFreeShuttingDown()) return;
            auto& batch = batches[i];  // 各线程各写各的元素，无需加锁
            try {
                ttplayer::video::VideoListing listing =
                    ttplayer::video::FetchHome(site, mgr->network());
                batch.items = std::move(listing.items);
                batch.categories = std::move(listing.categories);
                batch.error = listing.error;
            } catch (const std::exception& e) {
                batch.error = e.what();
            } catch (...) {
                batch.error = "未知错误";
            }
        });
    }
    for (auto& t : workers) t.join();

    // —— 2) 站点轮询交错 + 跨源同名去重（口径同 RebuildAggregatedVideoItems）——
    std::vector<ttplayer::video::VideoItem> flow;
    {
        std::set<std::string> seen;
        std::vector<std::size_t> cur(batches.size(), 0);
        bool added = true;
        while (added) {
            added = false;
            for (std::size_t i = 0; i < batches.size(); ++i) {
                if (cur[i] >= batches[i].items.size()) continue;
                added = true;
                const ttplayer::video::VideoItem& it = batches[i].items[cur[i]++];
                if (it.title.empty()) continue;
                const std::string key = NormalizeVideoTitle(it.title, it.year);
                if (!key.empty() && !seen.insert(key).second) continue;
                flow.push_back(it);
            }
        }
    }

    // —— 3) 分区分桶（friendly 归一 + SectionPriority 排序，见函数头注释）——
    out["sections"] = BuildSectionsFromFlow(flow);
    out["sites"] = sites.size();

    // —— 4) 顶部分类页签（影视仓式）：收集各站首页 CMS class，按友好标题归一并
    // 记录「归一名 → [(站点key, class_id)]」。前端按归一名展示页签，点进去再逐站
    // 用 (key, class_id) 拉分类影片（video.category 命令）。「推荐/最新」之类不单独
    // 成页签（它们本就是首页轮播，已包含在 sections 里）。
    std::map<std::wstring, std::vector<std::pair<std::string, std::string>>> cat_map;
    for (std::size_t i = 0; i < batches.size(); ++i) {
        for (const auto& c : batches[i].categories) {
            if (c.id.empty()) continue;
            const std::wstring t = CanonicalCatTitle(c.name);
            if (t.empty()) continue;  // 非标准内容分类（全部/其他/时间/体育…）丢弃
            cat_map[t].push_back({batches[i].key, c.id});
        }
    }
    std::vector<std::wstring> cat_order;
    for (const auto& kv : cat_map) cat_order.push_back(kv.first);
    std::stable_sort(cat_order.begin(), cat_order.end(),
                     [](const std::wstring& a, const std::wstring& b) {
                         return SectionPriority(a) < SectionPriority(b);
                     });
    json cats = json::array();
    for (const auto& t : cat_order) {
        json cat;
        cat["title"] = core::WideToUtf8(t);
        json srcs = json::array();
        for (const auto& pr : cat_map[t])
            srcs.push_back(json{{"key", pr.first}, {"id", pr.second}});
        cat["sources"] = std::move(srcs);
        cats.push_back(std::move(cat));
    }
    out["cats"] = std::move(cats);

    // 失败站点不再塞进 errors 数组：几十个免费源里总有几个死站，把
    // 「部分站点加载失败（N 个）」红色横幅顶在首页上只会让用户以为页面坏了。
    // 这里只留一个诊断计数（页面不展示），有内容就正常推。
    int failed = 0;
    for (const auto& b : batches)
        if (!b.error.empty()) ++failed;
    if (failed > 0) out["failedSites"] = failed;
    if (flow.empty()) out["error"] = "各视频源都没有取到推荐内容";
    return out;
}

// 分类页签聚合：前端传「归一名」对应的 [(站点key, class_id)]，逐站 FetchCategory
// 拉该分类第一页，交错去重后按 BuildSectionsFromFlow 分桶。与首页聚合同口径，
// 但走「分类」接口而非「首页推荐」。
json AggregateVideoCategory(ttplayer::video::VideoSourceManager* mgr,
                            const json& sources,
                            const std::string& cat_title) {
    json out = json::object();
    out["sections"] = json::array();
    mgr->Reload();
    std::vector<ttplayer::video::VideoSite> sites;
    try {
        sites = mgr->AllSites();
    } catch (const std::exception& e) {
        out["error"] = std::string("读取视频源失败: ") + e.what();
        return out;
    }
    DropCloudSites(&sites);   // 纯网盘源不参与分类聚合
    if (!sources.is_array() || sources.empty()) {
        out["error"] = "分类缺少 sources（站点/分类 id 映射）";
        return out;
    }
    std::map<std::string, ttplayer::video::VideoSite> by_key;
    for (const auto& s : sites) {
        const std::string k = s.key.empty() ? s.name : s.key;
        by_key[k] = s;
    }
    struct CatBatch {
        std::vector<ttplayer::video::VideoItem> items;
        std::string error;
    };
    std::vector<CatBatch> batches(sources.size());
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const std::string key = sources[i].value("key", "");
        const std::string id = sources[i].value("id", "");
        auto it = by_key.find(key);
        if (it == by_key.end() || id.empty()) continue;
        workers.emplace_back([mgr, &batches, i, site = it->second, id] {
            BgThreadGuard guard;
            if (IsMusicFreeShuttingDown()) return;
            try {
                auto listing = ttplayer::video::FetchCategory(
                    site, id, 1, mgr->network());
                batches[i].items = std::move(listing.items);
                batches[i].error = listing.error;
            } catch (const std::exception& e) {
                batches[i].error = e.what();
            } catch (...) {
                batches[i].error = "未知错误";
            }
        });
    }
    for (auto& t : workers) t.join();

    // 交错 + 跨源同名去重（同首页口径）
    std::vector<ttplayer::video::VideoItem> flow;
    {
        std::set<std::string> seen;
        std::vector<std::size_t> cur(batches.size(), 0);
        bool added = true;
        while (added) {
            added = false;
            for (std::size_t i = 0; i < batches.size(); ++i) {
                if (cur[i] >= batches[i].items.size()) continue;
                added = true;
                ttplayer::video::VideoItem it = batches[i].items[cur[i]++];
                if (it.title.empty()) continue;
                // 分类拉到的条目多数没有 type_name：用当前分类名回填，分区标题
                // 才会显示「热播电影」而不是笼统的「推荐」。
                if (it.type_name.empty() && !cat_title.empty())
                    it.type_name = cat_title;
                const std::string key = NormalizeVideoTitle(it.title, it.year);
                if (!key.empty() && !seen.insert(key).second) continue;
                flow.push_back(std::move(it));
            }
        }
    }
    out["sections"] = BuildSectionsFromFlow(flow);
    int failed = 0;
    for (const auto& b : batches)
        if (!b.error.empty()) ++failed;
    if (failed > 0) out["failedSites"] = failed;
    if (flow.empty()) out["error"] = "该分类下各视频源都没有取到内容";
    return out;
}

// ───────────────────────── 视频源导入报告（文案对齐既有状态行） ─────────────────
//
// ShowVideoSourceDialog 只返回「是否改动」，报告口径复刻 OpenVideoSourceDialog
// 的收尾统计（L4705-4730）：按「真能取到内容的东西」分类说话。
json SourceReportJson(ttplayer::video::VideoSourceManager* mgr, bool changed) {
    json j;
    j["changed"] = changed;
    int count = 0, cms = 0, native = 0, spider = 0, lives = 0;
    try {
        const auto sources = mgr->ListSources();
        count = static_cast<int>(sources.size());
        for (const auto& src : sources) {
            lives += static_cast<int>(src.lives.size());
            for (const auto& s : src.sites) {
                if (s.kind == ttplayer::video::VideoSourceKind::kCms) ++cms;
                else if (s.kind == ttplayer::video::VideoSourceKind::kNativeSpider) ++native;
                else if (s.kind == ttplayer::video::VideoSourceKind::kSpider) ++spider;
            }
        }
    } catch (...) {
        // 统计失败不影响回执：下面按 count==0 分支给兜底文案。
    }
    std::string text;
    if (count == 0) {
        text = "尚未导入视频源，点「导入」添加影视仓 / TVBox 地址";
    } else if (cms > 0 || native > 0) {
        text = "已导入 " + std::to_string(count) + " 个视频源，点卡片查看剧集";
    } else if (lives > 0) {
        text = "已导入 " + std::to_string(count) + " 个视频源：" +
               std::to_string(lives) + " 个直播源可点开观看" +
               (spider > 0 ? "（SPIDER 站点需 TVBox 运行库，暂不支持）" : "");
    } else if (spider > 0) {
        text = "已导入 " + std::to_string(count) + " 个视频源，但 " +
               std::to_string(spider) +
               " 个站点全是 SPIDER 类型，本程序取不到内容；请换含 CMS 接口"
               "（provide/vod）的源";
    } else {
        text = "已导入 " + std::to_string(count) +
               " 个视频源，但未识别到可播放站点；请检查配置里的 api 是否有效";
    }
    j["sources"] = count;
    j["cms"] = cms;
    j["native"] = native;
    j["spider"] = spider;
    j["lives"] = lives;
    j["text"] = text;
    j["message"] = text;  // 文案双字段：页面按任一字段渲染都拿得到
    return j;
}

// ───────────────────────── 播放上下文装配 ─────────────────────────
//
// TVBox 能不能播，一大半取决于「站点自定义请求头」和「播放器 JS」有没有被
// 带到播放窗：很多 m3u8 主索引可以不带 Referer 打开，但切片 / #EXT-X-KEY
// 一律 403，表现就是「解析成功却播不出来」。以前 video.open 只把页面显式
// 传来的 headers 透传，站点配置里的 header / ua / player 全丢了。
// 这里统一按站点键装配：headers（站点头 + 页面给的覆盖）、player_url、
// 以及 playerUrl 为相对路径时的拼接基准。
struct VideoPlayContext {
    std::map<std::string, std::string> headers;
    std::string player_url;
    std::string base_url;
};

VideoPlayContext BuildVideoPlayContext(
    ttplayer::video::VideoSourceManager* mgr, const std::string& site_key,
    const std::string& line_key,
    const std::map<std::string, std::string>& page_headers) {
    VideoPlayContext ctx;
    ctx.headers = page_headers;
    if (!mgr) return ctx;
    try {
        // ListSources 带锁返回拷贝：详情线程里调用也不会与 Reload 竞态。
        const auto imported = mgr->ListSources();
        // 页面点选集时常常只带 url/line，不带 site —— 用线路名再找一轮。
        for (const std::string& want : {site_key, line_key}) {
            if (want.empty()) continue;
            for (const auto& src : imported) {
                for (const auto& s : src.sites) {
                    const std::string k = s.key.empty() ? s.name : s.key;
                    if (k != want && s.name != want) continue;
                    // 站点头先铺底，页面显式给的优先覆盖。
                    for (const auto& kv :
                         ttplayer::video::ParseVideoSiteHeaders(s)) {
                        bool overridden = false;
                        for (const auto& p : page_headers)
                            if (_stricmp(p.first.c_str(), kv.first.c_str()) == 0)
                                overridden = true;
                        if (!overridden) ctx.headers[kv.first] = kv.second;
                    }
                    ctx.player_url = s.player.empty() ? src.player_url : s.player;
                    ctx.base_url = src.source_url;
                    if (ctx.base_url.empty() &&
                        (s.api.rfind("http://", 0) == 0 ||
                         s.api.rfind("https://", 0) == 0))
                        ctx.base_url = s.api;
                    return ctx;
                }
            }
        }
    } catch (...) {}
    // 没定位到站点时，仍把订阅级 playerUrl 给出去（首页卡片来自缓存、
    // 站点键对不上的情况）。
    try {
        const auto urls = mgr->AllPlayerUrls();
        if (!urls.empty()) ctx.player_url = urls.front();
    } catch (...) {}
    return ctx;
}

// 把详情里的选集整理成播放窗的「选集」菜单数据：以 pick_line 这条线路为
// 准取集名/地址，同时按相同下标收集其它线路的同集地址（换集后仍可切换源）。
std::vector<ttplayer::video::VideoPlayerEpisode> BuildPlayerEpisodes(
    const ttplayer::video::VideoDetail& detail,
    const ttplayer::video::VideoSite& site, int pick_line) {
    std::vector<ttplayer::video::VideoPlayerEpisode> out;
    if (pick_line < 0 ||
        static_cast<std::size_t>(pick_line) >= detail.lines.size())
        return out;
    auto abs_url = [&site](std::string u) {
        if (!site.play_url.empty() && u.find("://") == std::string::npos)
            u = site.play_url + u;
        return u;
    };
    const auto& base = detail.lines[static_cast<std::size_t>(pick_line)];
    const std::size_t count = std::min<std::size_t>(base.episodes.size(), 400);
    for (std::size_t ei = 0; ei < count; ++ei) {
        ttplayer::video::VideoPlayerEpisode ep;
        ep.title = core::Utf8ToWide(base.episodes[ei].name);
        ep.line = core::Utf8ToWide(base.name);
        ep.url = core::Utf8ToWide(abs_url(base.episodes[ei].url));
        if (ep.url.empty()) continue;
        for (const auto& ln : detail.lines) {
            if (ei >= ln.episodes.size()) continue;
            const std::string u = abs_url(ln.episodes[ei].url);
            if (u.empty()) continue;
            ttplayer::video::VideoPlayerSource s;
            s.url = core::Utf8ToWide(u);
            s.name = core::Utf8ToWide(ln.name);
            ep.sources.push_back(std::move(s));
        }
        out.push_back(std::move(ep));
    }
    return out;
}

// ───────────────────────── 观看历史（播放列表「视频」栏目） ─────────────────
//
// 打开任何视频都往 <exe>\video\history.json 记一条：整部剧的选集 / 每集线路 /
// 当前集 / 进度，不是单一播放直链。主窗口播放列表的「视频」栏目读同一份文件；
// 直链失效时 OpenVideoFromHistory 还能用 site + detail_id 重新取详情解析。
std::wstring RecordVideoHistory(
    const std::string& title, const std::string& site_key,
    const std::string& detail_id, const std::string& cover,
    const std::string& line,
    const std::vector<ttplayer::video::VideoPlayerEpisode>& episodes,
    const std::wstring& url) {
    using namespace ttplayer::video;
    VideoHistoryEntry entry;
    entry.title = core::Utf8ToWide(title);
    entry.site = core::Utf8ToWide(site_key);
    entry.detail_id = core::Utf8ToWide(detail_id);
    entry.cover = core::Utf8ToWide(cover);
    entry.line = core::Utf8ToWide(line);
    entry.episodes = episodes;
    // 电影没有选集时也存成「一条一条目」，播放列表 / 续播才能统一处理。
    if (entry.episodes.empty() && !url.empty()) {
        VideoPlayerEpisode ep;
        ep.url = url;
        ep.line = entry.line;
        entry.episodes.push_back(std::move(ep));
    }
    if (entry.episodes.empty()) return {};
    entry.current_index = 0;
    for (std::size_t i = 0; i < entry.episodes.size(); ++i) {
        if (entry.episodes[i].url == url) {
            entry.current_index = static_cast<int>(i);
            break;
        }
    }
    // 与播放窗「选集」菜单同口径：最多 400 集，避免 history.json 被长剧撑大。
    // 当前集若在 400 之后，交换到第 400 位再截断，保证下次打开播的仍是这一集。
    if (entry.episodes.size() > 400) {
        if (entry.current_index >= 400) {
            std::swap(entry.episodes[399],
                      entry.episodes[static_cast<std::size_t>(entry.current_index)]);
            entry.current_index = 399;
        }
        entry.episodes.resize(400);
    }
    const std::wstring key = entry.detail_id.empty() ? url : entry.detail_id;
    entry.id = MakeVideoHistoryId(entry.site, key);
    if (entry.id.empty() || entry.id == L"|") return {};
    // 同一部片、同一集再次打开：沿用上次进度；换集则从头开始。
    VideoHistoryEntry old;
    if (FindVideoHistory(entry.id, &old) &&
        old.current_index == entry.current_index) {
        entry.position_ms = old.position_ms;
        entry.duration_ms = old.duration_ms;
    }
    const std::wstring id = entry.id;
    UpsertVideoHistory(std::move(entry));
    VideoHistoryLog("RECORD id=" + core::WideToUtf8(id) + " title=" + title +
                    " site=" + site_key + " detail=" + detail_id + " eps=" +
                    std::to_string(episodes.size()) + " line=" + line);
    return id;
}

// 打开 C++ 视频播放窗（供自动播复用）。
// 口径与上方 video.open 的 url 分支完全一致（作品皮肤外框 / parses 聚合 /
// 线路数组原样透传）。抽成函数是为了让「点卡片自动播第 1 集」不必复制一份
// 相同的参数装配逻辑；**必须在 UI 线程调用**（内部会创建窗口）。
// 返回空串=已打开；否则是中文错误原因。
std::string OpenVideoPlayerNow(
    const std::string& url, const std::string& title, const std::string& line,
    const std::map<std::string, std::string>& headers,
    const std::vector<ttplayer::video::VideoPlayerSource>& sources,
    const std::string& player_url = {},
    const std::string& player_base_url = {},
    const std::vector<ttplayer::video::VideoPlayerEpisode>& episodes = {},
    const std::string& site = {}, const std::string& detail_id = {},
    const std::string& cover = {}, int start_ms = 0,
    const std::wstring& history_id = {}) {
    if (url.empty()) return "缺少播放地址";
    const HWND owner = MusicFreeBrowserWindow();
    if (!owner) return "音乐窗已关闭，无法打开播放窗";
    // 未显式给出历史 id → 本次打开先记一条（整部剧选集 + 当前集）。
    std::wstring hid = history_id;
    if (hid.empty())
        hid = RecordVideoHistory(title, site, detail_id, cover, line, episodes,
                                 core::Utf8ToWide(url));
    // 续播：同一集有旧进度就从那里开始；快播完（离结尾 <5 秒）则从头来。
    if (start_ms <= 0) {
        ttplayer::video::VideoHistoryEntry saved;
        if (!hid.empty() &&
            ttplayer::video::FindVideoHistory(hid, &saved) &&
            saved.position_ms > 0) {
            const bool near_end = saved.duration_ms > 0 &&
                saved.position_ms + 5000 >= saved.duration_ms;
            start_ms = near_end ? 0 : saved.position_ms;
        }
    }
    const skin::LegacySkin* legacy = MusicFreeBrowserLegacySkin();
    const skin::BrowserSkin* browser =
        (legacy && legacy->Browser().valid) ? &legacy->Browser() : nullptr;
    std::vector<ttplayer::video::VideoParse> parses;
    try {
        if (auto* mgr = BridgeVideoManager()) parses = mgr->AllParses();
    } catch (...) {
        // 拿不到 parses：播放窗会回退内置默认解析接口。
    }
    try {
        ttplayer::video::ShowVideoPlayer(
            owner, core::Utf8ToWide(url), core::Utf8ToWide(title),
            core::Utf8ToWide(line), browser, headers, sources, parses,
            player_url, player_base_url, episodes, start_ms, hid);
        return {};
    } catch (const std::exception& e) {
        return std::string("打开播放窗失败: ") + e.what();
    } catch (...) {
        return "打开播放窗失败: 未知错误";
    }
}

// ───────────────────────── video.open：详情选集 → 页面视图模型 ─────────────────
//
// 选集地址补全（站点 play_url 前缀）与跨线路 sources 打包的口径完全对齐
// kMsgVideoDetailReady（L5742-5776）：页面拿到的 url 即可直接回传 video.open。
json DetailToJson(const ttplayer::video::VideoDetail& detail,
                  const ttplayer::video::VideoSite& site) {
    json result;
    result["mode"] = "detail";
    result["id"] = detail.id;
    result["title"] = detail.title;
    if (!detail.remarks.empty()) result["remarks"] = detail.remarks;
    json lines = json::array();
    std::size_t total = 0;
    for (std::size_t li = 0; li < detail.lines.size(); ++li) {
        const auto& line = detail.lines[li];
        json ln;
        ln["name"] = line.name;
        json eps = json::array();
        for (std::size_t ei = 0; ei < line.episodes.size(); ++ei) {
            const auto& ep = line.episodes[ei];
            std::string final_url = ep.url;
            if (!site.play_url.empty() && final_url.find("://") == std::string::npos)
                final_url = site.play_url + final_url;
            json e;
            e["name"] = ep.name;
            e["url"] = final_url;
            // player flag：TVBox 用它派发自定义播放器，页面回传时带上，
            // 播放窗按线路名兜底也能用。
            if (!ep.flag.empty()) e["flag"] = ep.flag;
            else if (!line.name.empty()) e["flag"] = line.name;
            // 同一集在其它线路的地址（切源按钮数据）：必须含当前线路。
            json arr = json::array();
            for (std::size_t li2 = 0; li2 < detail.lines.size(); ++li2) {
                const auto& ln2 = detail.lines[li2];
                if (ei >= ln2.episodes.size()) continue;
                std::string u = ln2.episodes[ei].url;
                if (!site.play_url.empty() && u.find("://") == std::string::npos)
                    u = site.play_url + u;
                arr.push_back(json{{"url", u}, {"name", ln2.name}});
            }
            if (!arr.empty()) e["sources"] = std::move(arr);
            eps.push_back(std::move(e));
            ++total;
        }
        ln["episodes"] = std::move(eps);
        lines.push_back(std::move(ln));
    }
    result["lines"] = std::move(lines);
    result["count"] = total;
    result["message"] = total > 0
        ? "已加载 " + std::to_string(total) + " 个选集"
        : std::string("该视频暂无可播放选集");
    return result;
}

// ───────────────────────── 跨源同名直链回退 ─────────────────
//
// 首页聚合按 NormalizeVideoTitle 跨源去重只留首份卡片，留下的站点可能正好只给
// 官方网页线路（v.qq.com / v.youku.com / bilibili.com / iqiyi.com）：点开就进
// WebView2 打开官方页，匿名/非会员只能试看。而同一配置里往往还有多个 CMS 站点
// 给出同名影片的直链 m3u8。
// 这里在当前详情「没有任何直链」时，于同一工作线程串行搜索其它站点，把命中影片
// 的直链线路并到 detail.lines 最前面（直链优先），原官方网页线路保留在后，
// 用户仍可切回（WebView2 兜底）。
bool HasDirectLine(const ttplayer::video::VideoDetail& detail) {
    for (const auto& line : detail.lines) {
        for (const auto& ep : line.episodes) {
            if (!ep.url.empty() && ttplayer::video::IsDirectMediaUrl(ep.url))
                return true;
        }
    }
    return false;
}

std::string NowStamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// 追加一段诊断日志到 <exe>\video\cross_source.log（失败静默，不影响播放）。
void AppendCrossSourceLog(const std::string& text) {
    try {
        std::wstring exe_w(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, exe_w.data(),
                                           static_cast<DWORD>(exe_w.size()));
        if (n == 0 || n >= exe_w.size()) return;
        exe_w.resize(n);
        const std::filesystem::path path =
            std::filesystem::path(exe_w).parent_path() / L"video" / L"cross_source.log";
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::app | std::ios::binary);
        if (!f) return;
        f << text << "\n";
    } catch (...) {
        // 日志失败不影响主流程。
    }
}

struct CrossSourceOutcome {
    std::vector<ttplayer::video::VideoLine> lines;  // 跨源直链线路（按命中顺序）
    std::string first_site;                         // 首个提供直链的站点名
    std::vector<std::string> hits;                  // 诊断：站点名 + 线路/集数
    int candidates = 0;                             // 除当前站点外的候选数
    int scanned = 0;                                // 实际扫描站点数
    long long elapsed_ms = 0;
};

// 片名尾部标点（CMS 站点常写「八仙！」/「八仙。」）。NormalizeVideoTitle 只去
// 空白 + 小写，同名影片会因一个感叹号漏配，这里再剥一层尾部标点。
std::string TrimTitleTail(std::string s) {
    static const char* kTails[] = {"！", "!", "？", "?", "。", "，", ",",
                                   "、", "·", "：", ":", "；", ";", " "};
    bool changed = true;
    while (changed && !s.empty()) {
        changed = false;
        for (const char* tail : kTails) {
            const std::string t(tail);
            if (s.size() >= t.size() &&
                s.compare(s.size() - t.size(), t.size(), t) == 0) {
                s.resize(s.size() - t.size());
                changed = true;
                break;
            }
        }
    }
    return s;
}

// 候选条目是否就是当前影片：先按既有去重键（带年 / 不带年），再按原文与
// 「去尾部标点」兜底，避免《八仙！》与《八仙》这类站点写法差异漏配。
bool SameVideoTitle(const ttplayer::video::VideoItem& it,
                    const std::string& title, const std::string& want) {
    if (it.title == title) return true;
    if (NormalizeVideoTitle(it.title, it.year) == want) return true;
    const std::string plain = NormalizeVideoTitle(it.title, "");
    if (plain == want) return true;
    return TrimTitleTail(plain) == TrimTitleTail(want);
}

// 串行扫描其它站点找同名影片的直链线路。整体截止 25 秒、最多 40 个站点；
// 拿到 ≥6 条直链线路或 ≥3 个可用站点即提前结束；单站点异常跳过。
CrossSourceOutcome FindCrossSourceDirectLines(
    ttplayer::video::VideoSourceManager* mgr, const std::string& current_key,
    const std::string& title) {
    CrossSourceOutcome out;
    if (!mgr || title.empty()) return out;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&started] {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
    };

    // 失效站点按全局 20s 超时会拖死整段，这里把本线程后续请求压到 6s。
    ttplayer::musicfree::ScopedHttpTimeout guard(6000);

    std::vector<ttplayer::video::VideoSite> sites;
    try {
        mgr->Reload();
        sites = mgr->AllSites();
    } catch (...) {
        out.elapsed_ms = elapsed_ms();
        return out;
    }
    DropCloudSites(&sites);   // 纯网盘源不参与直链扫描
    for (const auto& s : sites) {
        const std::string skey = s.key.empty() ? s.name : s.key;
        if (skey != current_key) ++out.candidates;
    }

    const std::string want = NormalizeVideoTitle(title, "");
    // 单个请求最长 6s（video_cms 内部也把单请求超时压到 6s）：只在剩余预算
    // 还够一次请求时才发起下一个请求，保证整段 ≤ 25s 返回。
    constexpr long long kBudgetMs = 25000;
    constexpr long long kRequestMs = 6000;
    std::set<std::string> seen_urls;
    int hit_sites = 0;
    for (const auto& s : sites) {
        if (out.scanned >= 40) break;
        if (elapsed_ms() + kRequestMs > kBudgetMs) break;
        if (IsMusicFreeShuttingDown()) break;
        const std::string skey = s.key.empty() ? s.name : s.key;
        if (skey == current_key) continue;
        ++out.scanned;
        try {
            const ttplayer::video::VideoListing listing =
                ttplayer::video::FetchSearch(s, title, 1, mgr->network());
            const ttplayer::video::VideoItem* hit = nullptr;
            for (const auto& it : listing.items) {
                if (SameVideoTitle(it, title, want)) {
                    hit = &it;
                    break;
                }
            }
            if (!hit) continue;
            if (elapsed_ms() + kRequestMs > kBudgetMs) break;
            const ttplayer::video::VideoDetail detail =
                ttplayer::video::FetchDetail(s, *hit, mgr->network());
            int new_lines = 0;
            std::size_t new_eps = 0;
            for (const auto& line : detail.lines) {
                ttplayer::video::VideoLine cross;
                cross.name = s.name + "·" + line.name;
                for (const auto& ep : line.episodes) {
                    if (ep.url.empty()) continue;
                    if (!ttplayer::video::IsDirectMediaUrl(ep.url)) continue;
                    if (!seen_urls.insert(ep.url).second) continue;
                    cross.episodes.push_back(ep);
                }
                if (cross.episodes.empty()) continue;
                new_eps += cross.episodes.size();
                ++new_lines;
                out.lines.push_back(std::move(cross));
            }
            if (new_lines > 0) {
                ++hit_sites;
                if (out.first_site.empty()) out.first_site = s.name;
                out.hits.push_back(s.name + "(线路" + std::to_string(new_lines) +
                                   "/集" + std::to_string(new_eps) + ")");
            }
        } catch (...) {
            // 单个站点搜索 / 详情失败不影响其它源。
        }
        if (out.lines.size() >= 6 || hit_sites >= 3) break;
    }
    out.elapsed_ms = elapsed_ms();
    return out;
}

// ───────────────────────── 跨源同名详情回退 ─────────────────
//
// 图3：当前站点详情返回被截断的 JSON（parse_error.101），或详情里一条选集
// 都没有。此时不该把「加载选集失败」直接丢给用户 —— 同一配置里往往还有别的
// 站点收录了同名影片。这里按片名串行搜索其它站点，取第一个能成功拿到选集的
// 详情，并带回该站点（play_url 前缀口径不同，必须一起换）。
struct CrossDetailOutcome {
    bool found = false;
    ttplayer::video::VideoSite site;
    ttplayer::video::VideoDetail detail;
    int scanned = 0;          // 实际扫描站点数
    long long elapsed_ms = 0;
};

CrossDetailOutcome FindCrossSourceDetail(
    ttplayer::video::VideoSourceManager* mgr, const std::string& current_key,
    const std::string& title) {
    CrossDetailOutcome out;
    if (!mgr || title.empty()) return out;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&started] {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
    };

    // 失效站点按全局 20s 超时会拖死整段，这里把本线程后续请求压到 6s。
    ttplayer::musicfree::ScopedHttpTimeout guard(6000);

    std::vector<ttplayer::video::VideoSite> sites;
    try {
        sites = mgr->AllSites();
    } catch (...) {
        return out;
    }
    DropCloudSites(&sites);   // 纯网盘源不参与跨源详情回退

    // 原请求最多已花 6s、页面 video.open 超时 30s：回退段预算压到 18s。
    // 但 36 个源串行扫、每站最长 6s 时实测要 18~23s——用户看到的就是
    // 「点开影片半天不出选集」。改成 6 线程并行抢答：谁先拿到同名详情就用谁，
    // 其余 worker 见到 stop 立即退出，整段预算压到 8s。
    constexpr long long kBudgetMs = 8000;
    constexpr int kWorkers = 6;
    constexpr int kMaxScan = 24;
    std::atomic<bool> stop{false};
    std::atomic<int> scanned{0};
    std::atomic<std::size_t> cursor{0};
    std::mutex out_mtx;
    const std::string want = NormalizeVideoTitle(title, "");

    auto worker = [&] {
        // ScopedHttpTimeout 是每线程覆盖值：各 worker 独立设置互不影响。
        ttplayer::musicfree::ScopedHttpTimeout guard(6000);
        for (;;) {
            if (stop.load(std::memory_order_relaxed) ||
                IsMusicFreeShuttingDown())
                return;
            if (elapsed_ms() > kBudgetMs) return;
            if (scanned.load(std::memory_order_relaxed) >= kMaxScan) return;
            const std::size_t i = cursor.fetch_add(1);
            if (i >= sites.size()) return;
            const auto& s = sites[i];
            const std::string skey = s.key.empty() ? s.name : s.key;
            if (skey == current_key) continue;
            scanned.fetch_add(1, std::memory_order_relaxed);
            try {
                const ttplayer::video::VideoListing listing =
                    ttplayer::video::FetchSearch(s, title, 1, mgr->network());
                const ttplayer::video::VideoItem* hit = nullptr;
                for (const auto& it : listing.items) {
                    if (SameVideoTitle(it, title, want)) {
                        hit = &it;
                        break;
                    }
                }
                if (!hit) continue;
                if (stop.load(std::memory_order_relaxed)) return;
                ttplayer::video::VideoDetail detail =
                    ttplayer::video::FetchDetail(s, *hit, mgr->network());
                if (detail.lines.empty()) continue;
                if (detail.title.empty()) detail.title = hit->title;
                if (detail.pic.empty()) detail.pic = hit->pic;
                std::lock_guard<std::mutex> lk(out_mtx);
                if (out.found) return;   // 别的 worker 抢先拿到
                out.found = true;
                out.site = s;
                out.detail = std::move(detail);
                stop.store(true, std::memory_order_relaxed);
                return;
            } catch (...) {
                // 单个站点搜索 / 详情失败继续看下一个源。
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(kWorkers);
    for (int i = 0; i < kWorkers && i < static_cast<int>(sites.size()); ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        if (t.joinable()) t.join();
    out.scanned = scanned.load();
    out.elapsed_ms = elapsed_ms();
    return out;
}

// ───────────────────────── 视频首页磁盘缓存 ─────────────────
//
// 首页聚合是「72 个站点各起一线程」的重量级操作，每次打开音乐窗都重跑一遍就是
// 用户感知到的「视频首页要等很久」。这里把上一轮的完整首页 JSON 落盘到
// <exe>\video\home_cache.json：
//   · signature：对 mgr->AllSites() 的 (key|api) 排序后做 FNV-1a，站点增删 /
//     改地址即不匹配；导入成功后 video.sources 会直接删文件，双保险；
//   · TTL 30 分钟：未过期不重新聚合，避免每次开窗 72 站齐发；
//   · 过期 / 无缓存：命中旧缓存也先推页面（秒出），再后台聚合，成功后回写。
constexpr std::int64_t kHomeCacheTtlSeconds = 30 * 60;
constexpr std::uintmax_t kHomeCacheMaxBytes = 16 * 1024 * 1024;

std::int64_t NowUnixSeconds() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// exe 同级 video 目录（与 BridgeVideoManager 的站点缓存同目录）。
std::filesystem::path HomeCachePath() {
    std::wstring exe_w(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe_w.data(),
                                       static_cast<DWORD>(exe_w.size()));
    if (n == 0 || n >= exe_w.size()) return {};
    exe_w.resize(n);
    return std::filesystem::path(exe_w).parent_path() / L"video" /
           L"home_cache.json";
}

std::string Fnv1aHex(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a 64 位 offset basis
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        hash ^= c;
        hash *= 1099511628211ULL;                 // FNV 质数
    }
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(hash));
    return buf;
}

// 只读 AllSites()（返回值是拷贝，管理器内部 loaded_ 另有锁），无竞态。
std::string HomeSitesSignature(ttplayer::video::VideoSourceManager* mgr) {
    if (!mgr) return {};
    std::vector<std::string> parts;
    try {
        const auto sites = mgr->AllSites();
        parts.reserve(sites.size());
        for (const auto& s : sites) {
            const std::string key = s.key.empty() ? s.name : s.key;
            parts.push_back(key + "|" + s.api);
        }
    } catch (...) {
        return {};
    }
    if (parts.empty()) return {};
    std::sort(parts.begin(), parts.end());
    std::string joined;
    for (const auto& p : parts) {
        joined += p;
        joined.push_back('\n');
    }
    // 版本后缀：首页聚合结构演进（如新增 cats 分类页签）时改这里，旧缓存签名
    // 不匹配会被强制重新聚合，避免老缓存把新版字段（cats）吞掉。
    return Fnv1aHex(joined) + "#catv1";
}

// 签名必须匹配且 out.sections 非空才算有效（避免把上次的失败页当缓存推）。
bool ReadHomeCacheFile(const std::filesystem::path& path,
                       const std::string& signature, json* out,
                       std::int64_t* saved_at) {
    if (!out || path.empty() || signature.empty()) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kHomeCacheMaxBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    json root;
    try {
        in >> root;
    } catch (...) {
        return false;
    }
    if (!root.is_object()) return false;
    std::int64_t saved = 0;
    std::string sig;
    try {
        if (root.at("version").get<int>() != 1) return false;
        saved = root.at("saved_at").get<std::int64_t>();
        sig = root.at("signature").get<std::string>();
    } catch (...) {
        return false;
    }
    if (saved <= 0 || sig != signature) return false;
    const auto it = root.find("out");
    if (it == root.end() || !it->is_object()) return false;
    const auto sections = it->find("sections");
    if (sections == it->end() || !sections->is_array() || sections->empty())
        return false;
    *out = *it;
    // 旧缓存里可能残留第三方图片代理地址（wsrv.nl，本机连不通，表现为
    // 「有的封面空白」）：读出时统一解包，不必等 TTL 过期重新聚合。
    if (auto secs = out->find("sections");
        secs != out->end() && secs->is_array()) {
        for (auto& sec : *secs) {
            if (!sec.is_object()) continue;
            auto cards = sec.find("cards");
            if (cards == sec.end() || !cards->is_array()) continue;
            for (auto& c : *cards) {
                if (!c.is_object()) continue;
                auto cov = c.find("cover");
                if (cov == c.end() || !cov->is_string()) continue;
                const std::string v = cov->get<std::string>();
                const std::string u = UnwrapImageProxy(v);
                if (u != v) *cov = u;
            }
        }
    }
    // 旧版本缓存里的失败站点列表不再使用（页面不再展示该横幅）。
    out->erase("errors");
    if (saved_at) *saved_at = saved;
    return true;
}

// 先写 .tmp 再原子替换；只有拿到非空分区才写（失败轮次不污染好缓存）。
void WriteHomeCacheFile(const std::filesystem::path& path,
                        const std::string& signature, const json& out) {
    if (path.empty() || signature.empty() || !out.is_object()) return;
    const auto sections = out.find("sections");
    if (sections == out.end() || !sections->is_array() || sections->empty()) return;
    try {
        json root = json::object();
        root["version"] = 1;
        root["saved_at"] = NowUnixSeconds();
        root["signature"] = signature;
        root["out"] = out;
        const std::string text = root.dump();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path tmp(path.wstring() + L".tmp");
        {
            std::ofstream fout(tmp, std::ios::binary | std::ios::trunc);
            if (!fout) return;
            fout.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            std::filesystem::remove(tmp, ec);
    } catch (...) {
        // 缓存写失败不影响页面（下一轮仍可重新聚合）。
    }
}

}  // namespace

// ───────────────────────── 命令注册 ─────────────────────────

void RegisterVideoCommands(MusicFreeBridge& bridge) {
    // ── video.home：首页聚合 → evt video.home 推回分区卡片 ──
    // payload: { force?: bool }（保留字段；缓存未过期时不会重新聚合）。
    // 有缓存且签名匹配 → 立刻推 evt（页面秒开），未过期（30 分钟）连网络都不动；
    // 过期 / 无缓存才起线程聚合，成功后回写缓存再推更新。
    bridge.Register("video.home",
                    [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
                        (void)payload;
                        auto* mgr = BridgeVideoManager();
                        if (!mgr) {
                            reply(false, json{},
                                  "视频源管理器初始化失败（读不到已导入的视频源）");
                            return;
                        }
                        const std::string signature = HomeSitesSignature(mgr);
                        json cached_out;
                        std::int64_t saved_at = 0;
                        bool served = false;
                        if (ReadHomeCacheFile(HomeCachePath(), signature, &cached_out,
                                              &saved_at)) {
                            if (BridgeAlive()) {
                                bridge.PushEvent("video.home", cached_out);
                                served = true;
                            }
                        }
                        if (served) {
                            const std::int64_t now = NowUnixSeconds();
                            if (saved_at <= now &&
                                now - saved_at < kHomeCacheTtlSeconds) {
                                reply(true,
                                      json{{"pending", true}, {"cached", true}},
                                      {});
                                return;
                            }
                        }
                        if (g_home_busy.exchange(true)) {
                            reply(true,
                                  json{{"pending", true},
                                       {"already_running", true},
                                       {"cached", served}},
                                  {});
                            return;
                        }
                        try {
                            std::thread([&bridge, mgr, signature, served] {
                                FlagReset reset{g_home_busy};
                                BgThreadGuard guard;
                                json out;
                                try {
                                    out = AggregateVideoHome(mgr);
                                } catch (const std::exception& e) {
                                    out = json{{"sections", json::array()},
                                               {"error",
                                                std::string("视频首页聚合失败: ") + e.what()}};
                                } catch (...) {
                                    out = json{{"sections", json::array()},
                                               {"error", "视频首页聚合失败: 未知错误"}};
                                }
                                if (!BridgeAlive()) return;
                                const auto sections = out.find("sections");
                                const bool got =
                                    sections != out.end() &&
                                    sections->is_array() && !sections->empty();
                                if (!got) {
                                    // 聚合失败 / 全空：已显示缓存的页面保持原样，
                                    // 绝不用空数据覆盖；本来就没缓存才维持旧行为。
                                    if (!served) bridge.PushEvent("video.home", out);
                                    return;
                                }
                                // 聚合过程中可能已 Reload 到新站点表，写缓存用刷新后
                                // 的签名（与读取端同一算法）。
                                const std::string sig = HomeSitesSignature(mgr);
                                WriteHomeCacheFile(HomeCachePath(),
                                                   sig.empty() ? signature : sig,
                                                   out);
                                bridge.PushEvent("video.home", out);
                            }).detach();
                        } catch (...) {
                            g_home_busy.store(false);
                            reply(false, json{}, "无法启动视频聚合线程（资源不足）");
                            return;
                        }
                        reply(true, json{{"pending", true}, {"cached", served}}, {});
                    });

    // ── video.category：分类页签聚合 → evt video.category 推回分区卡片 ──
    // payload: { sources: [{key, id}] }（来自视频首页 evt 的 cats[].sources，
    // 即「归一名」对应的各站 (站点key, class_id) 映射）。逐站 FetchCategory
    // 拉该分类第一页，交错去重后分桶。与 video.home 同模式（accepted + evt），
    // 避免阻塞 UI 线程（分类聚合也涉及多站并发网络请求）。
    bridge.Register(
        "video.category",
        [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
            auto* mgr = BridgeVideoManager();
            if (!mgr) {
                reply(false, json{},
                      "视频源管理器初始化失败（读不到已导入的视频源）");
                return;
            }
            if (g_cat_busy.exchange(true)) {
                reply(true, json{{"pending", true}, {"already_running", true}}, {});
                return;
            }
            try {
                const json sources = payload.value("sources", json::array());
                const std::string cat_title = payload.value("cat", "");
                std::thread([&bridge, mgr, sources, cat_title] {
                    FlagReset reset{g_cat_busy};
                    BgThreadGuard guard;
                    json out;
                    try {
                        out = AggregateVideoCategory(mgr, sources, cat_title);
                    } catch (const std::exception& e) {
                        out = json{{"sections", json::array()},
                                   {"error",
                                    std::string("分类加载失败: ") + e.what()}};
                    } catch (...) {
                        out = json{{"sections", json::array()},
                                   {"error", "分类加载失败: 未知错误"}};
                    }
                    if (!BridgeAlive()) return;
                    bridge.PushEvent("video.category", out);
                }).detach();
            } catch (...) {
                g_cat_busy.store(false);
                reply(false, json{}, "无法启动分类聚合线程（资源不足）");
                return;
            }
            reply(true, json{{"pending", true}}, {});
        });

    // ── video.open：打开视频 ──
    // payload（页面点卡片时直接带全即可播）：
    //   { url, title?, line?, sources?: [{url,name}], headers?: {k:v}, site? }
    // payload（页面点卡片尚无播放地址，只带卡片标识）：
    //   { id, site, title? } → 取详情（多路线路选集）回给页面渲染 #episode-list
    bridge.Register("video.open",
                    [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
                        const std::string url = JsonStr(payload, "url");
                        if (!url.empty()) {
                            try {
                                std::map<std::string, std::string> headers;
                                if (payload.contains("headers") &&
                                    payload.at("headers").is_object()) {
                                    const json& h = payload.at("headers");
                                    for (auto it = h.begin(); it != h.end(); ++it)
                                        if (it.value().is_string())
                                            headers[it.key()] =
                                                it.value().get<std::string>();
                                }
                                // 线路数组原样透传（不做增删改，口径同
                                // PlaySelectedVideoHit L3056-3068）。
                                std::vector<ttplayer::video::VideoPlayerSource> sources;
                                if (payload.contains("sources") &&
                                    payload.at("sources").is_array()) {
                                    for (const auto& s : payload.at("sources")) {
                                        if (!s.is_object() ||
                                            !s.contains("url") ||
                                            !s["url"].is_string())
                                            continue;
                                        ttplayer::video::VideoPlayerSource src;
                                        src.url = core::Utf8ToWide(
                                            s["url"].get<std::string>());
                                        if (s.contains("name") &&
                                            s["name"].is_string())
                                            src.name = core::Utf8ToWide(
                                                s["name"].get<std::string>());
                                        sources.push_back(std::move(src));
                                    }
                                }
                                // 剧集选集：页面点某一集时会把整部剧的选集/
                                // 每集线路一起带上来，播放窗据此显示「选集」。
                                std::vector<ttplayer::video::VideoPlayerEpisode>
                                    episodes;
                                if (payload.contains("episodes") &&
                                    payload.at("episodes").is_array()) {
                                    for (const auto& e : payload.at("episodes")) {
                                        if (!e.is_object() || !e.contains("url") ||
                                            !e["url"].is_string())
                                            continue;
                                        ttplayer::video::VideoPlayerEpisode ep;
                                        ep.url = core::Utf8ToWide(
                                            e["url"].get<std::string>());
                                        if (ep.url.empty()) continue;
                                        if (e.contains("title") &&
                                            e["title"].is_string())
                                            ep.title = core::Utf8ToWide(
                                                e["title"].get<std::string>());
                                        if (e.contains("line") &&
                                            e["line"].is_string())
                                            ep.line = core::Utf8ToWide(
                                                e["line"].get<std::string>());
                                        if (e.contains("sources") &&
                                            e["sources"].is_array()) {
                                            for (const auto& s : e["sources"]) {
                                                if (!s.is_object() ||
                                                    !s.contains("url") ||
                                                    !s["url"].is_string())
                                                    continue;
                                                ttplayer::video::VideoPlayerSource
                                                    src;
                                                src.url = core::Utf8ToWide(
                                                    s["url"].get<std::string>());
                                                if (s.contains("name") &&
                                                    s["name"].is_string())
                                                    src.name = core::Utf8ToWide(
                                                        s["name"].get<std::string>());
                                                ep.sources.push_back(
                                                    std::move(src));
                                            }
                                        }
                                        episodes.push_back(std::move(ep));
                                    }
                                }
                                const std::string title = JsonStr(payload, "title");
                                // 线路名取页面给的 line（没有则回退站点名），
                                // 不拿 title 顶替——播放窗标题栏按「线路」显示。
                                std::string line = JsonStr(payload, "line");
                                if (line.empty()) line = JsonStr(payload, "site");
                                const skin::LegacySkin* legacy =
                                    MusicFreeBrowserLegacySkin();
                                const skin::BrowserSkin* browser =
                                    (legacy && legacy->Browser().valid)
                                        ? &legacy->Browser()
                                        : nullptr;
                                std::vector<ttplayer::video::VideoParse> parses;
                                auto* vmgr = BridgeVideoManager();
                                try {
                                    if (vmgr) parses = vmgr->AllParses();
                                } catch (...) {
                                    // 拿不到 parses：播放窗会回退内置默认解析接口。
                                }
                                // 站点 header / ua / playerUrl 一并装配：不带
                                // Referer 的 m3u8 切片会被 CDN 拒掉（403），这是
                                // TVBox 能播而这里播不了的主因之一。
                                const VideoPlayContext pctx =
                                    BuildVideoPlayContext(
                                        vmgr, JsonStr(payload, "site"), line,
                                        headers);
                                const HWND owner = MusicFreeBrowserWindow();
                                if (!owner) {
                                    reply(false, json{},
                                          "音乐窗已关闭，无法打开播放窗");
                                    return;
                                }
                                // 观看历史：整部剧选集 + 当前集（播放列表「视频」
                                // 栏目与续播都读这一条；直链失效可按 site+id 重解析）。
                                const std::wstring history_id = RecordVideoHistory(
                                    title, JsonStr(payload, "site"),
                                    JsonStr(payload, "id"), {}, line, episodes,
                                    core::Utf8ToWide(url));
                                ttplayer::video::ShowVideoPlayer(
                                    owner, core::Utf8ToWide(url),
                                    core::Utf8ToWide(title),
                                    core::Utf8ToWide(line), browser,
                                    pctx.headers, sources, parses,
                                    pctx.player_url, pctx.base_url, episodes,
                                    0, history_id);
                                reply(true,
                                      json{{"mode", "play"}, {"opened", true}},
                                      {});
                            } catch (const std::exception& e) {
                                reply(false, json{},
                                      std::string("打开播放窗失败: ") + e.what());
                            } catch (...) {
                                reply(false, json{}, "打开播放窗失败: 未知错误");
                            }
                            return;
                        }

                        // 无 url → 取详情（选集）：站点定位口径与
                        // StartOpenVideoItem（L4409-4410）一致：key 空回退 name。
                        const std::string id = JsonStr(payload, "id");
                        const std::string site_key = JsonStr(payload, "site");
                        const std::string title = JsonStr(payload, "title");
                        // id / site 缺失（部分来源的聚合条目不带站点键/详情编号）
                        // 不再直接报错：只要带片名就放行，进详情线程按片名走
                        // 跨源同名回退（FindCrossSourceDetail）拿到选集。
                        const bool missing_identity =
                            id.empty() || site_key.empty();
                        if (missing_identity && title.empty()) {
                            reply(false, json{},
                                  "缺少 payload.url（播放地址）或 payload.id / "
                                  "payload.site（卡片标识）");
                            return;
                        }
                        if (site_key.rfind("live:", 0) == 0) {
                            // 直播卡的开播链路（StartOpenVideoLive）无公开入口，
                            // 结论见文件头注释。
                            reply(false, json{}, "直播入口待接线");
                            return;
                        }
                        auto* mgr = BridgeVideoManager();
                        if (!mgr) {
                            reply(false, json{},
                                  "视频源管理器初始化失败（读不到已导入的视频源）");
                            return;
                        }
                        ttplayer::video::VideoSite site;
                        bool found = false;
                        try {
                            for (const auto& s : mgr->AllSites()) {
                                if ((s.key.empty() ? s.name : s.key) == site_key) {
                                    site = s;
                                    found = true;
                                    break;
                                }
                            }
                        } catch (...) {
                            found = false;
                        }
                        // 站点消失（远程源清单更新删了它，而首页卡片来自旧缓存）
                        // 或卡片缺标识：都进详情线程按片名走跨源同名详情回退
                        // （FindCrossSourceDetail），所有源都拿不到才报错。
                        const std::string site_missing_error =
                            missing_identity
                                ? "卡片缺少站点/编号标识，已按片名跨源搜索"
                                : "未找到站点: " + site_key +
                                      "（可能已被删除，请重新导入视频源）";
                        try {
                            std::thread([&bridge, reply, mgr, site, found,
                                         id, site_key, title,
                                         site_missing_error] {
                                BgThreadGuard guard;
                                json result;
                                bool ok = true;
                                std::string error;
                                try {
                                    if (IsMusicFreeShuttingDown()) return;
                                    ttplayer::video::VideoItem item;
                                    item.id = id;
                                    item.title = title;
                                    item.site_key = site_key;

                                    // 详情取自哪个站点：正常情况下就是被点的站点；
                                    // 该站数据损坏（图3 的截断 JSON）、没有选集、
                                    // 甚至整站已从最新源清单消失（found=false）时，
                                    // 换成同名影片的其它源继续，而不是直接报错。
                                    ttplayer::video::VideoSite use_site = site;
                                    ttplayer::video::VideoDetail detail;
                                    std::string first_error;
                                    bool got = false;
                                    // 聚合卡片/旧缓存常缺详情编号（vod_id）：先在
                                    // 该站按片名搜一次拿真实编号，再取详情。以前
                                    // 缺编号直接进跨源扫描，本可以 1 次请求解决。
                                    if (found && item.id.empty() &&
                                        !item.title.empty()) {
                                        try {
                                            const auto listing =
                                                ttplayer::video::FetchSearch(
                                                    site, item.title, 1,
                                                    mgr->network());
                                            const std::string want_key =
                                                NormalizeVideoTitle(item.title, "");
                                            for (const auto& it : listing.items) {
                                                if (it.id.empty()) continue;
                                                if (!SameVideoTitle(it, item.title,
                                                                    want_key))
                                                    continue;
                                                item.id = it.id;
                                                if (!it.raw.is_null())
                                                    item.raw = it.raw;
                                                break;
                                            }
                                        } catch (...) {
                                        }
                                    }
                                    try {
                                        if (found) {
                                            detail = ttplayer::video::FetchDetail(
                                                site, item, mgr->network());
                                            got = true;
                                        } else {
                                            first_error = site_missing_error;
                                            // 诊断：卡片里的 site 已不在当前源清单
                                            // 时，把清单前 8 个 key(名字) 打出来，
                                            // 便于判断是「key 变了」还是「源被删了」。
                                            std::string sample;
                                            int total = 0;
                                            try {
                                                for (const auto& s : mgr->AllSites()) {
                                                    ++total;
                                                    if (sample.size() > 400) continue;
                                                    if (!sample.empty()) sample += ", ";
                                                    sample +=
                                                        (s.key.empty() ? s.name : s.key) +
                                                        "(" + s.name + ")";
                                                }
                                            } catch (...) {
                                            }
                                            AppendCrossSourceLog(
                                                "[" + NowStamp() +
                                                "] OPEN site-miss site=" + site_key +
                                                " total=" + std::to_string(total) +
                                                " list=" + sample);
                                        }
                                    } catch (const std::exception& e) {
                                        first_error = e.what();
                                    } catch (...) {
                                        first_error = "未知错误";
                                    }
                                    // 诊断：详情链路每一段的真实结果（用户反馈
                                    // 「无法得到任何选集」时，看这一行就能分清是
                                    // 站点没匹配上、详情接口空、还是跨源也没命中）。
                                    AppendCrossSourceLog(
                                        "[" + NowStamp() + "] OPEN 《" + title +
                                        "》 id=" + id + " site=" + site_key +
                                        (found ? " found=1(" + site.name + ")"
                                               : " found=0") +
                                        " detail=" + (got ? "ok" : "fail") +
                                        " lines=" +
                                        std::to_string(detail.lines.size()) +
                                        (first_error.empty()
                                             ? ""
                                             : " err=" + first_error));
                                    // 网盘线路剔除（用户：「需要登录的源都不要」）：
                                    // 玩偶哥哥这类「网盘资源站」的选集全是夸克/UC
                                    // 分享链接，线路名形如「复制《xx》xx地址」——
                                    // 整条线路的选集全是网盘页就剔除；全被剔光则
                                    // lines.empty() 触发下面的跨源同名回退，去其它
                                    // CMS 源找直链。空线路一并剔除（无用）。
                                    bool cloud_only = false;
                                    if (got) {
                                        const size_t before =
                                            detail.lines.size();
                                        detail.lines.erase(
                                            std::remove_if(
                                                detail.lines.begin(),
                                                detail.lines.end(),
                                                [](const ttplayer::video::VideoLine& line) {
                                                    for (const auto& ep : line.episodes)
                                                        if (!ttplayer::video::IsCloudShareUrl(
                                                                ep.url))
                                                            return false;
                                                    return true;
                                                }),
                                            detail.lines.end());
                                        cloud_only = before > 0 &&
                                                     detail.lines.empty();
                                        // 原站有线路、但全是网盘分享页：这不是
                                        // 「站点坏了」，而是该源本身就是网盘资源
                                        // 站。把原因说清楚，避免用户以为程序坏了。
                                        if (cloud_only) {
                                            first_error =
                                                "该片在当前源只有网盘分享链接"
                                                "（百度/夸克/UC 等），需要登录网盘"
                                                "客户端才能播放，播放器无法直接播放";
                                            // 学习：这个源只有网盘资源，之后首页 /
                                            // 搜索 / 跨源扫描都跳过它。同时删掉首页
                                            // 缓存，下次聚合就不会再出现它的卡片。
                                            MarkCloudSite(site_key);
                                            std::error_code rmec;
                                            std::filesystem::remove(
                                                CloudSitesPath().parent_path() /
                                                    L"home_cache.json",
                                                rmec);
                                        }
                                    }
                                    const std::string match_title =
                                        title.empty() ? detail.title : title;
                                    CrossDetailOutcome alt;
                                    if ((!got || detail.lines.empty()) &&
                                        !match_title.empty()) {
                                        alt = FindCrossSourceDetail(
                                            mgr, site_key, match_title);
                                        if (!alt.found)
                                            AppendCrossSourceLog(
                                                "[" + NowStamp() +
                                                "] OPEN cross-miss 《" +
                                                match_title + "》 scanned:" +
                                                std::to_string(alt.scanned) +
                                                " 耗时:" +
                                                std::to_string(alt.elapsed_ms) +
                                                "ms");
                                        if (alt.found) {
                                            detail = std::move(alt.detail);
                                            use_site = alt.site;
                                            got = true;
                                            AppendCrossSourceLog(
                                                "[" + NowStamp() + "] 详情回退《" +
                                                match_title + "》 原站:" +
                                                (found ? site.name
                                                       : site_key + "（已消失）") +
                                                " → " +
                                                alt.site.name + " 扫描:" +
                                                std::to_string(alt.scanned) +
                                                " 耗时:" +
                                                std::to_string(alt.elapsed_ms) +
                                                "ms 原因:" +
                                                (first_error.empty()
                                                     ? "当前站点无选集"
                                                     : first_error));
                                        }
                                    }
                                    // 剔除网盘线路后一行不剩（cloud_only）时
                                    // 也要报错：以前只判 !got，结果 reply 成功但
                                    // lines=[]，页面只显示空列表，用户以为「选集
                                    // 没了」却看不到原因。
                                    if (!got || detail.lines.empty()) {
                                        throw std::runtime_error(
                                            first_error.empty()
                                                ? "当前站点没有返回可用选集"
                                                : first_error);
                                    }

                                    // 「预告片 / 数据接口」类源：选集地址是返回 JSON
                                    // 的接口（?view=api&mode=download-data，含
                                    // movurl 直链），不是网页播放页。这里先把每条
                                    // 线路的第一集（即自动播的那一集）解析成真实
                                    // 直链写回：
                                    //   · 播放窗拿到直链即可秒开，不用再走 15s 爬虫；
                                    //   · 让下面的 HasDirectLine 判定成立，跳过
                                    //     30+ 站点的 10~25 秒跨源扫描。
                                    for (auto& ln : detail.lines) {
                                        if (ln.episodes.empty()) continue;
                                        auto& first_ep = ln.episodes.front();
                                        if (first_ep.url.empty() ||
                                            !ttplayer::video::LooksLikeJsonApiUrl(
                                                first_ep.url))
                                            continue;
                                        std::string media;
                                        try {
                                            media =
                                                ttplayer::video::ResolveJsonApiMedia(
                                                    first_ep.url, mgr->network(),
                                                    {});
                                        } catch (...) {
                                            media.clear();
                                        }
                                        if (!media.empty()) first_ep.url = media;
                                    }

                                    // 当前详情全是官方网页线路（没有任何直链）时，
                                    // 去其它站点找同名影片的直链线路并前置：播放窗
                                    // 因此直接起流，官方线路留在后面兜底。
                                    CrossSourceOutcome cross;
                                    if (!HasDirectLine(detail)) {
                                        const std::string eff_key =
                                            use_site.key.empty() ? use_site.name
                                                                 : use_site.key;
                                        cross = FindCrossSourceDirectLines(
                                            mgr, eff_key, match_title);
                                        if (!cross.lines.empty()) {
                                            detail.lines.insert(
                                                detail.lines.begin(),
                                                cross.lines.begin(),
                                                cross.lines.end());
                                        }
                                        std::string hits;
                                        for (const auto& h : cross.hits)
                                            hits += (hits.empty() ? "" : "；") + h;
                                        AppendCrossSourceLog(
                                            "[" + NowStamp() + "] 影片《" +
                                            match_title + "》 当前站点:" +
                                            use_site.name + " 候选:" +
                                            std::to_string(cross.candidates) +
                                            " 扫描:" +
                                            std::to_string(cross.scanned) +
                                            " 命中:" +
                                            (hits.empty() ? "无" : hits) +
                                            " 直链线路:" +
                                            std::to_string(cross.lines.size()) +
                                            " 耗时:" +
                                            std::to_string(cross.elapsed_ms) +
                                            "ms 结果:" +
                                            (cross.lines.empty()
                                                 ? "未找到，保持官方网页"
                                                 : "已并入直链线路"));
                                    }
                                    // ── TVBox playerContent 层（C++ 原生 spider）──
                                    // Java spider jar 的播放解析对应 SpiderEngine::Player，
                                    // 接口定义了却从没被调用过：详情给的是播放串，
                                    // 直接交给播放器就成了「解析成功却播不出来」。
                                    // 这里按线路 flag 逐集过一遍；普通 m3u8 线路的
                                    // Player 是 return 原址（零成本），只有自定义 flag
                                    // 才真正执行解析。放在详情工作线程，不卡 UI。
                                    if (got && use_site.kind ==
                                        ttplayer::video::VideoSourceKind::kNativeSpider) {
                                        for (auto& ln : detail.lines) {
                                            if (ttplayer::video::IsBuiltinPlayerFlag(
                                                    ln.name))
                                                continue;
                                            for (auto& ep : ln.episodes) {
                                                try {
                                                    const std::string resolved =
                                                        ttplayer::video::SpiderPlayer(
                                                            use_site, ep,
                                                            mgr->network());
                                                    if (!resolved.empty() &&
                                                        resolved != ep.url)
                                                        ep.url = resolved;
                                                } catch (...) {}
                                            }
                                        }
                                    }
                                    result = DetailToJson(detail, use_site);
                                    result["site"] = site_key;
                                    if (!cross.first_site.empty())
                                        result["crossSource"] = cross.first_site;
                                    if (alt.found)
                                        result["fallbackSite"] = alt.site.name;

                                    // 「点卡片即播」：影视仓的行为是点开就放第一集，
                                    // 不像早期实现那样要求用户再点一次选集。这里取第 1 个
                                    // 有地址的集，连同其同集跨线路地址一起投递给播放窗
                                    // （poster 可跨线程调，内部会切回 UI 线程建窗）。
                                    int pick_line = -1, pick_ep = -1;
                                    for (std::size_t li = 0;
                                         li < detail.lines.size() && pick_line < 0;
                                         ++li) {
                                        for (std::size_t ei = 0;
                                             ei < detail.lines[li].episodes.size();
                                             ++ei) {
                                            if (!detail.lines[li]
                                                     .episodes[ei]
                                                     .url.empty()) {
                                                pick_line = static_cast<int>(li);
                                                pick_ep = static_cast<int>(ei);
                                                break;
                                            }
                                        }
                                    }
                                    if (pick_line >= 0) {
                                        auto abs_url = [&use_site](std::string u) {
                                            if (!use_site.play_url.empty() &&
                                                u.find("://") == std::string::npos)
                                                u = use_site.play_url + u;
                                            return u;
                                        };
                                        const auto& pl = detail.lines[static_cast<std::size_t>(pick_line)];
                                        std::vector<ttplayer::video::VideoPlayerSource> srcs;
                                        for (const auto& ln2 : detail.lines) {
                                            if (static_cast<std::size_t>(pick_ep) >=
                                                ln2.episodes.size())
                                                continue;
                                            const std::string u = abs_url(
                                                ln2.episodes[static_cast<std::size_t>(pick_ep)].url);
                                            if (u.empty()) continue;
                                            ttplayer::video::VideoPlayerSource s;
                                            s.url = core::Utf8ToWide(u);
                                            s.name = core::Utf8ToWide(ln2.name);
                                            srcs.push_back(std::move(s));
                                        }
                                        const std::string play_url = abs_url(
                                            pl.episodes[static_cast<std::size_t>(pick_ep)].url);
                                        const std::string play_ep =
                                            pl.episodes[static_cast<std::size_t>(pick_ep)].name;
                                        const std::string play_title =
                                            detail.title.empty() ? title : detail.title;
                                        result["autoPlayed"] = true;
                                        result["autoEpisode"] = play_ep;
                                        const std::string final_line = pl.name;
                                        // 整部剧的选集列表：播放窗里可直接切集
                                        // （以前只有电影有入口，剧集必须回音乐窗点）。
                                        const auto player_episodes =
                                            BuildPlayerEpisodes(detail, use_site,
                                                                pick_line);
                                        // 自动播第一集也必须带站点头 / player：
                                        // 以前这里传 {}，等于把 Referer 全丢了。
                                        const VideoPlayContext pctx =
                                            BuildVideoPlayContext(
                                                mgr, site_key, final_line, {});
                                        if (!play_url.empty() && bridge.poster) {
                                            const std::string detail_id = item.id;
                                            const std::string detail_pic = detail.pic;
                                            bridge.poster([play_url, play_title,
                                                           final_line, srcs, pctx,
                                                           player_episodes,
                                                           site_key, detail_id,
                                                           detail_pic] {
                                                OpenVideoPlayerNow(
                                                    play_url, play_title,
                                                    final_line, pctx.headers, srcs,
                                                    pctx.player_url,
                                                    pctx.base_url,
                                                    player_episodes, site_key,
                                                    detail_id, detail_pic);
                                            });
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    ok = false;
                                    error = std::string("加载选集失败: ") + e.what();
                                } catch (...) {
                                    ok = false;
                                    error = "加载选集失败: 未知错误";
                                }
                                if (!BridgeAlive()) return;
                                // 回执会做 JSON 序列化（DumpJsonSafe 已兜住非法
                                // UTF-8）；这里再兜一层，任何异常都不能从
                                // detached 线程逃逸——否则 std::terminate 直接
                                // 让进程闪退（0xc0000409）。
                                try {
                                    reply(ok, std::move(result), std::move(error));
                                } catch (...) {
                                }
                            }).detach();
                        } catch (...) {
                            reply(false, json{}, "无法启动选集加载线程（资源不足）");
                        }
                    });

    // ── video.search：全源搜索 → evt video.search 推回结果列表 ──
    // payload: { q: string }。八路并发扫全部站点：单站请求 6s（video_cms 内
    // 已封顶）、整轮预算 25s；结果按站点轮询交错 + 跨源同名去重（与首页同口径）。
    bridge.Register("video.search",
                    [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
                        const std::string q = JsonStr(payload, "q");
                        if (q.empty()) {
                            reply(false, json{}, "搜索关键词为空（payload.q 缺失）");
                            return;
                        }
                        auto* mgr = BridgeVideoManager();
                        if (!mgr) {
                            reply(false, json{},
                                  "视频源管理器初始化失败（读不到已导入的视频源）");
                            return;
                        }
                        if (g_search_busy.exchange(true)) {
                            reply(true,
                                  json{{"accepted", true}, {"q", q},
                                       {"already_running", true}},
                                  {});
                            return;
                        }
                        try {
                            std::thread([&bridge, mgr, q] {
                                FlagReset reset{g_search_busy};
                                BgThreadGuard guard;
                                if (IsMusicFreeShuttingDown()) return;

                                json out;
                                out["q"] = q;
                                std::vector<ttplayer::video::VideoSite> sites;
                                try {
                                    mgr->Reload();
                                    sites = mgr->AllSites();
                                } catch (const std::exception& e) {
                                    out["cards"] = json::array();
                                    out["error"] =
                                        std::string("读取视频源失败: ") + e.what();
                                    if (BridgeAlive())
                                        bridge.PushEvent("video.search", out);
                                    return;
                                }
                                DropCloudSites(&sites);   // 纯网盘源不出现在搜索结果
                                if (sites.empty()) {
                                    out["cards"] = json::array();
                                    out["error"] =
                                        "尚未导入视频源：请先点「导入视频源」添加影视仓 / TVBox 配置";
                                    if (BridgeAlive())
                                        bridge.PushEvent("video.search", out);
                                    return;
                                }

                                // 每站点一条结果；各线程只写自己的槽位，无需加锁。
                                const std::size_t n = sites.size();
                                std::vector<std::vector<ttplayer::video::VideoItem>> hits(n);
                                std::vector<std::string> errs(n);
                                std::vector<char> done(n, 0);
                                std::mutex done_mtx;
                                std::atomic<bool> finished{false};
                                std::atomic<std::size_t> cursor{0};
                                const auto started = std::chrono::steady_clock::now();
                                const auto elapsed_ms = [&started] {
                                    return static_cast<long long>(
                                        std::chrono::duration_cast<
                                            std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started)
                                            .count());
                                };
                                constexpr long long kBudgetMs = 25000;
                                constexpr long long kRequestMs = 6000;
                                constexpr int kWorkers = 8;

                                // 站点轮询交错 + 跨源同名去重（口径同首页聚合）。
                                // mask 只统计「已完成」的站点：边搜边推时先到的
                                // 结果立即上屏，不必等最慢的 12s 站点。
                                auto merge_cards = [&](const std::vector<char>& mask) {
                                    json cards = json::array();
                                    std::set<std::string> seen;
                                    std::vector<std::size_t> cur(n, 0);
                                    bool added = true;
                                    while (added) {
                                        added = false;
                                        for (std::size_t i = 0; i < n; ++i) {
                                            if (!mask.empty() && !mask[i]) continue;
                                            if (cur[i] >= hits[i].size()) continue;
                                            added = true;
                                            const ttplayer::video::VideoItem& it =
                                                hits[i][cur[i]++];
                                            if (it.title.empty()) continue;
                                            const std::string key =
                                                NormalizeVideoTitle(it.title, it.year);
                                            if (!key.empty() &&
                                                !seen.insert(key).second)
                                                continue;
                                            cards.push_back(CardToJson(it));
                                        }
                                    }
                                    return cards;
                                };

                                // 增量推送：每 400ms 把已完成的站点合并推给页面。
                                // 以前等全部站点（实测 32s）才推一次，页面看上去
                                // 就是「搜索很慢/没结果」。
                                std::thread publisher([&] {
                                    std::size_t last_total = 0;
                                    while (!finished.load(std::memory_order_acquire)) {
                                        std::this_thread::sleep_for(
                                            std::chrono::milliseconds(400));
                                        std::vector<char> snap;
                                        {
                                            std::lock_guard<std::mutex> lk(done_mtx);
                                            snap = done;
                                        }
                                        json cards = merge_cards(snap);
                                        if (cards.size() == last_total) continue;
                                        last_total = cards.size();
                                        if (!BridgeAlive()) continue;
                                        json evt;
                                        evt["q"] = q;
                                        evt["partial"] = true;
                                        evt["total"] = cards.size();
                                        evt["cards"] = std::move(cards);
                                        bridge.PushEvent("video.search",
                                                         std::move(evt));
                                    }
                                });

                                std::vector<std::thread> workers;
                                const int worker_count = static_cast<int>(
                                    std::min<std::size_t>(n, kWorkers));
                                for (int w = 0; w < worker_count; ++w) {
                                    workers.emplace_back([&] {
                                        // 线程局部超时覆盖：本线程所有请求 6s 封顶。
                                        ttplayer::musicfree::ScopedHttpTimeout tick(
                                            kRequestMs);
                                        for (;;) {
                                            const std::size_t i = cursor.fetch_add(
                                                1, std::memory_order_relaxed);
                                            if (i >= n) break;
                                            if (IsMusicFreeShuttingDown()) break;
                                            if (elapsed_ms() + kRequestMs > kBudgetMs)
                                                break;
                                            try {
                                                ttplayer::video::VideoListing listing =
                                                    ttplayer::video::FetchSearch(
                                                        sites[i], q, 1, mgr->network());
                                                hits[i] = std::move(listing.items);
                                            } catch (const std::exception& e) {
                                                errs[i] = e.what();
                                            } catch (...) {
                                                errs[i] = "未知错误";
                                            }
                                            {
                                                std::lock_guard<std::mutex> lk(
                                                    done_mtx);
                                                done[i] = 1;
                                            }
                                        }
                                    });
                                }
                                for (auto& t : workers) t.join();
                                finished.store(true, std::memory_order_release);
                                publisher.join();

                                // 最终全量合并（已完成的增量结果此前已经推过）
                                json cards = merge_cards(std::vector<char>(n, 1));
                                const std::size_t total = cards.size();
                                out["cards"] = std::move(cards);
                                out["total"] = total;
                                out["partial"] = false;
                                out["sites"] = n;
                                int failed = 0;
                                for (const auto& e : errs)
                                    if (!e.empty()) ++failed;
                                if (failed > 0) out["failedSites"] = failed;
                                if (total == 0)
                                    out["error"] = "未找到「" + q + "」相关内容";
                                if (BridgeAlive())
                                    bridge.PushEvent("video.search",
                                                     std::move(out));
                            }).detach();
                        } catch (...) {
                            g_search_busy.store(false);
                            reply(false, json{}, "无法启动视频搜索线程（资源不足）");
                            return;
                        }
                        reply(true, json{{"accepted", true}, {"q", q}}, {});
                    });

    // ── video.sources：视频源导入对话框 ──
    // payload: {}（导入 / 管理对话框）| { live:true }（直播入口，暂未接线）。
    // 对话框是模态的：直接在 handler 里开（UI 线程），与既有
    // OpenVideoSourceDialog（L4679-4699，按钮点击路径）完全同一条调用方式。
    bridge.Register("video.sources",
                    [&bridge](const json& payload, MusicFreeBridge::ReplyFn reply) {
                        if (JsonBool(payload, "live", false)) {
                            reply(false, json{}, "直播入口待接线");
                            return;
                        }
                        if (g_dialog_open.exchange(true)) {
                            reply(false, json{}, "视频源对话框已在打开中");
                            return;
                        }
                        FlagReset reset{g_dialog_open};
                        auto* mgr = BridgeVideoManager();
                        if (!mgr) {
                            reply(false, json{},
                                  "视频源管理器初始化失败（读不到已导入的视频源）");
                            return;
                        }
                        const HWND owner = MusicFreeBrowserWindow();
                        if (!owner) {
                            reply(false, json{}, "音乐窗已关闭，无法打开视频源对话框");
                            return;
                        }
                        bool changed = false;
                        try {
                            // 皮肤回调口径同既有调用方（state->host.skin）。
                            changed = ttplayer::video::ShowVideoSourceDialog(
                                owner, mgr,
                                []() -> const skin::LegacySkin* {
                                    return MusicFreeBrowserLegacySkin();
                                });
                        } catch (const std::exception& e) {
                            reply(false, json{},
                                  std::string("打开视频源对话框失败: ") + e.what());
                            return;
                        } catch (...) {
                            reply(false, json{},
                                  "打开视频源对话框失败: 未知错误");
                            return;
                        }
                        // 完成：回执报告 + evt source.result（导入报告文案）。
                        // 站点表变了 → 首页缓存作废（签名也会失配，这里主动删掉，
                        // 下次打开首页不会先闪一屏旧站点卡片）。
                        if (changed) {
                            std::error_code ec;
                            const auto cache_path = HomeCachePath();
                            if (!cache_path.empty())
                                std::filesystem::remove(cache_path, ec);
                        }
                        json report = SourceReportJson(mgr, changed);
                        json evt = report;
                        reply(true, report, {});
                        bridge.PushEvent("source.result", std::move(evt));
                    });
}

}  // namespace ttplayer::musicfree

// ───────────────────────── 待宿主接线的符号清单（汇总） ─────────────────────────
// 1) std::wstring FriendlySectionTitle(const std::string&)            —— L3081 移出匿名命名空间
// 2) int SectionPriority(const std::wstring&)                          —— L3100 去 static 并移出
// 3) std::string NormalizeVideoTitle(const std::string&, const std::string&) —— L3889 移出
// 4) const skin::LegacySkin* MusicFreeBrowserLegacySkin()             —— 新增薄封装（建议 L863 旁）
// 另：CMakeLists.txt target_sources 需登记本 .cpp；ShowMusicFreeBrowser 创建
// 桥后需调用一次 RegisterVideoCommands(*state->bridge)。
// 直播入口：需要宿主提供无状态入口（如把 StartOpenVideoLive 的 FetchLive 段
// 抽成不依赖 BrowserState 的函数，或导出 AddLiveSourceCards 所需状态访问器）。
