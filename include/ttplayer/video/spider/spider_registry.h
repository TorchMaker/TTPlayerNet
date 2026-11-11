#pragma once

// 原生 spider 注册表
//
// 站点 api 有三种写法：csp_T4Guard（配置里的原始类名）、csp_T4（去掉 Guard）、
// T4（裸类名）。注册与查询都先归一成裸类名作为键，避免同一实现被重复注册或
// 查不到。内置实现只为 T4（模板 A，复用 video_cms）；其余类由后续任务的引擎
// 自行注册，不在这里硬编码。

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ttplayer/video/spider/spider_api.h"

namespace ttplayer::video {

// csp_T4Guard / csp_T4 / T4 / com.xx.csp.T4Guard → "T4"（保留大小写）。
std::string NormalizeSpiderId(std::string_view raw);
// "T4" / "csp_T4Guard" → "csp_T4"（注册表里的规范 id）。
std::string CanonicalSpiderId(std::string_view raw);

using SpiderFactory = std::function<std::unique_ptr<SpiderEngine>()>;

struct SpiderEntry {
    SpiderMeta meta;
    SpiderFactory factory;
};

class SpiderRegistry {
public:
    static SpiderRegistry& Instance();

    // 重复注册同 id 时后者覆盖（便于测试或替换实现）。
    void Register(SpiderMeta meta, SpiderFactory factory);

    // 查询：接受上述三种写法。返回的指针指向注册表内部节点，注册表只增不删，
    // 因此在进程生命周期内有效。
    const SpiderMeta* Find(std::string_view id) const;
    bool Has(std::string_view id) const;

    std::vector<SpiderMeta> All() const;
    int ImplementedCount() const;

    // 按 id 创建引擎；未注册 / 工厂为空返回 nullptr。
    std::unique_ptr<SpiderEngine> Create(std::string_view id) const;

private:
    SpiderRegistry();
    SpiderRegistry(const SpiderRegistry&) = delete;
    SpiderRegistry& operator=(const SpiderRegistry&) = delete;

    mutable std::mutex mutex_;
    // map 节点地址稳定，Find 返回的指针不会被后续 Register 失效。
    std::map<std::string, SpiderEntry> entries_;  // key = 裸类名（小写）
};

} // namespace ttplayer::video
