// deadpool-decomp: kernel and XAM imports Deadpool uses that SpeedBreaker's runtime didn't need.
// Behaviour follows Xenia's implementations. Networking answers as a console with no network:
// start-up succeeds, there is no link and no address, and sockets fail with WSAENETDOWN, so the
// game runs offline. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <cpu/guest_thread.h>

#include <pthread.h>

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t WSAENETDOWN = 10050;
    constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF;

    thread_local uint32_t t_wsaLastError = 0;

    uint32_t NetFail()
    {
        t_wsaLastError = WSAENETDOWN;
        return SOCKET_ERROR;
    }
}

// ---- Memory ---------------------------------------------------------------------------------

// XamAlloc(flags, size, out): zeroed memory from the runtime's heap (Xenia: the system heap).
uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* out)
{
    void* p = g_userHeap.Alloc(size ? size : 1);
    if (p == nullptr)
    {
        *out = 0;
        return 0x8007000E;  // E_OUTOFMEMORY
    }
    memset(p, 0, size);
    *out = g_memory.MapVirtual(p);
    return X_ERROR_SUCCESS;
}

uint32_t XamFree(uint32_t ptr)
{
    if (ptr != 0)
        g_userHeap.Free(g_memory.Translate(ptr));
    return X_ERROR_SUCCESS;
}

// ---- Threads, DPCs, exceptions ---------------------------------------------------------------

void KeEnableFpuExceptions(uint32_t enable)
{
    // The host runs with FP exceptions masked, as the game expects in release builds (Xenia: no-op).
}

uint32_t KeQueryBasePriorityThread(uint32_t thread)
{
    return 0;
}

void ExRegisterTitleTerminateNotification(uint32_t registration, uint32_t create)
{
    // Called on the title's way out; the runtime ends the process itself (Xenia: no-op).
}

// X_KDPC: { be16 type; u8 selected_cpu; u8 desired_cpu; LIST_ENTRY list; routine; context; arg1; arg2 }
void KeInitializeDpc(uint8_t* dpc, uint32_t routine, uint32_t context)
{
    memset(dpc, 0, 0x20);
    *reinterpret_cast<be<uint16_t>*>(dpc + 0) = 19;  // DpcObject
    *reinterpret_cast<be<uint32_t>*>(dpc + 0x10) = routine;
    *reinterpret_cast<be<uint32_t>*>(dpc + 0x14) = context;
}

// X_EXCEPTION_RECORD: { code; flags; record; address; num_params; information[15] }
void RtlRaiseException(uint8_t* record)
{
    const uint32_t code = *reinterpret_cast<be<uint32_t>*>(record);
    if (code == 0x406D1388)
    {
        // MSVC's "set thread name" exception: information = { 0x1000, name, thread id, flags }.
        const uint32_t name = *reinterpret_cast<be<uint32_t>*>(record + 0x14 + 4);
        const uint32_t threadId = *reinterpret_cast<be<uint32_t>*>(record + 0x14 + 8);
        if (name != 0 && (threadId == 0xFFFFFFFF || threadId == GuestThread::GetCurrentThreadId()))
        {
#ifdef __APPLE__
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", static_cast<const char*>(g_memory.Translate(name)));
            pthread_setname_np(buf);
#else
            char buf[16];  // Linux: 15 characters and the NUL
            snprintf(buf, sizeof(buf), "%s", static_cast<const char*>(g_memory.Translate(name)));
            pthread_setname_np(pthread_self(), buf);
#endif
        }
        return;
    }
    fprintf(stderr, "[kernel] RtlRaiseException code %08X (not handled; continuing)\n", code);
}

