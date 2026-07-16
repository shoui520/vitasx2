// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MTVU.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/GsTrace.h"
#include "DebugTools/IpuTrace.h"
#include "DebugTools/Spu2Trace.h"
#include "DebugTools/VuTrace.h"
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
#include "DebugTools/CoreEventTrace.h"
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#if defined(VITASX2_QEMU_VALIDATION)
#include "DebugTools/EeTrace.h"
#endif
#include "Hw.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopMem.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SaveState.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "vita/VitaCore.h"
#include "vita/VitaEeBlockCompiler.h"
#include "vita/VitaEeExecutor.h"
#include "vita/VitaIopBlockCompiler.h"
#include "vita/VitaVuBlockCompiler.h"
#include "vtlb.h"

#include "common/Assertions.h"
#include "common/Console.h"

#if defined(VITASX2_QEMU_VALIDATION)
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

static VitaEePreInstructionTraceCallback s_ee_pre_instruction_trace_callback = nullptr;
static VitaEePreInstructionTraceWindowSkipCallback s_ee_pre_instruction_trace_window_skip_callback = nullptr;
static VitaIopPreInstructionTraceCallback s_iop_pre_instruction_trace_callback = nullptr;
static VitaEE::BlockExecutor s_ee_a32_executor;
static VitaIOP::BlockExecutor s_iop_a32_executor{true};
#if defined(__arm__)
static uptr s_iop_wait_resume_event_context = 0;
static uptr s_iop_wait_resume_event_target = 0;
static uptr s_iop_scheduler_resume_event_context = 0;
bool g_vita_a32_iop_private_event_entry_available =
	VitaIOP::VitaIopA32PrivateTimesliceEntrySupported();
bool g_vita_a32_iop_private_wait_resume_entry_available =
	VitaIOP::VitaIopA32PrivateWaitResumeEntrySupported();
bool g_vita_a32_iop_private_scheduler_resume_entry_available =
	VitaIOP::VitaIopA32PrivateSchedulerResumeEntrySupported();
bool g_vita_a32_iop_private_scheduler_prediction_entry_available =
	VitaIOP::VitaIopA32PrivateSchedulerPredictionEntrySupported();
bool g_vita_a32_iop_private_scheduler_dispatch_cache_entry_available =
	VitaIOP::VitaIopA32PrivateSchedulerDispatchCacheEntrySupported();
extern "C" s32 VitaIopA32ExecuteProviderTimesliceAapcs(void*, s32 ee_cycles)
{
	return psxCpu->ExecuteBlock(ee_cycles);
}
VitaA32IopEventEntry g_vita_a32_iop_event_entry = {
	0, reinterpret_cast<uptr>(&VitaIopA32ExecuteProviderTimesliceAapcs)};
#else
bool g_vita_a32_iop_private_event_entry_available = false;
bool g_vita_a32_iop_private_wait_resume_entry_available = false;
bool g_vita_a32_iop_private_scheduler_resume_entry_available = false;
bool g_vita_a32_iop_private_scheduler_prediction_entry_available = false;
bool g_vita_a32_iop_private_scheduler_dispatch_cache_entry_available = false;
#endif
static VitaA32EeProviderStats s_ee_a32_stats;
static VitaA32IopProviderStats s_iop_a32_stats;
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
static VitaA32EeProviderStats s_ee_a32_session_fallback_stats;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
struct IopDispatchProfileEntry
{
	u32 opcode = 0;
	u32 instruction_count = 0;
	u64 dispatches = 0;
	u32 opcodes[16]{};
};
static std::unordered_map<u32, IopDispatchProfileEntry> s_iop_a32_dispatch_profile;
static std::unordered_map<u64, u64> s_iop_a32_dispatch_edge_profile;
static bool s_iop_a32_compact_provider_dispatch_enabled = true;
static bool s_iop_a32_runtime_stats_enabled = true;
static bool s_iop_a32_private_dispatcher_enabled = true;
bool g_vita_a32_iop_private_event_entry_enabled = true;
bool g_vita_a32_iop_wait_resume_event_entry_enabled = true;
bool g_vita_a32_iop_scheduler_resume_event_entry_enabled = true;
bool g_vita_a32_iop_scheduler_prediction_event_entry_enabled = true;
bool g_vita_a32_iop_scheduler_dispatch_cache_event_entry_enabled = true;
u64 g_vita_a32_iop_private_event_entries = 0;
static u64 s_iop_a32_compact_provider_dispatch_entries = 0;
static u64 s_iop_a32_compact_provider_cache_hit_entries = 0;
static u64 s_ee_a32_persistent_boundary_limit = 0;
static u64 s_ee_a32_persistent_boundaries = 0;
static bool s_ee_a32_persistent_boundary_hit_limit = false;
static bool s_ee_a32_link_rejection_profile_enabled = false;
#endif
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
static VitaA32EeTraceLimitStopCondition s_ee_a32_trace_limit_stop_condition =
	VitaA32EeTraceLimitStopCondition::None;
#endif
static bool s_ee_a32_exit_execution = false;
static bool s_ee_a32_cache_reset_requested = false;
static bool s_ee_a32_running_compiled_block = false;
static bool s_ee_a32_elf_booted = false;
static u64 s_ee_a32_pre_elf_boundaries = 0;
static bool s_ee_a32_direct_linking_enabled = false;
static bool s_ee_a32_persistent_dispatch_enabled = false;
static bool s_ee_a32_in_frame_event_resume_enabled = false;
static VitaA32EeTraceMode s_ee_a32_trace_mode = VitaA32EeTraceMode::InstructionWindow;
static bool s_ee_provider_trace_suppressed = false;
static bool s_ee_a32_prerecording_window = false;

void VitaNotifyIopPcDiscontinuity()
{
	s_iop_a32_executor.NotifyPcDiscontinuity();
}

u32 VitaNotifyA32EeRamWrite(const void* host_address, u32 size)
{
	// PCSX2 owner: vtlb.cpp::mmap_ClearCpuBlock() converts a faulting
	// eeMem->Main host page back to its physical RAM identity before calling
	// R5900::Dynarec::OpcodeImpl::recClear(). Vita cannot rely on host page
	// protection, so direct C++ writers publish that same backing identity here.
	if (!eeMem || !host_address || size == 0)
		return 0;

	const uptr ram_start = reinterpret_cast<uptr>(eeMem->Main);
	const uptr write_start = reinterpret_cast<uptr>(host_address);
	if (write_start < ram_start)
		return 0;

	const uptr backing_start = write_start - ram_start;
	if (backing_start >= Ps2MemSize::MainRam)
		return 0;

	const uptr remaining = Ps2MemSize::MainRam - backing_start;
	const u32 bounded_size = static_cast<u32>(size < remaining ? size : remaining);
	const u32 invalidated = s_ee_a32_executor.InvalidateRamSourceRange(
		static_cast<u32>(backing_start), bounded_size);
	s_ee_a32_stats.invalidated_blocks += invalidated;
	return invalidated;
}

#if defined(__arm__)
static void UpdateIopEventEntry()
{
#if defined(VITASX2_QEMU_VALIDATION)
	const bool private_enabled = g_vita_a32_iop_private_event_entry_enabled;
#else
	constexpr bool private_enabled = true;
#endif
	if (psxCpu == &psxRec && private_enabled &&
		g_vita_a32_iop_private_event_entry_available)
	{
		const bool wait_resume = s_iop_wait_resume_event_context != 0 &&
			s_iop_wait_resume_event_target != 0 &&
			g_vita_a32_iop_private_wait_resume_entry_available
#if defined(VITASX2_QEMU_VALIDATION)
			&& g_vita_a32_iop_wait_resume_event_entry_enabled
#endif
			;
		const bool scheduler_resume = !wait_resume &&
			s_iop_scheduler_resume_event_context != 0 &&
			g_vita_a32_iop_private_scheduler_resume_entry_available
#if defined(VITASX2_QEMU_VALIDATION)
			&& g_vita_a32_iop_scheduler_resume_event_entry_enabled
#endif
			;
		bool scheduler_prediction = scheduler_resume &&
			g_vita_a32_iop_private_scheduler_prediction_entry_available;
		bool scheduler_dispatch_cache = scheduler_prediction &&
			g_vita_a32_iop_private_scheduler_dispatch_cache_entry_available;
#if defined(VITASX2_QEMU_VALIDATION)
		scheduler_prediction = scheduler_prediction &&
			g_vita_a32_iop_scheduler_prediction_event_entry_enabled;
		scheduler_dispatch_cache = scheduler_dispatch_cache &&
			g_vita_a32_iop_scheduler_dispatch_cache_event_entry_enabled;
#endif
		g_vita_a32_iop_event_entry.context = wait_resume ?
			s_iop_wait_resume_event_context :
			(scheduler_resume ? s_iop_scheduler_resume_event_context :
				reinterpret_cast<uptr>(&s_iop_a32_executor));
		g_vita_a32_iop_event_entry.target = wait_resume ?
			s_iop_wait_resume_event_target :
			(scheduler_resume ? reinterpret_cast<uptr>(
				(scheduler_prediction ?
					(scheduler_dispatch_cache ?
						&VitaIopA32ExecuteProviderSchedulerDispatchCachedResumePrivate :
						&VitaIopA32ExecuteProviderSchedulerPredictedResumePrivate) :
					&VitaIopA32ExecuteProviderSchedulerDirectResumePrivate)) :
				reinterpret_cast<uptr>(&VitaIopA32ExecuteProviderTimeslicePrivate));
	}
	else
	{
		g_vita_a32_iop_event_entry.context = 0;
		g_vita_a32_iop_event_entry.target =
			reinterpret_cast<uptr>(&VitaIopA32ExecuteProviderTimesliceAapcs);
	}
}

void VitaSetA32IopWaitResumeEventEntry(uptr context, uptr target)
{
	s_iop_wait_resume_event_context = context;
	s_iop_wait_resume_event_target = target;
	UpdateIopEventEntry();
}

void VitaSetA32IopSchedulerDirectEventContext(uptr context)
{
	s_iop_scheduler_resume_event_context = context;
}
#endif

const char* VitaA32EeFallbackReasonName(VitaA32EeFallbackReason reason)
{
	switch (reason)
	{
		case VitaA32EeFallbackReason::None:
			return "none";
		case VitaA32EeFallbackReason::ScanUnsupportedOpcode:
			return "scan_unsupported";
		case VitaA32EeFallbackReason::ScanBoundary:
			return "scan_boundary";
		case VitaA32EeFallbackReason::ExactTraceBranchLikely:
			return "exact_trace_branch_likely";
		case VitaA32EeFallbackReason::ExecuteFailed:
			return "execute_failed";
		case VitaA32EeFallbackReason::InterpreterPath:
			return "interpreter_path";
		default:
			return "unknown";
	}
}

void VitaSetEePreInstructionTraceCallback(VitaEePreInstructionTraceCallback callback)
{
	s_ee_pre_instruction_trace_callback = callback;
}

bool VitaIsEePreInstructionTraceEnabled()
{
	return s_ee_pre_instruction_trace_callback != nullptr;
}

void VitaSetEePreInstructionTraceWindowSkipCallback(VitaEePreInstructionTraceWindowSkipCallback callback)
{
	s_ee_pre_instruction_trace_window_skip_callback = callback;
}

void VitaRequestA32EeCacheReset()
{
	s_ee_a32_executor.SuspendGeneratedLookupUntilReset();
	s_ee_a32_cache_reset_requested = true;
}

