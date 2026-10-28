// armb02: Metal GPU captures exist only on Apple platforms (apple/gpu_capture.mm); elsewhere
// these do nothing, as gpu_capture.h says. GPL-3.0-or-later (see COPYING).
#include "gpu_capture.h"

#ifndef __APPLE__
namespace platform::gpu_capture
{
    void Prepare() {}
    void Request(const std::filesystem::path&) {}
    void OnFrameBoundary() {}
}
#endif
