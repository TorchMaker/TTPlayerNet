#include "ttplayer/video/spider/spider_recipe.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "ttplayer/core/text.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/spider/spider_util.h"

namespace ttplayer::video {

namespace {

std::string TrimCopy(std::string_view raw) {
    const std::size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string_view::npos) return {};
    const std::size_t e = raw.find_last_not_of(" \t\r\n");
    return std::string(raw.substr(s, e - s + 1));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path ExeDir() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

void AddUnique(std::vector<std::filesystem::path>& dirs,
               const std::filesystem::path& dir) {
    if (dir.empty()) return;
    const std::filesystem::path normalized = dir.lexically_normal();
    for (const auto& existing : dirs)
        if (existing.lexically_normal() == normalized) return;
    dirs.push_back(dir);
}

} // namespace

// ===== 内嵌 recipe（由 tools/gen_embedded_spider_recipes.ps1 自动生成）=====
// 部署版 exe 周围可能不存在 sources/spiders，内嵌保证 recipe 在任何形态下都可用。
namespace {
#include "embedded_spider_recipes.inc"

// 内嵌 recipe 回退：目录未命中时，按 id（原始/Canonical/Normalize 任一）从内嵌表解码。
bool TryLoadEmbeddedRecipe(const std::vector<std::string>& names, SpiderRecipeLoad& result) {
    for (const auto& entry : kEmbeddedSpiderRecipes) {
        const std::string key(entry.first);
        bool hit = false;
        for (const auto& n : names) if (n == key) { hit = true; break; }
        if (!hit) continue;
        std::string raw;
        if (!spider_util::Base64Decode(entry.second, &raw)) {
            result.error = std::string("内嵌 recipe 解码失败：") + key;
            return false;
        }
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB &&
            static_cast<unsigned char>(raw[2]) == 0xBF)
            raw.erase(0, 3);
        nlohmann::json root;
        try {
            root = nlohmann::json::parse(raw);
        } catch (const std::exception& e) {
            result.error = std::string("内嵌 recipe JSON 解析失败：") + key + "：" + e.what();
            return false;
        }
        result.path = std::filesystem::path("embedded:" + key + ".json");
        if (!ValidateSpiderRecipe(root, key + ".json", &result.recipe, &result.error))
            return false;
        result.ok = true;
        return true;
    }
    return false;
}

// 扫描内嵌 recipe 表，注册匹配模板的变体（部署版零外部文件也能点亮）。
void RegisterEmbeddedSpiderVariants(SpiderTemplate tmpl, SpiderFactory factory,
                                    bool skip_if_registered) {
    const std::string_view want = SpiderTemplateName(tmpl);
    if (want.empty()) return;
    SpiderRegistry& reg = SpiderRegistry::Instance();
    for (const auto& entry : kEmbeddedSpiderRecipes) {
        const std::string key(entry.first);
        std::string raw;
        if (!spider_util::Base64Decode(entry.second, &raw)) continue;
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB &&
            static_cast<unsigned char>(raw[2]) == 0xBF)
            raw.erase(0, 3);
        nlohmann::json root;
        try {
            root = nlohmann::json::parse(raw);
        } catch (...) {
            continue;
        }
        SpiderRecipe rec;
        std::string err;
        if (!ValidateSpiderRecipe(root, key + ".json", &rec, &err)) continue;
        if (rec.tmpl != want) continue;
        if (skip_if_registered && reg.Has(rec.id)) continue;
        SpiderMeta meta;
        meta.id = rec.id;
        meta.class_name = rec.cls.empty() ? rec.id : rec.cls;
        meta.tmpl = tmpl;
        meta.implemented = true;
        meta.note = std::string("TVBox recipe（内嵌，") + std::string(want) +
                    " 模板）：自动注册";
        reg.Register(std::move(meta), factory);
    }
}
} // namespace

