// deadpool-decomp. GPL-3.0-or-later (see COPYING).
//
// One-frame Metal GPU captures for Xcode (DP_GPU_CAPTURE=1, then F11 or Cmd+Shift+G): the next
// frame's GPU work is written to logs/captures/<session>-<n>.gputrace, which Xcode opens (every
// draw, its shaders, textures and targets). macOS only; elsewhere these do nothing.
#pragma once
#include <filesystem>

namespace platform::gpu_capture
{
    // Before Vulkan starts: lets Metal capture without Xcode attached (MTL_CAPTURE_ENABLED).
    void Prepare();
    // Any thread: capture the next whole frame to `path`.
    void Request(const std::filesystem::path& path);
    // The renderer, at each frame boundary: starts or ends a requested capture.
    void OnFrameBoundary();
}
