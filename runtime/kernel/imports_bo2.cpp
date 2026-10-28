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
