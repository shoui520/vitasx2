// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "common/StringUtil.h"
#include "ps2/BiosTools.h"
#include "R5900.h"
#include "R3000A.h"
#include "ps2/pgif.h" // pgif init
#include "VUmicro.h"
#include "COP0.h"
#include "MTVU.h"
#include "VMManager.h"
#if defined(VITASX2_VITA)
#include "vita/VitaCore.h"
#include "vita/VitaEeDmacWait.h"
#include "vita/VitaPerformanceTelemetry.h"
#endif

#include "Hardware.h"
#include "IopHw.h"
#include "IPU/IPUdma.h"

#include "Elfheader.h"
#include "CDVD/CDVD.h"
#include "Patch.h"
#include "GameDatabase.h"
#include "GSDumpReplayer.h"

#include "DebugTools/Breakpoints.h"
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
#include "DebugTools/CoreEventTrace.h"
#endif
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#include "DebugTools/MIPSAnalyst.h"
#include "DebugTools/SymbolGuardian.h"
#include "R5900OpcodeTables.h"

#include "fmt/format.h"

#include <limits>

using namespace R5900;	// for R5900 disasm tools

static __fi void NotifyEeRamHostWrite(const void* address, u32 size)
{
#if defined(VITASX2_VITA)
	VitaNotifyA32EeRamWrite(address, size);
#else
	(void)address;
	(void)size;
#endif
}

s32 EEsCycle;		// used to sync the IOP to the EE
u64 EEoCycle;

alignas(16) cpuRegistersPack _cpuRegistersPack;
alignas(16) tlbs tlb[48];
cachedTlbs_t cachedTlbs;

R5900cpu *Cpu = NULL;

static constexpr uint eeWaitCycles = 3072;
static bool cpuIntsEnabled(int Interrupt);
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
static constexpr uint eeRetainedIopWaitCycles = eeWaitCycles * 2;
static u64 s_vita_ee_owner_event_cycle;
static u64 s_vita_ee_non_counter_owner_event_cycle;
static u64 s_vita_ee_counter_owner_event_cycle;
static bool s_vita_next_event_iop_only;

#if defined(VITASX2_QEMU_VALIDATION)
bool g_vita_ee_interleave_scheduler_validation_enabled = false;
u64 g_vita_ee_full_scheduler_validation_entries = 0;
u64 g_vita_ee_iop_only_scheduler_validation_entries = 0;
bool g_vita_joint_wait_horizon_validation_enabled = false;
#endif

static __fi bool VitaEeInterleaveSchedulerActive()
{
#if defined(VITASX2_QEMU_VALIDATION)
	return g_vita_ee_interleave_scheduler_validation_enabled;
#else
	return true;
#endif
}

static __fi bool VitaCanCoalesceRetainedIopWait()
{
	// CP0 Count is updated lazily when read, but its compare interrupt is
	// polled by _cpuTestTIMR(). Retain PCSX2's ordinary cadence whenever that
	// interrupt is enabled.
	return VitaA32IopRetainedWaitCoalescingActive() &&
		(cpuRegs.CP0.n.Status.val & 0x8000u) == 0;
}

static __fi u32 VitaEeInterleaveCycles()
{
	return VitaCanCoalesceRetainedIopWait() ?
		eeRetainedIopWaitCycles : eeWaitCycles;
}

static __fi void VitaResetEeDeadlineState(u64 deadline)
{
	s_vita_ee_owner_event_cycle = deadline;
	s_vita_ee_non_counter_owner_event_cycle = deadline;
	s_vita_ee_counter_owner_event_cycle = deadline;
	s_vita_next_event_iop_only = false;
}

static __fi void VitaRefreshCombinedEeOwner()
{
	s_vita_ee_owner_event_cycle = std::min(
		s_vita_ee_non_counter_owner_event_cycle,
		s_vita_ee_counter_owner_event_cycle);
	if (static_cast<s32>(
		s_vita_ee_owner_event_cycle - cpuRegs.nextEventCycle) <= 0)
	{
		s_vita_next_event_iop_only = false;
	}
}

static __fi void VitaBeginFullEeDeadlineCollection()
{
	// PCSX2's scheduler reconstructs its next owner from the callbacks and
	// counters it services below. Keep that owner separately from the bounded
	// EE/IOP execution seam so an ordinary IOP slice need not poll every EE
	// owner.
	const u64 no_owner =
		cpuRegs.cycle + static_cast<u32>(std::numeric_limits<s32>::max());
	s_vita_ee_non_counter_owner_event_cycle = no_owner;
	s_vita_ee_counter_owner_event_cycle = no_owner;
	s_vita_ee_owner_event_cycle = no_owner;
	s_vita_next_event_iop_only = false;
	cpuRegs.nextEventCycle = cpuRegs.cycle + VitaEeInterleaveCycles();
}

static __fi void VitaRefreshEeCounterOwner()
{
	// Counters.cpp owns this canonical absolute deadline. Counter register and
	// video-timing changes normally publish it through cpuSetNextEvent(), but
	// the representation can also be rebuilt while an older event deadline is
	// already due. Treat the canonical pair as an explicit calendar slot at
	// every scheduler boundary so a missed narrowing can cause one extra full
	// pass, never suppress HSync/VSync behind an IOP-only seam.
	s_vita_ee_counter_owner_event_cycle =
		nextStartCounter + static_cast<s32>(nextDeltaCounter);
	VitaRefreshCombinedEeOwner();
}

static __fi void VitaScheduleEeAndIopDeadlineDelta(s64 iop_owner_delta)
{
	const s32 iop_delta = iop_owner_delta <= 0 ? 0 :
		iop_owner_delta >= std::numeric_limits<s32>::max() ?
			std::numeric_limits<s32>::max() :
			static_cast<s32>(iop_owner_delta);
	cpuRegs.nextEventCycle = s_vita_ee_owner_event_cycle;
	if (static_cast<s32>(
			s_vita_ee_owner_event_cycle - cpuRegs.cycle) > iop_delta)
	{
		cpuRegs.nextEventCycle = cpuRegs.cycle + iop_delta;
		// Only an IOP-owned boundary strictly before every retained EE owner
		// may use the narrow scheduler. Equal deadlines belong to the EE owner.
		s_vita_next_event_iop_only =
			(cpuRegs.CP0.n.Status.val & 0x8000u) == 0;
	}
	else
	{
		s_vita_next_event_iop_only = false;
	}
}

static __fi void VitaScheduleEeAndIopDeadlines(s32 iop_owner_delta)
{
	VitaScheduleEeAndIopDeadlineDelta(std::min(
		iop_owner_delta, static_cast<s32>(VitaEeInterleaveCycles())));
}

static __fi bool VitaCanSkipEeOwnersAtInterleave()
{
	// The Counters.cpp start/delta pair is the semantic authority. A producer
	// ordinarily narrows the cached slot, and VitaRefreshEeCounterOwner()
	// repairs a rebuilt future slot. Still fail closed when the canonical
	// counter is already due: initialization and a past-target rebuild can
	// occur while an older branch deadline is also due, where a narrowing-only
	// publication is intentionally ignored by PCSX2's cpuSetNextEvent().
	if (cpuTestCycle(nextStartCounter, nextDeltaCounter) ||
		(cpuRegs.CP0.n.Status.val & 0x8000u) != 0 ||
		static_cast<s32>(
			s_vita_ee_owner_event_cycle - cpuRegs.cycle) <= 0 ||
		!VitaEeInterleaveSchedulerActive())
	{
		return false;
	}

	const u32 vu_running = VU0.VI[REG_VPU_STAT].UL;
	if ((vu_running & 1u) != 0 ||
		(!THREAD_VU1 && (vu_running & 0x100u) != 0) ||
		(THREAD_VU1 && vu1Thread.HasPendingChanges()))
	{
		return false;
	}

	// A visible CPU exception is not an interleave-only owner, even if a
	// producer failed to narrow the cached deadline. Likewise, the BIOS
	// instant-DMA compatibility path intentionally services pending work
	// without waiting for its nominal deadline.
	const uint exception_mask = intcInterrupt() | dmacInterrupt();
	if (cpuIntsEnabled(exception_mask) ||
		(CHECK_INSTANTDMAHACK && (cpuRegs.interrupt & 0x1ffffu) != 0))
	{
		return false;
	}

	return true;
}

static __fi bool VitaCanRunIopOnlyEeInterleave()
{
	return s_vita_next_event_iop_only &&
		static_cast<s32>(
			cpuRegs.cycle - cpuRegs.nextEventCycle) >= 0 &&
		VitaCanSkipEeOwnersAtInterleave();
}

static __attribute__((noinline, cold))
s32 VitaScaleNonstandardIopEventDeltaToEe(u64 iop_cycles)
{
	const float multiplier =
		static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
	return static_cast<s32>(static_cast<float>(iop_cycles) * multiplier);
}

