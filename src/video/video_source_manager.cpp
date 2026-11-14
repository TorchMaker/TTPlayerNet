#include "ttplayer/video/video_source_manager.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "ttplayer/core/text.h"
#include "ttplayer/video/spider/spider_api.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/video_config.h"

namespace ttplayer::video {

namespace {

// VideoSite / VideoLive → JSON（持久化用）。
nlohmann::json SiteToJson(const VideoSite& s) {
    nlohmann::json j;
    j["key"] = s.key;
    j["name"] = s.name;
    j["api"] = s.api;
    j["playUrl"] = s.play_url;
    j["ext"] = s.ext;
    j["group"] = s.group;
    j["ua"] = s.ua;
    j["header"] = s.header;
    j["player"] = s.player;
    j["kind"] = static_cast<int>(s.kind);
    j["auto_converted"] = s.auto_converted;
    j["spider_id"] = s.spider_id;
    j["native_ready"] = s.native_ready;
    return j;
}
nlohmann::json LiveToJson(const VideoLive& l) {
    nlohmann::json j;
    j["name"] = l.name;
    j["url"] = l.url;
    j["group"] = l.group;
    return j;
}
// VideoParse → JSON（解析接口，影视仓 parses 段）。
nlohmann::json ParseToJson(const VideoParse& p) {
    nlohmann::json j;
    j["name"] = p.name;
    j["type"] = p.type;
    j["url"] = p.url;
    j["ext"] = p.ext;
    return j;
}
VideoSite SiteFromJson(const nlohmann::json& j) {
    VideoSite s;
    auto str = [&](const char* k) -> std::string {
        if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
        return {};
    };
    s.key = str("key");
    s.name = str("name");
    s.api = str("api");
    s.play_url = str("playUrl");
    s.ext = str("ext");
    s.group = str("group");
    s.ua = str("ua");
    s.header = str("header");
    s.player = str("player");
    s.kind = j.contains("kind") && j["kind"].is_number()
                 ? static_cast<VideoSourceKind>(j["kind"].get<int>())
                 : VideoSourceKind::kUnknown;
    if (j.contains("auto_converted") && j["auto_converted"].is_boolean())
        s.auto_converted = j["auto_converted"].get<bool>();
    if (j.contains("spider_id") && j["spider_id"].is_string())
        s.spider_id = j["spider_id"].get<std::string>();
    if (j.contains("native_ready") && j["native_ready"].is_boolean())
        s.native_ready = j["native_ready"].get<bool>();
    // 缓存里的 kind / native_ready 是入库那一刻的快照，注册表变化后可能已经
    // 过期（例如通用 recipe 引擎一度把「原生未实现」的 csp_* 标成可用，缓存
    // 就把它们记成 kNativeSpider）。这里始终按当前注册表重新识别，避免旧缓存
    // 让站点继续走一条已经不存在的引擎。
    {
        const VideoSite detected = VideoSite::FromJson(j);
        if (detected.kind != VideoSourceKind::kUnknown) {
            s.kind = detected.kind;
            s.spider_id = detected.spider_id;
            s.native_ready = detected.native_ready;
            s.why_unsupported = detected.why_unsupported;
        }
    }
    return s;
}
VideoLive LiveFromJson(const nlohmann::json& j) {
    VideoLive l;
    if (j.contains("name") && j["name"].is_string()) l.name = j["name"].get<std::string>();
    if (j.contains("url") && j["url"].is_string()) l.url = j["url"].get<std::string>();
    if (j.contains("group") && j["group"].is_string()) l.group = j["group"].get<std::string>();
    return l;
}
VideoParse ParseFromJson(const nlohmann::json& j) {
    VideoParse p;
    if (j.contains("name") && j["name"].is_string()) p.name = j["name"].get<std::string>();
    if (j.contains("type") && j["type"].is_number())
        p.type = j["type"].get<int>();
    if (j.contains("url") && j["url"].is_string()) p.url = j["url"].get<std::string>();
    if (j.contains("ext") && j["ext"].is_string()) p.ext = j["ext"].get<std::string>();
    return p;
}

// 服务端返回网页 / 图片时，ParseVideoConfigText 只会给出「未解析出任何视频源」，
// 用户看不出真实原因（地址失效、域名被停放、防盗链拦截）。这里先判别内容形态，
// 把话说明白。
bool LooksLikeMarkupOrBinary(const std::string& text, std::string* why) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
                               text[i] == '\r' || text[i] == '\n'))
        ++i;
    if (i < text.size() && text[i] == '<') {
        *why = "服务端返回的是网页（HTML/XML），不是影视仓配置；该地址可能已失效或被域名停放页接管";
        return true;
    }
    int control = 0;
    for (unsigned char c : text) {
        if (c == 0) {
            *why = "服务端返回的是二进制内容（图片或其他文件），不是影视仓配置；可能是防盗链或地址已失效";
            return true;
        }
        if (c < 0x09) ++control;
    }
    if (!text.empty() && control * 20 > static_cast<int>(text.size())) {
        *why = "服务端返回的是二进制内容（图片或其他文件），不是影视仓配置；可能是防盗链或地址已失效";
        return true;
    }
    return false;
}

} // namespace

