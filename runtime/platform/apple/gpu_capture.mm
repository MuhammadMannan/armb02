// deadpool-decomp. GPL-3.0-or-later (see COPYING). See ../gpu_capture.h.
//
// MoltenVK renders through the system's MTLDevice (one per GPU, shared), so capturing that device
// captures the game's frames. Manual reference counting like the rest of the runtime.
#include <TargetConditionals.h>
#include <platform/gpu_capture.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#if TARGET_OS_OSX
#import <Metal/Metal.h>
#endif

namespace platform::gpu_capture
{
    namespace
    {
        std::mutex s_mutex;
        std::string s_path;
        std::atomic<int> s_state{ 0 };  // 0 idle, 1 requested, 2 capturing
        bool Enabled() { return std::getenv("DP_GPU_CAPTURE") != nullptr; }
    }

    void Prepare()
    {
        if (Enabled())
            setenv("MTL_CAPTURE_ENABLED", "1", 1);
    }

    void Request(const std::filesystem::path& path)
    {
        if (!Enabled())
        {
            fprintf(stderr, "[capture] set DP_GPU_CAPTURE=1 to capture frames for Xcode\n");
            return;
        }
        std::lock_guard lock(s_mutex);
        if (s_state != 0)
            return;
        s_path = path.string();
        s_state = 1;
    }

    void OnFrameBoundary()
    {
#if TARGET_OS_OSX
        int state = s_state.load(std::memory_order_relaxed);
        if (state == 0)
            return;
        std::lock_guard lock(s_mutex);
        @autoreleasepool
        {
            MTLCaptureManager* manager = [MTLCaptureManager sharedCaptureManager];
            if (s_state == 1)
            {
                std::filesystem::create_directories(std::filesystem::path(s_path).parent_path());
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                MTLCaptureDescriptor* desc = [[[MTLCaptureDescriptor alloc] init] autorelease];
                desc.captureObject = device;
                desc.destination = MTLCaptureDestinationGPUTraceDocument;
                desc.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:s_path.c_str()]];
                NSError* error = nil;
                if ([manager startCaptureWithDescriptor:desc error:&error])
                {
                    fprintf(stderr, "[capture] capturing a frame to %s\n", s_path.c_str());
                    s_state = 2;
                }
                else
                {
                    fprintf(stderr, "[capture] couldn't start: %s\n", error ? error.localizedDescription.UTF8String : "unknown error");
                    s_state = 0;
                }
                [device release];
            }
            else
            {
                [manager stopCapture];
                fprintf(stderr, "[capture] saved %s (open it in Xcode)\n", s_path.c_str());
                s_state = 0;
            }
        }
#endif
    }
}
