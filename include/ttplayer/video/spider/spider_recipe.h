#pragma once

// 原生 spider recipe（数据驱动的站点行为定义）
//
// 每个 spider 一份 JSON，随程序放在 <exe>\spiders\，开发期也可放在仓库的
// sources/spiders/。查找顺序：
//   1) 环境变量 TTPLAYER_SPIDER_RECIPE_DIR（多个目录用 ';' 分隔）
//   2) <exe>\spiders\<id>.json
//   3) 从 <exe> 所在目录向上最多 6 层的 <repo>\sources\spiders\<id>.json
//
// schema（后续任务按此生成数据）：
//   {"id","class","template","version","base","headers":{},
//    "request":{"method","sign":{"type","fields","key_field"},"params":{}},
//    "endpoints":{"home","category","detail","search","player"},
//    "extract":{"listing":{"items","fields":{}},"detail":{},"player":{}},
//    "html":{"selectors":{},"regex":{}},"notes","source_methods":[]}

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ttplayer/video/spider/spider_registry.h"

namespace ttplayer::video {

struct SpiderRecipe {
    std::string id;
    std::string cls;    // recipe 里的 "class"
    std::string tmpl;   // "cms" / "app" / "html" / "pan" / "live"
    std::string base;   // 站点根地址
    nlohmann::json raw; // 原样保留，供模板引擎读取任意字段
};

struct SpiderRecipeLoad {
    bool ok = false;
    SpiderRecipe recipe;
    std::filesystem::path path;  // 实际命中的文件（未找到时为空）
    std::string error;           // 可读错误（含文件名与字段名）
};

// 依次探测的 recipe 目录（环境变量 → <exe>\spiders → 开发期仓库目录）。
std::vector<std::filesystem::path> SpiderRecipeDirs();

// 按 id 加载：id 接受 csp_XGuard / csp_X / X 三种写法。
SpiderRecipeLoad LoadSpiderRecipe(std::string_view id);

// 校验 recipe JSON；成功回填 out，失败写 error。file_name 用于错误信息前缀。
bool ValidateSpiderRecipe(const nlohmann::json& root, std::string_view file_name,
                          SpiderRecipe* out, std::string* error);

// 遍历 recipe 目录（及内嵌副本），把所有 template==tmpl 的 csp_*.json 自动注册为 native
// spider。factory 负责创建对应模板的引擎（如 RecipeCmsEngine / RecipeHtmlEngine）。
// 引擎在调用时按 ctx.spider_id 加载各自的 recipe，因此一个工厂即可服务所有同模板站点。
//
// skip_if_registered：true 时若某 id 已被注册（通常是专用引擎诚实标为未实现，如
// MyDrive/Push/Fy/App* 需要外部密钥/凭据/代理），则跳过，零回归；false 时强制覆盖
// （用于 html/cms：把这些纯 C++ recipe 站点从“需 Java”纠正为原生，覆盖 specs 的占位 false）。
void RegisterRecipeSpiderVariants(SpiderTemplate tmpl, SpiderFactory factory,
                                  bool skip_if_registered = false);

} // namespace ttplayer::video
