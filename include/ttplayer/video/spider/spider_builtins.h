#pragma once

// 内置 spider 引擎注册入口
//
// 每个模板引擎在自己的 .cpp 里实现一个 RegisterXxxSpiderEngines()，这里统一
// 调用。这样并行开发时各模板只改自己的文件，不需要动注册表工厂。
// 注意：静态库的对象文件只有在被引用时才会被链接进来，所以必须用显式函数
// 调用（而不是全局对象的静态初始化）来注册。

namespace ttplayer::video {

// 一次性注册全部内置引擎（SpiderRegistry 构造时调用）。
void RegisterBuiltinSpiderEngines();

// 各模板的实现文件（src/video/spider/spider_engine_*.cpp）提供。
void RegisterCmsSpiderEngines();
void RegisterAppSpiderEngines();
void RegisterHtmlSpiderEngines();
void RegisterPanSpiderEngines();
void RegisterLiveSpiderEngines();

} // namespace ttplayer::video
