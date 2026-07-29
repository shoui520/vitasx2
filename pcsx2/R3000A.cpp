// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "Common.h"

#include "SIO/Sio0.h"
#include "Sif.h"
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
#include "DebugTools/CoreEventTrace.h"
#endif
#include "DebugTools/Breakpoints.h"
#include "R5900OpcodeTables.h"
#include "IopCounters.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopDma.h"
#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"
#include "vita/VitaCore.h"
#include "vita/VitaPerformanceTelemetry.h"

using namespace R3000A;

R3000Acpu *psxCpu;

// used for constant propagation
u32 g_psxConstRegs[32];
u32 g_psxHasConstReg, g_psxFlushedConstReg;

// Used to signal to the EE when important actions that need IOP-attention have
// happened (hsyncs, vsyncs, IOP exceptions, etc).  IOP runs code whenever this
// is true, even if it's already running ahead a bit.
bool iopEventAction = false;

static constexpr uint iopWaitCycles = 384; // Keep inline with EE wait cycle max.
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
// A retained unconditional wait has no guest work before a real IOP event.
// Keep every real deadline below, but do not manufacture the intervening
// event test solely to preserve PCSX2's desktop-oriented polling cadence.
static constexpr uint iopRetainedWaitCycles = iopWaitCycles * 2;
#endif

bool iopEventTestIsActive = false;

alignas(16) psxRegisters psxRegs;

#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
static __fi Pcsx2Trace::CoreEventId GetIopSifCoreEventId(IopEventId event)
{
	return event == IopEvt_SIF0 ? Pcsx2Trace::CoreEventId::IopSif0 :
		event == IopEvt_SIF1 ? Pcsx2Trace::CoreEventId::IopSif1 :
		Pcsx2Trace::CoreEventId::None;
}

static __fi void TraceIopCoreEvent(Pcsx2Trace::CoreEventKind kind,
	Pcsx2Trace::CoreEventPhase phase, Pcsx2Trace::CoreEventId event_id, u64 target_cycle)
{
	Pcsx2Trace::RecordCoreEvent(kind, phase, Pcsx2Trace::CoreEventDomain::Iop,
		event_id, target_cycle,
		cpuRegs.interrupt, cpuRegs.dmastall, psxRegs.interrupt,
		cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, psxRegs.CP0.n.Status,
		psxHu32(HW_ISTAT), psxHu32(HW_IMASK), psxHu32(HW_ICTRL),
		cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.BadVAddr);
}
#endif

void psxReset()
{
	std::memset(&psxRegs, 0, sizeof(psxRegs));

	psxRegs.pc = 0xbfc00000; // Start in bootstrap
	VitaNotifyIopPcDiscontinuity();
	psxRegs.CP0.n.Status = 0x00400000; // BEV = 1
	psxRegs.CP0.n.PRid   = 0x0000001f; // PRevID = Revision ID, same as the IOP R3000A

	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = -1;
	psxRegs.iopCycleEECarry = 0;
	psxRegs.iopNextEventCycle = psxRegs.cycle + 4;

	psxHwReset();
	PSXCLK = 36864000;
	ioman::reset();
	psxBiosReset();
}

void psxNotifyClockModeChange()
{
	// PCSX2's x86 iPsxAddEECycles() specializes HW_ICFG while compiling,
	// so changing the IOP clock mode retires translations carrying the old
	// formula. Interpreter Reset() is intentionally a no-op here.
	if (psxCpu)
		psxCpu->Reset();
}

void psxShutdown() {
	//psxCpu->Shutdown();
}