static __fi s32 VitaScaleIopEventDeltaToEe(u64 iop_cycles)
{
	// PCSX2 owner: R3000AInterpreter.cpp::intExecuteBlock() and
	// x86/iR3000A.cpp::iPsxAddEECycles(). Ordinary PS2 mode is exactly 8:1;
	// avoid converting through float and issuing a VFP divide at every EE
	// event seam. Keep the PS1 calculation out of the hot scheduler body so
	// its private A32 entry does not spill an otherwise-unused callee-saved
	// VFP register.
	if (PSXCLK == (PS2CLK / 8u)) [[likely]]
		return static_cast<s32>(iop_cycles * 8u);

	return VitaScaleNonstandardIopEventDeltaToEe(iop_cycles);
}

static __fi s32 VitaNextIopOwnerDelta()
{
	const s32 next_iop_event_delta =
		VitaScaleIopEventDeltaToEe(
			psxRegs.iopNextEventCycle - psxRegs.cycle);
	return EEsCycle >= next_iop_event_delta ?
		48 : next_iop_event_delta - EEsCycle;
}

static __fi s64 VitaNextIopExternalOwnerDelta(u64 external_cycle)
{
	const u64 iop_cycles = external_cycle > psxRegs.cycle ?
		external_cycle - psxRegs.cycle : 0;
	const s64 scaled_iop_cycles = static_cast<s64>(iop_cycles) * 8;
	return scaled_iop_cycles - static_cast<s64>(EEsCycle);
}

static __fi bool VitaJointWaitHorizonActive()
{
#if defined(VITASX2_QEMU_VALIDATION)
	return g_vita_joint_wait_horizon_validation_enabled;
#else
	return true;
#endif
}

static __fi bool VitaEeWaitCertificateHasExactWakeContract(
	const VitaA32EeWaitSchedulerCertificate& certificate)
{
	if (certificate.ram_write_observed != 0)
		return false;

	switch (certificate.origin)
	{
		case VitaA32EeWaitSchedulerOrigin::PollCallRamLoop:
			return certificate.ram_range_count == 1;
		case VitaA32EeWaitSchedulerOrigin::TwoPredicateRamLoop:
			return certificate.ram_range_count == 2;
		case VitaA32EeWaitSchedulerOrigin::IntcVblankStartAndRamLoop:
			return certificate.ram_range_count == 1;
		case VitaA32EeWaitSchedulerOrigin::RetainedUnconditionalLoop:
		case VitaA32EeWaitSchedulerOrigin::GsCsrVsintLoop:
		case VitaA32EeWaitSchedulerOrigin::DmacChcrStrPollLoop:
			return certificate.ram_range_count == 0;
		case VitaA32EeWaitSchedulerOrigin::None:
		case VitaA32EeWaitSchedulerOrigin::GenericRamLoop:
		default:
			return false;
	}
}

static __fi bool VitaTryScheduleJointWaitHorizon(
	const VitaA32EeWaitSchedulerCertificate& wait_certificate,
	u32 wait_pc)
{
	// PCSX2 owners: the EE wait-loop max(cycle,nextEventCycle) lowering and
	// R3000A.cpp::iopEventTest(). The EE certificate proves that another loop
	// iteration can observe only its watched RAM, the exact GSVSync owner, or an
	// enumerated DMAC CHCR.STR whose completion is deadline-owned; the retained
	// IOP descriptor proves that the IOP has no guest work before its next
	// counter/callback/interrupt owner. Every uncertain state keeps the existing
	// bounded 3072/6144-cycle seam.
	if (!VitaJointWaitHorizonActive() ||
		PSXCLK != (PS2CLK / 8u) ||
		!VitaA32IopRetainedWaitCoalescingActive() ||
		!VitaEeWaitCertificateHasExactWakeContract(wait_certificate) ||
		cpuRegs.pc != wait_pc || iopEventAction ||
		!VitaCanSkipEeOwnersAtInterleave())
	{
		return false;
	}

	const u64 external_cycle = VitaGetIopExternalEventCycle();
	if (external_cycle <= psxRegs.cycle)
		return false;
	const s64 iop_owner_delta =
		VitaNextIopExternalOwnerDelta(external_cycle);
	if (iop_owner_delta <= 0)
		return false;

	// HSync is normally the dominant EE calendar owner, but many games leave
	// every HBlank-counted/gated timer disabled and HSINT masked or already
	// latched while both processors wait. In that state its intermediate edges
	// change only the final HBlank level. Fold only edges strictly before the
	// first non-HSync EE counter, asynchronous EE event, or exact IOP owner.
	// Counters.cpp stops before VSync and before any unmasked first HSINT.
	const u64 iop_owner_cycle =
		cpuRegs.cycle + static_cast<u64>(iop_owner_delta);
	const u64 silent_hsync_limit = std::min({
		iop_owner_cycle,
		s_vita_ee_non_counter_owner_event_cycle,
		VitaGetNextNonHsyncCounterCycle()});
	const VitaSilentHsyncFoldResult hsync_fold =
		VitaFoldSilentHsyncBefore(silent_hsync_limit);
	VitaPerformanceTelemetry::RecordSilentHsyncFoldIfProfiling(
		hsync_fold.folded_edges,
		static_cast<u32>(hsync_fold.stop), hsync_fold.stop_detail);

	// Replace iopEventTest()'s manufactured polling seed only after both wait
	// proofs hold. A later PSX_INT()/counter write still narrows this canonical
	// slot through PCSX2's ordinary psxSetNextBranch() mechanism.
	psxRegs.iopNextEventCycle = external_cycle;
	VitaScheduleEeAndIopDeadlineDelta(iop_owner_delta);
	VitaPerformanceTelemetry::RecordJointWaitActivationIfProfiling(
		static_cast<u32>(cpuRegs.nextEventCycle - cpuRegs.cycle));
	return true;
}

#if defined(VITASX2_CPU_PROFILER)
static __fi void VitaRecordJointWaitShadowAtDeadline(
	const VitaA32EeWaitSchedulerCertificate& wait_certificate,
	u32 wait_pc)
{
	const bool iop_retained =
		VitaA32IopRetainedWaitCoalescingActive();
	const s64 ee_owner_delta = static_cast<s64>(
		s_vita_ee_owner_event_cycle - cpuRegs.cycle);
	const s64 iop_owner_delta = VitaNextIopExternalOwnerDelta(
		VitaGetIopExternalEventCycle());
	const s64 horizon_delta =
		std::min(ee_owner_delta, iop_owner_delta);
	const bool ram_poll =
		wait_certificate.origin ==
			VitaA32EeWaitSchedulerOrigin::GenericRamLoop ||
		wait_certificate.origin ==
			VitaA32EeWaitSchedulerOrigin::PollCallRamLoop ||
		wait_certificate.origin ==
			VitaA32EeWaitSchedulerOrigin::TwoPredicateRamLoop ||
		wait_certificate.origin ==
			VitaA32EeWaitSchedulerOrigin::IntcVblankStartAndRamLoop;
	const bool unknown_writer =
		ram_poll && wait_certificate.ram_range_count == 0;
	const bool blocked =
		wait_certificate.ram_write_observed != 0 ||
		cpuRegs.pc != wait_pc || iopEventAction ||
		(cpuRegs.CP0.n.Status.val & 0x8000u) != 0 ||
		!VitaCanSkipEeOwnersAtInterleave();
	VitaPerformanceTelemetry::RecordJointWaitShadowIfProfiling(
		static_cast<u32>(wait_certificate.origin), iop_retained,
		horizon_delta, unknown_writer, blocked,
		wait_certificate.ram_range_count != 0 ?
			wait_certificate.ram_offset[0] : UINT32_MAX,
		wait_certificate.ram_range_count != 0 ?
			wait_certificate.ram_size[0] : 0);
}
#endif
#endif

bool eeEventTestIsActive = false;
EE_intProcessStatus eeRunInterruptScan = INT_NOT_RUNNING;

u32 g_eeloadMain = 0, g_eeloadExec = 0, g_osdsys_str = 0;

#if defined(__arm__)
namespace
{
	[[noreturn]] inline __attribute__((always_inline))
	void ReturnFromPrivateCpuEventTestShared()
	{
		register void* caller_cfa asm("r0") = __builtin_dwarf_cfa();
		asm volatile(
			// The private entry reserved the body's ordinary nine-word save
			// area, but only LR belongs to it. The persistent EE event bridge
			// already owns r4-r11 for the complete generated-code run. Keep
			// floating-point work in AAPCS callees so the body itself never
			// acquires a callee-saved VFP frame.
			"ldr lr, [r0, #-4]\n"
			"mov sp, r0\n"
			"bx lr\n"
			:
			: "r"(caller_cfa)
			: "lr", "memory");
		__builtin_unreachable();
	}
}
#endif