bool VitaRecordEePreInstruction(u32 pc, u32 opcode)
{
	if (s_ee_provider_trace_suppressed)
		return false;

	const VitaEePreInstructionTraceCallback callback = s_ee_pre_instruction_trace_callback;
	const bool stop_for_ee_trace = callback ? callback(pc, opcode) : false;
	if (stop_for_ee_trace)
		return true;
	if (s_ee_a32_prerecording_window || s_ee_a32_trace_mode == VitaA32EeTraceMode::BlockBoundaryState)
		return false;

	if (Pcsx2Trace::DidIpuTraceHitLimit())
		return true;
	if (Pcsx2Trace::RecordVuPreEeInstruction(pc))
		return true;
	if (Pcsx2Trace::DidSpu2TraceHitLimit())
		return true;

	return Pcsx2Trace::RecordGsPreEeInstruction(pc);
}

void VitaSetA32EeTraceMode(VitaA32EeTraceMode mode)
{
	s_ee_a32_trace_mode = mode;
}

static bool s_ee_exact_trace_streams = false;

void VitaSetEeExactTraceStreams(bool enabled)
{
	s_ee_exact_trace_streams = enabled;
}

void VitaSetIopPreInstructionTraceCallback(VitaIopPreInstructionTraceCallback callback)
{
	const bool old_enabled = (s_iop_pre_instruction_trace_callback != nullptr);
	const bool new_enabled = (callback != nullptr);
	s_iop_pre_instruction_trace_callback = callback;
	if (old_enabled != new_enabled)
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxBranchTest(). Vita's A32 IOP block
		// tail now charges iopCycleEE before direct links, so trace/oracle mode
		// can keep the same linked-chain shape while still honoring timeslices.
		s_iop_a32_executor.SetDirectLinkingEnabled(true);
		s_iop_a32_executor.Reset();
	}
}

bool VitaIsIopPreInstructionTraceEnabled()
{
	return s_iop_pre_instruction_trace_callback != nullptr;
}

bool VitaRecordIopPreInstruction(u32 pc, u32 opcode)
{
	const VitaIopPreInstructionTraceCallback callback = s_iop_pre_instruction_trace_callback;
	return callback ? callback(pc, opcode) : false;
}

static void recRecordInterpreterFallbackInStats(VitaA32EeProviderStats& stats,
	u32 pc, u32 opcode, VitaA32EeFallbackReason reason)
{
	if (stats.interpreter_steps == 0)
	{
		stats.first_interpreter_pc = pc;
		stats.first_interpreter_opcode = opcode;
		stats.first_interpreter_reason = static_cast<u32>(reason);
	}

	stats.last_interpreter_pc = pc;
	stats.last_interpreter_opcode = opcode;
	stats.last_interpreter_reason = static_cast<u32>(reason);
	stats.interpreter_steps++;

	switch (reason)
	{
		case VitaA32EeFallbackReason::ScanUnsupportedOpcode:
			stats.scan_unsupported_fallbacks++;
			break;
		case VitaA32EeFallbackReason::ScanBoundary:
			stats.scan_boundary_fallbacks++;
			break;
		case VitaA32EeFallbackReason::ExactTraceBranchLikely:
			stats.exact_trace_branch_likely_fallbacks++;
			break;
		case VitaA32EeFallbackReason::ExecuteFailed:
			stats.execute_failed_fallbacks++;
			break;
		case VitaA32EeFallbackReason::InterpreterPath:
			stats.interpreter_path_fallbacks++;
			break;
		case VitaA32EeFallbackReason::None:
			break;
	}
}

static void recRecordInterpreterFallback(u32 pc, u32 opcode,
	VitaA32EeFallbackReason reason)
{
	recRecordInterpreterFallbackInStats(s_ee_a32_stats, pc, opcode, reason);
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	recRecordInterpreterFallbackInStats(
		s_ee_a32_session_fallback_stats, pc, opcode, reason);
#endif
}

static VitaA32EeFallbackReason recFallbackReasonForScanStop(VitaEE::BlockScanStop stop)
{
	return stop == VitaEE::BlockScanStop::UnsupportedOpcode ?
		VitaA32EeFallbackReason::ScanUnsupportedOpcode :
		VitaA32EeFallbackReason::ScanBoundary;
}

static void recInterpreterStepWithoutProviderTrace()
{
	const VitaEePreInstructionTraceCallback callback = s_ee_pre_instruction_trace_callback;
	s_ee_pre_instruction_trace_callback = nullptr;
	s_ee_provider_trace_suppressed = true;
	intCpu.Step();
	s_ee_provider_trace_suppressed = false;
	s_ee_pre_instruction_trace_callback = callback;
	VitaEE::RefreshRawGpr0KnownZero();
}

static void recTrimIncompleteBranchTail(u32 start_pc, u32* instruction_count)
{
	if (!instruction_count || *instruction_count == 0)
		return;

	const u32 tail_index = *instruction_count - 1;
	const u32 tail_pc = start_pc + tail_index * 4;
	const u32 tail_op = memRead32(tail_pc);
	if (!VitaEE::BlockCompiler::IsSupportedBranchOpcode(tail_op))
		return;

	// PCSX2 owners: Interpreter.cpp::_doBranch_shared() and
	// x86/ix86-32/iR5900.cpp::recRecompile() treat a branch and its delay slot
	// as one compiled execution unit. When the trace limit stops after recording
	// a delay slot, the executable prefix contains the branch but not the delay
	// slot. Trim that branch so the A32 compiler never sees an unpaired tail.
	if (tail_index != 0 &&
		VitaEE::BlockCompiler::IsSupportedBranchOpcode(memRead32(tail_pc - 4)))
	{
		return;
	}

	(*instruction_count)--;
}

static bool recRecordEeWindow(u32 start_pc, u32 instruction_count, u32* executable_instruction_count)
{
	if (!executable_instruction_count)
		return false;

	*executable_instruction_count = 0;
	if (!s_ee_pre_instruction_trace_callback)
	{
		*executable_instruction_count = instruction_count;
		return true;
	}

	s_ee_a32_prerecording_window = true;
	u32 first_recorded_instruction = 0;
	if (instruction_count != 0 &&
		s_ee_pre_instruction_trace_callback && s_ee_pre_instruction_trace_window_skip_callback)
	{
		const u32 requested_skip = s_ee_pre_instruction_trace_window_skip_callback(
			start_pc, memRead32(start_pc), instruction_count);
		first_recorded_instruction =
			requested_skip < instruction_count ? requested_skip : instruction_count;
		*executable_instruction_count = first_recorded_instruction;
	}

	for (u32 i = first_recorded_instruction; i < instruction_count; i++)
	{
		const u32 pc = start_pc + i * 4;
		if (VitaRecordEePreInstruction(pc, memRead32(pc)))
		{
			recTrimIncompleteBranchTail(start_pc, executable_instruction_count);
			s_ee_a32_prerecording_window = false;
			return false;
		}

		(*executable_instruction_count)++;
	}

	s_ee_a32_prerecording_window = false;
	return true;
}

static constexpr unsigned recRS(u32 op)
{
	return (op >> 21) & 0x1f;
}

static constexpr unsigned recRT(u32 op)
{
	return (op >> 16) & 0x1f;
}

static bool recEvaluateLikelyBranchTaken(u32 op, bool* taken)
{
	if (!taken)
		return false;

	switch (op >> 26)
	{
		case 0x01:
			switch (recRT(op))
			{
				case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
				case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
					*taken = cpuRegs.GPR.r[recRS(op)].SD[0] < 0;
					return true;
				case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
				case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
					*taken = cpuRegs.GPR.r[recRS(op)].SD[0] >= 0;
					return true;
				default:
					return false;
			}

		case 0x10: // BC0FL/BC0TL, owned by COP0.cpp::BC0FL()/BC0TL().
		{
			if (recRS(op) != 0x08 || (recRT(op) != 0x02 && recRT(op) != 0x03))
				return false;

			const bool cpc_cond = (((psHu32(DMAC_STAT) | ~psHu32(DMAC_PCR)) & 0x3ff) == 0x3ff);
			*taken = (recRT(op) == 0x03) ? cpc_cond : !cpc_cond;
			return true;
		}

		case 0x11: // BC1FL/BC1TL, owned by FPU.cpp::BC1FL()/BC1TL().
		{
			if (recRS(op) != 0x08 || (recRT(op) != 0x02 && recRT(op) != 0x03))
				return false;

			const bool fpu_cond = (fpuRegs.fprc[31] & 0x00800000u) != 0;
			*taken = (recRT(op) == 0x03) ? fpu_cond : !fpu_cond;
			return true;
		}

		case 0x12: // BC2FL/BC2TL, owned by COP2.cpp::BC2FL()/BC2TL().
		{
			if (recRS(op) != 0x08 || (recRT(op) != 0x02 && recRT(op) != 0x03))
				return false;

			const bool vu_cond = ((VU0.VI[29].US[0] >> 8) & 1) != 0;
			*taken = (recRT(op) == 0x03) ? vu_cond : !vu_cond;
			return true;
		}

		case 0x14: // BEQL, owned by Interpreter.cpp::BEQL().
			*taken = cpuRegs.GPR.r[recRS(op)].SD[0] == cpuRegs.GPR.r[recRT(op)].SD[0];
			return true;
		case 0x15: // BNEL, owned by Interpreter.cpp::BNEL().
			*taken = cpuRegs.GPR.r[recRS(op)].SD[0] != cpuRegs.GPR.r[recRT(op)].SD[0];
			return true;
		case 0x16: // BLEZL, owned by Interpreter.cpp::BLEZL().
			*taken = cpuRegs.GPR.r[recRS(op)].SD[0] <= 0;
			return true;
		case 0x17: // BGTZL, owned by Interpreter.cpp::BGTZL().
			*taken = cpuRegs.GPR.r[recRS(op)].SD[0] > 0;
			return true;

		default:
			return false;
	}
}

static bool recRecordEeLikelyBranchPair(u32 branch_pc, u32 branch_op, u32* executable_instruction_count)
{
	if (!executable_instruction_count)
		return false;

	*executable_instruction_count = 0;

	bool taken = false;
	if (!recEvaluateLikelyBranchTaken(branch_op, &taken))
		return false;

	s_ee_a32_prerecording_window = true;
	if (VitaRecordEePreInstruction(branch_pc, branch_op))
	{
		s_ee_a32_prerecording_window = false;
		return false;
	}

	*executable_instruction_count = 2;
	if (taken && VitaRecordEePreInstruction(branch_pc + 4, memRead32(branch_pc + 4)))
	{
		s_ee_a32_prerecording_window = false;
		return false;
	}

	s_ee_a32_prerecording_window = false;
	return true;
}

static void recRunInterpreterStepsWithoutProviderTrace(u32 instruction_count)
{
	for (u32 i = 0; i < instruction_count && !s_ee_a32_exit_execution; i++)
	{
		const u32 pc = cpuRegs.pc;
		const u32 opcode = memRead32(pc);
		recInterpreterStepWithoutProviderTrace();
		recRecordInterpreterFallback(pc, opcode, VitaA32EeFallbackReason::ExecuteFailed);
	}
}