VideoSourceManager::VideoSourceManager(std::filesystem::path cache_dir)
    : cache_dir_(std::move(cache_dir)), index_path_(cache_dir_ / "protocols" / "index.json") {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_ / "protocols", ec);
}

VideoSourceManager::~VideoSourceManager() = default;

VideoSourceManager::ImportReport VideoSourceManager::ImportFromUrl(std::string_view url) {
    ImportReport report;
    std::string origin = std::string(url);
    // 允许省略协议：用户常直接粘 "www.xxx.net/tv"（尤其中文域名的配置源）。
    if (!origin.empty() && origin.find("://") == std::string::npos)
        origin = "http://" + origin;
    std::filesystem::path local = cache_dir_ / "protocols" /
        ("repo_" + std::to_string(std::hash<std::string>{}(origin) % 100000) + ".json");
    try {
        // 二进制读取：图片伪装体里含 \0，用 DownloadText 也可以但要保证不截断。
        const auto body =
            musicfree::DownloadBinary(origin, network_, ConfigRequestHeaders());
        const std::string raw(body.begin(), body.end());

        // 统一解码链（明文 / 图片伪装 Base64 / "2423" AES-CBC / 文本壳 / JSONC
        // 清洗），按内容特征自动判断，用户只管粘贴网址，不需要知道源用了哪种
        // 加密。返回的 text 已是可直接 JSON.parse 的严格文本。
        std::string text;
        std::string kind;
        const bool decoded = DecodeTvboxPayload(raw, text, &kind);
        if (!decoded) {
            // 不是 JSON 形态：可能是裸 CMS 接口列表 / M3U 播放列表，原样交给
            // ParseTextRecursive 的对应分支，不要在这里提前判死。
            text = raw;
            std::string why;
            if (LooksLikeMarkupOrBinary(text, &why)) {
                report.errors.push_back(why + "（" + origin + "）");
                return report;
            }
            if (text.size() > 8 && text.compare(0, 4, "2423") == 0) {
                report.errors.push_back(
                    "源返回了 2423 加密配置，但密钥/密文结构不符合已知格式，解密失败（" +
                    origin + "）");
                return report;
            }
        }
        ImportReport result = ImportText(text, origin, origin, local);
        result.decode_kind = std::move(kind);
        if (result.decode_kind == "aes_cbc_self_2423")
            result.message += "；已自动解密：AES-CBC（2423 自解密）";
        else if (result.decode_kind == "image_marker_base64")
            result.message += "；已自动解密：图片伪装 + Base64";
        else if (result.decode_kind == "base64_plain" ||
                 result.decode_kind == "dollar_shell_base64")
            result.message += "；已自动解密：Base64 包装";
        else if (result.decode_kind == "json_plain")
            result.message += "；已自动规整配置格式（注释/BOM/尾部噪声）";
        return result;
    } catch (const std::exception& e) {
        report.errors.push_back(std::string("下载失败: ") + e.what());
        return report;
    }
}

