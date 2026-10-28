// armb02: diagnostics for Black Ops II's streaming thread, on with ARMB02_LOG_STREAM=1.
// The stream thread (8237AFD8) sleeps in 82380408 on the event 82380478 sets, then takes
// requests from 8237AE90 while its enable byte (83728FDD) is set. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include <kernel/function.h>
#include <cpu/guest_thread.h>

namespace
{
    bool Enabled()
    {
        static const bool enabled = [] { const char* v = std::getenv("ARMB02_LOG_STREAM"); return v && v[0] == '1'; }();
        return enabled;
    }

    std::atomic<uint32_t> s_lines{ 0 };
    bool Budget()
    {
        return s_lines.fetch_add(1) < 400;
    }
}

PPC_FUNC_IMPL(__imp__sub_82380478);
PPC_FUNC(sub_82380478)
{
    if (Enabled() && Budget())
        fprintf(stderr, "[stream] wake from lr=%08X (t%u)\n", uint32_t(ctx.lr), GuestThread::GetCurrentThreadId());
    __imp__sub_82380478(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82380408);
PPC_FUNC(sub_82380408)
{
    __imp__sub_82380408(ctx, base);
    if (Enabled() && Budget())
        fprintf(stderr, "[stream] woke; enable byte %u\n", base[0x83728FDD]);
}

PPC_FUNC_IMPL(__imp__sub_8237AE90);
PPC_FUNC(sub_8237AE90)
{
    __imp__sub_8237AE90(ctx, base);
    if (Enabled() && ctx.r3.u32 != 0 && Budget())
        fprintf(stderr, "[stream] request %08X\n", ctx.r3.u32);
}
