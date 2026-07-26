// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <memory>

class Error;
namespace VitaGpuVu
{
	class GpuVuDraw;
}

namespace VitaGS
{
	void SetNativePresenterEnabled(bool enabled);
	bool IsNativePresenterEnabled();
	// Mirrors PCSX2's trace-start frame ownership at
	// VMManager::EntryPointCompilingOnCPUThread(). Performance windows after this
	// call use the game ELF entry as their VSync origin.
	void NotifyPerformanceElfEntry();

	// Transfers one immutable PATH1 descriptor into the ordered MTGS ring.
	// Ownership returns false by destruction; a successful call is consumed
	// exactly once by the GS/GXM-owning thread.
	bool QueueGpuVuDraw(std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);

	// Wakes the GS owner after the asynchronous compiler publishes a completed
	// GXP. This is a CPU work notification only: it never waits for or flushes
	// GXM, and registration/patching still occurs exclusively on the GS thread.
	void NotifyGpuVuCompilerResult();

	const u8* GetLocalMemoryForTrace(size_t* size);

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
	bool CopyPrivilegedRegistersForValidation(u8* output, size_t size);
	u64 GetMtvuPacketTokenResyncsForValidation();
#endif

#if defined(VITASX2_PRODUCT_BOOT_VALIDATION) && VITASX2_PRODUCT_BOOT_VALIDATION
	struct CanonicalRingValidationResult
	{
		u32 canonical_bytes = 0;
		u32 packet_qwc = 0;
		u32 pixel_checks = 0;
		u32 address_checks = 0;
		u32 clut_cases = 0;
		u32 readback_checks = 0;
		u64 packet_hash = 0;
		u64 local_hash = 0;
		u64 reopened_hash = 0;
		bool reopened_clean = false;
	};

	bool ValidateCanonicalLocalMemoryRing(
		CanonicalRingValidationResult* result, Error* error);
#endif
}