#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
static __fi Pcsx2Trace::CoreEventId GetEeSifCoreEventId(u8 event)
{
	return event == DMAC_SIF0 ? Pcsx2Trace::CoreEventId::EeDmacSif0 :
		event == DMAC_SIF1 ? Pcsx2Trace::CoreEventId::EeDmacSif1 :
		Pcsx2Trace::CoreEventId::None;
}

static __fi void TraceEeCoreEvent(Pcsx2Trace::CoreEventKind kind,
	Pcsx2Trace::CoreEventPhase phase, Pcsx2Trace::CoreEventId event_id, u64 target_cycle)
{
	Pcsx2Trace::RecordCoreEvent(kind, phase, Pcsx2Trace::CoreEventDomain::Ee,
		event_id, target_cycle,
		cpuRegs.interrupt, cpuRegs.dmastall, psxRegs.interrupt,
		cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, psxRegs.CP0.n.Status,
		psxHu32(HW_ISTAT), psxHu32(HW_IMASK), psxHu32(HW_ICTRL),
		cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.BadVAddr);
}
#endif

/* I don't know how much space for args there is in the memory block used for args in full boot mode,
but in fast boot mode, the block we use can fit at least 16 argv pointers (varies with BIOS version).
The second EELOAD call during full boot has three built-in arguments ("EELOAD rom0:PS2LOGO <ELF>"),
meaning that only the first 13 game arguments supplied by the user can be added on and passed through.
In fast boot mode, 15 arguments can fit because the only call to EELOAD is "<ELF> <<args>>". */
const int kMaxArgs = 16;
uptr g_argPtrs[kMaxArgs];
#define DEBUG_LAUNCHARG 0 // show lots of helpful console messages as the launch arguments are passed to the game

void cpuReset()
{
	std::memset(&cpuRegs, 0, sizeof(cpuRegs));
	std::memset(&fpuRegs, 0, sizeof(fpuRegs));
	std::memset(&tlb, 0, sizeof(tlb));
	cachedTlbs.count = 0;

	cpuRegs.pc				= 0xbfc00000; //set pc reg to stack
	cpuRegs.CP0.n.Config	= 0x440;
	cpuRegs.CP0.n.Status.val= 0x70400004; //0x10900000 <-- wrong; // COP0 enabled | BEV = 1 | TS = 1
	cpuRegs.CP0.n.PRid		= 0x00002e20; // PRevID = Revision ID, same as R5900
	fpuRegs.fprc[0]			= 0x00002e30; // fpu Revision..
	fpuRegs.fprc[31]		= 0x01000001; // fpu Status/Control

	cpuRegs.nextEventCycle = cpuRegs.cycle + 4;
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	VitaResetEeDeadlineState(cpuRegs.nextEventCycle);
#endif
	EEsCycle = 0;
	EEoCycle = cpuRegs.cycle;

	psxReset();
	pgifInit();

	extern void Deci2Reset();		// lazy, no good header for it yet.
	Deci2Reset();

	AllowParams1 = !VMManager::Internal::IsFastBootInProgress();
	AllowParams2 = !VMManager::Internal::IsFastBootInProgress();
	ParamsRead = false;

	g_eeloadMain = 0;
	g_eeloadExec = 0;
	g_osdsys_str = 0;

	CBreakPoints::ClearSkipFirst();
}

__ri void cpuException(u32 code, u32 bd)
{
	bool errLevel2, checkStatus;
	u32 offset = 0;

    cpuRegs.branch = 0;		// Tells the interpreter that an exception occurred during a branch.
	cpuRegs.CP0.n.Cause = code & 0xffff;

	if(cpuRegs.CP0.n.Status.b.ERL == 0)
	{
		//Error Level 0-1
		errLevel2 = false;
		checkStatus = (cpuRegs.CP0.n.Status.b.BEV == 0); //  for TLB/general exceptions

		if (((code & 0x7C) >= 0x8) && ((code & 0x7C) <= 0xC))
			offset = 0x0; //TLB Refill
		else if ((code & 0x7C) == 0x0)
			offset = 0x200; //Interrupt
		else
			offset = 0x180; // Everything else
	}
	else
	{
		//Error Level 2
		errLevel2 = true;
		checkStatus = (cpuRegs.CP0.n.Status.b.DEV == 0); // for perf/debug exceptions

		Console.Error("*PCSX2* FIX ME: Level 2 cpuException");
		if ((code & 0x38000) <= 0x8000 )
		{
			//Reset / NMI
			cpuRegs.pc = 0xBFC00000;
			Console.Warning("Reset request");
			cpuUpdateOperationMode();
			return;
		}
		else if ((code & 0x38000) == 0x10000)
			offset = 0x80; //Performance Counter
		else if ((code & 0x38000) == 0x18000)
			offset = 0x100; //Debug
		else
			Console.Error("Unknown Level 2 Exception!! Cause %x", code);
	}

	if (cpuRegs.CP0.n.Status.b.EXL == 0)
	{
		cpuRegs.CP0.n.Status.b.EXL = 1;
		if (bd)
		{
			Console.Warning("branch delay!!");
			cpuRegs.CP0.n.EPC = cpuRegs.pc - 4;
			cpuRegs.CP0.n.Cause |= 0x80000000;
		}
		else
		{
			cpuRegs.CP0.n.EPC = cpuRegs.pc;
			cpuRegs.CP0.n.Cause &= ~0x80000000;
		}
	}
	else
	{
		offset = 0x180; //Override the cause
		if (errLevel2) Console.Warning("cpuException: Status.EXL = 1 cause %x", code);
	}

	if (checkStatus)
		cpuRegs.pc = 0x80000000 + offset;
	else
		cpuRegs.pc = 0xBFC00200 + offset;

	cpuUpdateOperationMode();
}

void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	// Avoid too much spamming on the interpreter
	if (Cpu != &intCpu || IsDebugBuild) {
		Console.Error("cpuTlbMiss pc:%x, cycl:%llx, addr: %x, status=%x, code=%x",
				cpuRegs.pc, cpuRegs.cycle, addr, cpuRegs.CP0.n.Status.val, excode);
	}

	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);
}

void cpuTlbMissR(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBL);
}

void cpuTlbMissW(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBS);
}

// sets a branch test to occur some time from an arbitrary starting point.
__fi void cpuSetNextEvent( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(cpuRegs.nextEventCycle - startCycle) > delta )
	{
		cpuRegs.nextEventCycle = startCycle + delta;
	}
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	if (VitaEeInterleaveSchedulerActive() &&
		static_cast<s32>(
			s_vita_ee_non_counter_owner_event_cycle - startCycle) > delta)
	{
		s_vita_ee_non_counter_owner_event_cycle = startCycle + delta;
		VitaRefreshCombinedEeOwner();
	}
#endif
}

#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
void cpuSetNextCounterEvent(u64 startCycle, s32 delta)
{
	if ((int)(cpuRegs.nextEventCycle - startCycle) > delta)
		cpuRegs.nextEventCycle = startCycle + delta;

	if (VitaEeInterleaveSchedulerActive())
	{
		s_vita_ee_counter_owner_event_cycle = startCycle + delta;
		VitaRefreshCombinedEeOwner();
	}
}
#endif

// sets a branch to occur some time from the current cycle
__fi void cpuSetNextEventDelta( s32 delta )
{
	cpuSetNextEvent( cpuRegs.cycle, delta );
}

__fi int cpuGetCycles(int interrupt)
{
	if(interrupt == VU_MTVU_BUSY && (!THREAD_VU1 || INSTANT_VU1))
		return 1;
	else
	{
		const int cycles = (cpuRegs.sCycle[interrupt] + cpuRegs.eCycle[interrupt]) - cpuRegs.cycle;
		return std::max(1, cycles);
	}

}

// tests the cpu cycle against the given start and delta values.
// Returns true if the delta time has passed.
__fi int cpuTestCycle( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(cpuRegs.cycle - startCycle) >= delta;
}

// tells the EE to run the branch test the next time it gets a chance.
__fi void cpuSetEvent()
{
	cpuRegs.nextEventCycle = cpuRegs.cycle;
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	if (VitaEeInterleaveSchedulerActive())
		VitaResetEeDeadlineState(cpuRegs.cycle);
#endif
}

__fi void cpuClearInt( uint i )
{
	pxAssume( i < 32 );
	cpuRegs.interrupt &= ~(1 << i);
	cpuRegs.dmastall &= ~(1 << i);
}