void psxException(u32 code, u32 bd)
{
//	PSXCPU_LOG("psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));
	//Console.WriteLn("!! psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));
	// Set the Cause
	psxRegs.CP0.n.Cause &= ~0x7f;
	psxRegs.CP0.n.Cause |= code;

	// Set the EPC & PC
	if (bd)
	{
		PSXCPU_LOG("bd set");
		psxRegs.CP0.n.Cause|= 0x80000000;
		psxRegs.CP0.n.EPC = (psxRegs.pc - 4);
	}
	else
		psxRegs.CP0.n.EPC = (psxRegs.pc);

	if (psxRegs.CP0.n.Status & 0x400000)
		psxRegs.pc = 0xbfc00180;
	else
		psxRegs.pc = 0x80000080;
	VitaNotifyIopPcDiscontinuity();

	// Set the Status
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status &~0x3f) |
						  ((psxRegs.CP0.n.Status & 0xf) << 2);

	/*if ((((PSXMu32(psxRegs.CP0.n.EPC) >> 24) & 0xfe) == 0x4a)) {
		// "hokuto no ken" / "Crash Bandicot 2" ... fix
		PSXMu32(psxRegs.CP0.n.EPC)&= ~0x02000000;
	}*/

	/*if (psxRegs.CP0.n.Cause == 0x400 && (!(psxHu32(0x1450) & 0x8))) {
		hwIntcIrq(INTC_SBUS);
	}*/
}

__fi void psxSetNextBranch( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(psxRegs.iopNextEventCycle - startCycle) > delta )
		psxRegs.iopNextEventCycle = startCycle + delta;
}

__fi void psxSetNextBranchDelta( s32 delta )
{
	psxSetNextBranch( psxRegs.cycle, delta );
}

__fi int psxTestCycle( u64 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(psxRegs.cycle - startCycle) >= delta;
}

__fi int psxRemainingCycles(IopEventId n)
{
	if (psxRegs.interrupt & (1 << n))
		return ((psxRegs.cycle - psxRegs.sCycle[n]) + psxRegs.eCycle[n]);
	else
		return 0;
}

__fi void PSX_INT( IopEventId n, s32 ecycle )
{
	// 19 is CDVD read int, it's supposed to be high.
	//if (ecycle > 8192 && n != 19)
	//	DevCon.Warning( "IOP cycles high: %d, n %d", ecycle, n );

	psxRegs.interrupt |= 1 << n;

	psxRegs.sCycle[n] = psxRegs.cycle;
	psxRegs.eCycle[n] = ecycle;
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	const Pcsx2Trace::CoreEventId trace_event_id = GetIopSifCoreEventId(n);
	if (trace_event_id != Pcsx2Trace::CoreEventId::None)
	{
		TraceIopCoreEvent(Pcsx2Trace::CoreEventKind::Event,
			Pcsx2Trace::CoreEventPhase::Schedule, trace_event_id,
			psxRegs.sCycle[n] + psxRegs.eCycle[n]);
	}
#endif

	psxSetNextBranchDelta(ecycle);
	const float mutiplier = static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
	const s32 iopDelta = (psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier;

	if (psxRegs.iopCycleEE < iopDelta)
	{
		// The EE called this int, so inform it to branch as needed:
		
		cpuSetNextEventDelta(iopDelta - psxRegs.iopCycleEE);
	}
}

static __fi void IopTestEvent( IopEventId n, void (*callback)() )
{
	if( !(psxRegs.interrupt & (1 << n)) ) return;

	if( psxTestCycle( psxRegs.sCycle[n], psxRegs.eCycle[n] ) )
	{
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		const Pcsx2Trace::CoreEventId trace_event_id = GetIopSifCoreEventId(n);
		if (trace_event_id != Pcsx2Trace::CoreEventId::None)
		{
			TraceIopCoreEvent(Pcsx2Trace::CoreEventKind::Event,
				Pcsx2Trace::CoreEventPhase::DispatchBegin, trace_event_id,
				psxRegs.sCycle[n] + psxRegs.eCycle[n]);
		}
#endif
		psxRegs.interrupt &= ~(1 << n);
#if defined(VITASX2_CPU_PROFILER)
		const auto profile_owner = [n]() {
			switch (n)
			{
				case IopEvt_SIF0:
				case IopEvt_SIF1:
				case IopEvt_SIF2:
					return VitaPerformanceTelemetry::CpuStage::Sif;
				case IopEvt_CdvdSectorReady:
				case IopEvt_CdvdRead:
				case IopEvt_Cdvd:
				case IopEvt_Cdrom:
				case IopEvt_CdromRead:
					return VitaPerformanceTelemetry::CpuStage::Cdvd;
				case IopEvt_Dma11:
				case IopEvt_Dma12:
					return VitaPerformanceTelemetry::CpuStage::Dma;
				case IopEvt_DEV9:
					return VitaPerformanceTelemetry::CpuStage::Dev9;
				case IopEvt_USB:
					return VitaPerformanceTelemetry::CpuStage::Usb;
				default:
					return VitaPerformanceTelemetry::CpuStage::OtherDevice;
			}
		}();
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			profile_owner);
#endif
		callback();
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
		if (trace_event_id != Pcsx2Trace::CoreEventId::None)
		{
			TraceIopCoreEvent(Pcsx2Trace::CoreEventKind::Event,
				Pcsx2Trace::CoreEventPhase::DispatchEnd, trace_event_id,
				psxRegs.sCycle[n] + psxRegs.eCycle[n]);
		}
#endif
	}
	else
		psxSetNextBranch( psxRegs.sCycle[n], psxRegs.eCycle[n] );
}

