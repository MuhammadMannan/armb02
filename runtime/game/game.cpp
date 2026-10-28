// deadpool-decomp game layer: Deadpool-specific behaviour on top of the generic runtime.
// Nothing game-specific yet: neutral answers for the interfaces the runtime asks.
// GPL-3.0-or-later (see COPYING).
#include "ultrawide.h"
#include "frame_time.h"

namespace game
{
    namespace
    {
        float s_scaleX = 1.0f, s_scaleY = 1.0f;
    }

    void SetAspectScale(float scaleX, float scaleY)
    {
        s_scaleX = scaleX;
        s_scaleY = scaleY;
    }

    float AspectScaleX() { return s_scaleX; }
    float AspectScaleY() { return s_scaleY; }
    bool HooksActive() { return false; }
    bool TallHooksActive() { return false; }
    float FrontEndScaleX() { return 0.0f; }
    float FrontEndScaleY() { return 0.0f; }

    FrameTimeStats GetFrameTimeStats() { return {}; }
}