#if defined(VITASX2_CPU_PROFILER)
static __fi VitaPerformanceTelemetry::CpuStage VitaEeEventProfileStage(u8 event)
{
	switch (event)
	{
		case VU_MTVU_BUSY:
		case VIF_VU0_FINISH:
		case VIF_VU1_FINISH:
			return VitaPerformanceTelemetry::CpuStage::VuSync;
		case DMAC_VIF0:
		case DMAC_VIF1:
		case DMAC_GIF:
		case DMAC_MFIFO_VIF:
		case DMAC_MFIFO_GIF:
			return VitaPerformanceTelemetry::CpuStage::VifGif;
		case DMAC_SIF0:
		case DMAC_SIF1:
			return VitaPerformanceTelemetry::CpuStage::Sif;
		case DMAC_FROM_IPU:
		case DMAC_TO_IPU:
			return VitaPerformanceTelemetry::CpuStage::IpuDma;
		case IPU_PROCESS:
			return VitaPerformanceTelemetry::CpuStage::Ipu;
		case DMAC_FROM_SPR:
		case DMAC_TO_SPR:
			return VitaPerformanceTelemetry::CpuStage::Dma;
		default:
			return VitaPerformanceTelemetry::CpuStage::OtherDevice;
	}
}
#endif

static __fi void TESTINT( u8 n, void (*callback)() )
{
	if( !(cpuRegs.interrupt & (1 << n)) ) return;

	if(CHECK_INSTANTDMAHACK || cpuTestCycle( cpuRegs.sCycle[n], cpuRegs.eCycle[n] ) )
	{
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		const Pcsx2Trace::CoreEventId trace_event_id = GetEeSifCoreEventId(n);
		if (trace_event_id != Pcsx2Trace::CoreEventId::None)
		{
			TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Event,
				Pcsx2Trace::CoreEventPhase::DispatchBegin, trace_event_id,
				cpuRegs.sCycle[n] + cpuRegs.eCycle[n]);
		}
#endif
		cpuClearInt( n );
#if defined(VITASX2_CPU_PROFILER)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaEeEventProfileStage(n));
#endif
		callback();
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		if (trace_event_id != Pcsx2Trace::CoreEventId::None)
		{
			TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Event,
				Pcsx2Trace::CoreEventPhase::DispatchEnd, trace_event_id,
				cpuRegs.sCycle[n] + cpuRegs.eCycle[n]);
		}
#endif
	}
	else
		cpuSetNextEvent( cpuRegs.sCycle[n], cpuRegs.eCycle[n] );
}

// [TODO] move this function to Dmac.cpp, and remove most of the DMAC-related headers from
// being included into R5900.cpp.
static __fi bool _cpuTestInterrupts()
{

	if (!dmacRegs.ctrl.DMAE || (psHu8(DMAC_ENABLER+2) & 1))
	{
		//Console.Write("DMAC Disabled or suspended");
		return false;
	}

	eeRunInterruptScan = INT_RUNNING;

	while (eeRunInterruptScan == INT_RUNNING)
	{
		/* These are 'pcsx2 interrupts', they handle asynchronous stuff
		   that depends on the cycle timings */
		TESTINT(VU_MTVU_BUSY, MTVUInterrupt);
		TESTINT(DMAC_VIF1, vif1Interrupt);
		TESTINT(DMAC_GIF, gifInterrupt);
		TESTINT(DMAC_SIF0, EEsif0Interrupt);
		TESTINT(DMAC_SIF1, EEsif1Interrupt);
		// Profile-guided Optimization (sorta)
		// The following ints are rarely called.  Encasing them in a conditional
		// as follows helps speed up most games.

		if (cpuRegs.interrupt & ((1 << DMAC_VIF0) | (1 << DMAC_FROM_IPU) | (1 << DMAC_TO_IPU)
			| (1 << DMAC_FROM_SPR) | (1 << DMAC_TO_SPR) | (1 << DMAC_MFIFO_VIF) | (1 << DMAC_MFIFO_GIF)
			| (1 << VIF_VU0_FINISH) | (1 << VIF_VU1_FINISH) | (1 << IPU_PROCESS)))
		{
			TESTINT(DMAC_VIF0, vif0Interrupt);

			TESTINT(DMAC_FROM_IPU, ipu0Interrupt);
			TESTINT(DMAC_TO_IPU, ipu1Interrupt);
			TESTINT(IPU_PROCESS, ipuCMDProcess);

			TESTINT(DMAC_FROM_SPR, SPRFROMinterrupt);
			TESTINT(DMAC_TO_SPR, SPRTOinterrupt);

			TESTINT(DMAC_MFIFO_VIF, vifMFIFOInterrupt);
			TESTINT(DMAC_MFIFO_GIF, gifMFIFOInterrupt);

			TESTINT(VIF_VU0_FINISH, vif0VUFinish);
			TESTINT(VIF_VU1_FINISH, vif1VUFinish);
		}

		if (eeRunInterruptScan == INT_REQ_LOOP)
			eeRunInterruptScan = INT_RUNNING;
		else
			break;
	}

	eeRunInterruptScan = INT_NOT_RUNNING;

	if ((cpuRegs.interrupt & 0x1FFFF) & ~cpuRegs.dmastall)
		return true;
	else
		return false;
}

static __fi void _cpuTestTIMR()
{
	// fixme: this looks like a hack to make up for the fact that the TIMR
	// doesn't yet have a proper mechanism for setting itself up on a nextEventCycle.
	// A proper fix would schedule the TIMR to trigger at a specific cycle anytime
	// the Count or Compare registers are modified.

	if (!(cpuRegs.CP0.n.Status.val & 0x8000))
		return;

	COP0_UpdateCount();
	if (cpuRegs.CP0.n.Count >= cpuRegs.CP0.n.Compare &&
		cpuRegs.CP0.n.Count < cpuRegs.CP0.n.Compare + 1000)
	{
		Console.WriteLn( Color_Magenta, "timr intr: %x, %x", cpuRegs.CP0.n.Count, cpuRegs.CP0.n.Compare);
		cpuException(0x808000, cpuRegs.branch);
	}
}

static __fi void _cpuTestPERF()
{
	// Perfs are updated when read by games (COP0's MFC0/MTC0 instructions), so we need
	// only update them at semi-regular intervals to keep cpuRegs.cycle from wrapping
	// around twice on us btween updates.  Hence this function is called from the cpu's
	// Counters update.

	COP0_UpdatePCCR();
}

// Checks the COP0.Status for exception enablings.
// Exception handling for certain modes is *not* currently supported, this function filters
// them out.  Exceptions while the exception handler is active (EIE), or exceptions of any
// level other than 0 are ignored here.

static bool cpuIntsEnabled(int Interrupt)
{
	bool IntType = !!(cpuRegs.CP0.n.Status.val & Interrupt); //Choose either INTC or DMAC, depending on what called it

	return IntType && cpuRegs.CP0.n.Status.b.EIE && cpuRegs.CP0.n.Status.b.IE &&
		!cpuRegs.CP0.n.Status.b.EXL && (cpuRegs.CP0.n.Status.b.ERL == 0);
}

#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
static __fi void VitaIopEventTestFromEe()
{
	// PCSX2 owner: R3000A.cpp::iopEventTest(). PSX_INT(), counter writes, and
	// the prior iopEventTest() publish iopNextEventCycle. If every published
	// owner is still in the future, rebuilding the same 384/768-cycle horizon
	// cannot expose new guest state. PSX_INT() publishes the earliest pending
	// callback through psxSetNextBranchDelta(), and iopEventTest() republishes
	// every not-yet-due callback through psxSetNextBranch(). A pending callback
	// therefore does not require dispatch before its published deadline. Keep
	// due/earlier counters and already-visible INTC state on the complete owner.
#if defined(VITASX2_QEMU_VALIDATION)
	if (!g_vita_a32_iop_deadline_gate_validation_enabled)
	{
		iopEventTest();
		return;
	}
#endif
	const bool deadline_due =
		static_cast<s64>(psxRegs.cycle - psxRegs.iopNextEventCycle) >= 0;
	const bool counter_due =
		static_cast<s32>(static_cast<u32>(
			psxRegs.cycle - psxNextStartCounter)) >= psxNextDeltaCounter;
	const bool counter_precedes_published =
		psxNextDeltaCounter <
			static_cast<s32>(
				psxRegs.iopNextEventCycle - psxNextStartCounter);
	const bool intc_visible =
		psxHu32(HW_ICTRL) != 0 &&
		(psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) != 0;
	const bool dispatch =
		deadline_due || counter_due || counter_precedes_published ||
		intc_visible;

#if defined(VITASX2_CPU_PROFILER)
	{
		const VitaPerformanceTelemetry::ScopedCpuStage diagnostics_stage(
			VitaPerformanceTelemetry::CpuStage::Diagnostics);
		bool callback_due = false;
		for (u32 event = IopEvt_SIF2; event <= IopEvt_USB; event++)
		{
			if ((psxRegs.interrupt & (1u << event)) != 0 &&
				static_cast<s64>(
					psxRegs.cycle -
						(psxRegs.sCycle[event] + psxRegs.eCycle[event])) >= 0)
			{
				callback_due = true;
				break;
			}
		}
		VitaPerformanceTelemetry::RecordIopDeadlineGateIfProfiling(
			dispatch, deadline_due, counter_due, counter_precedes_published,
			intc_visible, callback_due);
	}
#endif
	if (dispatch)
		iopEventTest();
}