static void recAccountEeBlockExecution(const VitaEE::BlockExecutionResult& result, u32 fallback_pc)
{
	if (result.path == VitaEE::BlockExecutionPath::Compiled)
	{
		s_ee_a32_stats.compiled_blocks++;
		s_ee_a32_stats.compiled_instructions += result.instruction_count;
	}
	else
	{
		recRecordInterpreterFallback(fallback_pc, memRead32(fallback_pc),
			VitaA32EeFallbackReason::InterpreterPath);
	}

	if (result.exit == VitaEE::BlockExitKind::Direct)
		s_ee_a32_stats.direct_exits++;
	else if (result.exit == VitaEE::BlockExitKind::Event)
		s_ee_a32_stats.event_exits++;

	if (result.cache_hit)
		s_ee_a32_stats.cache_hits++;
	else
		s_ee_a32_stats.cache_misses++;

	if (result.lookup_hit)
		s_ee_a32_stats.lookup_hits++;
	if (result.fast_dispatch_hit)
		s_ee_a32_stats.fast_dispatch_hits++;

#if defined(VITASX2_VITA) && !defined(VITASX2_PRODUCT_BOOT_VALIDATION) && \
	!defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	// A NoDisc OSDSYS session deliberately has no game ELF entry, so the
	// provider can remain in its PCSX2 EELOAD-visible lifecycle phase for a long
	// time. Publish exponentially sparse, coherent dispatcher-boundary progress
	// to the normal product log. This is startup/hang evidence, not a per-block
	// profiler: after 4096 cold provider boundaries it emits only when the count
	// doubles. Direct-linked blocks within a completed chain are not counted.
	if (!s_ee_a32_elf_booted)
	{
		const u64 boundaries = ++s_ee_a32_pre_elf_boundaries;
		if (boundaries >= 4096 && (boundaries & (boundaries - 1)) == 0)
		{
			Console.WriteLn(
				"Vita EE pre-ELF progress: boundaries=%llu start_pc=%08x next_pc=%08x "
				"ee_cycle=%llu ee_next=%llu iop_pc=%08x iop_cycle=%llu iop_next=%llu "
				"iop_budget=%d frame=%u in_frame_events=%u:%u:%u",
				static_cast<unsigned long long>(boundaries), fallback_pc, cpuRegs.pc,
				static_cast<unsigned long long>(cpuRegs.cycle),
				static_cast<unsigned long long>(cpuRegs.nextEventCycle), psxRegs.pc,
				static_cast<unsigned long long>(psxRegs.cycle),
				static_cast<unsigned long long>(psxRegs.iopNextEventCycle),
				psxRegs.iopCycleEE, g_FrameCount,
				s_ee_a32_stats.in_frame_event_tests,
				s_ee_a32_stats.in_frame_event_tests -
					s_ee_a32_stats.in_frame_event_resume_refusals,
				s_ee_a32_stats.in_frame_event_resume_refusals);
		}
	}
#endif
}

static void recSetEeDirectLinkingEnabled(bool enabled)
{
	if (s_ee_a32_direct_linking_enabled == enabled)
		return;

	s_ee_a32_executor.SetDirectLinkingEnabled(enabled);
	s_ee_a32_direct_linking_enabled = enabled;
}

static void recSetEePersistentDispatchEnabled(bool enabled)
{
	if (s_ee_a32_persistent_dispatch_enabled == enabled)
		return;

	s_ee_a32_executor.SetPersistentDispatchEnabled(enabled);
	s_ee_a32_persistent_dispatch_enabled = enabled;
}

static void recSetEeFastDispatchEnabled(bool enabled)
{
	// Callable and persistent blocks use different exit ABIs. On enable, reset
	// into the persistent ABI before exposing linked entry points; on disable,
	// unlink first so no callable-mode transition can retain a persistent edge.
	if (enabled)
	{
		recSetEePersistentDispatchEnabled(true);
		recSetEeDirectLinkingEnabled(true);
	}
	else
	{
		recSetEeDirectLinkingEnabled(false);
		recSetEePersistentDispatchEnabled(false);
	}
}

static void recResetEeDispatchState()
{
	s_ee_a32_executor.SetDirectLinkingEnabled(false);
	s_ee_a32_executor.SetPersistentDispatchEnabled(false);
	s_ee_a32_direct_linking_enabled = false;
	s_ee_a32_persistent_dispatch_enabled = false;
}

static bool recRefreshEeLifecycleDispatchBarriers()
{
	// These are owners, not independent registrations. Their addresses can
	// collide (or be distinct virtual aliases of one RAM word), so publish the
	// complete desired union in one executor transaction. EELOAD_START remains
	// present for later BIOS/OSDSYS reloads even after the other hooks move.
	std::array<u32, 4> barriers{};
	size_t count = 0;
	barriers[count++] = EELOAD_START;
	if (g_eeloadMain)
		barriers[count++] = g_eeloadMain;
	if (g_eeloadExec)
		barriers[count++] = g_eeloadExec;
	const u32 entry = VMManager::Internal::GetCurrentELFEntryPoint();
	if (entry != UINT32_MAX)
		barriers[count++] = entry;
	return s_ee_a32_executor.SetPersistentDispatchBarriers(
		barriers.data(), count);
}

static void recResetEeLifecycleDispatchBarriers()
{
	s_ee_a32_executor.ClearPersistentDispatchBarriers();
	// PCSX2's recRecompile() discovers the EELOAD main hook when it compiles
	// this entry. Keep only this exceptional entry on the provider boundary;
	// the rest of the BIOS may use the normal persistent/link ABI immediately.
	const u32 initial_barrier = EELOAD_START;
	pxAssertRel(s_ee_a32_executor.SetPersistentDispatchBarriers(
		&initial_barrier, 1),
		"failed to install the EELOAD lifecycle dispatch barrier");
}

static bool recProcessEeLifecycleBoundary(u32 pc)
{
	// PCSX2 owner: x86/ix86-32/iR5900.cpp::recRecompile(). Its generated
	// blocks call these helpers at entry. A32's dispatch barriers provide the
	// same before-block ordering while allowing every ordinary BIOS edge to
	// remain directly linked.
	const u32 lifecycle_pc = VitaEE::BlockExecutor::CanonicalizeRamBackedPc(pc);
	bool refresh_barriers = false;
	// recRecompile() owns this check before cache-reset processing and before all
	// EELOAD checks. Keep every check independent: lifecycle addresses are not
	// required to be distinct.
	if (const u32 entry = VMManager::Internal::GetCurrentELFEntryPoint();
		entry != UINT32_MAX && lifecycle_pc ==
			VitaEE::BlockExecutor::CanonicalizeRamBackedPc(entry))
	{
		VMManager::Internal::EntryPointCompilingOnCPUThread();
		refresh_barriers = true;
	}

	if (lifecycle_pc == EELOAD_START)
	{
		const u32 mainjump = memRead32(EELOAD_START + 0x9c);
		if (mainjump >> 26 == 3) // JAL
		{
			const u32 eeload_main = ((EELOAD_START + 0xa0) & 0xf0000000U) |
				(mainjump << 2 & 0x0fffffffU);
			if (g_eeloadMain != eeload_main)
			{
				g_eeloadMain = eeload_main;
				refresh_barriers = true;
			}
		}
	}
	if (g_eeloadMain && lifecycle_pc ==
		VitaEE::BlockExecutor::CanonicalizeRamBackedPc(g_eeloadMain))
	{
		// x86 tests this while compiling, before its emitted eeloadHook runs.
		// Snapshot it before invoking the hook to preserve that ordering on A32.
		const bool fast_boot_in_progress =
			VMManager::Internal::IsFastBootInProgress();
		eeloadHook();
		refresh_barriers = true;
		if (fast_boot_in_progress)
		{
			const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
			const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
			const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
			const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
			if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) ||
				(typeDexecjump >> 26 == 3))
			{
				g_eeloadExec = EELOAD_START + 0x2B8;
			}
			else if (typeAexecjump >> 26 == 3)
			{
				g_eeloadExec = EELOAD_START + 0x170;
			}
			else
			{
				Console.WriteLn("recExecute: Could not enable launch arguments for fast boot mode; unidentified BIOS version!");
			}
		}
	}
	if (g_eeloadExec && lifecycle_pc ==
		VitaEE::BlockExecutor::CanonicalizeRamBackedPc(g_eeloadExec))
	{
		eeloadHook2();
		refresh_barriers = true;
	}
	s_ee_a32_elf_booted = VMManager::Internal::HasBootedELF();
	// Ordinary persistent-dispatch returns cannot mutate any lifecycle owner.
	// Republishing the complete canonical barrier union here made every helper,
	// scheduler, and incompatible-link return pay a four-address transaction.
	// PCSX2 updates these hooks only at the EELOAD/ELF seams above, so reconcile
	// the executor only when one of those owners was actually observed.
	return !refresh_barriers || recRefreshEeLifecycleDispatchBarriers();
}

#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
static bool recDidEeTraceLimitHitAtNaturalBoundary()
{
	switch (s_ee_a32_trace_limit_stop_condition)
	{
		case VitaA32EeTraceLimitStopCondition::CoreEventTrace:
			return Pcsx2Trace::DidCoreEventTraceHitLimit();
		case VitaA32EeTraceLimitStopCondition::MachineCheckpointTrace:
			return Pcsx2Trace::DidMachineCheckpointTraceHitLimit();
		case VitaA32EeTraceLimitStopCondition::None:
		default:
			return false;
	}
}
#endif

static __attribute__((noinline)) u32 recRunEeEventForGeneratedResume()
{
	// PCSX2 owner: x86/ix86-32/iR5900.cpp::recEventTest() runs the complete
	// shared scheduler owner, checks the recompiler-exit request, and then falls
	// directly into _DynGen_DispatcherReg(). The persistent A32 dispatcher calls
	// this bridge without unwinding its private frame. recClear() has already
	// nulled the active generated directory before setting the reset request, so
	// returning false is enough to force the existing provider boundary safely.
	s_ee_a32_stats.in_frame_event_tests++;
	_cpuEventTest_Shared();

	bool resume = s_ee_a32_in_frame_event_resume_enabled &&
		!s_ee_a32_exit_execution && !s_ee_a32_cache_reset_requested &&
		s_ee_a32_running_compiled_block &&
		s_ee_a32_persistent_dispatch_enabled &&
		s_ee_pre_instruction_trace_callback == nullptr &&
		!VMManager::Internal::IsExecutionInterrupted();
	// SetState() normally publishes the same transition through ExitExecution;
	// keep the PCSX2 IsExecutionInterrupted() owner as a fail-closed guard for
	// lifecycle calls which occur inside the scheduler itself.
#if defined(VITASX2_QEMU_VALIDATION)
	// These counters deliberately observe every natural scheduler boundary.
	// Never bypass them when an explicit bounded validation owns the seam.
	resume = resume && s_ee_a32_persistent_boundary_limit == 0 &&
		!s_ee_a32_link_rejection_profile_enabled;
#endif
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	resume = resume && s_ee_a32_trace_limit_stop_condition ==
		VitaA32EeTraceLimitStopCondition::None;
#endif
	if (resume)
		VitaEE::RefreshRawGpr0KnownZero();
	else
		s_ee_a32_stats.in_frame_event_resume_refusals++;
	return resume ? 1u : 0u;
}

