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
	struct RawVifPayloadRef;
}

namespace VitaGS
{
	void SetNativePresenterEnabled(bool enabled);
	bool IsNativePresenterEnabled();
	// Mirrors PCSX2's trace-start frame ownership at
	// VMManager::EntryPointCompilingOnCPUThread(). Performance windows after this
	// call use the game ELF entry as their VSync origin.
	void NotifyPerformanceElfEntry();

	// Completes the next EE-reserved MTVU PATH1 ordering point with an immutable
	// direct draw. Ownership returns false by destruction; a successful call is
	// consumed exactly once by the GS/GXM-owning thread.
	bool QueueGpuVuDraw(std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);

	// Completes the next EE-reserved MTVU PATH1 ordering point with the packet
	// already published by Gif_Path::FinishGSPacketMTVU(). This replaces the
	// desktop per-dispatch semaXGkick lifecycle on the Vita mailbox.
	void CompleteMtvuPath1Packet();

	// Publishes a trailing run of direct descriptors before a strong MTVU
	// observation/drain which may have no following CPU PATH1 packet.
	void FlushMtvuPath1Completions();

	// Wakes the GS owner after the asynchronous compiler publishes a completed
	// GXP. This is a CPU work notification only: it never waits for or flushes
	// GXM, and registration/patching still occurs exclusively on the GS thread.
	void NotifyGpuVuCompilerResult();

	// Wakes the GS owner when immutable VIF capture has exhausted all four
	// mapped slots. The owner alone may submit/wait for GXM retirement; the
	// capture producer sleeps until descriptor destruction releases a slot.
	void RequestGpuVuInputRetirement(
		const VitaGpuVu::RawVifPayloadRef& blocked_generation);

	const u8* GetLocalMemoryForTrace(size_t* size);

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
	bool CopyPrivilegedRegistersForValidation(u8* output, size_t size);
	u64 GetMtvuPath1CompletionDeferralsForValidation();
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