#if defined(VITASX2_CPU_PROFILER)
static_assert(VitaPerformanceTelemetry::EE_DEADLINE_EVENT_SLOT_COUNT ==
	static_cast<size_t>(VU_MTVU_BUSY) + 1);

static __fi void VitaRecordIpuEpochOpportunityAtEntry(
	const VitaA32EeWaitSchedulerCertificate& wait_certificate,
	u32 wait_pc)
{
	const u32 packed_poll = wait_certificate.mmio_packed_poll;
	const u32 base_guest = packed_poll & 0x1fu;
	const bool from_ipu_wait =
		wait_certificate.origin ==
			VitaA32EeWaitSchedulerOrigin::DmacChcrStrPollLoop &&
		base_guest != 0 &&
		cpuRegs.GPR.r[base_guest].UL[0] == fromIPU_CHCR;

	if (!from_ipu_wait)
	{
		VitaPerformanceTelemetry::RecordIpuEpochOpportunityIfProfiling(
			false, wait_pc, 0, 0);
		return;
	}

	constexpr u32 ipu_events =
		(1u << DMAC_FROM_IPU) |
		(1u << DMAC_TO_IPU) |
		(1u << IPU_PROCESS);
	constexpr u32 tested_events =
		(1u << VU_MTVU_BUSY) |
		(1u << DMAC_VIF1) | (1u << DMAC_GIF) |
		(1u << DMAC_SIF0) | (1u << DMAC_SIF1) |
		(1u << DMAC_VIF0) | ipu_events |
		(1u << DMAC_FROM_SPR) | (1u << DMAC_TO_SPR) |
		(1u << DMAC_MFIFO_VIF) | (1u << DMAC_MFIFO_GIF) |
		(1u << VIF_VU0_FINISH) | (1u << VIF_VU1_FINISH);
	u32 due_events = 0;
	u32 pending = cpuRegs.interrupt & tested_events;
	while (pending != 0)
	{
		const u32 event = static_cast<u32>(std::countr_zero(pending));
		const u32 bit = 1u << event;
		pending &= pending - 1;
		if (CHECK_INSTANTDMAHACK ||
			cpuTestCycle(cpuRegs.sCycle[event], cpuRegs.eCycle[event]))
		{
			due_events |= bit;
		}
	}

	u32 blockers = 0;
	if (!VitaA32IopRetainedWaitCoalescingActive())
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerIopActive;
	if ((due_events & ipu_events) == 0)
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerNoDueIpu;
	if ((due_events & ~ipu_events) != 0)
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerDueNonIpu;
	if (cpuTestCycle(nextStartCounter, nextDeltaCounter))
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerEeCounter;
	// The complete scheduler updates Count lazily immediately before testing
	// Compare. Conservatively treat any enabled timer as an epoch fence until
	// Phase 1 gives it an explicit deadline slot.
	if ((cpuRegs.CP0.n.Status.val & 0x8000u) != 0)
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerCp0Timer;
	if (cpuIntsEnabled(intcInterrupt() | dmacInterrupt()))
		blockers |=
			VitaPerformanceTelemetry::IpuEpochBlockerVisibleException;

	const u32 vu_running = VU0.VI[REG_VPU_STAT].UL;
	if ((vu_running & 1u) != 0 ||
		(!THREAD_VU1 && (vu_running & 0x100u) != 0) ||
		(THREAD_VU1 && vu1Thread.HasPendingChanges()))
	{
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerVu;
	}
	if (!dmacRegs.ctrl.DMAE || (psHu8(DMAC_ENABLER + 2) & 1))
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerDmacSuspended;
	if (CHECK_INSTANTDMAHACK &&
		(cpuRegs.interrupt & 0x1ffffu) != 0)
	{
		blockers |= VitaPerformanceTelemetry::IpuEpochBlockerInstantDma;
	}

	const u32 compact_due_ipu =
		((due_events >> DMAC_FROM_IPU) & 1u) |
		(((due_events >> DMAC_TO_IPU) & 1u) << 1) |
		(((due_events >> IPU_PROCESS) & 1u) << 2);
	VitaPerformanceTelemetry::RecordIpuEpochOpportunityIfProfiling(
		true, wait_pc, compact_due_ipu, blockers);
}

static __fi void VitaRecordEeDeadlineHorizon(
	s32 iop_delta, bool iop_rapid)
{
	// Reconstruct a deadline-owner horizon beside the retained PCSX2
	// scheduler. The real nextEventCycle is seeded with eeWaitCycles on entry,
	// so inspecting it cannot reveal which later owner would permit a longer
	// run. This profiler-only scan is observational and never changes the
	// product deadline.
	s64 owner_delta = iop_delta;
	auto owner = VitaPerformanceTelemetry::EeDeadlineOwner::Iop;
	u32 ee_event_owner = UINT32_MAX;

	const s64 counter_delta = static_cast<s64>(
		nextStartCounter + static_cast<s64>(nextDeltaCounter) -
		cpuRegs.cycle);
	if (counter_delta < owner_delta)
	{
		owner_delta = counter_delta;
		owner = VitaPerformanceTelemetry::EeDeadlineOwner::EeCounter;
	}

	if (dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1))
	{
		constexpr u32 tested_events =
			(1u << VU_MTVU_BUSY) |
			(1u << DMAC_VIF1) | (1u << DMAC_GIF) |
			(1u << DMAC_SIF0) | (1u << DMAC_SIF1) |
			(1u << DMAC_VIF0) |
			(1u << DMAC_FROM_IPU) | (1u << DMAC_TO_IPU) |
			(1u << IPU_PROCESS) |
			(1u << DMAC_FROM_SPR) | (1u << DMAC_TO_SPR) |
			(1u << DMAC_MFIFO_VIF) | (1u << DMAC_MFIFO_GIF) |
			(1u << VIF_VU0_FINISH) | (1u << VIF_VU1_FINISH);
		u32 pending = cpuRegs.interrupt & tested_events;
		while (pending != 0)
		{
			const u32 event = static_cast<u32>(std::countr_zero(pending));
			pending &= pending - 1;
			const s64 event_delta = static_cast<s64>(
				cpuRegs.sCycle[event] +
				static_cast<s64>(cpuRegs.eCycle[event]) -
				cpuRegs.cycle);
			if (event_delta < owner_delta)
			{
				owner_delta = event_delta;
				owner = VitaPerformanceTelemetry::EeDeadlineOwner::EeEvent;
				ee_event_owner = event;
			}
		}
	}

	const bool timer_enabled =
		(cpuRegs.CP0.n.Status.val & 0x8000u) != 0;
	const u32 timer_delta =
		cpuRegs.CP0.n.Compare - cpuRegs.CP0.n.Count;
	VitaPerformanceTelemetry::RecordEeDeadlineHorizonIfProfiling(
		owner_delta, owner, ee_event_owner, EEsCycle, timer_enabled,
		timer_delta, iop_rapid);
}
#endif

#endif

// Shared portion of the branch test, called from both the Interpreter
// and the recompiler.  (moved here to help alleviate redundant code)
#if defined(__arm__)
extern "C" __attribute__((noinline, no_stack_protector,
	target("arm,general-regs-only")))
