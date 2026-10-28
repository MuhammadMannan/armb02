// deadpool-decomp: stores the recompiler marks as MMIO (followed by eieio) go through here.
// Force-included into the recompiled code (recomp/CMakeLists.txt). The store itself still lands in
// guest memory, as before (the GPU's registers are polled there: gpu/command_processor); writes to
// the XMA command registers (0x7FEA1940-0x7FEA1AFF: kick, lock, clear) also reach the XMA
// emulation (apu/xma.cpp). GPL-3.0-or-later (see COPYING).
#pragma once
#include <cstdint>

void XmaRegisterWrite(uint32_t address, uint32_t value);

inline void MmioStore32(uint8_t* base, uint32_t address, uint32_t value)
{
    // `value` as PPC_STORE_U32 takes it: stored big-endian.
    *(volatile uint32_t*)(base + address) = __builtin_bswap32(value);
    if (address - 0x7FEA1900u < 0x200u)
        XmaRegisterWrite(address, __builtin_bswap32(value));  // the register's value (Xenia byte-swaps)
}

#define PPC_MM_STORE_U32(x, y) MmioStore32(base, (x), (y))
