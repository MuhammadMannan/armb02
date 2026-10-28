// deadpool-decomp game layer: frame-step statistics for the performance log line.
// Deadpool has no frame-time hook yet; the counters stay zero. GPL-3.0-or-later (see COPYING).
#pragma once
#include <cstdint>

namespace game
{
    struct FrameTimeStats
    {
        uint64_t frames, noStep, multiStep, snapped;
    };
    FrameTimeStats GetFrameTimeStats();
}
