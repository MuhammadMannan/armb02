// deadpool-decomp game layer: wide/tall-screen hooks. Deadpool has none yet, so the guest
// frame is shown at its own 16:9 shape (pillar/letterboxed). The interface is the one
// SpeedBreaker's presenter and renderer call (its NFS:MW version lives in its game/ultrawide).
// GPL-3.0-or-later (see COPYING).
#pragma once

namespace game
{
    // The presenter reports the scale it fits the guest frame with.
    void SetAspectScale(float scaleX, float scaleY);
    float AspectScaleX();
    float AspectScaleY();
    // True when the game's camera/HUD hooks widen the view (none for Deadpool yet).
    bool HooksActive();
    bool TallHooksActive();
    // The front end's own scale, or 0 when the game doesn't set one.
    float FrontEndScaleX();
    float FrontEndScaleY();
}