// ---- Interlocked singly linked lists ----------------------------------------------------------
// X_SLIST_HEADER is 8 big-endian bytes: next (32), depth (16), sequence (16). The game pushes
// inline with ldarx/stdcx., which XenonRecomp turns into a 64-bit compare-and-swap on the same
// bytes, so pop and flush swap the whole header atomically too (as in Xenia).
namespace
{
    bool SwapHeader(uint64_t* header, uint64_t oldBe, uint64_t newValue)
    {
        return __atomic_compare_exchange_n(header, &oldBe, __builtin_bswap64(newValue), false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}

uint32_t InterlockedPopEntrySList(uint64_t* header)
{
    while (true)
    {
        const uint64_t oldBe = __atomic_load_n(header, __ATOMIC_SEQ_CST);
        const uint64_t old = __builtin_bswap64(oldBe);
        const uint32_t first = uint32_t(old >> 32);
        if (first == 0)
            return 0;
        const uint32_t next = *static_cast<be<uint32_t>*>(g_memory.Translate(first));
        const uint16_t depth = uint16_t(old >> 16) - 1, sequence = uint16_t(old);
        if (SwapHeader(header, oldBe, (uint64_t(next) << 32) | (uint64_t(depth) << 16) | sequence))
            return first;
    }
}

uint32_t InterlockedFlushSList(uint64_t* header)
{
    while (true)
    {
        const uint64_t oldBe = __atomic_load_n(header, __ATOMIC_SEQ_CST);
        const uint64_t old = __builtin_bswap64(oldBe);
        if (SwapHeader(header, oldBe, uint16_t(old)))  // next 0, depth 0, sequence kept
            return uint32_t(old >> 32);
    }
}

// ---- Audio ------------------------------------------------------------------------------------

uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
{
    *config = 0x00010001;  // Xenia's value (stereo-capable default)
    return X_ERROR_SUCCESS;
}

// ---- Networking: offline -------------------------------------------------------------------

uint32_t NetDll_XNetStartup(uint32_t caller, uint32_t params) { return 0; }
uint32_t NetDll_XNetCleanup(uint32_t caller) { return 0; }

// WSADATA is only read for version fields; report 2.2 and success.
uint32_t NetDll_WSAStartup(uint32_t caller, uint32_t version, uint8_t* data)
{
    if (data != nullptr)
    {
        memset(data, 0, 0x190);
        *reinterpret_cast<be<uint16_t>*>(data + 0) = 0x0202;
        *reinterpret_cast<be<uint16_t>*>(data + 2) = 0x0202;
    }
    return 0;
}

uint32_t NetDll_WSACleanup(uint32_t caller) { return 0; }
uint32_t NetDll_WSAGetLastError() { return t_wsaLastError; }
void NetDll_WSASetLastError(uint32_t error) { t_wsaLastError = error; }

uint32_t NetDll_XNetGetEthernetLinkStatus(uint32_t caller) { return 0; }  // no link
// armb02: Black Ops II stops at start-up ("Unable to find an XNADDR.") without an address, so,
// as in Xenia, the console reports a static local one: 127.0.0.1, no online address, and a
// fixed locally administered MAC. Sockets still fail, so the game stays offline.
uint32_t NetDll_XNetGetTitleXnAddr(uint32_t caller, uint8_t* addr)
{
    if (addr != nullptr)
    {
        memset(addr, 0, 0x24);
        *reinterpret_cast<be<uint32_t>*>(addr + 0) = 0x7F000001;  // ina
        static constexpr uint8_t kEnet[6] = { 0x02, 0x41, 0x52, 0x4D, 0x42, 0x02 };
        memcpy(addr + 10, kEnet, sizeof(kEnet));  // abEnet, after inaOnline and wPortOnline
    }
    return 0x00000002 | 0x00000004 | 0x00000020 | 0x00000040;  // XNET_GET_XNADDR_ETHERNET | STATIC | GATEWAY | DNS
}

uint32_t NetDll_socket(uint32_t caller, uint32_t af, uint32_t type, uint32_t protocol) { return NetFail(); }
uint32_t NetDll_closesocket(uint32_t caller, uint32_t s) { return NetFail(); }
uint32_t NetDll_bind(uint32_t caller, uint32_t s, uint32_t name, uint32_t len) { return NetFail(); }
uint32_t NetDll_connect(uint32_t caller, uint32_t s, uint32_t name, uint32_t len) { return NetFail(); }
uint32_t NetDll_listen(uint32_t caller, uint32_t s, uint32_t backlog) { return NetFail(); }
uint32_t NetDll_accept(uint32_t caller, uint32_t s, uint32_t addr, uint32_t len) { return NetFail(); }
uint32_t NetDll_select(uint32_t caller, uint32_t n, uint32_t r, uint32_t w, uint32_t e, uint32_t t) { return NetFail(); }
uint32_t NetDll_recv(uint32_t caller, uint32_t s, uint32_t buf, uint32_t len, uint32_t flags) { return NetFail(); }
uint32_t NetDll_recvfrom(uint32_t caller, uint32_t s, uint32_t buf, uint32_t len, uint32_t flags, uint32_t from) { return NetFail(); }
uint32_t NetDll_send(uint32_t caller, uint32_t s, uint32_t buf, uint32_t len, uint32_t flags) { return NetFail(); }
uint32_t NetDll_sendto(uint32_t caller, uint32_t s, uint32_t buf, uint32_t len, uint32_t flags, uint32_t to) { return NetFail(); }
uint32_t NetDll_setsockopt(uint32_t caller, uint32_t s, uint32_t level, uint32_t name, uint32_t val, uint32_t len) { return NetFail(); }
uint32_t NetDll_getsockopt(uint32_t caller, uint32_t s, uint32_t level, uint32_t name, uint32_t val, uint32_t len) { return NetFail(); }
uint32_t NetDll_getsockname(uint32_t caller, uint32_t s, uint32_t name, uint32_t len) { return NetFail(); }
uint32_t NetDll_ioctlsocket(uint32_t caller, uint32_t s, uint32_t cmd, uint32_t arg) { return NetFail(); }
uint32_t NetDll_inet_addr(uint32_t cp) { return 0xFFFFFFFF; }  // INADDR_NONE

// XNet address/QoS/DNS services: unavailable without a network.
uint32_t NetDll_XNetDnsLookup(uint32_t caller, uint32_t name, uint32_t event, be<uint32_t>* dns) { if (dns) *dns = 0; return WSAENETDOWN; }
uint32_t NetDll_XNetDnsRelease(uint32_t caller, uint32_t dns) { return 0; }
uint32_t NetDll_XNetQosListen(uint32_t caller, uint32_t id, uint32_t data, uint32_t size, uint32_t bps, uint32_t flags) { return WSAENETDOWN; }
uint32_t NetDll_XNetQosLookup(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetQosServiceLookup(uint32_t caller, uint32_t flags, uint32_t event, be<uint32_t>* qos) { if (qos) *qos = 0; return WSAENETDOWN; }
uint32_t NetDll_XNetQosRelease(uint32_t caller, uint32_t qos) { return 0; }
uint32_t NetDll_XNetRandom(uint32_t caller, uint8_t* buffer, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
        buffer[i] = uint8_t(rand());
    return 0;
}
uint32_t NetDll_XNetServerToInAddr(uint32_t caller, uint32_t ina, uint32_t service, be<uint32_t>* out) { if (out) *out = 0; return WSAENETDOWN; }
uint32_t NetDll_XNetXnAddrToInAddr(uint32_t caller, uint32_t xna, uint32_t xnkid, be<uint32_t>* out) { if (out) *out = 0; return WSAENETDOWN; }
uint32_t NetDll_XNetInAddrToXnAddr(uint32_t caller, uint32_t ina, uint32_t xna, uint32_t xnkid) { return WSAENETDOWN; }
uint32_t NetDll_XNetUnregisterInAddr(uint32_t caller, uint32_t ina) { return 0; }
uint32_t NetDll_XNetUnregisterKey(uint32_t caller, uint32_t xnkid) { return 0; }
uint32_t XNetLogonGetTitleID(uint32_t caller) { return 0x415608EC; }

GUEST_FUNCTION_HOOK(__imp__XamAlloc, XamAlloc);
GUEST_FUNCTION_HOOK(__imp__XamFree, XamFree);
GUEST_FUNCTION_HOOK(__imp__KeEnableFpuExceptions, KeEnableFpuExceptions);
GUEST_FUNCTION_HOOK(__imp__KeQueryBasePriorityThread, KeQueryBasePriorityThread);
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification);
GUEST_FUNCTION_HOOK(__imp__KeInitializeDpc, KeInitializeDpc);
GUEST_FUNCTION_HOOK(__imp__RtlRaiseException, RtlRaiseException);
GUEST_FUNCTION_HOOK(__imp__XAudioGetSpeakerConfig, XAudioGetSpeakerConfig);
GUEST_FUNCTION_HOOK(__imp__InterlockedPopEntrySList, InterlockedPopEntrySList);
GUEST_FUNCTION_HOOK(__imp__InterlockedFlushSList, InterlockedFlushSList);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetStartup, NetDll_XNetStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetCleanup, NetDll_XNetCleanup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAStartup, NetDll_WSAStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACleanup, NetDll_WSACleanup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAGetLastError, NetDll_WSAGetLastError);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSASetLastError, NetDll_WSASetLastError);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetEthernetLinkStatus, NetDll_XNetGetEthernetLinkStatus);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_socket, NetDll_socket);
GUEST_FUNCTION_HOOK(__imp__NetDll_closesocket, NetDll_closesocket);
GUEST_FUNCTION_HOOK(__imp__NetDll_bind, NetDll_bind);
GUEST_FUNCTION_HOOK(__imp__NetDll_connect, NetDll_connect);
GUEST_FUNCTION_HOOK(__imp__NetDll_listen, NetDll_listen);
GUEST_FUNCTION_HOOK(__imp__NetDll_accept, NetDll_accept);
GUEST_FUNCTION_HOOK(__imp__NetDll_select, NetDll_select);
GUEST_FUNCTION_HOOK(__imp__NetDll_recv, NetDll_recv);
GUEST_FUNCTION_HOOK(__imp__NetDll_recvfrom, NetDll_recvfrom);
GUEST_FUNCTION_HOOK(__imp__NetDll_send, NetDll_send);
GUEST_FUNCTION_HOOK(__imp__NetDll_sendto, NetDll_sendto);
GUEST_FUNCTION_HOOK(__imp__NetDll_setsockopt, NetDll_setsockopt);
GUEST_FUNCTION_HOOK(__imp__NetDll_getsockopt, NetDll_getsockopt);
GUEST_FUNCTION_HOOK(__imp__NetDll_getsockname, NetDll_getsockname);
GUEST_FUNCTION_HOOK(__imp__NetDll_ioctlsocket, NetDll_ioctlsocket);
GUEST_FUNCTION_HOOK(__imp__NetDll_inet_addr, NetDll_inet_addr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsLookup, NetDll_XNetDnsLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsRelease, NetDll_XNetDnsRelease);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosListen, NetDll_XNetQosListen);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosLookup, NetDll_XNetQosLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosServiceLookup, NetDll_XNetQosServiceLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosRelease, NetDll_XNetQosRelease);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetRandom, NetDll_XNetRandom);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetServerToInAddr, NetDll_XNetServerToInAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetXnAddrToInAddr, NetDll_XNetXnAddrToInAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetInAddrToXnAddr, NetDll_XNetInAddrToXnAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetUnregisterInAddr, NetDll_XNetUnregisterInAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetUnregisterKey, NetDll_XNetUnregisterKey);
GUEST_FUNCTION_HOOK(__imp__XNetLogonGetTitleID, XNetLogonGetTitleID);
