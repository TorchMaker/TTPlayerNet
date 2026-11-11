#include "ttplayer/video/spider/spider_builtins.h"

namespace ttplayer::video {

void RegisterBuiltinSpiderEngines() {
    RegisterCmsSpiderEngines();
    RegisterAppSpiderEngines();
    RegisterHtmlSpiderEngines();
    RegisterPanSpiderEngines();
    RegisterLiveSpiderEngines();
}

} // namespace ttplayer::video
