// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Common.h"

#include "common/SingleRegisterTypes.h"

void resetCache();
// Dumps all dirty cache entries to memory
// This is necessary to fix a bug when enabled the recompiler while the cache was enabled.
void writebackCache();
void executeCacheOp(u32 op, u32 addr);
// Executes the exact two-way DXLTG/tag-check/induction sweep owned by
// Cache.cpp and returns complete/self/event as 0/1/2 after publishing EE state.
u32 executeCacheDxltgTagSweep(u32 start_pc, u32 fallthrough_pc,
	u32 packed_guests, u32 packed_cycles);
// Executes consecutive DXWBIN index pairs at addr/addr+1, advancing addr by
// one 64-byte EE cache line per pair. This is the exact Cache.cpp-owned
// operation used by the EE kernel's two-way D-cache maintenance loop.
void executeCacheDxwbinPairRange(u32 addr, u32 pair_count);
void writeCache8(u32 mem, u8 value, bool validPFN = true);
void writeCache16(u32 mem, u16 value, bool validPFN = true);
void writeCache32(u32 mem, u32 value, bool validPFN = true);
void writeCache64(u32 mem, const u64 value, bool validPFN = true);
void writeCache128(u32 mem, const mem128_t* value, bool validPFN = true);
u8 readCache8(u32 mem, bool validPFN = true);
u16 readCache16(u32 mem, bool validPFN = true);
u32 readCache32(u32 mem, bool validPFN = true);
u64 readCache64(u32 mem, bool validPFN = true);
RETURNS_R128 readCache128(u32 mem, bool validPFN = true);