VideoSourceManager::ImportReport VideoSourceManager::ImportFromFile(
    const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ImportReport r; r.errors.push_back("无法打开本地文件");
        return r;
    }
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    std::u8string u8p = path.u8string();
    return ImportText(text, std::string(reinterpret_cast<const char*>(u8p.data()), u8p.size()), "", path);
}

VideoSourceManager::ImportReport VideoSourceManager::ImportText(
    std::string_view text, std::string_view origin, std::string_view source_url,
    const std::filesystem::path& local_path) {
    ImportReport report;
    ParsedConfigs parsed = ParseVideoConfigText(text, origin, network_);
    if (!parsed.error.empty()) report.errors.push_back(parsed.error);
    auto sources = FlattenConfigs(parsed);
    if (sources.empty()) {
        report.message = "未解析出任何视频源";
        return report;
    }
    report.sources_total = static_cast<int>(sources.size());
    // ★ 统计必须在去重 / 入库**之前**做完：下面的 std::move 会把 sources 里的
    // 每一项掏空（sites 变成空 vector），之后再统计就永远是 0/0，界面会显示
    // 「导入失败」、调用方也不会重新聚合首页。
    //
    // 分类计数（2026-11-07）：一个满屏 csp_XxxGuard 的 TVBox 仓库，50 个站点全是
    // SPIDER —— 以前一律算「受支持 50/50」，导入报告好看、首页却一片空白。现在
    // 只有 CMS / 直播算「可播」，SPIDER 单独计数并在报告里写清原因。
    int spider = 0;
    for (const auto& src : sources) {
        report.sources_ok++;
        report.lives_total += static_cast<int>(src.lives.size());
        for (const auto& s : src.sites) {
            report.sites_total++;
            if (s.auto_converted) report.sites_converted++;
            if (s.kind == ttplayer::video::VideoSourceKind::kCms) {
                report.sites_cms++;
                report.sites_supported++;
            } else if (s.kind == ttplayer::video::VideoSourceKind::kLive) {
                report.sites_supported++;
            } else if (s.kind == ttplayer::video::VideoSourceKind::kNativeSpider) {
                report.sites_native++;
                report.sites_supported++;
            } else if (s.kind == ttplayer::video::VideoSourceKind::kSpider) {
                report.sites_spider++;
                spider++;
                // 未实现的站点保留可定位原因：报告里能直接看出是「原生未实现」
                // 还是缺 ext/需要登录，而不是只给一个 SPIDER 数量。
                report.errors.push_back(
                    "站点「" + s.name + "」暂不支持：" +
                    (s.why_unsupported.empty() ? "需要 TVBox 运行库" :
                                                 s.why_unsupported));
            } else {
                report.errors.push_back("站点「" + s.name + "」不支持：" +
                                        s.why_unsupported);
            }
        }
    }
    report.source_name = sources.front().name;

    // 去重：同名来源已存在则更新
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& src : sources) {
            bool dup = false;
            for (auto& exist : imported_) {
                if (exist.name == src.name && !src.name.empty()) {
                    exist = std::move(src);
                    dup = true;
                    report.duplicated = true;
                    break;
                }
            }
            if (!dup) {
                // 保存原始配置文本，便于回看
                SaveRaw(local_path, text);
                src.source_url = std::string(source_url);
                src.local_path = local_path;
                imported_.push_back(std::move(src));
            }
        }
    }

    report.message = "导入 " + std::to_string(report.sources_ok) + " 个来源：可播站点 " +
                     std::to_string(report.sites_supported) + "/" +
                     std::to_string(report.sites_total);
    if (report.lives_total > 0)
        report.message += "，直播源 " + std::to_string(report.lives_total) + " 个";
    if (report.sites_converted > 0)
        report.message += "；" + std::to_string(report.sites_converted) +
                          " 个扩展站点已从加密 ext 自动还原为可播 CMS 接口";
    if (report.sites_native > 0) {
        // 分母是「Java spider 站点总数」（已实现 + 未实现），分子是本机能直接
        // 拉取的原生站点数；这样 45 站配置能一眼看到 18 个类/26 个站点的覆盖。
        const int spider_total = report.sites_native + report.sites_spider;
        report.message += "；原生 spider 已实现 " +
                          std::to_string(report.sites_native) + "/" +
                          std::to_string(spider_total);
    }
    if (report.sites_spider > 0)
        report.message += "；其中 " + std::to_string(report.sites_spider) +
                          " 个 SPIDER 站点需要 TVBox 的 JS/Java 运行库，本程序暂不支持";
    if (report.sites_supported == 0 && report.sites_spider > 0)
        report.message +=
            "。建议：改导入含标准 CMS 接口（api.php/provide/vod）的配置源，"
            "或在本框每行粘贴一条 CMS 接口地址；也可导入多仓地址一次展开多条线路";
    Persist();
    return report;
}

