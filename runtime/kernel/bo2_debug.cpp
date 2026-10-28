// armb02: diagnostics for Black Ops II's streaming thread, on with ARMB02_LOG_STREAM=1.
// The stream thread (8237AFD8) sleeps in 82380408 on the event 82380478 sets, then takes
// requests from 8237AE90 while its enable byte (83728FDD) is set. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include <kernel/function.h>
#include <cpu/guest_thread.h>

#include <format>
#include <unordered_set>

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

    // A line is printed the first time it is seen (up to 2000 distinct lines), so a request
    // retried in a loop shows once per distinct state.
    void LogOnce(const std::string& line)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> seen;
        std::lock_guard lock(mutex);
        if (seen.size() < 2000 && seen.insert(line).second)
            fprintf(stderr, "%s\n", line.c_str());
    }

    uint32_t Word(uint8_t* base, uint32_t address)
    {
        return *reinterpret_cast<be<uint32_t>*>(base + address);
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

// The request check-and-read: the request's words, then its file slot (stride 288 at 837247D8:
// +256 handle, +272 position) and the result.
PPC_FUNC_IMPL(__imp__sub_8237AB40);
PPC_FUNC(sub_8237AB40)
{
    uint32_t req = ctx.r3.u32;
    if (!Enabled())
    {
        __imp__sub_8237AB40(ctx, base);
        return;
    }
    std::string before = std::format("[stream] read req {:08X}:", req);
    for (uint32_t i = 0; i < 14; i++)
        before += std::format(" {:08X}", Word(base, req + i * 4));
    before += std::format(" b48={}", base[req + 48]);
    uint32_t slot = 0x837247D8 + Word(base, req + 4) * 288;
    before += std::format(" | slot handle {:08X} pos {:08X}", Word(base, slot + 256), Word(base, slot + 272));
    __imp__sub_8237AB40(ctx, base);
    LogOnce(before + std::format(" -> {}", ctx.r3.u32 & 0xFF));
}