void VitaCpuEventTestSharedPrivateBody()
#else
__fi void _cpuEventTest_Shared()
#endif
{
#if defined(__arm__)
	// Force a stable first A32 instruction for the verified private entry.
	// Its alternative AAPCS adapter below executes this save normally.
	asm volatile("" ::: "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr");
#endif
#if defined(VITASX2_VITA)
	VitaPerformanceTelemetry::OnEeSchedulerEntry(
		cpuRegs.pc, cpuRegs.cycle, psxRegs.pc, psxRegs.cycle);
	const VitaA32EeWaitSchedulerCertificate* vita_ee_wait_certificate =
		VitaConsumeA32EeWaitSchedulerCertificate();
	const u32 vita_ee_wait_pc = cpuRegs.pc;
#if defined(VITASX2_CPU_PROFILER)
	{
		const VitaPerformanceTelemetry::ScopedCpuStage diagnostics_stage(
			VitaPerformanceTelemetry::CpuStage::Diagnostics);
		VitaRecordIpuEpochOpportunityAtEntry(
			*vita_ee_wait_certificate, vita_ee_wait_pc);
	}
#endif
#endif
	eeEventTestIsActive = true;
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	VitaRefreshEeCounterOwner();
	const bool vita_iop_only_interleave =
		VitaCanRunIopOnlyEeInterleave();
	VitaPerformanceTelemetry::RecordEeSchedulerPathIfProfiling(
		vita_iop_only_interleave,
		VitaA32IopRetainedWaitCoalescingActive());
#if defined(VITASX2_QEMU_VALIDATION)
	if (VitaEeInterleaveSchedulerActive())
	{
		if (vita_iop_only_interleave)
			g_vita_ee_iop_only_scheduler_validation_entries++;
		else
			g_vita_ee_full_scheduler_validation_entries++;
	}
#endif
	if (!vita_iop_only_interleave)
	{
		if (VitaEeInterleaveSchedulerActive())
			VitaBeginFullEeDeadlineCollection();
		else
			cpuRegs.nextEventCycle =
				cpuRegs.cycle + VitaEeInterleaveCycles();
	}
#else
	cpuRegs.nextEventCycle = cpuRegs.cycle + eeWaitCycles;
#endif
	cpuRegs.lastEventCycle = cpuRegs.cycle;
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Scheduler,
		Pcsx2Trace::CoreEventPhase::Enter, Pcsx2Trace::CoreEventId::None,
		cpuRegs.nextEventCycle);
#endif
	// ---- INTC / DMAC (CPU-level Exceptions) -----------------
	// Done first because exceptions raised during event tests need to be postponed a few
	// cycles (fixes Grandia II [PAL], which does a spin loop on a vsync and expects to
	// be able to read the value before the exception handler clears it).

#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	if (!vita_iop_only_interleave)
#endif
	{
#if defined(VITASX2_VITA)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::EeExceptions);
#endif
		uint mask = intcInterrupt() | dmacInterrupt();
		if (cpuIntsEnabled(mask))
			cpuException(mask, cpuRegs.branch);
	}

	// ---- IOP -------------
	// * It's important to run a iopEventTest before calling ExecuteBlock. This
	//   is because the IOP does not always perform branch tests before returning
	//   (during the prev branch) and also so it can act on the state the EE has
	//   given it before executing any code.
	//
	// * The IOP cannot always be run.  If we run IOP code every time through the
	//   cpuEventTest, the IOP generally starts to run way ahead of the EE.

	// It's also important to sync up the IOP before updating the timers, since gates will depend on starting/stopping in the right place!
	EEsCycle += cpuRegs.cycle - EEoCycle;
	EEoCycle = cpuRegs.cycle;

	if (EEsCycle > 0)
		iopEventAction = true;

	if (iopEventAction)
	{
#if defined(VITASX2_VITA)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::IopGenerated);
#endif
		//if( EEsCycle < -450 )
		//	Console.WriteLn( " IOP ahead by: %d cycles", -EEsCycle );

#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Scheduler,
			Pcsx2Trace::CoreEventPhase::Before, Pcsx2Trace::CoreEventId::None,
			cpuRegs.nextEventCycle);
#endif

#if defined(__arm__)
		// PCSX2 owner: x86/iR3000A.cpp::recExecuteBlock() documents the intended
		// direct iopEnterRecompiledCode() scheduler seam. Provider selection
		// installs either the verified private A32 entry or its generic AAPCS
		// adapter, so this hot scheduler path needs no per-call provider test.
		EEsCycle = VitaExecuteA32IopTimesliceFromEeEvent(EEsCycle);
#if defined(VITASX2_QEMU_VALIDATION)
		if (g_vita_a32_iop_private_event_entry_enabled &&
			g_vita_a32_iop_private_event_entry_available && psxCpu == &psxRec)
		{
			g_vita_a32_iop_private_event_entries++;
		}
#endif
#else
		{
			EEsCycle = psxCpu->ExecuteBlock(EEsCycle);
		}
#endif

#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Scheduler,
			Pcsx2Trace::CoreEventPhase::After, Pcsx2Trace::CoreEventId::None,
			cpuRegs.nextEventCycle);
#endif

		iopEventAction = false;
	}

#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	{
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::IopEvent);
		VitaIopEventTestFromEe();
	}
#else
	iopEventTest();
#endif

#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	if (vita_iop_only_interleave)
	{
		if (VitaCanSkipEeOwnersAtInterleave())
		{
			// Recompute the PCSX2 IOP owner after its guest slice and due-owner
			// gate, then retain unrelated EE owners until their exact deadline.
			// The active-IOP cadence remains a bounded IOP-owned seam.
			if (!VitaTryScheduleJointWaitHorizon(
					*vita_ee_wait_certificate, vita_ee_wait_pc))
			{
				VitaScheduleEeAndIopDeadlines(VitaNextIopOwnerDelta());
			}
#if defined(VITASX2_CPU_PROFILER)
			{
				const VitaPerformanceTelemetry::ScopedCpuStage diagnostics_stage(
					VitaPerformanceTelemetry::CpuStage::Diagnostics);
				VitaRecordJointWaitShadowAtDeadline(
					*vita_ee_wait_certificate, vita_ee_wait_pc);
			}
#endif
		}
		else
		{
			// IOP work can make an EE exception or compatibility owner visible.
			// Re-enter the complete scheduler immediately, before another guest
			// EE instruction can execute, rather than continuing after the
			// exception scan which this narrow entry deliberately skipped.
			cpuSetEvent();
		}
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		Pcsx2Trace::RecordPendingMachineCheckpointAtEventTest();
#endif
		eeEventTestIsActive = false;
#if defined(VITASX2_VITA)
		VitaFinishA32EeWaitSchedulerCertificate();
#endif
		VitaPerformanceTelemetry::OnEeSchedulerExit();
#if defined(__arm__)
		ReturnFromPrivateCpuEventTestShared();
#else
		return;
#endif
	}
#endif

	{
#if defined(VITASX2_VITA)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::EeCounters);
#endif
		if (cpuTestCycle(nextStartCounter, nextDeltaCounter))
		{
			rcntUpdate();
			_cpuTestPERF();
		}

		_cpuTestTIMR();
	}

	// ---- Interrupts -------------
	// These are basically just DMAC-related events, which also piggy-back the same bits as
	// the PS2's own DMA channel IRQs and IRQ Masks.

	{
#if defined(VITASX2_VITA)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::EeInterrupts);
#endif
		if (cpuRegs.interrupt)
		{
			// This is a BIOS hack because the coding in the BIOS is terrible but the bug is masked by Data Cache
			// where a DMA buffer is overwritten without waiting for the transfer to end, which causes the fonts to get all messed up
			// so to fix it, we run all the DMA's instantly when in the BIOS.
			// Only use the lower 17 bits of the cpuRegs.interrupt as the upper bits are for VU0/1 sync which can't be done in a tight loop
			if (CHECK_INSTANTDMAHACK && dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1) && (cpuRegs.interrupt & 0x1FFFF))
			{
				while ((cpuRegs.interrupt & 0x1FFFF) && _cpuTestInterrupts())
					;
			}
			else
				_cpuTestInterrupts();
		}
	}

	// ---- VU Sync -------------
	// We're in a EventTest.  All dynarec registers are flushed
	// so there is no need to freeze registers here.
	// PCSX2 owner: VUmicro.cpp::BaseVUmicroCPU::ExecuteBlock(). Preserve its
	// exact running-bit contract, but reject the two inactive virtual calls in
	// the shared scheduler TU. Threaded VU1 must still collect MTVU changes even
	// when VPU_STAT is clear.
	{
#if defined(VITASX2_VITA)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::VuSync);
#endif
		const u32 vu_running = VU0.VI[REG_VPU_STAT].UL;
		if (vu_running & 1)
			CpuVU0->ExecuteBlock();
		if (THREAD_VU1 || (vu_running & 0x100))
			CpuVU1->ExecuteBlock();
	}

	// ---- Schedule Next Event Test --------------
	{
#if defined(VITASX2_VITA)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::Deadline);
#endif
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		const s32 iop_owner_delta = VitaNextIopOwnerDelta();
#else
		const float mutiplier = static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
		const int nextIopEventDelta =
			((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier);
		// 8 or more cycles behind and there's an event scheduled
		if (EEsCycle >= nextIopEventDelta)
		{
			// EE's running way ahead of the IOP still, so we should branch quickly to give the
			// IOP extra timeslices in short order.

			cpuSetNextEventDelta(48);
			//Console.Warning( "EE ahead of the IOP -- Rapid Event!  %d", EEsCycle );
		}
		else
		{
			// Otherwise IOP is caught up/not doing anything so we can wait for the next event.
			cpuSetNextEventDelta(((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier) - EEsCycle);
		}
#endif

#if defined(VITASX2_CPU_PROFILER)
		{
			const VitaPerformanceTelemetry::ScopedCpuStage diagnostics_stage(
				VitaPerformanceTelemetry::CpuStage::Diagnostics);
			VitaRecordEeDeadlineHorizon(
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
				iop_owner_delta,
				iop_owner_delta == 48);
#else
				EEsCycle >= nextIopEventDelta ?
					48 : nextIopEventDelta - EEsCycle,
				EEsCycle >= nextIopEventDelta);
#endif
		}
#endif

		// Apply vsync and other counter nextCycles
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		cpuSetNextCounterEvent(nextStartCounter, nextDeltaCounter);
#else
		cpuSetNextEvent(nextStartCounter, nextDeltaCounter);
#endif
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		if (VitaEeInterleaveSchedulerActive())
		{
			// CPU_INT(), INTC/DMAC tests and counters publish EE owners through
			// cpuSetNextEvent(). The IOP deadline is deliberately kept separate,
			// so an IOP-owned boundary can avoid rebuilding unrelated EE owners.
			if (!VitaTryScheduleJointWaitHorizon(
					*vita_ee_wait_certificate, vita_ee_wait_pc))
			{
				VitaScheduleEeAndIopDeadlines(iop_owner_delta);
			}
#if defined(VITASX2_CPU_PROFILER)
			{
				const VitaPerformanceTelemetry::ScopedCpuStage diagnostics_stage(
					VitaPerformanceTelemetry::CpuStage::Diagnostics);
				VitaRecordJointWaitShadowAtDeadline(
					*vita_ee_wait_certificate, vita_ee_wait_pc);
			}
#endif
		}
		else
		{
			// Focused validation can disable the split calendar while retaining
			// the pre-existing Vita scheduler contract as its matched control.
			cpuSetNextEventDelta(iop_owner_delta);
			if (!VitaCanCoalesceRetainedIopWait())
				cpuSetNextEventDelta(eeWaitCycles);
		}
#endif
	}

#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Scheduler,
		Pcsx2Trace::CoreEventPhase::Exit, Pcsx2Trace::CoreEventId::None,
		cpuRegs.nextEventCycle);
