#pragma once
#include "ttplayer/audio/audio_engine.h"
#include <objidl.h>
namespace ttplayer::audio {
// Lengthless HTTP MPEG audio, using ttpcomm's original ordinal 11..14 ABI.
std::unique_ptr<DecodedAudioSource> CreateHttpMpegSource(HMODULE comm, IStream* stream);
}