static bool recPersistentEeBoundary(void*, const VitaEE::BlockExecutionResult& result)
{
	recAccountEeBlockExecution(result, cpuRegs.pc);
	if (!recProcessEeLifecycleBoundary(cpuRegs.pc))
	{
		Console.Error("Vita EE could not preserve a PCSX2 lifecycle dispatch seam.");
		s_ee_a32_exit_execution = true;
		return false;
	}
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	// The trace-free full-core route must retain the production persistent
	// dispatch/link shape. Poll bounded CORE/checkpoint completion only here,
	// after generated code has reached the same natural tail which owns the
	// scheduler event test; never install an EE pre-instruction stop hook. A
	// PCSX2-style <=6-instruction structural prefix returns through this cold
	// callback only until its direct link is patched, but deliberately has no
	// iBranchTest(). Do not turn that link seam into an oracle checkpoint.
	const bool natural_scheduler_boundary = !result.scheduler_test_elided;
	if (natural_scheduler_boundary && recDidEeTraceLimitHitAtNaturalBoundary())
	{
		s_ee_a32_exit_execution = true;
		return false;
	}
#endif
#if defined(VITASX2_QEMU_VALIDATION)
	if (natural_scheduler_boundary && s_ee_a32_persistent_boundary_limit != 0 &&
		++s_ee_a32_persistent_boundaries >= s_ee_a32_persistent_boundary_limit)
	{
		s_ee_a32_persistent_boundary_hit_limit = true;
		s_ee_a32_exit_execution = true;
		return false;
	}
#endif
	// EntryPointCompilingOnCPUThread() owns a whole-provider cache reset.  The
	// callback is running from the persistent dispatcher's cold exit at that
	// point, so it must unwind that frame before newly compiled callable/persistent
	// code can be entered.  Continuing here would tail-enter a block compiled for
	// the replacement ABI from the old dispatch frame.
	return !s_ee_a32_exit_execution && !s_ee_a32_cache_reset_requested &&
		s_ee_a32_persistent_dispatch_enabled &&
		s_ee_pre_instruction_trace_callback == nullptr;
}

static bool recCanSplitEeBlockForCodeBudget()
{
	if (!s_ee_pre_instruction_trace_callback)
		return true;
#if defined(VITASX2_QEMU_VALIDATION)
	// Exact trace windows must not pre-record instructions which an artificial
	// continuation defers.  Once the bounded EE oracle sample is full it no
	// longer consumes those records; the retained callback only watches the
	// stronger cross-core terminal condition, so ordinary budget splitting is
	// safe again for the remainder of that integrated run.
	return s_ee_exact_trace_streams && Pcsx2Trace::DidEeTraceHitLimit();
#else
	return false;
#endif
}

static void recReserve()
{
}

static void recShutdown()
{
	s_ee_a32_executor.ClearPersistentDispatchBarriers();
	s_ee_a32_executor.Shutdown();
	recResetEeDispatchState();
	s_ee_a32_cache_reset_requested = false;
	s_ee_a32_elf_booted = false;
	s_ee_a32_pre_elf_boundaries = 0;
}

static void recReset()
{
	intCpu.Reset();
	VitaEE::RefreshRawGpr0KnownZero();
	s_ee_a32_executor.Reset();
	recResetEeDispatchState();
	recResetEeLifecycleDispatchBarriers();
	VitaResetA32EeProviderStats();
	s_ee_a32_exit_execution = false;
	s_ee_a32_cache_reset_requested = false;
	s_ee_a32_elf_booted = false;
	s_ee_a32_pre_elf_boundaries = 0;
}

static void recStep()
{
	intCpu.Step();
	VitaEE::RefreshRawGpr0KnownZero();
}

static void recExecute()
{
	s_ee_a32_exit_execution = false;
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	// A cold PCSX2-style short structural prefix returns through the provider
	// without iBranchTest() until its successor link exists. Do not stop a
	// bounded trace at that non-architectural seam.
	bool can_observe_trace_limit = true;
#endif

	while (!s_ee_a32_exit_execution)
	{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		// Observe a completed limit before the next block only when the preceding
		// return owned a scheduler test. Lifecycle dispatch barriers keep PCSX2's
		// EELOAD/entry hooks visible without forcing all pre-entry blocks callable.
		if (can_observe_trace_limit && recDidEeTraceLimitHitAtNaturalBoundary())
		{
			s_ee_a32_exit_execution = true;
			break;
		}
#endif
		// Exact instruction traces retain callable block boundaries. Normal boot
		// uses the persistent PCSX2-style dispatcher from reset onward; only the
		// explicit EELOAD/entry barriers above return to the provider for lifecycle
		// hooks. A no-disc OSDSYS boot therefore no longer waits for a game ELF
		// before gaining direct links.
		const bool fast_dispatch = s_ee_pre_instruction_trace_callback == nullptr;
		recSetEeFastDispatchEnabled(fast_dispatch);

		if (s_ee_a32_cache_reset_requested)
		{
			s_ee_a32_stats.invalidated_blocks += s_ee_a32_executor.Reset();
			s_ee_a32_cache_reset_requested = false;
		}

		const u32 pc = cpuRegs.pc;
		if (!recProcessEeLifecycleBoundary(pc))
		{
			Console.Error("Vita EE could not install a PCSX2 lifecycle dispatch seam.");
			s_ee_a32_exit_execution = true;
			break;
		}
		// The ELF-entry owner may have reset every CPU provider and changed the EE
		// block ABI. Re-enter this loop so recSetEeFastDispatchEnabled() publishes
		// the requested ABI before executing the first ELF instruction.
		if (fast_dispatch && !s_ee_a32_persistent_dispatch_enabled)
			continue;

		if (!s_ee_pre_instruction_trace_callback)
		{
			VitaEE::BlockExecutionResult result;
			s_ee_a32_running_compiled_block = true;
			const bool executed = fast_dispatch ?
				s_ee_a32_executor.ExecutePersistentAtPc(pc, true,
					&recPersistentEeBoundary, nullptr, &result,
					s_ee_a32_in_frame_event_resume_enabled ?
						&recRunEeEventForGeneratedResume : nullptr) :
				s_ee_a32_executor.ExecuteCompiledBlockAtPc(pc, true, &result);
			s_ee_a32_running_compiled_block = false;
			if (executed)
			{
				if (!fast_dispatch)
				{
					recAccountEeBlockExecution(result, pc);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
					can_observe_trace_limit =
						!result.scheduler_test_elided;
#endif
				}

				if (s_ee_a32_cache_reset_requested)
				{
					s_ee_a32_stats.invalidated_blocks += s_ee_a32_executor.Reset();
					s_ee_a32_cache_reset_requested = false;
				}

				continue;
			}

			if (fast_dispatch)
			{
				// A persistent block can only fail before entering generated code or
				// while resolving its next boundary. Step the current instruction
				// through PCSX2's interpreter and retry the dispatcher at the new PC;
				// callable blocks cannot run in the persistent exit ABI.
				const u32 fallback_pc = cpuRegs.pc;
				const u32 fallback_opcode = memRead32(fallback_pc);
				intCpu.Step();
				VitaEE::RefreshRawGpr0KnownZero();
				recRecordInterpreterFallback(fallback_pc, fallback_opcode,
					VitaA32EeFallbackReason::ExecuteFailed);
				continue;
			}
		}

		VitaEE::BlockScanResult scan;
		if (!VitaEE::BlockExecutor::ScanStraightLineBlock(pc,
				VitaEE::BlockExecutor::MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &scan) ||
			scan.instruction_count == 0)
		{
			// Step through Interpreter.cpp::execI() with the pre-instruction
			// hook active: execI records this instruction itself and, for
			// branches, records and executes the delay slot inside
			// intDoBranch() — the same record stream the interpreter provider
			// produces. A stop request exits through Cpu->ExitExecution().
			const u32 opcode = memRead32(pc);
			intCpu.Step();
			VitaEE::RefreshRawGpr0KnownZero();
			recRecordInterpreterFallback(pc, opcode, recFallbackReasonForScanStop(scan.stop));
			continue;
		}

		u32 window_instruction_count = scan.instruction_count;
		if (s_ee_exact_trace_streams && s_ee_pre_instruction_trace_callback)
		{
			for (u32 i = 0; i + 1 < window_instruction_count; i++)
			{
				if (!VitaEE::BlockCompiler::RequiresTraceWindowEndAfterOpcode(
						memRead32(pc + i * sizeof(u32))))
				{
					continue;
				}

				window_instruction_count = i + 1;
				break;
			}
		}
		bool likely_pair_trace_recorded = false;
		bool likely_pair_full_window_recorded = false;
		u32 likely_pair_executable_instruction_count = 0;
		if (s_ee_exact_trace_streams && s_ee_pre_instruction_trace_callback &&
			scan.stop == VitaEE::BlockScanStop::Branch &&
			window_instruction_count >= 2 &&
			VitaEE::BlockCompiler::IsBranchLikely(memRead32(pc + (window_instruction_count - 2) * 4)))
		{
			// Branch-likely cancels its delay slot on the not-taken path
			// (Interpreter.cpp::BEQL() and friends), so a pre-recorded window
			// would log a delay slot that never executes. In trace mode, drop
			// the branch pair from the window and let execI() step it with
			// exact delay-slot recording; non-trace runs keep the native
			// likely-branch blocks.
			window_instruction_count -= 2;
			if (window_instruction_count == 0)
			{
				const u32 opcode = memRead32(pc);
				likely_pair_trace_recorded = true;
				likely_pair_full_window_recorded =
					recRecordEeLikelyBranchPair(pc, opcode, &likely_pair_executable_instruction_count);
				window_instruction_count = likely_pair_executable_instruction_count;
			}
		}

		u32 executable_instruction_count = 0;
		bool full_window_recorded = false;
		if (likely_pair_trace_recorded)
		{
			executable_instruction_count = likely_pair_executable_instruction_count;
			full_window_recorded = likely_pair_full_window_recorded;
		}
		else if (s_ee_pre_instruction_trace_callback &&
			s_ee_a32_trace_mode == VitaA32EeTraceMode::BlockBoundaryState)
		{
			full_window_recorded = !VitaRecordEePreInstruction(pc, memRead32(pc));
			if (full_window_recorded)
				executable_instruction_count = window_instruction_count;
		}
		else
		{
			full_window_recorded =
				recRecordEeWindow(pc, window_instruction_count, &executable_instruction_count);
		}
		if (executable_instruction_count == 0)
			break;

		VitaEE::BlockExecutionResult result;
		s_ee_a32_running_compiled_block = true;
		const bool executed = s_ee_a32_executor.ExecuteCompiledBlock(pc,
			executable_instruction_count, true, &result,
			recCanSplitEeBlockForCodeBudget());
		s_ee_a32_running_compiled_block = false;
		if (!executed)
		{
			s_ee_a32_stats.failed_blocks++;
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
			s_ee_a32_session_fallback_stats.failed_blocks++;
#endif
			recRunInterpreterStepsWithoutProviderTrace(executable_instruction_count);
			if (!full_window_recorded)
				break;

			continue;
		}

		recAccountEeBlockExecution(result, pc);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		// A code-budget split can still end in a PCSX2-owned concatenated short
		// prefix. Keep the same natural-boundary contract as the callable path
		// above even though an exact trace normally exits immediately afterward.
		can_observe_trace_limit = !result.scheduler_test_elided;
#endif

		if (s_ee_a32_cache_reset_requested)
		{
			s_ee_a32_stats.invalidated_blocks += s_ee_a32_executor.Reset();
			s_ee_a32_cache_reset_requested = false;
		}

		if (!full_window_recorded)
			break;
	}
}

static void recExitExecution()
{
	s_ee_a32_exit_execution = true;
}

static void recCancelInstruction()
{
	s_ee_a32_exit_execution = true;
}

static void recClear(u32 addr, u32 size)
{
	// PCSX2 owner: x86/ix86-32/iR5900.cpp::recClear(addr, size), where size is
	// measured in 32-bit guest words.
	if (s_ee_a32_running_compiled_block)
	{
		// A returning helper may reach a dynamic exception/continuation tail
		// before the provider can unwind and consume this reset. Suppress its
		// generated lookup immediately so stale code cannot be entered.
		s_ee_a32_executor.SuspendGeneratedLookupUntilReset();
		s_ee_a32_cache_reset_requested = true;
		return;
	}

	s_ee_a32_stats.invalidated_blocks += s_ee_a32_executor.InvalidateRange(addr, size);
}