#endif
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	Pcsx2Trace::RecordPendingMachineCheckpointAtEventTest();
#endif
	eeEventTestIsActive = false;
#if defined(VITASX2_VITA)
	VitaFinishA32EeWaitSchedulerCertificate();
	VitaPerformanceTelemetry::OnEeSchedulerExit();
#endif
#if defined(__arm__)
	ReturnFromPrivateCpuEventTestShared();
#endif
}

#if defined(__arm__)
__attribute__((naked, noinline, target("arm"))) void _cpuEventTest_Shared()
{
	asm volatile(
		// Cold AAPCS adapter for the interpreter, diagnostics, and callable EE
		// paths which do not already own a full private frame. Enter the body
		// normally here so a future compiler-prologue change cannot make this
		// fallback depend on the private entry's verified PUSH shape.
		"push {r4-r11, lr}\n"
		"sub sp, sp, #4\n"
		"bl VitaCpuEventTestSharedPrivateBody\n"
		"add sp, sp, #4\n"
		"pop {r4-r11, pc}\n");
}

extern "C" __attribute__((naked, noinline, target("arm")))
void VitaCpuEventTestSharedPrivate()
{
	asm volatile(
		// Match the compiler body's nine-word CFA without transferring the
		// caller-owned register bank. Body+4 skips its verified A32 PUSH.
		"sub sp, sp, #36\n"
		"str lr, [sp, #32]\n"
		"b VitaCpuEventTestSharedPrivateBody + 4\n");
}

bool VitaCpuEventTestSharedPrivateSupported()
{
	extern void VitaCpuEventTestSharedPrivateBodySymbol() __asm__(
		"VitaCpuEventTestSharedPrivateBody");
	constexpr u32 EXPECTED_PUSH_R4_R11_LR = 0xe92d4ff0u;
	return *reinterpret_cast<const u32*>(
		reinterpret_cast<uptr>(&VitaCpuEventTestSharedPrivateBodySymbol)) ==
		EXPECTED_PUSH_R4_R11_LR;
}
#endif

__ri void cpuTestINTCInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x400
	// bit (which is INTC master toggle).
	if (!cpuIntsEnabled(0x400))
		return;

	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestDMACInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x800
	// bit (which is the DMAC master toggle).
	if (!cpuIntsEnabled(0x800))
		return;

	if (((psHu16(0xe012) & psHu16(0xe010)) == 0) &&
		((psHu16(0xe010) & 0x8000) == 0))
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestTIMRInts()
{
	if ((cpuRegs.CP0.n.Status.val & 0x10007) == 0x10001)
	{
		_cpuTestPERF();
		_cpuTestTIMR();
	}
}

__fi void cpuTestHwInts()
{
	cpuTestINTCInts();
	cpuTestDMACInts();
	cpuTestTIMRInts();
}

__fi void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	if (set)
		cpuRegs.dmastall |= 1 << n;
	else
		cpuRegs.dmastall &= ~(1 << n);
}

__fi void CPU_INT( EE_EventType n, s32 ecycle)
{
	// If it's retunning too quick, just rerun the DMA, there's no point in running the EE for < 4 cycles.
	// This causes a huge uplift in performance for ONI FMV's.
	if (ecycle < 4 && !(cpuRegs.dmastall & (1 << n)) && eeRunInterruptScan != INT_NOT_RUNNING)
	{
		eeRunInterruptScan = INT_REQ_LOOP;
		cpuRegs.interrupt |= 1 << n;
		cpuRegs.sCycle[n] = cpuRegs.cycle;
		cpuRegs.eCycle[n] = 0;
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		const Pcsx2Trace::CoreEventId trace_event_id = GetEeSifCoreEventId(n);
		if (trace_event_id != Pcsx2Trace::CoreEventId::None)
		{
			TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Event,
				Pcsx2Trace::CoreEventPhase::Schedule, trace_event_id, cpuRegs.cycle);
		}
#endif
		return;
	}

	// EE events happen 8 cycles in the future instead of whatever was requested.
	// This can be used on games with PATH3 masking issues for example, or when
	// some FMV look bad.
	if (CHECK_EETIMINGHACK && n < VIF_VU0_FINISH)
		ecycle = 8;

	cpuRegs.interrupt |= 1 << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = ecycle;
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	const Pcsx2Trace::CoreEventId trace_event_id = GetEeSifCoreEventId(n);
	if (trace_event_id != Pcsx2Trace::CoreEventId::None)
	{
		TraceEeCoreEvent(Pcsx2Trace::CoreEventKind::Event,
			Pcsx2Trace::CoreEventPhase::Schedule, trace_event_id,
			cpuRegs.sCycle[n] + cpuRegs.eCycle[n]);
	}
#endif

	// Interrupt is happening soon: make sure both EE and IOP are aware.

	if (ecycle <= 28 && psxRegs.iopCycleEE > 0)
	{
		// If running in the IOP, force it to break immediately into the EE.
		// the EE's branch test is due to run.

		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}

	cpuSetNextEventDelta(cpuRegs.eCycle[n]);
}

// Count arguments, save their starting locations, and replace the space separators with null terminators so they're separate strings
int ParseArgumentString(u32 arg_block)
{
	if (!arg_block)
		return 0;

	int argc = 0;
	bool wasSpace = true; // status of last char. scanned
	int args_len = strlen((char *)PSM(arg_block));
	for (int i = 0; i < args_len; i++)
	{
		char curchar = *(char *)PSM(arg_block + i);
		if (curchar == '\0')
			break; // should never reach this

		bool isSpace = (curchar == ' ');
		if (isSpace)
		{
			void* const separator = PSM(arg_block + i);
			memset(separator, 0, 1);
			NotifyEeRamHostWrite(separator, 1);
		}
		else if (wasSpace) // then we're at a new arg
		{
			if (argc < kMaxArgs)
			{
				g_argPtrs[argc] = arg_block + i;
				argc++;
			}
			else
			{
				Console.WriteLn("ParseArgumentString: Discarded additional arguments beyond the maximum of %d.", kMaxArgs);
				break;
			}
		}
		wasSpace = isSpace;
	}
#if DEBUG_LAUNCHARG
	// Check our args block
	Console.WriteLn("ParseArgumentString: Saving these strings:");
	for (int a = 0; a < argc; a++)
		Console.WriteLn("%p -> '%s'.", g_argPtrs[a], (char *)PSM(g_argPtrs[a]));
#endif
	return argc;
}