std::vector<ImportedVideoSource> VideoSourceManager::ListSources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return imported_;
}

std::vector<VideoSite> VideoSourceManager::AllSites() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VideoSite> out;
    for (const auto& src : imported_)
        for (const auto& s : src.sites) {
            // CMS 与原生 spider 都能拉：后者的 api 是类名，但 FetchHome 会在
            // video_cms 里转调 spider 引擎，不会拿类名拼 URL。
            // 未实现的 SPIDER 仍然排除，否则首页会被几十次必然超时拖住。
            if (s.kind != ttplayer::video::VideoSourceKind::kCms &&
                s.kind != ttplayer::video::VideoSourceKind::kNativeSpider)
                continue;
            // 网盘聚合源（pan 模板：盘搜 / 易搜 / 盘她 / 盘他 / 我的云盘…）
            // 给出的都是网盘分享页 —— 必须登录/转存才可能播放。用户明确要求
            // 「需要登录的源都不要」：整类排除，不进首页/搜索/详情/回退。
            // 判定口径与导入分类一致（Find(site.api) 由注册表规范化类名）。
            const SpiderMeta* meta =
                SpiderRegistry::Instance().Find(s.api);
            if (meta && meta->tmpl == SpiderTemplate::kPan) continue;
            // 纯网盘资源站（实测详情线路全部是网盘分享页、无直链，线路名形如
            // 「复制《xx》xx地址」）：连首页都不进，避免用户点开卡片后整屏
            // 「复制地址」再白等跨源回退。详情层另有线路级网盘过滤兜底未知站点。
            if (s.spider_id == "csp_WoGG") continue;
            // 纯预告片源（6huo 等）：只返回「预告片」线路 / 数据接口，点开全是
            // 预告片且大多没有可播直链，对「看正片」毫无用处。整类排除，避免首页与
            // 详情被预告片刷屏（用户实测：点任何片右侧都只剩「预告片」）。
            {
                if (s.name.find("预告片") != std::string::npos) continue;
                std::string nl = s.name, al = s.api;
                for (char& c : nl) c = (char)::tolower((unsigned char)c);
                for (char& c : al) c = (char)::tolower((unsigned char)c);
                if (nl.find("trailer") != std::string::npos ||
                    al.find("6huo.com") != std::string::npos)
                    continue;
            }
            out.push_back(s);
        }
    return out;
}

std::vector<VideoLive> VideoSourceManager::AllLives() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VideoLive> out;
    for (const auto& src : imported_) {
        for (auto live : src.lives) {
            if (live.group.empty()) live.group = src.name;
            out.push_back(std::move(live));
        }
    }
    return out;
}

// 所有来源的解析接口合并去重（影视仓 parses 段）。播放时按顺序尝试：
// 先官方/直链解析器，再 JSON 接口，再 HTML 接口。
std::vector<VideoParse> VideoSourceManager::AllParses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VideoParse> out;
    std::vector<std::string> seen;
    for (const auto& src : imported_) {
        for (const auto& p : src.parses) {
            if (p.url.empty()) continue;
            // 按「地址 + 类型」去重（同一接口常在多仓里重复出现）。
            const std::string key = std::to_string(p.type) + "|" + p.url;
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            out.push_back(p);
        }
    }
    // 排序：直链(3) → JSON(1) → HTML(2)；嗅探(0) 无效直接剔除。
    std::stable_sort(out.begin(), out.end(),
                     [](const VideoParse& a, const VideoParse& b) {
                         auto rank = [](const VideoParse& p) {
                             if (p.type == 3) return 0;
                             if (p.type == 1) return 1;
                             if (p.type == 2) return 2;
                             return 3;
                         };
                         return rank(a) < rank(b);
                     });
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const VideoParse& p) { return p.type == 0; }),
              out.end());
    return out;
}