static __fi void Sio0TestEvent(IopEventId n)
{
	if (!(psxRegs.interrupt & (1 << n)))
	{
		return;
	}

	if (psxTestCycle(psxRegs.sCycle[n], psxRegs.eCycle[n]))
	{
		psxRegs.interrupt &= ~(1 << n);
#if defined(VITASX2_CPU_PROFILER)
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::OtherDevice);
#endif
		g_Sio0.Interrupt(Sio0Interrupt::TEST_EVENT);
	}
	else
	{
		psxSetNextBranch(psxRegs.sCycle[n], psxRegs.eCycle[n]);
	}
}

static __fi void _psxTestInterrupts()
{
	IopTestEvent(IopEvt_SIF0,		sif0Interrupt);	// SIF0
	IopTestEvent(IopEvt_SIF1,		sif1Interrupt);	// SIF1
	IopTestEvent(IopEvt_SIF2,		sif2Interrupt);	// SIF2
	Sio0TestEvent(IopEvt_SIO);
	IopTestEvent(IopEvt_CdvdSectorReady, cdvdSectorReady);
	IopTestEvent(IopEvt_CdvdRead,	cdvdReadInterrupt);

	// Profile-guided Optimization (sorta)
	// The following ints are rarely called.  Encasing them in a conditional
	// as follows helps speed up most games.

	if( psxRegs.interrupt & ((1 << IopEvt_Cdvd) | (1 << IopEvt_Dma11) | (1 << IopEvt_Dma12)
		| (1 << IopEvt_Cdrom) | (1 << IopEvt_CdromRead) | (1 << IopEvt_DEV9) | (1 << IopEvt_USB)))
	{
		IopTestEvent(IopEvt_Cdvd,		cdvdActionInterrupt);
		IopTestEvent(IopEvt_Dma11,		psxDMA11Interrupt);	// SIO2
		IopTestEvent(IopEvt_Dma12,		psxDMA12Interrupt);	// SIO2
		IopTestEvent(IopEvt_Cdrom,		cdrInterrupt);
		IopTestEvent(IopEvt_CdromRead,	cdrReadInterrupt);
		IopTestEvent(IopEvt_DEV9,		dev9Interrupt);
		IopTestEvent(IopEvt_USB,		usbInterrupt);
	}
}

__ri void iopEventTest()
{
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	psxRegs.iopNextEventCycle = psxRegs.cycle +
		(VitaA32IopRetainedWaitCoalescingActive() ?
				iopRetainedWaitCycles :
				iopWaitCycles);
#else
	psxRegs.iopNextEventCycle = psxRegs.cycle + iopWaitCycles;
#endif
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	TraceIopCoreEvent(Pcsx2Trace::CoreEventKind::Scheduler,
		Pcsx2Trace::CoreEventPhase::Enter, Pcsx2Trace::CoreEventId::None,
		psxRegs.iopNextEventCycle);
#endif

	if (psxTestCycle(psxNextStartCounter, psxNextDeltaCounter))
	{
		{
			const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
				VitaPerformanceTelemetry::CpuStage::IopCounters);
			psxRcntUpdate();
		}
		iopEventAction = true;
	}
	else
	{
		// start the next branch at the next counter event by default
		// the interrupt code below will assign nearer branches if needed.
		if (psxNextDeltaCounter < static_cast<s32>(psxRegs.iopNextEventCycle - psxNextStartCounter))
			psxRegs.iopNextEventCycle = psxNextStartCounter + psxNextDeltaCounter;
	}

	{
		const VitaPerformanceTelemetry::ScopedCpuStage profile_stage(
			VitaPerformanceTelemetry::CpuStage::IopInterrupts);
		if (psxRegs.interrupt)
		{
			iopEventTestIsActive = true;
			_psxTestInterrupts();
			iopEventTestIsActive = false;
		}

		if ((psxHu32(HW_ICTRL) != 0) &&
			((psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) != 0))
		{
			if ((psxRegs.CP0.n.Status & 0xFE01) >= 0x401)
			{
				PSXCPU_LOG("Interrupt: %x  %x", psxHu32(HW_ISTAT), psxHu32(HW_IMASK));
				psxException(0, 0);
				iopEventAction = true;
			}
		}
	}
