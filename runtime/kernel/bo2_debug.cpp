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

// ARMB02_SKIP_INTRO=1: register com_introPlayed (Dvar_RegisterBool, 823CFE40) as true, so the
// game skips its intro movies, as it does once a profile has seen them.
PPC_FUNC_IMPL(__imp__sub_823CFE40);
PPC_FUNC(sub_823CFE40)
{
    static const bool skipIntro = [] { const char* v = std::getenv("ARMB02_SKIP_INTRO"); return v && v[0] == '1'; }();
    if (skipIntro && ctx.r3.u32 == 0x82042C78)
    {
        fprintf(stderr, "[game] com_introPlayed registered as 1 (ARMB02_SKIP_INTRO)\n");
        ctx.r4.u64 = 1;
    }
    __imp__sub_823CFE40(ctx, base);
}

// ARMB02_LOG_GPULOCK=1: the render lock (a ticket lock at 8372A9FC: +0 now serving, +412 next
// ticket, +284 waiters, +292 owner). Acquire is 82380570, release 82380718. The last 64
// acquires and releases are kept and printed once when an acquire has waited 3 s.
namespace
{
    struct LockEvent
    {
        double t;
        uint32_t thread, lr, serving, next, owner;
        char op;  // 'A' acquire entered, 'a' acquired, 'R' release
    };
    std::mutex s_lockMutex;
    LockEvent s_lockRing[64];
    uint32_t s_lockCount = 0;
    std::unordered_map<uint32_t, double> s_lockWaiting;  // thread -> when its acquire started

    bool LockLogEnabled()
    {
        static const bool enabled = [] { const char* v = std::getenv("ARMB02_LOG_GPULOCK"); return v && v[0] == '1'; }();
        return enabled;
    }

    double Now()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    void LockEventAdd(uint8_t* base, char op, uint32_t lr)
    {
        uint32_t thread = GuestThread::GetCurrentThreadId();
        std::lock_guard lock(s_lockMutex);
        s_lockRing[s_lockCount++ % 64] = { Now(), thread, lr, Word(base, 0x8372A9FC), Word(base, 0x8372A9FC + 412),
            Word(base, 0x8372A9FC + 292), op };
        if (op == 'A')
            s_lockWaiting[thread] = Now();
        else if (op == 'a')
            s_lockWaiting.erase(thread);

        static bool watching = false;
        if (!watching)
        {
            watching = true;
            std::thread([base] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    std::lock_guard lock(s_lockMutex);
                    for (auto [thread, since] : s_lockWaiting)
                    {
                        if (Now() - since < 3.0)
                            continue;
                        fprintf(stderr, "[gpulock] t%u has waited %.1f s; serving %u, next %u, waiters %u, owner t%u. Last %u events:\n",
                            thread, Now() - since, Word(base, 0x8372A9FC), Word(base, 0x8372A9FC + 412), Word(base, 0x8372A9FC + 284),
                            Word(base, 0x8372A9FC + 292), std::min<uint32_t>(s_lockCount, 64));
                        for (uint32_t i = s_lockCount > 64 ? s_lockCount - 64 : 0; i < s_lockCount; i++)
                        {
                            const LockEvent& e = s_lockRing[i % 64];
                            fprintf(stderr, "[gpulock]   %.3f %c t%u lr=%08X serving %u next %u owner t%u\n", e.t, e.op, e.thread, e.lr,
                                e.serving, e.next, e.owner);
                        }
                        return;
                    }
                }
            }).detach();
        }
    }
}

PPC_FUNC_IMPL(__imp__sub_82380570);
PPC_FUNC(sub_82380570)
{
    if (!LockLogEnabled())
    {
        __imp__sub_82380570(ctx, base);
        return;
    }
    uint32_t lr = uint32_t(ctx.lr);
    LockEventAdd(base, 'A', lr);
    __imp__sub_82380570(ctx, base);
    LockEventAdd(base, 'a', lr);
}

PPC_FUNC_IMPL(__imp__sub_82380718);
PPC_FUNC(sub_82380718)
{
    if (LockLogEnabled())
        LockEventAdd(base, 'R', uint32_t(ctx.lr));
    __imp__sub_82380718(ctx, base);
}