// Called from recompilers; define is mandatory.
void eeloadHook()
{
	std::string elfname;
	int argc = cpuRegs.GPR.n.a0.SD[0];
	if (argc) // calls to EELOAD *after* the first one during the startup process will come here
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook: EELOAD was called with %d arguments according to $a0 and %d according to vargs block:",
			argc, memRead32(cpuRegs.GPR.n.a1.UD[0] - 4));
		for (int a = 0; a < argc; a++)
			Console.WriteLn("argv[%d]: %p -> %p -> '%s'", a, cpuRegs.GPR.n.a1.UL[0] + (a * 4),
				memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)), (char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		if (argc > 1)
			elfname = (char*)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + 4)); // argv[1] in OSDSYS's invocation "EELOAD <game ELF>"

		// This code fires if the user chooses "full boot". First the Sony Computer Entertainment screen appears. This is the result
		// of an EELOAD call that does not want to accept launch arguments (but we patch it to do so in eeloadHook2() in fast boot
		// mode). Then EELOAD is called with the argument "rom0:PS2LOGO". At this point, we do not need any additional tricks
		// because EELOAD is now ready to accept launch arguments. So in full-boot mode, we simply wait for PS2LOGO to be called,
		// then we add the desired launch arguments. PS2LOGO passes those on to the game itself as it calls EELOAD a third time.
		if (!EmuConfig.CurrentGameArgs.empty() && elfname == "rom0:PS2LOGO")
		{
			const char *argString = EmuConfig.CurrentGameArgs.c_str();
			Console.WriteLn("eeloadHook: Supplying launch argument(s) '%s' to module '%s'...", argString, elfname.c_str());

			// Join all arguments by space characters so they can be processed as one string by ParseArgumentString(), then add the
			// user's launch arguments onto the end
			u32 arg_ptr = 0;
			int arg_len = 0;
			for (int a = 0; a < argc; a++)
			{
				arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4));
				arg_len = strlen((char *)PSM(arg_ptr));
				void* const separator = PSM(arg_ptr + arg_len);
				memset(separator, 0x20, 1);
				NotifyEeRamHostWrite(separator, 1);
			}
			char* const argument_destination =
				static_cast<char*>(PSM(arg_ptr + arg_len + 1));
			strcpy(argument_destination, EmuConfig.CurrentGameArgs.c_str());
			NotifyEeRamHostWrite(argument_destination,
				static_cast<u32>(EmuConfig.CurrentGameArgs.size() + 1));
			u32 first_arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0]);
#if DEBUG_LAUNCHARG
			Console.WriteLn("eeloadHook: arg block is '%s'.", (char *)PSM(first_arg_ptr));
#endif
			argc = ParseArgumentString(first_arg_ptr);

			// Write pointer to next slot in $a1
			for (int a = 0; a < argc; a++)
				memWrite32(cpuRegs.GPR.n.a1.UD[0] + (a * 4), g_argPtrs[a]);
			cpuRegs.GPR.n.a0.SD[0] = argc;
#if DEBUG_LAUNCHARG
			// Check our work
			Console.WriteLn("eeloadHook: New arguments are:");
			for (int a = 0; a < argc; a++)
				Console.WriteLn("argv[%d]: %p -> '%s'", a, memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)),
				(char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		}
		// else it's presumed that the invocation is "EELOAD <game ELF> <<launch args>>", coming from PS2LOGO, and we needn't do
		// anything more
	}
#if DEBUG_LAUNCHARG
	// This code fires in full/fast boot mode when EELOAD is called the first/only time. When EELOAD is not given any arguments,
	// it calls rom0:OSDSYS by default, which displays the Sony Computer Entertainment screen. OSDSYS then calls "EELOAD
	// rom0:PS2LOGO" and we end up above.
	else
		Console.WriteLn("eeloadHook: EELOAD was called with no arguments.");
#endif

	// If "fast boot" was chosen, then on EELOAD's first call we won't yet know what the game's ELF is. Find the name and write it
	// into EELOAD's memory.
	if (VMManager::Internal::IsFastBootInProgress() && elfname.empty())
	{
		const std::string& elf_override = VMManager::Internal::GetELFOverride();
		if (!elf_override.empty())
		{
			elfname = fmt::format("host:{}", elf_override);
		}
		else
		{
			CDVDDiscType disc_type;
			std::string disc_elf;
			cdvdGetDiscInfo(nullptr, &disc_elf, nullptr, nullptr, &disc_type);
			if (disc_type == CDVDDiscType::PS2Disc)
			{
				// only allow fast boot for PS2 games
				elfname = std::move(disc_elf);
			}
			else
			{
				Console.Warning(fmt::format("Not allowing fast boot for non-PS2 ELF {}", disc_elf));
			}
		}

		// When fast-booting, we insert the game's ELF name into EELOAD so that the game is called instead of the default call of
		// "rom0:OSDSYS"; any launch arguments supplied by the user will be inserted into EELOAD later by eeloadHook2()
		if (!elfname.empty())
		{
			// Find and save location of default/fallback call "rom0:OSDSYS"; to be used later by eeloadHook2()
			for (g_osdsys_str = EELOAD_START; g_osdsys_str < EELOAD_START + EELOAD_SIZE; g_osdsys_str += 8) // strings are 64-bit aligned
			{
				if (!strcmp((char*)PSM(g_osdsys_str), "rom0:OSDSYS"))
				{
					// Overwrite OSDSYS with game's ELF name
					char* const destination = static_cast<char*>(PSM(g_osdsys_str));
					strcpy(destination, elfname.c_str());
					NotifyEeRamHostWrite(destination,
						static_cast<u32>(elfname.size() + 1));
					break;
				}
			}
		}
		else
		{
			// Stop fast forwarding if we're doing that for boot.
			VMManager::Internal::DisableFastBoot();
			AllowParams1 = true;
			AllowParams2 = true;
		}
	}

	VMManager::Internal::ELFLoadingOnCPUThread(std::move(elfname));

	if (CHECK_EXTRAMEM)
	{
		// Map extra memory.
		vtlb_VMap(Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);

		// Map RAM mirrors for extra memory.
		vtlb_VMap(0x20000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
		vtlb_VMap(0x30000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
	}
}

// Called from recompilers; define is mandatory.
// Only called if g_SkipBiosHack is true
void eeloadHook2()
{
	if (EmuConfig.CurrentGameArgs.empty())
		return;

	if (!g_osdsys_str)
	{
		Console.WriteLn("eeloadHook2: Called before \"rom0:OSDSYS\" was found by eeloadHook()!");
		return;
	}

	const char *argString = EmuConfig.CurrentGameArgs.c_str();
	Console.WriteLn("eeloadHook2: Supplying launch argument(s) '%s' to ELF '%s'.", argString, (char *)PSM(g_osdsys_str));

	// Add args string after game's ELF name that was written over "rom0:OSDSYS" by eeloadHook(). In between the ELF name and args
	// string we insert a space character so that ParseArgumentString() has one continuous string to process.
	int game_len = strlen((char *)PSM(g_osdsys_str));
	void* const separator = PSM(g_osdsys_str + game_len);
	memset(separator, 0x20, 1);
	NotifyEeRamHostWrite(separator, 1);
	char* const argument_destination =
		static_cast<char*>(PSM(g_osdsys_str + game_len + 1));
	strcpy(argument_destination, EmuConfig.CurrentGameArgs.c_str());
	NotifyEeRamHostWrite(argument_destination,
		static_cast<u32>(EmuConfig.CurrentGameArgs.size() + 1));
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: arg block is '%s'.", (char *)PSM(g_osdsys_str));
#endif
	int argc = ParseArgumentString(g_osdsys_str);

	// Back up 4 bytes from start of args block for every arg + 4 bytes for start of argv pointer block, write pointers
	uptr block_start = g_osdsys_str - (argc * 4);
	for (int a = 0; a < argc; a++)
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook2: Writing address %p to location %p.", g_argPtrs[a], block_start + (a * 4));
#endif
		memWrite32(block_start + (a * 4), g_argPtrs[a]);
	}

	// Save argc and argv as incoming arguments for EELOAD function which calls ExecPS2()
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: Saving %d and %p in $a0 and $a1.", argc, block_start);
#endif
	cpuRegs.GPR.n.a0.SD[0] = argc;
	cpuRegs.GPR.n.a1.UD[0] = block_start;
}

inline bool isBranchOrJump(u32 addr)
{
	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	// Return false for eret & syscall as they are branch type in pcsx2 debugging tools,
	// but shouldn't have delay slot in isBreakpointNeeded/isMemcheckNeeded.
	if ((opcode.flags == (IS_BRANCH | BRANCHTYPE_SYSCALL)) || (opcode.flags == (IS_BRANCH | BRANCHTYPE_ERET)))
		return false;

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int isBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (isBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr+4))
		bpFlags += 2;

	return bpFlags;
}

int isMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (isBranchOrJump(addr))
		addr += 4;

	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