R5900cpu recCpu = {
	recReserve,
	recShutdown,
	recReset,
	recStep,
	recExecute,
	recExitExecution,
	recCancelInstruction,
	recClear,
};

static void psxRecReserve()
{
}

static void psxRecReset()
{
	psxInt.Reset();
	const u32 invalidated = s_iop_a32_executor.Reset();
#if defined(VITASX2_QEMU_VALIDATION)
	if (s_iop_a32_runtime_stats_enabled)
		s_iop_a32_stats.invalidated_blocks += invalidated;
#else
	(void)invalidated;
#endif
}

static s32 psxRecExecuteBlock(s32 eeCycles)
{
#if !defined(VITASX2_QEMU_VALIDATION)
	return s_iop_a32_executor.ExecuteProviderTimeslice(eeCycles);
#else
	if (s_iop_a32_private_dispatcher_enabled)
		return s_iop_a32_executor.ExecuteProviderTimeslice(eeCycles);

	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;
	bool force_logical_continuation = false;
	bool first_dispatch = true;

	for (;;)
	{
		const bool forced_continuation =
			std::exchange(force_logical_continuation, false);
		// PCSX2 owner: x86/iR3000A.cpp::recExecuteBlock() enters
		// _DynGen_EnterRecompiledCode() unconditionally. Its dispatcher executes
		// the selected first BaseBlock before iPsxBranchTest() applies the signed
		// budget exit. The EE event path can deliberately arrive with a
		// non-positive budget when iopEventAction is pending, so do not turn that
		// call into an interpreter-style no-op.
		if ((!first_dispatch || VitaIsIopPreInstructionTraceEnabled()) &&
			!forced_continuation && psxRegs.iopCycleEE <= 0)
			break;
		first_dispatch = false;
		if (!VitaIOP::BlockExecutor::CompiledPs1BiosGateEnabled() &&
			(psxHu32(HW_ICFG) & 8) &&
			((psxRegs.pc & 0x1fffffffU) == 0xa0 ||
			 (psxRegs.pc & 0x1fffffffU) == 0xb0 ||
			 (psxRegs.pc & 0x1fffffffU) == 0xc0))
		{
			psxBiosCall();
		}

		const u32 pc = psxRegs.pc;
		u32 dispatch_flags = 0;
#if defined(VITASX2_QEMU_VALIDATION)
		VitaIOP::ProviderCompileResult compile_result;
		if (s_iop_a32_compact_provider_dispatch_enabled)
		{
			dispatch_flags = s_iop_a32_executor.ExecuteProviderBlockAtPc(
				pc, &compile_result, forced_continuation);
			if ((dispatch_flags & VitaIOP::ProviderDispatchSuccess) != 0)
			{
				s_iop_a32_compact_provider_dispatch_entries++;
				if ((dispatch_flags & VitaIOP::ProviderDispatchCacheHit) != 0)
					s_iop_a32_compact_provider_cache_hit_entries++;
			}
		}
		else
		{
			VitaIOP::BlockExecutionResult legacy_result;
			if (s_iop_a32_executor.ExecuteCompiledBlockAtPc(
					pc, &legacy_result, false, forced_continuation))
			{
				dispatch_flags = VitaIOP::ProviderDispatchSuccess |
					(legacy_result.cache_hit ? VitaIOP::ProviderDispatchCacheHit : 0) |
					(legacy_result.lookup_hit ? VitaIOP::ProviderDispatchLookupHit : 0) |
					(legacy_result.fast_dispatch_hit ? VitaIOP::ProviderDispatchFastHit : 0) |
					(legacy_result.wait_loop_fast_forward ?
						VitaIOP::ProviderDispatchWaitForward : 0) |
					(legacy_result.isolate_mode_switched ?
						VitaIOP::ProviderDispatchIsolateSwitch : 0) |
					(legacy_result.exit == VitaIOP::BlockExitKind::LogicalContinuation ||
						legacy_result.exit ==
							VitaIOP::BlockExitKind::LogicalContinuationIsolateModeWrite ?
						VitaIOP::ProviderDispatchLogicalContinuation : 0);
				compile_result.instruction_count = legacy_result.instruction_count;
				if (!legacy_result.cache_hit)
				{
					compile_result.native_instruction_count =
						legacy_result.native_instruction_count;
					compile_result.helper_instruction_count =
						legacy_result.helper_instruction_count;
					compile_result.code_cache_resets = legacy_result.code_cache_resets;
				}
			}
		}
#else
		// PCSX2 owner: x86/iR3000A.cpp::_DynGen_EnterRecompiledCode() returns
		// dispatcher control in registers. The compact Vita provider does the
		// same. Product execution does not consume cold-compile diagnostics.
		dispatch_flags = s_iop_a32_executor.ExecuteProviderBlockAtPc(
			pc, nullptr, forced_continuation);
#endif
		if ((dispatch_flags & VitaIOP::ProviderDispatchSuccess) == 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			// Keep the rare fallback sentinel live even when hot-path statistics
			// are disabled, so QEMU can still prove that product-equivalent
			// execution never entered the interpreter.
			u32 opcode = UINT32_MAX;
			VitaIOP::BlockExecutor::ReadRecompilerOwnedOpcode(pc, &opcode);
			// FindProviderBlockAtPcSlow already performed the complete logical
			// scan. Count that rejected candidate directly instead of rereading up
			// to 65,535 guest words on the diagnostic fallback path.
			s_iop_a32_stats.failed_blocks++;
			if (s_iop_a32_stats.interpreter_blocks == 0)
			{
				s_iop_a32_stats.first_interpreter_pc = pc;
				s_iop_a32_stats.first_interpreter_opcode = opcode;
			}
			s_iop_a32_stats.last_interpreter_pc = pc;
			s_iop_a32_stats.last_interpreter_opcode = opcode;
			s_iop_a32_stats.interpreter_blocks++;
#endif
			if (VitaIsIopPreInstructionTraceEnabled())
				return s_iop_a32_executor.ExecuteInterpreterFallbackTimeslice(
					psxRegs.iopCycleEE);

			bool interpreter_logical_continuation = false;
			bool interpreter_execution_terminated = false;
			const s32 fallback_result =
				s_iop_a32_executor.ExecuteInterpreterRecompilerBlock(
					psxRegs.iopCycleEE, &interpreter_logical_continuation,
					&interpreter_execution_terminated);
			if (interpreter_execution_terminated)
				return fallback_result;
			if (interpreter_logical_continuation)
			{
				force_logical_continuation = true;
				continue;
			}
			if (psxRegs.iopCycleEE > 0)
				continue;
			return fallback_result;
		}
		if ((dispatch_flags & VitaIOP::ProviderDispatchLogicalContinuation) != 0)
			force_logical_continuation = true;

#if defined(VITASX2_QEMU_VALIDATION)
		if (s_iop_a32_runtime_stats_enabled)
		{
			if ((dispatch_flags & VitaIOP::ProviderDispatchCacheHit) != 0)
			{
				s_iop_a32_stats.cache_hits++;
			}
			else
			{
				s_iop_a32_stats.cache_misses++;
				s_iop_a32_stats.compiled_blocks++;
				s_iop_a32_stats.compiled_instructions += compile_result.instruction_count;
				s_iop_a32_stats.native_instructions += compile_result.native_instruction_count;
				s_iop_a32_stats.helper_instructions += compile_result.helper_instruction_count;
				s_iop_a32_stats.code_cache_resets = compile_result.code_cache_resets;
			}
			if ((dispatch_flags & VitaIOP::ProviderDispatchLookupHit) != 0)
				s_iop_a32_stats.lookup_hits++;
			if ((dispatch_flags & VitaIOP::ProviderDispatchFastHit) != 0)
				s_iop_a32_stats.fast_dispatch_hits++;
			if ((dispatch_flags & VitaIOP::ProviderDispatchIsolateSwitch) != 0)
				s_iop_a32_stats.isolate_mode_switches++;
		}
#endif
		if ((dispatch_flags & VitaIOP::ProviderDispatchWaitForward) != 0)
			continue;

#if defined(VITASX2_QEMU_VALIDATION)
		IopDispatchProfileEntry& dispatch_profile = s_iop_a32_dispatch_profile[pc];
		if (dispatch_profile.dispatches == 0)
		{
			dispatch_profile.opcode = iopMemRead32(pc);
			dispatch_profile.instruction_count = compile_result.instruction_count;
			for (u32 i = 0; i < std::min<u32>(compile_result.instruction_count, 16); i++)
				dispatch_profile.opcodes[i] = iopMemRead32(pc + i * 4);
		}
		dispatch_profile.dispatches++;
		const u64 edge_key = (static_cast<u64>(pc) << 32) | psxRegs.pc;
		s_iop_a32_dispatch_edge_profile[edge_key]++;
		if (s_iop_a32_runtime_stats_enabled)
		{
			s_iop_a32_stats.executed_blocks++;
			s_iop_a32_stats.direct_exits++;
		}
#endif
	}

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
#endif
}

static void psxRecClear(u32 addr, u32 size)
{
	// PCSX2 owner: x86/iR3000A.cpp::recClearIOP(addr, size), where size is
	// measured in 32-bit guest words.
	const u32 invalidated = s_iop_a32_executor.InvalidateRange(addr, size);
#if defined(VITASX2_QEMU_VALIDATION)
	if (s_iop_a32_runtime_stats_enabled)
		s_iop_a32_stats.invalidated_blocks += invalidated;
#else
	(void)invalidated;
#endif
}

static void psxRecShutdown()
{
	s_iop_a32_executor.Shutdown();
}

R3000Acpu psxRec = {
	psxRecReserve,
	psxRecReset,
	psxRecExecuteBlock,
	psxRecClear,
	psxRecShutdown,
};

recMicroVU0 CpuMicroVU0;
recMicroVU1 CpuMicroVU1;

recMicroVU0::recMicroVU0()
{
	m_Idx = 0;
	IsInterpreter = false;
}

void recMicroVU0::Reserve()
{
}

void recMicroVU0::Shutdown()
{
	VitaVU::ShutdownVu0Blocks();
}

void recMicroVU0::Reset()
{
	CpuIntVU0.Reset();
	VitaVU::ResetVu0Blocks();
}

void recMicroVU0::Step()
{
	CpuIntVU0.Step();
}

void recMicroVU0::SetStartPC(u32 startPC)
{
	CpuIntVU0.SetStartPC(startPC);
}

void recMicroVU0::Execute(u32 cycles)
{
	// PCSX2 owner: InterpVU0::Execute()'s loop, with scan-proven windows
	// routed through the A32 block provider in pcsx2/vita/VitaVuBlockCompiler.cpp.
	VitaVU::ExecuteVu0Blocks(cycles);
}

void recMicroVU0::Clear(u32 addr, u32 size)
{
	VuMicroInvalidateDecodedCache(0, addr, size);
	VitaVU::InvalidateVu0Blocks(addr, size);
}

recMicroVU1::recMicroVU1()
{
	m_Idx = 1;
	IsInterpreter = false;
}

void recMicroVU1::Reserve()
{
}

void recMicroVU1::Shutdown()
{
	VitaVU::ShutdownVu1Blocks();
}

void recMicroVU1::Reset()
{
	CpuIntVU1.Reset();
	VitaVU::ResetVu1Blocks();
}

void recMicroVU1::Step()
{
	CpuIntVU1.Step();
}

