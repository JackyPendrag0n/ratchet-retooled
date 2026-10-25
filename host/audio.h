// Host audio sink fed by the cellAudio mixer (48 kHz, stereo float, 256-sample blocks).
#pragma once
#include "guest.h"

#include <functional>

extern std::function<void(const float* stereo, u32 frames)> audio_sink;
