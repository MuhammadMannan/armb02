// armb02: kernel and XAM imports Black Ops II uses that the Deadpool runtime didn't need.
// Behaviour follows Xenia's implementations. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/status.h>
#include <kernel/module.h>
#include <kernel/arena.h>

#include <map>
#include <mutex>

namespace
{
    constexpr uint32_t kStatusEntrypointNotFound = 0xC0000139;

    struct SystemModule
    {
        const char* name;
        uint32_t handle = 0;  // a zeroed guest block standing in for its LDR entry
        std::map<uint32_t, uint32_t> thunks;  // ordinal -> import thunk
    };

    std::mutex s_mutex;
    SystemModule s_modules[] = { { "xam.xex" }, { "xboxkrnl.exe" } };

    SystemModule* FindModule(const char* name)
    {
        for (SystemModule& m : s_modules)
            if (name != nullptr && strcasecmp(name, m.name) == 0)
                return &m;
        return nullptr;
    }

    SystemModule* FindModule(uint32_t handle)
    {
        for (SystemModule& m : s_modules)
            if (handle != 0 && handle == m.handle)
                return &m;
        return nullptr;
    }
}

void module::RegisterImportThunk(const std::string& library, uint32_t ordinal, uint32_t thunk)
{
    std::lock_guard lock(s_mutex);
    if (SystemModule* m = FindModule(library.c_str()))
        m->thunks[ordinal] = thunk;
}

uint32_t module::SystemModuleHandle(const char* name)
{
    std::lock_guard lock(s_mutex);
    SystemModule* m = FindModule(name);
    if (m == nullptr)
        return 0;
    if (m->handle == 0)
        m->handle = arena::Alloc(0x100);
    return m->handle;
}

// ---- Modules --------------------------------------------------------------------------------

// XexLoadImage(name, flags, minVersion, out handle): the system modules are always loaded; any
// other module (the debug monitor, a DLC library) isn't found and the game carries on without it.
uint32_t XexLoadImage(const char* name, uint32_t flags, uint32_t minVersion, be<uint32_t>* handle)
{
    if (uint32_t system = module::SystemModuleHandle(name))
    {
        if (handle != nullptr)
            *handle = system;
        return STATUS_SUCCESS;
    }
    fprintf(stderr, "[module] XexLoadImage(\"%s\", %08X, %08X): no such module\n", name ? name : "(null)", flags,
        minVersion);
    if (handle != nullptr)
        *handle = 0;
    return STATUS_NO_SUCH_FILE;
}

uint32_t XexUnloadImage(uint32_t handle)
{
    return STATUS_SUCCESS;
}

uint32_t XexGetProcedureAddress(uint32_t handle, uint32_t ordinal, be<uint32_t>* address)
{
    {
        std::lock_guard lock(s_mutex);
        if (SystemModule* m = FindModule(handle))
        {
            if (auto it = m->thunks.find(ordinal); it != m->thunks.end())
            {
                *address = it->second;
                return STATUS_SUCCESS;
            }
            fprintf(stderr, "[module] XexGetProcedureAddress(%s, %u): not one of the game's imports\n", m->name, ordinal);
            *address = 0;
            return kStatusEntrypointNotFound;
        }
    }
    fprintf(stderr, "[module] XexGetProcedureAddress(%08X, %u): not found\n", handle, ordinal);
    if (address != nullptr)
        *address = 0;
    return kStatusEntrypointNotFound;
}

GUEST_FUNCTION_HOOK(__imp__XexLoadImage, XexLoadImage);
GUEST_FUNCTION_HOOK(__imp__XexUnloadImage, XexUnloadImage);
GUEST_FUNCTION_HOOK(__imp__XexGetProcedureAddress, XexGetProcedureAddress);

// ---- Video and system ------------------------------------------------------------------------

// XGetVideoCapabilities(): capability flags; none, as in Xenia. Black Ops II tests bit 4.
uint32_t XGetVideoCapabilities()
{
    return 0;
}

// XGetDisplaySize(): Black Ops II keeps the low 16 bits next to XGetVideoMode's answer.
// Nothing to report beyond the video mode: 0.
uint32_t XGetDisplaySize()
{
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XGetVideoCapabilities, XGetVideoCapabilities);
GUEST_FUNCTION_HOOK(__imp__XGetDisplaySize, XGetDisplaySize);

// ---- Users, voice and notifications ----------------------------------------------------------

// XamUserGetDeviceContext(user, unknown, out context): the controller a user signed in with. The
// game reads 0 when this fails, so 0 is stored either way; as in Xenia, a signed-in user (every
// user here, see XamUserGetSigninState) or 0xFF (any user) succeeds.
uint32_t XamUserGetDeviceContext(uint32_t userIndex, uint32_t unknown, be<uint32_t>* context)
{
    if (context != nullptr)
        *context = 0;
    return userIndex < 4 || (userIndex & 0xFF) == 0xFF ? 0 : 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
}

// XamVoiceIsActiveProcess(): whether this title owns the voice chat; no voice chat here.
uint32_t XamVoiceIsActiveProcess()
{
    return 0;
}

// XNotifyPositionUI(position): where the system's notification toasts appear; there are none.
uint32_t XNotifyPositionUI(uint32_t position)
{
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XamUserGetDeviceContext, XamUserGetDeviceContext);
GUEST_FUNCTION_HOOK(__imp__XamVoiceIsActiveProcess, XamVoiceIsActiveProcess);
GUEST_FUNCTION_HOOK(__imp__XNotifyPositionUI, XNotifyPositionUI);

// ---- The game's fatal errors -----------------------------------------------------------------

// Sys_Error (82408A90) formats its message on its own stack frame, at r1+96, then calls
// Sys_SuspendOtherThreads (823802C0) and finally relaunches default.xex. The message never
// reaches an import, so it is logged here, on the way into Sys_SuspendOtherThreads from there.
PPC_FUNC_IMPL(__imp__sub_823802C0);
PPC_FUNC(sub_823802C0)
{
    if (ctx.lr == 0x82408AF0)
    {
        const char* message = reinterpret_cast<const char*>(base + ctx.r1.u32 + 96);
        fprintf(stderr, "[game] Sys_Error: %.1024s\n", message);
        fflush(stderr);
    }
    __imp__sub_823802C0(ctx, base);
}

// XamBackgroundDownloadSetMode(mode): whether the console may download content in the
// background while the title runs; there are no downloads.
void XamBackgroundDownloadSetMode(uint32_t mode)
{
}

GUEST_FUNCTION_HOOK(__imp__XamBackgroundDownloadSetMode, XamBackgroundDownloadSetMode);