void recMicroVU1::SetStartPC(u32 startPC)
{
	CpuIntVU1.SetStartPC(startPC);
}

void recMicroVU1::Execute(u32 cycles)
{
	// PCSX2 owner: InterpVU1::Execute()'s loop, with eligible windows routed
	// through the A32 block provider in pcsx2/vita/VitaVuBlockCompiler.cpp.
	VitaVU::ExecuteVu1Blocks(cycles);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	Pcsx2Trace::NotifyMachineCheckpointVu1ExecutionCompleted();
#endif
}

void recMicroVU1::Clear(u32 addr, u32 size)
{
	VuMicroInvalidateDecodedCache(1, addr, size);
	VitaVU::InvalidateVu1Blocks(addr, size);
}

void recMicroVU1::ResumeXGkick()
{
	CpuIntVU1.ResumeXGkick();
}

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits,
	bool is_signed, bool is_load, bool is_fpr)
{
	pxFailRel("Vita ARM32 fastmem backpatching is disabled.");
}

bool SaveStateBase::vuJITFreeze()
{
	// PCSX2's x86 microVU owner opens vu1Thread from recMicroVU1::Reserve(), so
	// its save barrier can unconditionally wait for the worker. Vita's A32 VU1
	// provider executes synchronously and deliberately leaves that worker closed;
	// waiting on its empty semaphore would have no thread capable of signalling it.
	if (IsSaving() && vu1Thread.IsOpen())
		vu1Thread.WaitVU();

	Console.Warning("recompiler state is unavailable in the Vita ARM32 interpreter build.");

	std::array<u8, 96> empty_data{};
	Freeze(empty_data);
	Freeze(empty_data);
	return true;
}

void VitaSelectInterpreterCpuProviders()
{
	Cpu = &intCpu;
	psxCpu = &psxInt;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSelectA32EeCpuProviders()
{
	Cpu = &recCpu;
	psxCpu = &psxInt;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSelectA32IopCpuProviders()
{
	Cpu = &intCpu;
	psxCpu = &psxRec;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSelectA32EeIopCpuProviders()
{
	Cpu = &recCpu;
	psxCpu = &psxRec;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSelectConfiguredCpuProviders()
{
	// PCSX2 owner: VMManager.cpp::UpdateCPUImplementations(). The Vita fork
	// maps the EE and IOP recompiler flags to Vita A32 providers, and the VU
	// recompiler flags to the A32 micro block providers.
	if (EmuConfig.Cpu.Recompiler.EnableEE && EmuConfig.Cpu.Recompiler.EnableIOP)
		VitaSelectA32EeIopCpuProviders();
	else if (EmuConfig.Cpu.Recompiler.EnableEE)
		VitaSelectA32EeCpuProviders();
	else if (EmuConfig.Cpu.Recompiler.EnableIOP)
		VitaSelectA32IopCpuProviders();
	else
		VitaSelectInterpreterCpuProviders();

	if (EmuConfig.Cpu.Recompiler.EnableVU0)
		CpuVU0 = &CpuMicroVU0;
	if (EmuConfig.Cpu.Recompiler.EnableVU1)
		CpuVU1 = &CpuMicroVU1;
}

void VitaResetA32EeProviderStats()
{
	s_ee_a32_stats = {};
#if defined(VITASX2_QEMU_VALIDATION)
	s_ee_a32_executor.ResetDirectLinkRejectionProfile();
	s_ee_a32_persistent_boundaries = 0;
	s_ee_a32_persistent_boundary_hit_limit = false;
#endif
}

VitaA32EeProviderStats VitaGetA32EeProviderStats()
{
	VitaA32EeProviderStats result = s_ee_a32_stats;
	result.in_frame_event_resume_candidates =
		result.in_frame_event_tests - result.in_frame_event_resume_refusals;
	return result;
}

void VitaSetA32EeInFrameEventResumeEnabled(bool enabled)
{
	// This is a product/validation policy switch, not guest state. If it changes
	// during a live chain, the callback checks the new value before permitting
	// another generated lookup and therefore unwinds at the next due event.
	s_ee_a32_in_frame_event_resume_enabled = enabled;
}

#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
void VitaResetA32EeSessionFallbackStats()
{
	s_ee_a32_session_fallback_stats = {};
}

VitaA32EeProviderStats VitaGetA32EeSessionFallbackStats()
{
	return s_ee_a32_session_fallback_stats;
}
#endif

#if defined(VITASX2_QEMU_VALIDATION)
void VitaSetA32EeLinkRejectionProfileEnabled(bool enabled)
{
	s_ee_a32_link_rejection_profile_enabled = enabled;
	s_ee_a32_executor.SetDirectLinkRejectionProfileEnabled(enabled);
}

void VitaSetA32EePersistentBoundaryLimit(u64 limit)
{
	s_ee_a32_persistent_boundary_limit = limit;
	s_ee_a32_persistent_boundaries = 0;
	s_ee_a32_persistent_boundary_hit_limit = false;
}

bool VitaDidA32EePersistentBoundaryHitLimit()
{
	return s_ee_a32_persistent_boundary_hit_limit;
}

VitaA32EeLinkRejectionProfile VitaGetA32EeLinkRejectionProfile()
{
	return s_ee_a32_executor.GetDirectLinkRejectionProfile();
}
#endif

#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
void VitaSetA32EeTraceLimitStopCondition(VitaA32EeTraceLimitStopCondition condition)
{
	s_ee_a32_trace_limit_stop_condition = condition;
}
#endif

void VitaResetA32IopProviderStats()
{
	s_iop_a32_stats = {};
#if defined(VITASX2_QEMU_VALIDATION)
	s_iop_a32_dispatch_profile.clear();
	s_iop_a32_dispatch_edge_profile.clear();
	s_iop_a32_compact_provider_dispatch_entries = 0;
	s_iop_a32_compact_provider_cache_hit_entries = 0;
	g_vita_a32_iop_private_event_entries = 0;
#endif
	s_iop_a32_executor.ResetInstrumentationCounters();
}

void VitaSetA32IopCompactProviderDispatchEnabled(bool enabled)
{
#if defined(VITASX2_QEMU_VALIDATION)
	s_iop_a32_compact_provider_dispatch_enabled = enabled;
#else
	(void)enabled;
#endif
}

#if defined(VITASX2_QEMU_VALIDATION)
void VitaSetA32IopRuntimeStatsEnabled(bool enabled)
{
	s_iop_a32_runtime_stats_enabled = enabled;
}

void VitaSetA32IopPrivateDispatcherEnabled(bool enabled)
{
	s_iop_a32_private_dispatcher_enabled = enabled;
}

void VitaSetA32IopPrivateEventEntryEnabled(bool enabled)
{
	g_vita_a32_iop_private_event_entry_enabled = enabled;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSetA32IopPrivateHotPathEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetPrivateDispatcherHotPathEnabled(enabled);
}

void VitaSetA32IopHotDispatchOwnershipEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetHotDispatchOwnershipEnabled(enabled);
}

void VitaSetA32IopCachedWaitDescriptorEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetCachedWaitDescriptorEnabled(enabled);
}

void VitaSetA32IopInlineWaitFastForwardEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetInlineWaitFastForwardEnabled(enabled);
}

void VitaSetA32IopWaitResumeCacheEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetWaitResumeCacheEnabled(enabled);
}

void VitaSetA32IopWaitResumeEventEntryEnabled(bool enabled)
{
	g_vita_a32_iop_wait_resume_event_entry_enabled = enabled;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSetA32IopSchedulerResumeEventEntryEnabled(bool enabled)
{
	g_vita_a32_iop_scheduler_resume_event_entry_enabled = enabled;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSetA32IopSchedulerPredictionEventEntryEnabled(bool enabled)
{
	g_vita_a32_iop_scheduler_prediction_event_entry_enabled = enabled;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSetA32IopSchedulerDispatchCacheEventEntryEnabled(bool enabled)
{
	g_vita_a32_iop_scheduler_dispatch_cache_event_entry_enabled = enabled;
#if defined(__arm__)
	UpdateIopEventEntry();
#endif
}

void VitaSetA32IopWaitResumeFirstEntryOwnershipEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetWaitResumeFirstEntryOwnershipEnabled(enabled);
}

void VitaSetA32IopWaitResumeKindEntryEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetWaitResumeKindEntryEnabled(enabled);
}

void VitaSetA32IopWaitResumeClockEntryEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetWaitResumeClockEntryEnabled(enabled);
}

void VitaSetA32IopWaitResumeNoLinkEntryEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetWaitResumeNoLinkEntryEnabled(enabled);
}

void VitaSetA32IopWaitResumeDescriptorSpecializationEnabled(bool enabled)
{
	VitaIOP::BlockExecutor::SetWaitResumeDescriptorSpecializationEnabled(enabled);
}
#endif

