// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MemoryMap.h"

#include "MemoryTypes.h"
#include "DebugTools/Debug.h"
#include "common/AlignedMalloc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

uptr* psxMemWLUT = nullptr;
const uptr* psxMemRLUT = nullptr;

namespace
{
	mem8_t nullRead8(u32 mem)
	{
		MEM_LOG("Read uninstalled memory at address %08x", mem);
		return 0;
	}

	mem16_t nullRead16(u32 mem)
	{
		MEM_LOG("Read uninstalled memory at address %08x", mem);
		return 0;
	}

	mem32_t nullRead32(u32 mem)
	{
		MEM_LOG("Read uninstalled memory at address %08x", mem);
		return 0;
	}

	mem64_t nullRead64(u32 mem)
	{
		MEM_LOG("Read uninstalled memory at address %08x", mem);
		return 0;
	}

	RETURNS_R128 nullRead128(u32 mem)
	{
		MEM_LOG("Read uninstalled memory at address %08x", mem);
		return r128_zero();
	}

	void nullWrite8(u32 mem, mem8_t)
	{
		MEM_LOG("Write uninstalled memory at address %08x", mem);
	}

	void nullWrite16(u32 mem, mem16_t)
	{
		MEM_LOG("Write uninstalled memory at address %08x", mem);
	}

	void nullWrite32(u32 mem, mem32_t)
	{
		MEM_LOG("Write uninstalled memory at address %08x", mem);
	}

	void nullWrite64(u32 mem, mem64_t)
	{
		MEM_LOG("Write uninstalled memory at address %08x", mem);
	}

	void TAKES_R128 nullWrite128(u32 mem, r128)
	{
		MEM_LOG("Write uninstalled memory at address %08x", mem);
	}
} // namespace

vtlbHandler Ps2MemoryMap::RegisterNullMemoryHandler()
{
	return vtlb_RegisterHandler(nullRead8, nullRead16, nullRead32, nullRead64, nullRead128,
		nullWrite8, nullWrite16, nullWrite32, nullWrite64, nullWrite128);
}

void Ps2MemoryMap::MapEERamHighMemoryAndRoms(vtlbHandler null_handler)
{
	// Main memory.
	vtlb_MapBlock(eeMem->Main, 0x00000000, Ps2MemSize::ExposedRam);

	// High memory, uninstalled on the configuration we emulate.
	vtlb_MapHandler(null_handler, Ps2MemSize::ExposedRam, 0x10000000 - Ps2MemSize::ExposedRam);

	// Various ROMs (all read-only).
	vtlb_MapBlock(eeMem->ROM, 0x1fc00000, Ps2MemSize::Rom);
	vtlb_MapBlock(eeMem->ROM1, 0x1e000000, Ps2MemSize::Rom1);
	vtlb_MapBlock(eeMem->ROM2, 0x1e400000, Ps2MemSize::Rom2);
}

void Ps2MemoryMap::MapKernelVirtualMirrors()
{
	// 0x8* cached mirror.
	vtlb_VMap(0x80000000, 0x00000000, _1mb * 512);

	// 0xa* uncached mirror.
	vtlb_VMap(0xa0000000, 0x00000000, _1mb * 512);
}

void Ps2MemoryMap::MapDirectVirtualMemoryWindow()
{
	vtlb_VMap(0x00000000, 0x00000000, _1mb * 512);
	vtlb_VMapUnmap(0x20000000, 0x60000000);
	// PCSX2 owner: the first 32 MiB of the direct user window above maps the
	// retail EE RAM block one-to-one. TLB and scratchpad mappings go through the
	// vTLB mutation functions, which retire this ARM32 proof if they replace any
	// part of that window with a non-identity mapping.
	vtlb_private::SetDefaultMainRamIdentityWindow(
		Ps2MemSize::ExposedRam == Ps2MemSize::MainRam);
}

void Ps2MemoryMap::AllocateIopMemoryLookupTables()
{
	if (psxMemWLUT)
		return;

	psxMemWLUT = static_cast<uptr*>(_aligned_malloc(0x2000 * sizeof(uptr) * 2, 16));
	if (!psxMemWLUT)
	{
		std::fputs("Failed to allocate IOP memory lookup table\n", stderr);
		std::abort();
	}

	psxMemRLUT = psxMemWLUT + 0x2000;
}

void Ps2MemoryMap::ResetIopMemoryLookupTables()
{
	if (!psxMemWLUT || !psxMemRLUT || !eeMem || !iopMem)
	{
		std::fputs("IOP memory lookup table reset before memory allocation\n", stderr);
		std::abort();
	}

	std::memset(psxMemWLUT, 0, 0x2000 * sizeof(uptr) * 2);

	// Map IOP main memory, read/write and mirrored at 0x00000000,
	// 0x80000000, and 0xa0000000 in IOP-visible address space.
	for (int i = 0; i < 0x0080; i++)
	{
		const u32 mask = (Ps2MemSize::ExposedIopRam / _64kb) - 1;
		psxMemWLUT[i + 0x0000] = reinterpret_cast<uptr>(&iopMem->Main[(i & mask) << 16]);
		psxMemWLUT[i + 0x2000] = reinterpret_cast<uptr>(&iopMem->Main[(i & mask) << 16]);
	}

	psxMemWLUT[0x2000 + 0x1f00] = reinterpret_cast<uptr>(iopMem->P);
	psxMemWLUT[0x2000 + 0x1f80] = reinterpret_cast<uptr>(iopHw);

	psxMemWLUT[0x1f00] = reinterpret_cast<uptr>(iopMem->P);
	psxMemWLUT[0x1f80] = reinterpret_cast<uptr>(iopHw);

	// Read-only memory areas, so do not map WLUT for these.
	for (int i = 0; i < 0x0040; i++)
		psxMemWLUT[i + 0x2000 + 0x1fc0] = reinterpret_cast<uptr>(&eeMem->ROM[i << 16]);

	for (int i = 0; i < 0x0040; i++)
		psxMemWLUT[i + 0x2000 + 0x1e00] = reinterpret_cast<uptr>(&eeMem->ROM1[i << 16]);

	for (int i = 0; i < 0x0040; i++)
		psxMemWLUT[i + 0x2000 + 0x1e40] = reinterpret_cast<uptr>(&eeMem->ROM2[i << 16]);

	psxMemWLUT[0x2000 + 0x1d00] = reinterpret_cast<uptr>(iopMem->Sif);
}

void Ps2MemoryMap::ReleaseIopMemoryLookupTables()
{
	safe_aligned_free(psxMemWLUT);
	psxMemWLUT = nullptr;
	psxMemRLUT = nullptr;
}
