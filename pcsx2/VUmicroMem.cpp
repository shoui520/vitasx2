// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "VUmicro.h"
#include "MTVU.h"

#include <type_traits>

alignas(16) VURegs vuRegs[2];

void vuMemAllocate()
{
	u8* curpos = SysMemory::GetVUMem();
	VU0.Micro	= curpos; curpos += VU0_PROGSIZE;
	VU0.Mem		= curpos; curpos += VU0_MEMSIZE;
	VU1.Micro	= curpos; curpos += VU1_PROGSIZE;
	VU1.Mem		= curpos; curpos += VU1_MEMSIZE;
}

void vuMemRelease()
{
	VU0.Micro = VU0.Mem = nullptr;
	VU1.Micro = VU1.Mem = nullptr;
}

void vuMemReset()
{
	pxAssert( VU0.Mem );
	pxAssert( VU1.Mem );

	// Below memMap is already called by "void eeMemoryReserve::Reset()"
	//memMapVUmicro();

	// === VU0 Initialization ===
	std::memset(&VU0.ACC, 0, sizeof(VU0.ACC));
	std::memset(VU0.VF, 0, sizeof(VU0.VF));
	std::memset(VU0.VI, 0, sizeof(VU0.VI));
    VU0.VF[0].f.x = 0.0f;
	VU0.VF[0].f.y = 0.0f;
	VU0.VF[0].f.z = 0.0f;
	VU0.VF[0].f.w = 1.0f;
	VU0.VI[0].UL = 0;

	// === VU1 Initialization ===
	std::memset(&VU1.ACC, 0, sizeof(VU1.ACC));
	std::memset(VU1.VF, 0, sizeof(VU1.VF));
	std::memset(VU1.VI, 0, sizeof(VU1.VI));
	VU1.VF[0].f.x = 0.0f;
	VU1.VF[0].f.y = 0.0f;
	VU1.VF[0].f.z = 0.0f;
	VU1.VF[0].f.w = 1.0f;
	VU1.VI[0].UL = 0;
}