VitaA32IopProviderStats VitaGetA32IopProviderStats()
{
	s_iop_a32_stats.code_cache_resets = s_iop_a32_executor.GetCodeCacheResetCount();
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	// Product execution uses BlockExecutor::ExecuteProviderTimeslice(), bypassing
	// the detailed QEMU dispatcher counters below. These three bounded sentinels
	// prove native activity and zero retained interpreter/invalid-result exits
	// without enabling the profiling dispatcher on real hardware.
	const VitaIOP::PortableValidationStats portable =
		s_iop_a32_executor.GetPortableValidationStats();
	s_iop_a32_stats.executed_blocks = portable.native_provider_entries;
	s_iop_a32_stats.interpreter_blocks = portable.interpreter_fallback_entries;
	s_iop_a32_stats.failed_blocks = portable.invalid_provider_results;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
	VitaIOP::BlockExecutionResult snapshot{};
	s_iop_a32_executor.SnapshotInstrumentation(&snapshot);
	s_iop_a32_stats.hot_dispatch_cache_hits = snapshot.hot_dispatch_cache_hits;
	s_iop_a32_stats.hot_dispatch_cache_misses = snapshot.hot_dispatch_cache_misses;
	s_iop_a32_stats.hot_dispatch_cache_way_probes = snapshot.hot_dispatch_cache_way_probes;
	s_iop_a32_stats.hot_dispatch_cache_64_set_hits =
		snapshot.hot_dispatch_cache_64_set_hits;
	s_iop_a32_stats.hot_dispatch_cache_64_set_misses =
		snapshot.hot_dispatch_cache_64_set_misses;
	s_iop_a32_stats.hot_dispatch_cache_64_set_way_probes =
		snapshot.hot_dispatch_cache_64_set_way_probes;
	s_iop_a32_stats.hot_dispatch_cache_way_probes_removed =
		(snapshot.hot_dispatch_cache_64_set_way_probes >=
			snapshot.hot_dispatch_cache_way_probes) ?
			(snapshot.hot_dispatch_cache_64_set_way_probes -
				snapshot.hot_dispatch_cache_way_probes) : 0;
	s_iop_a32_stats.scheduler_direct_resume_candidates =
		snapshot.scheduler_direct_resume_candidates;
	s_iop_a32_stats.scheduler_direct_resume_installs =
		snapshot.scheduler_direct_resume_installs;
	s_iop_a32_stats.scheduler_direct_resume_attempts =
		snapshot.scheduler_direct_resume_attempts;
	s_iop_a32_stats.scheduler_direct_resume_hits =
		snapshot.scheduler_direct_resume_hits;
	s_iop_a32_stats.scheduler_direct_resume_misses =
		snapshot.scheduler_direct_resume_misses;
	s_iop_a32_stats.scheduler_direct_resume_no_target =
		snapshot.scheduler_direct_resume_no_target;
	s_iop_a32_stats.scheduler_direct_resume_target_mismatch =
		snapshot.scheduler_direct_resume_target_mismatch;
	s_iop_a32_stats.scheduler_direct_resume_hot_lookups_removed =
		snapshot.scheduler_direct_resume_hits;
	s_iop_a32_stats.scheduler_direct_event_entries =
		snapshot.scheduler_direct_event_entries;
	s_iop_a32_stats.scheduler_direct_event_forwards =
		snapshot.scheduler_direct_event_forwards;
	s_iop_a32_stats.scheduler_direct_event_fallbacks =
		snapshot.scheduler_direct_event_fallbacks;
	s_iop_a32_stats.scheduler_direct_event_remainders =
		snapshot.scheduler_direct_event_remainders;
	s_iop_a32_stats.scheduler_direct_event_installs =
		snapshot.scheduler_direct_event_installs;
	s_iop_a32_stats.scheduler_direct_event_clears =
		snapshot.scheduler_direct_event_clears;
	s_iop_a32_stats.scheduler_prediction_attempts =
		snapshot.scheduler_prediction_attempts;
	s_iop_a32_stats.scheduler_prediction_hits = snapshot.scheduler_prediction_hits;
	s_iop_a32_stats.scheduler_prediction_misses = snapshot.scheduler_prediction_misses;
	s_iop_a32_stats.scheduler_prediction_two_way_hits =
		snapshot.scheduler_prediction_two_way_hits;
	s_iop_a32_stats.scheduler_prediction_four_way_hits =
		snapshot.scheduler_prediction_four_way_hits;
	s_iop_a32_stats.scheduler_prediction_forwards =
		snapshot.scheduler_prediction_forwards;
	s_iop_a32_stats.scheduler_prediction_fallbacks =
		snapshot.scheduler_prediction_fallbacks;
	s_iop_a32_stats.scheduler_prediction_remainders =
		snapshot.scheduler_prediction_remainders;
	s_iop_a32_stats.scheduler_dispatch_cache_attempts =
		snapshot.scheduler_dispatch_cache_attempts;
	s_iop_a32_stats.scheduler_dispatch_cache_hits =
		snapshot.scheduler_dispatch_cache_hits;
	s_iop_a32_stats.scheduler_dispatch_cache_misses =
		snapshot.scheduler_dispatch_cache_misses;
	s_iop_a32_stats.scheduler_dispatch_cache_forwards =
		snapshot.scheduler_dispatch_cache_forwards;
	s_iop_a32_stats.scheduler_dispatch_cache_fallbacks =
		snapshot.scheduler_dispatch_cache_fallbacks;
	s_iop_a32_stats.scheduler_dispatch_cache_remainders =
		snapshot.scheduler_dispatch_cache_remainders;
	s_iop_a32_stats.scheduler_dispatch_cache_installs =
		snapshot.scheduler_dispatch_cache_installs;
	s_iop_a32_stats.hot_dispatch_trusted_raw_hits = snapshot.hot_dispatch_trusted_raw_hits;
	s_iop_a32_stats.hot_dispatch_owned_hits = snapshot.hot_dispatch_owned_hits;
	s_iop_a32_stats.hot_dispatch_hit_pc_count = snapshot.hot_dispatch_hit_pc_count;
	for (u32 i = 0; i < snapshot.hot_dispatch_hit_pc_count; i++)
	{
		s_iop_a32_stats.hot_dispatch_hit_pcs[i] = snapshot.hot_dispatch_hit_pcs[i];
		s_iop_a32_stats.hot_dispatch_hit_pc_hits[i] = snapshot.hot_dispatch_hit_pc_hits[i];
	}
	s_iop_a32_stats.interpreter_fallback_pc_count =
		snapshot.interpreter_fallback_pc_count;
	for (u32 i = 0; i < snapshot.interpreter_fallback_pc_count; i++)
	{
		s_iop_a32_stats.interpreter_fallback_pcs[i] =
			snapshot.interpreter_fallback_pcs[i];
		s_iop_a32_stats.interpreter_fallback_owner_pcs[i] =
			snapshot.interpreter_fallback_owner_pcs[i];
		s_iop_a32_stats.interpreter_fallback_owner_opcodes[i] =
			snapshot.interpreter_fallback_owner_opcodes[i];
		s_iop_a32_stats.interpreter_fallback_instruction_counts[i] =
			snapshot.interpreter_fallback_instruction_counts[i];
		s_iop_a32_stats.interpreter_fallback_source_hashes[i] =
			snapshot.interpreter_fallback_source_hashes[i];
		s_iop_a32_stats.interpreter_fallback_hits[i] =
			snapshot.interpreter_fallback_hits[i];
	}
	// The retired stale-entry arm loads/checks CachedBlock::valid and reloads/
	// checks CachedBlock::start_pc after the cache record already matched. Product
	// Cortex-A9 disassembly attributes six instructions to that arm.
	s_iop_a32_stats.hot_dispatch_stale_guard_instructions_removed =
		snapshot.hot_dispatch_owned_hits * 6u;
	s_iop_a32_stats.wait_resume_cache_attempts = snapshot.wait_resume_cache_attempts;
	s_iop_a32_stats.wait_resume_cache_hits = snapshot.wait_resume_cache_hits;
	s_iop_a32_stats.wait_resume_cache_misses = snapshot.wait_resume_cache_misses;
	s_iop_a32_stats.wait_resume_event_entries = snapshot.wait_resume_event_entries;
	s_iop_a32_stats.wait_resume_event_forwards = snapshot.wait_resume_event_forwards;
	s_iop_a32_stats.wait_resume_event_fallbacks = snapshot.wait_resume_event_fallbacks;
	s_iop_a32_stats.wait_resume_event_installs = snapshot.wait_resume_event_installs;
	s_iop_a32_stats.wait_resume_event_clears = snapshot.wait_resume_event_clears;
	s_iop_a32_stats.wait_resume_first_entry_owned =
		snapshot.wait_resume_first_entry_owned;
	// Product control disassembly attributes fifteen instructions to the
	// scheduler-redundant null, retained-block, PC, and isolate identity arm.
	s_iop_a32_stats.wait_resume_first_entry_identity_instructions_removed =
		snapshot.wait_resume_first_entry_owned * 15u;
	s_iop_a32_stats.wait_resume_post_event_identity_checks =
		snapshot.wait_resume_post_event_identity_checks;
	s_iop_a32_stats.wait_resume_kind_specific_entries =
		snapshot.wait_resume_kind_specific_entries;
	// Product generic-kind disassembly performs one context-kind load in the
	// entry thunk, then three classification instructions per tested kind:
	// unconditional tests one, poll tests two, and conditional tests three.
	s_iop_a32_stats.wait_resume_kind_instructions_removed =
		snapshot.wait_resume_kind_specific_entries +
		snapshot.wait_resume_kind_specific_unconditional_forwards * 3u +
		snapshot.wait_resume_kind_specific_poll_forwards * 6u +
		snapshot.wait_resume_kind_specific_conditional_forwards * 9u;
	s_iop_a32_stats.wait_resume_clock_specific_entries =
		snapshot.wait_resume_clock_specific_entries;
	// Selecting PCSX2's current IOP clock contract when the retained wait is
	// installed removes one iopHw base setup per entry and the HW_ICFG load/test
	// pair from every descriptor forward. psxNotifyClockModeChange() resets the
	// provider before a different clock-specific entry can execute.
	s_iop_a32_stats.wait_resume_clock_instructions_removed =
		snapshot.wait_resume_clock_specific_entries +
		snapshot.wait_resume_clock_specific_forwards * 2u;
	s_iop_a32_stats.wait_resume_no_link_specific_entries =
		snapshot.wait_resume_no_link_specific_entries;
	// PCSX2's recompiler knows at block compilation whether the self-branch
	// publishes r31. Product control disassembly attributes five instructions to
	// the no-link path's descriptor-byte test and predicated publication arm.
	s_iop_a32_stats.wait_resume_no_link_instructions_removed =
		snapshot.wait_resume_no_link_specific_forwards * 5u;
	s_iop_a32_stats.wait_resume_descriptor_forwards =
		snapshot.wait_resume_descriptor_forwards;
	s_iop_a32_stats.wait_resume_unconditional_forwards =
		snapshot.wait_resume_unconditional_forwards;
	s_iop_a32_stats.wait_resume_poll_forwards = snapshot.wait_resume_poll_forwards;
	s_iop_a32_stats.wait_resume_conditional_forwards =
		snapshot.wait_resume_conditional_forwards;
	s_iop_a32_stats.direct_budget_exit_provider_entries =
		snapshot.direct_budget_exit_provider_entries;
	s_iop_a32_stats.constant_cycle_budget_provider_entries =
		snapshot.constant_cycle_budget_provider_entries;
	s_iop_a32_stats.validation_calls = snapshot.validation_calls;
	s_iop_a32_stats.validation_words = snapshot.validation_words;
	s_iop_a32_stats.raw_validation_calls = snapshot.raw_validation_calls;
	s_iop_a32_stats.raw_validation_words = snapshot.raw_validation_words;
	s_iop_a32_stats.translated_validation_words = snapshot.translated_validation_words;
	s_iop_a32_stats.wait_loop_configuration_checks = snapshot.wait_loop_configuration_checks;
	s_iop_a32_stats.trusted_source_hits = snapshot.trusted_source_hits;
	s_iop_a32_stats.trusted_source_audit_words = snapshot.trusted_source_audit_words;
	s_iop_a32_stats.trusted_source_audit_failures = snapshot.trusted_source_audit_failures;
	s_iop_a32_stats.ram_invalidation_calls = snapshot.ram_invalidation_calls;
	s_iop_a32_stats.ram_invalidation_record_visits = snapshot.ram_invalidation_record_visits;
	s_iop_a32_stats.clock_mode_check_instructions_removed =
		snapshot.clock_mode_check_instructions_removed;
	s_iop_a32_stats.saved_register_stack_words_removed =
		snapshot.saved_register_stack_words_removed;
	s_iop_a32_stats.saved_register_frame_instructions_added =
		snapshot.saved_register_frame_instructions_added;
	s_iop_a32_stats.saved_register_frame_instructions_removed =
		snapshot.saved_register_frame_instructions_removed;
	s_iop_a32_stats.batched_cycle_instructions_removed =
		snapshot.batched_cycle_instructions_removed;
	s_iop_a32_stats.batched_cycle_stack_words_removed =
		snapshot.batched_cycle_stack_words_removed;
	s_iop_a32_stats.expanded_cycle_batching_provider_entries =
		snapshot.expanded_cycle_batching_provider_entries;
	s_iop_a32_stats.linked_frame_bypass_entries = snapshot.linked_frame_bypass_entries;
	s_iop_a32_stats.linked_frame_instructions_removed =
		snapshot.linked_frame_instructions_removed;
	s_iop_a32_stats.linked_frame_stack_words_removed =
		snapshot.linked_frame_stack_words_removed;
	s_iop_a32_stats.sequential_qword_copy_fast_paths =
		snapshot.sequential_qword_copy_fast_paths;
	s_iop_a32_stats.sequential_qword_copy_instructions_removed =
		snapshot.sequential_qword_copy_instructions_removed;
	s_iop_a32_stats.branch_event_candidates = snapshot.branch_event_candidates;
	s_iop_a32_stats.branch_event_budget_positive =
		snapshot.branch_event_budget_positive;
	s_iop_a32_stats.branch_event_tests_entered = snapshot.branch_event_tests_entered;
	s_iop_a32_stats.budget_before_event_fast_exits =
		snapshot.budget_before_event_fast_exits;
	s_iop_a32_stats.budget_before_event_instructions_removed =
		snapshot.budget_before_event_instructions_removed;
	s_iop_a32_stats.event_deadline_fast_skips = snapshot.event_deadline_fast_skips;
	s_iop_a32_stats.event_deadline_instructions_removed =
		snapshot.event_deadline_instructions_removed;
	s_iop_a32_stats.compact_provider_dispatch_entries =
		s_iop_a32_compact_provider_dispatch_entries;
	s_iop_a32_stats.compact_provider_cache_hit_entries =
		s_iop_a32_compact_provider_cache_hit_entries;
	// Product Cortex-A9 disassembly of the retired aggregate contract loads
	// cache, lookup, fast, isolate, and wait result bytes after every successful
	// call. The flag-word contract consumes r0 directly and removes all five.
	s_iop_a32_stats.compact_provider_result_loads_removed =
		s_iop_a32_compact_provider_dispatch_entries * 5u;
	// Product A32 before this iteration executes 20 provider-stat instructions
	// on every cache hit, six more executed/direct counter instructions on an
	// ordinary cache-hit entry, and at least 20 instructions in the wait-forward
	// recorder before/after its uncounted __aeabi_uldivmod body. Subtract every
	// cold miss from ordinary entries to keep the lower bound conservative.
	const u64 ordinary_cache_hits =
		s_iop_a32_stats.executed_blocks > s_iop_a32_stats.cache_misses ?
			s_iop_a32_stats.executed_blocks - s_iop_a32_stats.cache_misses : 0;
	s_iop_a32_stats.provider_runtime_stats_instructions_removed =
		snapshot.private_dispatcher_provider_entries == 0 ?
			static_cast<u64>(s_iop_a32_stats.cache_hits) * 20u +
				ordinary_cache_hits * 6u +
				s_iop_a32_stats.wait_loop_fast_forwards * 20u : 0;
	s_iop_a32_stats.private_dispatcher_calls = snapshot.private_dispatcher_calls;
	s_iop_a32_stats.private_dispatcher_provider_entries =
		snapshot.private_dispatcher_provider_entries;
	s_iop_a32_stats.private_dispatcher_wait_forwards =
		snapshot.private_dispatcher_wait_forwards;
	s_iop_a32_stats.private_dispatcher_generated_entries =
		snapshot.private_dispatcher_generated_entries;
	s_iop_a32_stats.private_dispatcher_fallbacks = snapshot.private_dispatcher_fallbacks;
	// Inlining the provider body removes at least the caller BL and callee
	// return on every successful entry. Exclude argument setup, prologue,
	// epilogue, and any optimizer-visible state reuse from this lower bound.
	s_iop_a32_stats.private_dispatcher_control_transfers_removed =
		snapshot.private_dispatcher_provider_entries * 2u;
	s_iop_a32_stats.private_dispatcher_inlined_hot_entries =
		snapshot.private_dispatcher_inlined_hot_entries;
	s_iop_a32_stats.private_frame_provider_entries =
		snapshot.private_frame_provider_entries;
	s_iop_a32_stats.private_frame_stack_words_removed =
		snapshot.private_frame_stack_words_removed;
	s_iop_a32_stats.private_frame_zero_scratch_entries =
		snapshot.private_frame_zero_scratch_entries;
	// PCSX2's dispatcher performs neither C++ call: each inlined exact hit
	// removes the caller BL and callee return for both lookup and execution.
	s_iop_a32_stats.private_dispatcher_hot_path_control_transfers_removed =
		snapshot.private_dispatcher_inlined_hot_entries * 4u;
	// The product-configured A32 control retains seven guard setup instructions
	// before useful dispatcher work and nine guard check instructions on both the
	// normal and interpreter-tail exits. The body has no addressable buffer.
	s_iop_a32_stats.private_dispatcher_stack_guard_instructions_removed =
		snapshot.private_dispatcher_calls * 16u;
	s_iop_a32_stats.private_event_entries = g_vita_a32_iop_private_event_entries;
	// The private entry/exit transfer only LR. The former AAPCS PUSH/POP also
	// transferred r4-r11 in both directions: sixteen words retired per entry.
	s_iop_a32_stats.private_event_stack_words_removed =
		g_vita_a32_iop_private_event_entries * 16u;
	// The private entry and CFA exit add four simple instructions relative to
	// the former PUSH/SUB + ADD/POP frame.
	s_iop_a32_stats.private_event_frame_instructions_added =
		g_vita_a32_iop_private_event_entries * 4u;
	s_iop_a32_stats.cached_wait_descriptor_checks = snapshot.cached_wait_descriptor_checks;
	s_iop_a32_stats.cached_wait_descriptor_forwards = snapshot.cached_wait_descriptor_forwards;
	s_iop_a32_stats.cached_wait_descriptor_opcode_reads_removed =
		snapshot.cached_wait_descriptor_opcode_reads_removed;
	// Every retired iopMemRead32() opcode fetch crossed AAPCS in both
	// directions. Exclude its address translation and memory work.
	s_iop_a32_stats.cached_wait_descriptor_memory_control_transfers_removed =
		snapshot.cached_wait_descriptor_opcode_reads_removed * 2u;
	s_iop_a32_stats.cached_wait_descriptor_unconditional_checks =
		snapshot.cached_wait_descriptor_unconditional_checks;
	s_iop_a32_stats.cached_wait_descriptor_control_transfers_removed =
		(snapshot.cached_wait_descriptor_opcode_reads_removed +
		 snapshot.cached_wait_descriptor_unconditional_checks) * 2u;
	s_iop_a32_stats.compiled_ps1_bios_gate_blocks =
		snapshot.compiled_ps1_bios_gate_blocks;
	s_iop_a32_stats.compiled_ps1_bios_gate_entries =
		snapshot.compiled_ps1_bios_gate_entries;
	s_iop_a32_stats.dispatcher_ps1_bios_gate_checks_removed =
		snapshot.dispatcher_ps1_bios_gate_checks_removed;
	// Product/control disassembly owns the exact instruction attribution. This
	// lower bound charges only the four always-executed instructions removed
	// from an ordinary normal-clock dispatcher iteration: iopHw base setup, the
	// HW_ICFG load, its test, and the skip branch. The PC load is still required
	// by dispatch and is only rescheduled, so it is deliberately not counted.
	s_iop_a32_stats.dispatcher_ps1_bios_gate_instructions_removed =
		snapshot.dispatcher_ps1_bios_gate_checks_removed * 4u;
	s_iop_a32_stats.inline_wait_fast_forwards = snapshot.inline_wait_fast_forwards;
	// Product Cortex-A9 disassembly of VitaIopA32FastForwardWaitLoop() saves
	// r4-r8/LR and returns through the matching POP. The private dispatcher
	// already owns its wider frame, so inlining retires all twelve word transfers
	// and the caller BL/callee return without adding another save set.
	s_iop_a32_stats.inline_wait_stack_words_removed =
		snapshot.inline_wait_fast_forwards * 12u;
	s_iop_a32_stats.inline_wait_control_transfers_removed =
		snapshot.inline_wait_fast_forwards * 2u;
	s_iop_a32_stats.pinned_gpr_memory_ops_saved =
		snapshot.total_pinned_gpr_memory_ops_saved;
	s_iop_a32_stats.pinned_branch_operand_moves_removed =
		snapshot.total_pinned_branch_operand_moves_removed;
	s_iop_a32_stats.condition_code_branch_instructions_removed =
		snapshot.total_condition_code_branch_instructions_removed;
	s_iop_a32_stats.producer_branch_compare_instructions_removed =
		snapshot.total_producer_branch_compare_instructions_removed;
	s_iop_a32_stats.fused_ram_guard_instructions_removed =
		snapshot.total_fused_ram_guard_instructions_removed;
	s_iop_a32_stats.source_page_guard_instructions_removed =
		snapshot.total_source_page_guard_instructions_removed;
	s_iop_a32_stats.source_page_literal_instructions_removed =
		snapshot.total_source_page_literal_instructions_removed;
	s_iop_a32_stats.isolate_cache_guard_instructions_removed =
		snapshot.total_isolate_cache_guard_instructions_removed;
#endif
	return s_iop_a32_stats;
}

