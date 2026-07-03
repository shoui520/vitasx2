// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "vtlb.h"

namespace Ps2MemoryMap
{
	vtlbHandler RegisterNullMemoryHandler();

	void MapEERamHighMemoryAndRoms(vtlbHandler null_handler);
	void MapKernelVirtualMirrors();
	void MapDirectVirtualMemoryWindow();

	void AllocateIopMemoryLookupTables();
	void ResetIopMemoryLookupTables();
	void ReleaseIopMemoryLookupTables();
}

extern uptr* psxMemWLUT;
extern const uptr* psxMemRLUT;
