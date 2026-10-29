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
    // ARMB02_DVARS="sm_sunEnable=0,r_foo=1": a bool dvar registered with that default instead
    // (a bring-up aid: turn one renderer feature off and see what changes).
    static const std::unordered_map<std::string, bool> overrides = [] {
        std::unordered_map<std::string, bool> out;
        if (const char* v = std::getenv("ARMB02_DVARS"))
            for (std::string all = v; !all.empty();)
            {
                const size_t comma = all.find(',');
                const std::string item = all.substr(0, comma);
                all = comma == std::string::npos ? std::string() : all.substr(comma + 1);
                if (const size_t eq = item.find('='); eq != std::string::npos)
                    out[item.substr(0, eq)] = item[eq + 1] == '1';
            }
        return out;
    }();
    if (!overrides.empty())
    {
        auto it = overrides.find(reinterpret_cast<const char*>(base + ctx.r3.u32));
        if (it != overrides.end())
        {
            fprintf(stderr, "[game] %s registered as %u (ARMB02_DVARS)\n", it->first.c_str(), unsigned(it->second));
            ctx.r4.u64 = it->second;
        }
    }
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
        uint32_t thread, lr, serving, next, owner, teb;
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

    void LockEventAdd(PPCContext& ctx, uint8_t* base, char op, uint32_t lr)
    {
        uint32_t thread = GuestThread::GetCurrentThreadId();
        // What the game's own GetCurrentThreadId (825C15A0) sees: TEB (r13+0x100) + 0x14C.
        const uint32_t teb = Word(base, Word(base, ctx.r13.u32 + 0x100) + 0x14C);
        std::lock_guard lock(s_lockMutex);
        s_lockRing[s_lockCount++ % 64] = { Now(), thread, lr, Word(base, 0x8372A9FC), Word(base, 0x8372A9FC + 412),
            Word(base, 0x8372A9FC + 292), teb, op };
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
                            fprintf(stderr, "[gpulock]   %.3f %c t%u (teb id %u) lr=%08X serving %u next %u owner t%u\n", e.t, e.op, e.thread, e.teb, e.lr,
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
    LockEventAdd(ctx, base, 'A', lr);
    __imp__sub_82380570(ctx, base);
    LockEventAdd(ctx, base, 'a', lr);
}

PPC_FUNC_IMPL(__imp__sub_82380718);
PPC_FUNC(sub_82380718)
{
    if (LockLogEnabled())
        LockEventAdd(ctx, base, 'R', uint32_t(ctx.lr));
    __imp__sub_82380718(ctx, base);
}

// ARMB02_LOG_GAME=1: the game's own console text. The retail build compiles its prints out, but
// their calls remain, folded into one empty function (82498858, 459 calls): Com_Printf-style
// (channel in r3, format in r4, arguments from r5) and plain strings in r3 (Com_Error passes its
// formatted message, 8368FC68, so). Printed as "[con] ...", formatted here from the guest's
// arguments (r5-r10, then the caller's stack from r1+56; doubles arrive as raw 64-bit words).
namespace
{
    bool GameLogEnabled()
    {
        static const bool enabled = [] { const char* v = std::getenv("ARMB02_LOG_GAME"); return v && v[0] == '1'; }();
        return enabled;
    }

    // A guest pointer to a NUL-terminated, printable string of at most `max` characters.
    bool GuestString(uint8_t* base, uint32_t address, std::string& out, size_t max = 1024)
    {
        if (address < 0x40000000u || address >= 0xC0000000u)  // heap and image ranges only
            return false;
        out.clear();
        for (size_t i = 0; i < max; i++)
        {
            uint8_t c = base[address + i];
            if (c == 0)
                return !out.empty();
            if (c < 0x20 && c != '\n' && c != '\t' && c != '\r' && c != '^')
                return false;
            if (c >= 0x7F)
                return false;
            out += char(c);
        }
        return false;
    }

    std::string FormatGuest(PPCContext& ctx, uint8_t* base, const std::string& fmt)
    {
        uint32_t next = 0;
        auto arg = [&]() -> uint64_t {
            const uint64_t regs[] = { ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64 };
            uint32_t i = next++;
            if (i < 6)
                return regs[i];
            return *reinterpret_cast<be<uint64_t>*>(base + ctx.r1.u32 + 56 + (i - 6) * 8);
        };
        std::string out;
        for (size_t i = 0; i < fmt.size(); i++)
        {
            if (fmt[i] != '%' || i + 1 == fmt.size())
            {
                out += fmt[i];
                continue;
            }
            size_t j = i + 1;
            std::string spec = "%";
            while (j < fmt.size() && strchr("-+ #0123456789.*hlLzjt", fmt[j]))
            {
                if (fmt[j] == '*')
                    spec += std::to_string(int32_t(arg()));
                else if (!strchr("hlLzjt", fmt[j]))
                    spec += fmt[j];
                j++;
            }
            if (j == fmt.size())
                break;
            char conv = fmt[j];
            char buffer[512];
            switch (conv)
            {
            case '%': out += '%'; break;
            case 's':
            {
                std::string s;
                uint32_t p = uint32_t(arg());
                out += GuestString(base, p, s) ? s : std::format("<{:08X}>", p);
                break;
            }
            case 'd': case 'i': snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), int32_t(arg())); out += buffer; break;
            case 'u': case 'x': case 'X': case 'o': case 'c':
                snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), uint32_t(arg())); out += buffer; break;
            case 'p': snprintf(buffer, sizeof(buffer), "%08X", uint32_t(arg())); out += buffer; break;
            case 'f': case 'g': case 'e': case 'F': case 'G': case 'E':
                snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), std::bit_cast<double>(arg())); out += buffer; break;
            default: out += spec + conv; break;
            }
            i = j;
        }
        return out;
    }
}

