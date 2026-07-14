// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstddef>

#if defined(VITASX2_QEMU_VALIDATION)
using SceUID = int;
#else
#include <psp2common/types.h>
#endif

namespace VitaVM
{
	void* AllocJitMemory(size_t size, SceUID* out_uid = nullptr);
	void FreeJitMemory(void* ptr);
	bool BeginJitWrite();
	bool EndJitWrite();
	bool EndJitWriteAndSync(void* address, size_t size);
	bool SyncJitMemory(void* address, size_t size);
} // namespace VitaVM