#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	// An event above may have changed PC or invalidated the retained block.
	// Never let its extended arbitrary seam escape after ownership is lost.
	const u64 ordinary_wait_deadline = psxRegs.cycle + iopWaitCycles;
	if (!VitaA32IopRetainedWaitCoalescingActive() &&
		psxRegs.iopNextEventCycle > ordinary_wait_deadline)
	{
		psxRegs.iopNextEventCycle = ordinary_wait_deadline;
	}
#endif
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	TraceIopCoreEvent(Pcsx2Trace::CoreEventKind::Scheduler,
		Pcsx2Trace::CoreEventPhase::Exit, Pcsx2Trace::CoreEventId::None,
		psxRegs.iopNextEventCycle);
#endif
}

#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
u64 VitaGetIopExternalEventCycle()
{
	// Reconstruct the same semantic owners consumed by iopEventTest(), but
	// deliberately omit its fixed desktop polling seed. A joint EE/IOP wait
	// certificate may use this calendar only while both processors are proven
	// unable to observe state before one of these owners becomes due.
	u64 deadline = ~static_cast<u64>(0);
	if (psxNextDeltaCounter <= 0)
	{
		deadline = psxRegs.cycle;
	}
	else
	{
		deadline = psxNextStartCounter +
			static_cast<u32>(psxNextDeltaCounter);
	}

	constexpr u32 IOP_EVENT_COUNT =
		static_cast<u32>(IopEvt_USB) + 1;
	u32 pending = psxRegs.interrupt &
		((1u << IOP_EVENT_COUNT) - 1u);
	while (pending != 0)
	{
		const u32 event = static_cast<u32>(std::countr_zero(pending));
		pending &= pending - 1;
		const s64 event_delta = static_cast<s64>(
			psxRegs.sCycle[event] +
			static_cast<s64>(psxRegs.eCycle[event]) -
			psxRegs.cycle);
		const u64 event_deadline = event_delta <= 0 ?
			psxRegs.cycle :
			psxRegs.cycle + static_cast<u64>(event_delta);
		if (event_deadline < deadline)
			deadline = event_deadline;
	}

	// A visible interrupt is already an IOP execution owner, regardless of
	// whether the current CP0 mask will turn it into an exception.
	if (psxHu32(HW_ICTRL) != 0 &&
		(psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) != 0)
	{
		deadline = psxRegs.cycle;
	}
	return deadline;
}
#endif

void iopTestIntc()
{
	if( psxHu32(HW_ICTRL) == 0 ) return;
	if( (psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) == 0 ) return;

	if( !eeEventTestIsActive )
	{
		// An iop exception has occurred while the EE is running code.
		// Inform the EE to branch so the IOP can handle it promptly:

		cpuSetNextEventDelta( 16 );
		iopEventAction = true;
		//Console.Error( "** IOP Needs an EE EventText, kthx **  %d", iopCycleEE );

		// Note: No need to set the iop's branch delta here, since the EE
		// will run an IOP branch test regardless.
	}
	else if ( !iopEventTestIsActive )
		psxSetNextBranchDelta( 2 );
}

inline bool psxIsBranchOrJump(u32 addr)
{
	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int psxIsBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (psxIsBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr + 4))
		bpFlags += 2;

	return bpFlags;
}

int psxIsMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (psxIsBranchOrJump(addr))
		addr += 4;

	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