PPC_FUNC_IMPL(__imp__sub_82498858);
PPC_FUNC(sub_82498858)
{
    if (GameLogEnabled())
    {
        std::string fmt, text;
        if (GuestString(base, ctx.r4.u32, fmt) && (fmt.find('%') != std::string::npos || fmt.back() == '\n'))
            text = FormatGuest(ctx, base, fmt);
        else if (!GuestString(base, ctx.r3.u32, text))
            text.clear();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (!text.empty())
        {
            static std::mutex mutex;
            static std::string last;
            std::lock_guard lock(mutex);
            if (text != last)
                fprintf(stderr, "[con] %s (lr %08X)\n", text.c_str(), uint32_t(ctx.lr));
            last = text;
        }
    }
    __imp__sub_82498858(ctx, base);
}

// ARMB02_LOG_JOBS=1: BO2's job submissions (Job_Add, 823C9F78), by job type name (the descriptor's
// first word points at the type, whose first word is its name and third its entry), at the 1st,
// 10th, 100th, 1000th and 10000th submission of each.
PPC_FUNC_IMPL(__imp__sub_823C9F78);
PPC_FUNC(sub_823C9F78)
{
    static const bool on = [] { const char* v = std::getenv("ARMB02_LOG_JOBS"); return v && v[0] == '1'; }();
    if (on)
    {
        static std::mutex mutex;
        static std::unordered_map<uint32_t, uint32_t> counts;
        uint32_t def = ctx.r3.u32, type = Word(base, def), namePtr = type ? Word(base, type) : 0;
        std::string name;
        GuestString(base, namePtr, name, 64);
        std::lock_guard lock(mutex);
        uint32_t n = ++counts[def];
        if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000)
            fprintf(stderr, "[jobs] submit #%u def %08X type %08X \"%s\" entry %08X words %08X %08X %08X %08X %08X r5 %08X t%u lr %08X\n", n, def, type,
                name.c_str(), type ? Word(base, type + 8) : 0, Word(base, def + 4), Word(base, def + 8), Word(base, def + 12), Word(base, def + 16),
                Word(base, def + 20), ctx.r5.u32, GuestThread::GetCurrentThreadId(), uint32_t(ctx.lr));
    }
    __imp__sub_823C9F78(ctx, base);
}

