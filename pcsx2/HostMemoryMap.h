// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "MemoryTypes.h"
#include "common/BitUtils.h"

// This is a table of default virtual map addresses for ps2vm components. These
// locations are provided and used to assist in debugging and possibly hacking;
// it makes it possible for a programmer to know exactly where to look
// (consistently!) for the base address of the various virtual machine
// components. These addresses can be keyed directly into the debugger's disasm
// window to get disassembly of recompiled code, and they can be used to help
// identify recompiled code addresses in the callstack.
//
// All of these areas should be reserved as soon as possible during program
// startup, and it's important that none of the areas overlap. In all but
// superVU's case, failure due to overlap or other conflict will result in the
// operating system picking a preferred address for the mapping.

namespace HostMemoryMap
{
	//////////////////////////////////////////////////////////////////////////
	// Main
	//////////////////////////////////////////////////////////////////////////

	// PS2 main memory, SPR, and ROMs (approximately 143MB).
	// Needs to be big enough to fit the EEVM_MemoryAllocMess struct.
	static constexpr u32 EEmemOffset = 0x00000000;
	static constexpr u32 EEmemSize = Common::AlignUp(sizeof(EEVM_MemoryAllocMess), _1mb);

	// IOP main memory (approximately 3MB).
	// Needs to be big enough to fit the IopVM_MemoryAllocMess struct.
	static constexpr u32 IOPmemOffset = EEmemOffset + EEmemSize;
	static constexpr u32 IOPmemSize = Common::AlignUp(sizeof(IopVM_MemoryAllocMess), _1mb);

	// VU0 and VU1 memory (40KB, rounded up to 1MB for simplicity).
	static constexpr u32 VUmemOffset = IOPmemOffset + IOPmemSize;
	static constexpr u32 VUmemSize = 0x100000;

	// VTLB virtual map ((4GB / 4096) * sizeof(ptr)).
	static constexpr u32 VTLBVirtualMapOffset = VUmemOffset + VUmemSize;
	static constexpr u32 VTLBVirtualMapSize = (0x100000000ULL / 4096) * sizeof(void*);

	// VTLB address map ((4GB / 4096) * sizeof(u32)).
	static constexpr u32 VTLBAddressMapOffset = VTLBVirtualMapOffset + VTLBVirtualMapSize;
	static constexpr u32 VTLBAddressMapSize = (0x100000000ULL / 4096) * sizeof(u32);

	// Overall size.
	static constexpr u32 MainSize = VTLBAddressMapOffset + VTLBAddressMapSize;

	//////////////////////////////////////////////////////////////////////////
	// Code
	//////////////////////////////////////////////////////////////////////////

	// EE recompiler code cache area (64mb desktop, smaller on Vita).
	static constexpr u32 EErecOffset = 0x00000000;
#if defined(ARCH_ARM32)
	static constexpr u32 EErecSize = 0x800000;
#else
	static constexpr u32 EErecSize = 0x4000000;
#endif

	// IOP recompiler code cache area (32mb desktop, smaller on Vita).
	static constexpr u32 IOPrecOffset = EErecOffset + EErecSize;
#if defined(ARCH_ARM32)
	static constexpr u32 IOPrecSize = 0x100000;
#else
	static constexpr u32 IOPrecSize = 0x2000000;
#endif

	// newVif0 recompiler code cache area (8mb desktop, smaller on Vita).
	static constexpr u32 VIF0recOffset = IOPrecOffset + IOPrecSize;
#if defined(ARCH_ARM32)
	static constexpr u32 VIF0recSize = 0x80000;
#else
	static constexpr u32 VIF0recSize = 0x800000;
#endif

	// newVif1 recompiler code cache area (8mb desktop, smaller on Vita).
	static constexpr u32 VIF1recOffset = VIF0recOffset + VIF0recSize;
#if defined(ARCH_ARM32)
	static constexpr u32 VIF1recSize = 0x80000;
#else
	static constexpr u32 VIF1recSize = 0x800000;
#endif

	// microVU1 recompiler code cache area (64mb desktop, smaller on Vita).
	static constexpr u32 mVU0recOffset = VIF1recOffset + VIF1recSize;
#if defined(ARCH_ARM32)
	static constexpr u32 mVU0recSize = 0x100000;
#else
	static constexpr u32 mVU0recSize = 0x4000000;
#endif

	// microVU0 recompiler code cache area (64mb desktop, smaller on Vita).
	static constexpr u32 mVU1recOffset = mVU0recOffset + mVU0recSize;
#if defined(ARCH_ARM32)
	static constexpr u32 mVU1recSize = 0x200000;
#else
	static constexpr u32 mVU1recSize = 0x4000000;
#endif

	// Optimized VIF unpack functions (1mb desktop, smaller on Vita).
	static constexpr u32 VIFUnpackRecOffset = mVU1recOffset + mVU1recSize;
#if defined(ARCH_ARM32)
	static constexpr u32 VIFUnpackRecSize = 0x80000;
#else
	static constexpr u32 VIFUnpackRecSize = 0x100000;
#endif

	// Software Renderer JIT buffer (64mb desktop, omitted on interpreter-only Vita).
	static constexpr u32 SWrecOffset = VIFUnpackRecOffset + VIFUnpackRecSize;
#if defined(ARCH_ARM32)
	static constexpr u32 SWrecSize = 0;
#else
	static constexpr u32 SWrecSize = 0x04000000;
#endif

	// Overall size.
	static constexpr u32 CodeSize = SWrecOffset + SWrecSize;
} // namespace HostMemoryMap