std::vector<std::filesystem::path> SpiderRecipeDirs() {
    std::vector<std::filesystem::path> dirs;

    // 1) 环境变量覆盖（多个目录用 ';' 分隔，便于调试与灰度）。
    {
        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD len = GetEnvironmentVariableW(L"TTPLAYER_SPIDER_RECIPE_DIR",
                                                  buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
        if (len > 0 && len < buffer.size()) {
            std::wstring value(buffer.data(), len);
            std::size_t start = 0;
            while (start <= value.size()) {
                const std::size_t sep = value.find(L';', start);
                const std::wstring part = value.substr(
                    start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
                if (!part.empty()) AddUnique(dirs, std::filesystem::path(part));
                if (sep == std::wstring::npos) break;
                start = sep + 1;
            }
        }
    }

    // 2) <exe>\spiders（随程序分发，可被用户覆盖）。
    const std::filesystem::path exe_dir = ExeDir();
    if (!exe_dir.empty()) AddUnique(dirs, exe_dir / L"spiders");

    // 3) 开发期回退：从 exe 目录向上最多 6 层找 <repo>\sources\spiders。
    std::filesystem::path cur = exe_dir;
    for (int depth = 0; depth < 7 && !cur.empty(); ++depth) {
        const std::filesystem::path candidate = cur / L"sources" / L"spiders";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec)) AddUnique(dirs, candidate);
        const std::filesystem::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return dirs;
}

bool ValidateSpiderRecipe(const nlohmann::json& root, std::string_view file_name,
                          SpiderRecipe* out, std::string* error) {
    const std::string prefix = std::string(file_name) + ": ";
    auto fail = [&](const std::string& message) {
        if (error) *error = prefix + message;
        return false;
    };

    if (!root.is_object()) return fail("顶层必须是 JSON 对象");

    if (!root.contains("id") || !root["id"].is_string() ||
        root["id"].get<std::string>().empty())
        return fail("缺少必填字段 id");
    if (!root.contains("template") || !root["template"].is_string() ||
        root["template"].get<std::string>().empty())
        return fail("缺少必填字段 template");

    const std::string tmpl = root["template"].get<std::string>();
    SpiderTemplate parsed = SpiderTemplate::kCms;
    if (!SpiderTemplateFromName(tmpl, &parsed))
        return fail("字段 template 非法：" + tmpl + "（应为 cms/app/html/pan/live）");

    for (const char* key : {"headers", "request", "endpoints", "extract", "html"}) {
        if (root.contains(key) && !root[key].is_object())
            return fail(std::string("字段 ") + key + " 必须是对象");
    }
    if (root.contains("source_methods") && !root["source_methods"].is_array())
        return fail("字段 source_methods 必须是数组");
    if (root.contains("base") && !root["base"].is_string())
        return fail("字段 base 必须是字符串");
    if (root.contains("class") && !root["class"].is_string())
        return fail("字段 class 必须是字符串");

    if (out) {
        out->id = root["id"].get<std::string>();
        out->cls = root.contains("class") ? root["class"].get<std::string>() : std::string();
        out->tmpl = std::string(SpiderTemplateName(parsed));
        out->base = root.contains("base") ? root["base"].get<std::string>() : std::string();
        out->raw = root;
    }
    if (error) error->clear();
    return true;
}

SpiderRecipeLoad LoadSpiderRecipe(std::string_view id) {
    SpiderRecipeLoad result;
    const std::string raw = TrimCopy(id);
    if (raw.empty()) {
        result.error = "recipe id 为空";
        return result;
    }

    // 文件名的三种写法都试：原始 / csp_X / 裸类名。
    std::vector<std::string> names;
    auto add_name = [&](const std::string& name) {
        if (name.empty()) return;
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    };
    add_name(raw);
    add_name(CanonicalSpiderId(raw));
    add_name(NormalizeSpiderId(raw));

    const std::vector<std::filesystem::path> dirs = SpiderRecipeDirs();
    for (const auto& dir : dirs) {
        for (const auto& name : names) {
            const std::filesystem::path path = dir / (name + ".json");
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) continue;

            std::ifstream file(path, std::ios::binary);
            if (!file) {
                result.path = path;
                result.error = PathToUtf8(path) + ": 无法打开文件";
                return result;
            }
            std::stringstream ss;
            ss << file.rdbuf();
            std::string text = ss.str();
            // UTF-8 BOM 会让 JSON 解析直接失败。
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB &&
                static_cast<unsigned char>(text[2]) == 0xBF)
                text.erase(0, 3);

            nlohmann::json root;
            try {
                root = nlohmann::json::parse(text);
            } catch (const std::exception& e) {
                result.path = path;
                result.error = PathToUtf8(path) + ": JSON 解析失败：" + e.what();
                return result;
            }

            result.path = path;
            if (!ValidateSpiderRecipe(root, PathToUtf8(path.filename()),
                                      &result.recipe, &result.error))
                return result;
            result.ok = true;
            return result;
        }
    }

    // 目录未命中 → 尝试内嵌 recipe（随二进制分发，无需外部文件）。
    if (TryLoadEmbeddedRecipe(names, result)) return result;

    result.error = "未找到 recipe：" + raw + "（已探测 " +
                   std::to_string(dirs.size()) + " 个目录，且无内嵌副本）";
    return result;
}

// 遍历 recipe 目录，把所有 template==tmpl 的 csp_*.json 自动注册为 native spider
// （用 factory 创建的引擎）。用于把 sources/spiders 里社区 spider 的 recipe 配置
// 直接点亮，无需 Java 运行库。live/app 模板由各自的 RegisterXxxSpiderMetas 处理；
// pan 模板没有统一 recipe 引擎，也不在此自动注册。
void RegisterRecipeSpiderVariants(SpiderTemplate tmpl, SpiderFactory factory,
                                  bool skip_if_registered) {
    const std::string_view want = SpiderTemplateName(tmpl);
    if (want.empty()) return;
    for (const auto& dir : SpiderRecipeDirs()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        std::error_code it_ec;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, it_ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const auto& p = it->path();
            const std::string fname = PathToUtf8(p.filename());
            if (PathToUtf8(p.extension()) != ".json") continue;
            if (fname.rfind("csp_", 0) != 0) continue;  // 只处理社区 spider recipe
            std::ifstream file(p, std::ios::binary);
            if (!file) continue;
            std::stringstream ss;
            ss << file.rdbuf();
            std::string text = ss.str();
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB &&
                static_cast<unsigned char>(text[2]) == 0xBF)
                text.erase(0, 3);
            nlohmann::json root;
            try {
                root = nlohmann::json::parse(text);
            } catch (...) {
                continue;
            }
            SpiderRecipe rec;
            std::string err;
            if (!ValidateSpiderRecipe(root, fname, &rec, &err)) continue;
            if (rec.tmpl != want) continue;  // 只注册与本模板匹配的 recipe
            SpiderMeta meta;
            meta.id = rec.id;
            meta.class_name = rec.cls.empty() ? rec.id : rec.cls;
            meta.tmpl = tmpl;
            meta.implemented = true;
            meta.note = std::string("TVBox recipe（smali 还原，") + std::string(want) +
                        " 模板）：自动注册";
            SpiderRegistry::Instance().Register(std::move(meta), factory);
        }
    }

    // 内嵌 recipe（部署版）：把所有匹配的模板也注册，保证零外部文件也能点亮。
    RegisterEmbeddedSpiderVariants(tmpl, factory, skip_if_registered);
}

} // namespace ttplayer::video
