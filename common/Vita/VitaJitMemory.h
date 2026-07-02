// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstddef>

#include <psp2common/types.h>

namespace VitaVM
{
	void* AllocJitMemory(size_t size, SceUID* out_uid = nullptr);
	void FreeJitMemory(void* ptr);
} // namespace VitaVM