// 订阅级 playerUrl 汇总（TVBox 自定义播放器 JS）。按 URL 去重，供播放窗在
// 需要按 flag 执行 player 解析时使用。
std::vector<std::string> VideoSourceManager::AllPlayerUrls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& src : imported_) {
        if (src.player_url.empty()) continue;
        if (std::find(out.begin(), out.end(), src.player_url) != out.end()) continue;
        out.push_back(src.player_url);
    }
    return out;
}

// 站点级 player 优先；否则回退所属来源的订阅级 playerUrl。
std::string VideoSourceManager::PlayerUrlForSite(std::string_view site_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& src : imported_) {
        for (const auto& s : src.sites) {
            const std::string k = s.key.empty() ? s.name : s.key;
            if (k != site_key) continue;
            if (!s.player.empty()) return s.player;
            return src.player_url;
        }
    }
    return {};
}

bool VideoSourceManager::RemoveSource(std::string_view name) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto before = imported_.size();
        imported_.erase(
            std::remove_if(imported_.begin(), imported_.end(),
                          [&](const ImportedVideoSource& s) { return s.name == name; }),
            imported_.end());
        changed = imported_.size() != before;
    }
    // 必须先把上面的锁释放再写盘：Persist() 内部会自己锁 mutex_，而 std::mutex
    // 不可重入，重入会抛 std::system_error 让进程直接 abort —— 表现就是点
    // 「删除所选」弹出「Runtime Error / abnormal program termination」。
    if (changed) Persist();
    return changed;
}

void VideoSourceManager::LoadCached() {
    LoadFromIndex();
}

void VideoSourceManager::Reload() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        imported_.clear();
    }
    LoadFromIndex();
}

VideoSourceManager::RefreshReport VideoSourceManager::RefreshAll() {
    RefreshReport rep;
    std::vector<std::pair<std::string, std::string>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s : imported_) {
            if (!s.source_url.empty())
                targets.emplace_back(s.name, s.source_url);
        }
    }

    struct Pending {
        std::string name;
        std::string url;
        std::vector<VideoSite> sites;
        std::vector<VideoLive> lives;
        std::vector<VideoParse> parses;
        std::string player_url;
    };
    std::vector<Pending> pending;
    for (const auto& target : targets) {
        ++rep.checked;
        try {
            const auto body = musicfree::DownloadBinary(
                target.second, network_, ConfigRequestHeaders());
            const std::string raw(body.begin(), body.end());
            std::string text;
            std::string kind;
            // 与首次导入完全相同的解码链；加密方式变了也照样能解。
            if (!DecodeTvboxPayload(raw, text, &kind)) text = raw;
            ParsedConfigs parsed = ParseVideoConfigText(text, target.second, network_);
            auto flat = FlattenConfigs(parsed);
            Pending p;
            p.name = target.first;
            p.url = target.second;
            for (auto& f : flat) {
                for (auto& site : f.sites) p.sites.push_back(std::move(site));
                for (auto& live : f.lives) p.lives.push_back(std::move(live));
                for (auto& parse : f.parses) p.parses.push_back(std::move(parse));
                if (p.player_url.empty()) p.player_url = f.player_url;
            }
            // 关键：解不出站点就跳过，绝不用空结果覆盖缓存里的旧配置。
            if (p.sites.empty() && p.lives.empty()) {
                ++rep.failed;
                rep.errors.push_back(target.first + "：未解析出站点，保留旧配置");
                continue;
            }
            pending.push_back(std::move(p));
        } catch (const std::exception& e) {
            ++rep.failed;
            rep.errors.push_back(target.first + "：" + e.what());
        }
    }

    if (!pending.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& p : pending) {
                bool found = false;
                for (auto& s : imported_) {
                    if (!p.name.empty() && s.name == p.name) {
                        s.sites = std::move(p.sites);
                        s.lives = std::move(p.lives);
                        s.parses = std::move(p.parses);
                        s.player_url = std::move(p.player_url);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ImportedVideoSource s;
                    s.name = std::move(p.name);
                    s.source_url = std::move(p.url);
                    s.sites = std::move(p.sites);
                    s.lives = std::move(p.lives);
                    s.parses = std::move(p.parses);
                    s.player_url = std::move(p.player_url);
                    imported_.push_back(std::move(s));
                }
                ++rep.updated;
            }
        }
        Persist();
    }
    return rep;
}

