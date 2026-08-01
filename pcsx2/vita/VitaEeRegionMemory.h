// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/VitaEeRegionIR.h"

namespace VitaEE
{
	class BlockExecutor;
}

namespace VitaEE::RegionIR
{
	// Reference implementation of the direct-memory proof which the Phase 3
	// A32 backend will inline. It accepts only one-page, non-handler mappings
	// backed by retail EE RAM. Stores additionally require every touched
	// 64-byte source chunk to be unowned before the write occurs.
	struct VtlbMemoryContext
	{
		const u8* ram_source_page_live_flags = nullptr;
		const u8* ram_source_chunk_live_bits = nullptr;
	};

	VtlbMemoryContext MakeVtlbMemoryContext(const VitaEE::BlockExecutor& executor);
	RegionMemoryInterface MakeVtlbMemoryInterface(VtlbMemoryContext* context);
	MemoryProbeResult ProbeVtlbMemory(void* context,
		const MemoryRequest& request);
} // namespace VitaEE::RegionIR