bool SaveStateBase::vuMicroFreeze()
{
	// PCSX2's x86 microVU provider opens vu1Thread before this save barrier.
	// Vita's synchronous A32 VU1 provider has no worker, so waiting on the closed
	// worker's empty semaphore would deadlock with nobody able to signal it.
	if (IsSaving() && vu1Thread.IsOpen())
		vu1Thread.WaitVU();

	// Portable replays are captured only at the all-native checkpoint's idle VU
	// seam. MachineCheckpointTrace::HashVuState() defines the state which can
	// affect later PS2 execution there: published registers/cycles remain strict,
	// while interpreter/microVU bookkeeping is either empty or reconstructible.
	// Reject a real continuation before replacing its provider-private storage
	// representation with the portable canonical zero.
	if (IsPortableReplay() && IsSaving())
	{
		const auto is_zero = [](const auto& value) {
			using T = std::remove_cv_t<std::remove_reference_t<decltype(value)>>;
			static_assert(std::is_trivially_copyable_v<T>);
			T zero;
			std::memset(&zero, 0, sizeof(T));
			return std::memcmp(&value, &zero, sizeof(T)) == 0;
		};
		const bool idle = (VU0.VI[REG_VPU_STAT].UL & 0x101u) == 0 &&
			is_zero(VU0.branch) && is_zero(VU0.takedelaybranch) && is_zero(VU0.ebit) &&
			is_zero(VU0.VIBackupCycles) && is_zero(VU0.fmaccount) && is_zero(VU0.fdiv.enable) &&
			is_zero(VU0.efu.enable) && is_zero(VU0.ialucount) &&
			is_zero(VU1.branch) && is_zero(VU1.takedelaybranch) && is_zero(VU1.ebit) &&
			is_zero(VU1.VIBackupCycles) && is_zero(VU1.fmaccount) && is_zero(VU1.fdiv.enable) &&
			is_zero(VU1.efu.enable) && is_zero(VU1.ialucount) && is_zero(VU1.xgkickenable);
		if (!idle)
		{
			Console.Error("Portable replay capture reached a non-idle VU continuation.");
			m_error = true;
			return false;
		}
	}

	if (!FreezeTag("vuMicroRegs"))
		return false;

	// Preserve the native savestate schema exactly. For a portable replay this
	// freezes an equally-sized zero object instead, verifies that loads contain
	// the canonical representation, and clears only provider-private live state.
	// Published architectural state never passes through this helper.
	const auto freeze_portable_zero = [this](auto& live, const char* field) {
		if (!IsPortableReplay())
		{
			Freeze(live);
			return IsOkay();
		}

		using T = std::remove_cv_t<std::remove_reference_t<decltype(live)>>;
		static_assert(std::is_trivially_copyable_v<T>);
		T canonical;
		std::memset(&canonical, 0, sizeof(T));
		Freeze(canonical);
		if (!IsOkay())
			return false;

		if (IsLoading())
		{
			T zero;
			std::memset(&zero, 0, sizeof(T));
			if (std::memcmp(&canonical, &zero, sizeof(T)) != 0)
			{
				Console.Error("Portable replay contains non-canonical VU scratch field '%s'.", field);
				m_error = true;
				return false;
			}
			std::memcpy(&live, &zero, sizeof(T));
		}
		return true;
	};

	// VU0 state information

	Freeze(VU0.ACC);
	Freeze(VU0.VF);
	Freeze(VU0.VI);
	if (IsPortableReplay() && IsLoading() &&
		(HasError() || (VU0.VI[REG_VPU_STAT].UL & 0x101u) != 0))
	{
		Console.Error("Portable replay contains a busy VU continuation.");
		m_error = true;
		return false;
	}
	if (!freeze_portable_zero(VU0.q, "VU0.q"))
		return false;

	Freeze(VU0.cycle);
	Freeze(VU0.flags);
	if (!freeze_portable_zero(VU0.code, "VU0.code") ||
		!freeze_portable_zero(VU0.start_pc, "VU0.start_pc") ||
		!freeze_portable_zero(VU0.branch, "VU0.branch") ||
		!freeze_portable_zero(VU0.branchpc, "VU0.branchpc") ||
		!freeze_portable_zero(VU0.delaybranchpc, "VU0.delaybranchpc") ||
		!freeze_portable_zero(VU0.takedelaybranch, "VU0.takedelaybranch") ||
		!freeze_portable_zero(VU0.ebit, "VU0.ebit") ||
		!freeze_portable_zero(VU0.pending_q, "VU0.pending_q") ||
		!freeze_portable_zero(VU0.pending_p, "VU0.pending_p") ||
		!freeze_portable_zero(VU0.micro_macflags, "VU0.micro_macflags") ||
		!freeze_portable_zero(VU0.micro_clipflags, "VU0.micro_clipflags") ||
		!freeze_portable_zero(VU0.micro_statusflags, "VU0.micro_statusflags") ||
		!freeze_portable_zero(VU0.macflag, "VU0.macflag") ||
		!freeze_portable_zero(VU0.statusflag, "VU0.statusflag") ||
		!freeze_portable_zero(VU0.clipflag, "VU0.clipflag"))
	{
		return false;
	}
	Freeze(VU0.nextBlockCycles);
	if (!freeze_portable_zero(VU0.VIBackupCycles, "VU0.VIBackupCycles") ||
		!freeze_portable_zero(VU0.VIOldValue, "VU0.VIOldValue") ||
		!freeze_portable_zero(VU0.VIRegNumber, "VU0.VIRegNumber") ||
		!freeze_portable_zero(VU0.fmac, "VU0.fmac") ||
		!freeze_portable_zero(VU0.fmacreadpos, "VU0.fmacreadpos") ||
		!freeze_portable_zero(VU0.fmacwritepos, "VU0.fmacwritepos") ||
		!freeze_portable_zero(VU0.fmaccount, "VU0.fmaccount") ||
		!freeze_portable_zero(VU0.fdiv, "VU0.fdiv") ||
		!freeze_portable_zero(VU0.efu, "VU0.efu") ||
		!freeze_portable_zero(VU0.ialu, "VU0.ialu") ||
		!freeze_portable_zero(VU0.ialureadpos, "VU0.ialureadpos") ||
		!freeze_portable_zero(VU0.ialuwritepos, "VU0.ialuwritepos") ||
		!freeze_portable_zero(VU0.ialucount, "VU0.ialucount"))
	{
		return false;
	}

	// VU1 state information
	Freeze(VU1.ACC);
	Freeze(VU1.VF);
	Freeze(VU1.VI);
	if (!freeze_portable_zero(VU1.q, "VU1.q") || !freeze_portable_zero(VU1.p, "VU1.p"))
		return false;

	Freeze(VU1.cycle);
	Freeze(VU1.flags);
	if (!freeze_portable_zero(VU1.code, "VU1.code") ||
		!freeze_portable_zero(VU1.start_pc, "VU1.start_pc") ||
		!freeze_portable_zero(VU1.branch, "VU1.branch") ||
		!freeze_portable_zero(VU1.branchpc, "VU1.branchpc") ||
		!freeze_portable_zero(VU1.delaybranchpc, "VU1.delaybranchpc") ||
		!freeze_portable_zero(VU1.takedelaybranch, "VU1.takedelaybranch") ||
		!freeze_portable_zero(VU1.ebit, "VU1.ebit") ||
		!freeze_portable_zero(VU1.pending_q, "VU1.pending_q") ||
		!freeze_portable_zero(VU1.pending_p, "VU1.pending_p") ||
		!freeze_portable_zero(VU1.micro_macflags, "VU1.micro_macflags") ||
		!freeze_portable_zero(VU1.micro_clipflags, "VU1.micro_clipflags") ||
		!freeze_portable_zero(VU1.micro_statusflags, "VU1.micro_statusflags") ||
		!freeze_portable_zero(VU1.macflag, "VU1.macflag") ||
		!freeze_portable_zero(VU1.statusflag, "VU1.statusflag") ||
		!freeze_portable_zero(VU1.clipflag, "VU1.clipflag"))
	{
		return false;
	}
	Freeze(VU1.nextBlockCycles);
	if (!freeze_portable_zero(VU1.xgkickaddr, "VU1.xgkickaddr") ||
		!freeze_portable_zero(VU1.xgkickdiff, "VU1.xgkickdiff") ||
		!freeze_portable_zero(VU1.xgkicksizeremaining, "VU1.xgkicksizeremaining") ||
		!freeze_portable_zero(VU1.xgkicklastcycle, "VU1.xgkicklastcycle") ||
		!freeze_portable_zero(VU1.xgkickcyclecount, "VU1.xgkickcyclecount") ||
		!freeze_portable_zero(VU1.xgkickenable, "VU1.xgkickenable") ||
		!freeze_portable_zero(VU1.xgkickendpacket, "VU1.xgkickendpacket") ||
		!freeze_portable_zero(VU1.VIBackupCycles, "VU1.VIBackupCycles") ||
		!freeze_portable_zero(VU1.VIOldValue, "VU1.VIOldValue") ||
		!freeze_portable_zero(VU1.VIRegNumber, "VU1.VIRegNumber") ||
		!freeze_portable_zero(VU1.fmac, "VU1.fmac") ||
		!freeze_portable_zero(VU1.fmacreadpos, "VU1.fmacreadpos") ||
		!freeze_portable_zero(VU1.fmacwritepos, "VU1.fmacwritepos") ||
		!freeze_portable_zero(VU1.fmaccount, "VU1.fmaccount") ||
		!freeze_portable_zero(VU1.fdiv, "VU1.fdiv") ||
		!freeze_portable_zero(VU1.efu, "VU1.efu") ||
		!freeze_portable_zero(VU1.ialu, "VU1.ialu") ||
		!freeze_portable_zero(VU1.ialureadpos, "VU1.ialureadpos") ||
		!freeze_portable_zero(VU1.ialuwritepos, "VU1.ialuwritepos") ||
		!freeze_portable_zero(VU1.ialucount, "VU1.ialucount"))
	{
		return false;
	}

	if (IsPortableReplay() && IsLoading())
	{
		// The portable bytes deliberately omit provider-specific delayed copies,
		// but both microVU's next dispatcher entry and the A32/interpreter path
		// consume them. Rebuild the idle representation from the published flags
		// and scalar registers before either provider can resume.
		const auto rebuild_idle_provider_state = [](VURegs& vu) {
			vu.q.UL = vu.VI[REG_Q].UL;
			vu.p.UL = vu.VI[REG_P].UL;
			vu.pending_q = vu.VI[REG_Q].UL;
			vu.pending_p = vu.VI[REG_P].UL;
			vu.macflag = vu.VI[REG_MAC_FLAG].UL;
			vu.statusflag = vu.VI[REG_STATUS_FLAG].UL;
			vu.clipflag = vu.VI[REG_CLIP_FLAG].UL;
			// PCSX2 owner: x86/microVU_Alloc.inl::mVUallocSFLAGd().
			const u32 denormalized_status = ((vu.statusflag >> 3) & 0x18u) |
				((vu.statusflag << 11) & 0x1800u) |
				((vu.statusflag << 14) & 0x03cf0000u);
			for (u32 lane = 0; lane < 4; lane++)
			{
				vu.micro_macflags[lane] = vu.macflag;
				vu.micro_clipflags[lane] = vu.clipflag;
				vu.micro_statusflags[lane] = denormalized_status;
			}
		};
		rebuild_idle_provider_state(VU0);
		rebuild_idle_provider_state(VU1);
	}

	return IsOkay();
}