void VideoSourceManager::LoadFromIndex() {
    std::error_code ec;
    if (!std::filesystem::exists(index_path_, ec)) return;
    std::ifstream f(index_path_, std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    nlohmann::json root;
    try { root = nlohmann::json::parse(ss.str()); } catch (...) { return; }
    if (!root.contains("sources") || !root["sources"].is_array()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : root["sources"]) {
        if (!s.is_object()) continue;
        ImportedVideoSource src;
        try {
            if (s.contains("name")) src.name = s["name"].get<std::string>();
            if (s.contains("source_url")) src.source_url = s["source_url"].get<std::string>();
            if (s.contains("local_path")) {
                // path::operator=(string) 会做窄→宽编码转换，源串含非法
                // 字节序列时抛 std::system_error。索引文件是外部配置，
                // 必须容错，绝不能让异常穿过窗口过程（terminate → abort，
                // 表现就是退出时 "abnormal program termination"）。
                // 由外层 try-catch 接住该异常并跳过这条损坏的源记录。
                src.local_path = s["local_path"].get<std::string>();
            }
            if (s.contains("sites") && s["sites"].is_array())
                for (const auto& site : s["sites"]) src.sites.push_back(SiteFromJson(site));
            if (s.contains("lives") && s["lives"].is_array())
                for (const auto& live : s["lives"]) src.lives.push_back(LiveFromJson(live));
            if (s.contains("parses") && s["parses"].is_array())
                for (const auto& p : s["parses"]) src.parses.push_back(ParseFromJson(p));
            if (s.contains("playerUrl") && s["playerUrl"].is_string())
                src.player_url = s["playerUrl"].get<std::string>();
        } catch (...) { continue; }
        imported_.push_back(std::move(src));
    }
}

void VideoSourceManager::Persist() {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json root;
    root["sources"] = nlohmann::json::array();
    for (const auto& src : imported_) {
        nlohmann::json j;
        j["name"] = src.name;
        j["source_url"] = src.source_url;
        {
            std::u8string u8lp = src.local_path.u8string();
            j["local_path"] = std::string(reinterpret_cast<const char*>(u8lp.data()), u8lp.size());
        }
        j["sites"] = nlohmann::json::array();
        for (const auto& s : src.sites) j["sites"].push_back(SiteToJson(s));
        j["lives"] = nlohmann::json::array();
        for (const auto& l : src.lives) j["lives"].push_back(LiveToJson(l));
        j["parses"] = nlohmann::json::array();
        for (const auto& p : src.parses) j["parses"].push_back(ParseToJson(p));
        j["playerUrl"] = src.player_url;
        root["sources"].push_back(std::move(j));
    }
    std::error_code ec;
    std::filesystem::create_directories(index_path_.parent_path(), ec);
    std::ofstream f(index_path_, std::ios::binary | std::ios::trunc);
    if (f) f << root.dump(2);

    // 源清单变更（导入 / 删除来源 / RefreshAll 原地替换）→ 首页聚合缓存立即
    // 失效。缓存的 signature 基于 AllSites() 虽会自动失配，但这里直接删文件，
    // 保证「改源后绝不残留旧聚合内容与分类」——否则页面上还挂着旧站点的
    // 卡片，点击就会报「未找到站点: xxx（可能已被删除）」。
    // home_cache.json 由音乐窗桥/旧管线写在 <exe>\video\ 下，与本管理器的
    // cache_dir_ 同目录；文件不存在时 remove 只是 no-op，无副作用。
    std::filesystem::remove(cache_dir_ / "home_cache.json", ec);
    ec.clear();
}

void VideoSourceManager::SaveRaw(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace ttplayer::video