#if defined(VITASX2_QEMU_VALIDATION)
VitaA32IopDispatchProfile VitaGetA32IopDispatchProfile()
{
	std::vector<std::pair<u32, IopDispatchProfileEntry>> sorted;
	sorted.reserve(s_iop_a32_dispatch_profile.size());
	for (const auto& entry : s_iop_a32_dispatch_profile)
		sorted.push_back(entry);
	std::sort(sorted.begin(), sorted.end(), [](const auto& lhs, const auto& rhs) {
		return lhs.second.dispatches != rhs.second.dispatches ?
			lhs.second.dispatches > rhs.second.dispatches : lhs.first < rhs.first;
	});

	VitaA32IopDispatchProfile result;
	result.count = std::min<u32>(static_cast<u32>(sorted.size()), VITA_A32_IOP_HOT_DISPATCH_COUNT);
	for (u32 i = 0; i < result.count; i++)
	{
		result.entries[i].pc = sorted[i].first;
		result.entries[i].opcode = sorted[i].second.opcode;
		result.entries[i].instruction_count = sorted[i].second.instruction_count;
		result.entries[i].dispatches = sorted[i].second.dispatches;
		std::copy(std::begin(sorted[i].second.opcodes), std::end(sorted[i].second.opcodes),
			std::begin(result.entries[i].opcodes));
	}

	std::vector<std::pair<u64, u64>> sorted_edges;
	sorted_edges.reserve(s_iop_a32_dispatch_edge_profile.size());
	for (const auto& edge : s_iop_a32_dispatch_edge_profile)
		sorted_edges.push_back(edge);
	std::sort(sorted_edges.begin(), sorted_edges.end(), [](const auto& lhs, const auto& rhs) {
		return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
	});
	result.edge_count =
		std::min<u32>(static_cast<u32>(sorted_edges.size()), VITA_A32_IOP_HOT_DISPATCH_COUNT);
	for (u32 i = 0; i < result.edge_count; i++)
	{
		result.edges[i].source_pc = static_cast<u32>(sorted_edges[i].first >> 32);
		result.edges[i].target_pc = static_cast<u32>(sorted_edges[i].first);
		result.edges[i].dispatches = sorted_edges[i].second;
	}
	return result;
}
#endif

#if defined(VITASX2_QEMU_VALIDATION)
void VitaRecordA32IopWaitLoopFastForward(u64 iop_cycles, u32 block_cycles)
{
	if (!s_iop_a32_runtime_stats_enabled)
		return;
	s_iop_a32_stats.wait_loop_fast_forwards++;
	s_iop_a32_stats.wait_loop_iop_cycles += iop_cycles;
	const u64 equivalent_blocks = block_cycles ? (iop_cycles / block_cycles) : 0;
	if (equivalent_blocks > 1)
		s_iop_a32_stats.wait_loop_block_entries_elided += equivalent_blocks - 1;
}

void VitaRecordA32IopWaitLoopDispatchElision()
{
	if (!s_iop_a32_runtime_stats_enabled)
		return;
	s_iop_a32_stats.wait_loop_dispatches_elided++;
}

void VitaRecordA32IopPollCallWaitLoopDispatchElision()
{
	if (!s_iop_a32_runtime_stats_enabled)
		return;
	s_iop_a32_stats.poll_call_wait_loop_fast_forwards++;
	s_iop_a32_stats.poll_call_wait_loop_dispatches_elided++;
}
#endif
