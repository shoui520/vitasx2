// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MTVU.h"
#include "Config.h"
#include "DebugTools/GsTrace.h"
#include "DebugTools/IpuTrace.h"
#include "DebugTools/Spu2Trace.h"
#include "DebugTools/VuTrace.h"
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
#include <vector>
#endif

static VitaEePreInstructionTraceCallback s_ee_pre_instruction_trace_callback = nullptr;
static VitaEePreInstructionTraceWindowSkipCallback s_ee_pre_instruction_trace_window_skip_callback = nullptr;
static VitaIopPreInstructionTraceCallback s_iop_pre_instruction_trace_callback = nullptr;
static VitaEE::BlockExecutor s_ee_a32_executor;
static VitaIOP::BlockExecutor s_iop_a32_executor;
static VitaA32EeProviderStats s_ee_a32_stats;
static VitaA32IopProviderStats s_iop_a32_stats;
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
static u64 s_iop_a32_compact_provider_dispatch_entries = 0;
static u64 s_iop_a32_compact_provider_cache_hit_entries = 0;
#endif
static bool s_ee_a32_exit_execution = false;
static bool s_ee_a32_cache_reset_requested = false;
static bool s_ee_a32_running_compiled_block = false;
static bool s_ee_a32_elf_booted = false;
static bool s_ee_a32_direct_linking_enabled = false;
static bool s_ee_a32_persistent_dispatch_enabled = false;
static VitaA32EeTraceMode s_ee_a32_trace_mode = VitaA32EeTraceMode::InstructionWindow;
static bool s_ee_provider_trace_suppressed = false;
static bool s_ee_a32_prerecording_window = false;

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

static void recRecordInterpreterFallback(u32 pc, u32 opcode, VitaA32EeFallbackReason reason)
{
	if (s_ee_a32_stats.interpreter_steps == 0)
	{
		s_ee_a32_stats.first_interpreter_pc = pc;
		s_ee_a32_stats.first_interpreter_opcode = opcode;
		s_ee_a32_stats.first_interpreter_reason = static_cast<u32>(reason);
	}

	s_ee_a32_stats.last_interpreter_pc = pc;
	s_ee_a32_stats.last_interpreter_opcode = opcode;
	s_ee_a32_stats.last_interpreter_reason = static_cast<u32>(reason);
	s_ee_a32_stats.interpreter_steps++;

	switch (reason)
	{
		case VitaA32EeFallbackReason::ScanUnsupportedOpcode:
			s_ee_a32_stats.scan_unsupported_fallbacks++;
			break;
		case VitaA32EeFallbackReason::ScanBoundary:
			s_ee_a32_stats.scan_boundary_fallbacks++;
			break;
		case VitaA32EeFallbackReason::ExactTraceBranchLikely:
			s_ee_a32_stats.exact_trace_branch_likely_fallbacks++;
			break;
		case VitaA32EeFallbackReason::ExecuteFailed:
			s_ee_a32_stats.execute_failed_fallbacks++;
			break;
		case VitaA32EeFallbackReason::InterpreterPath:
			s_ee_a32_stats.interpreter_path_fallbacks++;
			break;
		case VitaA32EeFallbackReason::None:
			break;
	}
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

static bool recPersistentEeBoundary(void*, const VitaEE::BlockExecutionResult& result)
{
	recAccountEeBlockExecution(result, cpuRegs.pc);
	return !s_ee_a32_exit_execution && !s_ee_a32_cache_reset_requested &&
		s_ee_pre_instruction_trace_callback == nullptr;
}

static void recReserve()
{
}

static void recShutdown()
{
	s_ee_a32_executor.Shutdown();
	recResetEeDispatchState();
	s_ee_a32_cache_reset_requested = false;
	s_ee_a32_elf_booted = false;
}

static void recReset()
{
	intCpu.Reset();
	VitaEE::RefreshRawGpr0KnownZero();
	s_ee_a32_executor.Reset();
	recResetEeDispatchState();
	VitaResetA32EeProviderStats();
	s_ee_a32_exit_execution = false;
	s_ee_a32_cache_reset_requested = false;
	s_ee_a32_elf_booted = false;
}

static void recStep()
{
	intCpu.Step();
	VitaEE::RefreshRawGpr0KnownZero();
}

static void recExecute()
{
	s_ee_a32_exit_execution = false;

	while (!s_ee_a32_exit_execution)
	{
		// Direct-linked chains bypass this dispatcher, so linking stays off
		// while tracing and until the ELF boots: pre-boot, every arrival at
		// the EELOAD/entry hook pcs below must pass through here, matching
		// Interpreter.cpp::intExecute() and the compile-time hooks in
		// x86/ix86-32/iR5900.cpp::recRecompile().
		if (!s_ee_a32_elf_booted)
			s_ee_a32_elf_booted = VMManager::Internal::HasBootedELF();
		const bool elf_booted = s_ee_a32_elf_booted;
		const bool fast_dispatch = s_ee_pre_instruction_trace_callback == nullptr && elf_booted;
		recSetEeFastDispatchEnabled(fast_dispatch);

		if (s_ee_a32_cache_reset_requested)
		{
			s_ee_a32_stats.invalidated_blocks += s_ee_a32_executor.Reset();
			s_ee_a32_cache_reset_requested = false;
		}

		const u32 pc = cpuRegs.pc;

		if (!elf_booted)
		{
			if (pc == EELOAD_START)
			{
				// The EELOAD _start function is the same across all BIOS versions.
				const u32 mainjump = memRead32(EELOAD_START + 0x9c);
				if (mainjump >> 26 == 3) // JAL
					g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);
			}
			else if (g_eeloadMain && pc == g_eeloadMain)
			{
				eeloadHook();
				if (VMManager::Internal::IsFastBootInProgress())
				{
					// See comments on this code in iR5900.cpp's recRecompile().
					const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
					const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
					const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
					const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
					if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3))
						g_eeloadExec = EELOAD_START + 0x2B8;
					else if (typeAexecjump >> 26 == 3)
						g_eeloadExec = EELOAD_START + 0x170;
					else
						Console.WriteLn("recExecute: Could not enable launch arguments for fast boot mode; unidentified BIOS version!");
				}
			}
			else if (g_eeloadExec && pc == g_eeloadExec)
			{
				eeloadHook2();
			}
			else if (pc == VMManager::Internal::GetCurrentELFEntryPoint())
			{
				VMManager::Internal::EntryPointCompilingOnCPUThread();
			}
		}

		if (!s_ee_pre_instruction_trace_callback)
		{
			VitaEE::BlockExecutionResult result;
			s_ee_a32_running_compiled_block = true;
			const bool executed = fast_dispatch ?
				s_ee_a32_executor.ExecutePersistentAtPc(pc, true,
					&recPersistentEeBoundary, nullptr, &result) :
				s_ee_a32_executor.ExecuteCompiledBlockAtPc(pc, true, &result);
			s_ee_a32_running_compiled_block = false;
			if (executed)
			{
				if (!fast_dispatch)
					recAccountEeBlockExecution(result, pc);

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
		const bool executed = s_ee_a32_executor.ExecuteCompiledBlock(pc, executable_instruction_count, true, &result);
		s_ee_a32_running_compiled_block = false;
		if (!executed)
		{
			s_ee_a32_stats.failed_blocks++;
			recRunInterpreterStepsWithoutProviderTrace(executable_instruction_count);
			if (!full_window_recorded)
				break;

			continue;
		}

		recAccountEeBlockExecution(result, pc);

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
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;

	while (psxRegs.iopCycleEE > 0)
	{
		if ((psxHu32(HW_ICFG) & 8) &&
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
			dispatch_flags =
				s_iop_a32_executor.ExecuteProviderBlockAtPc(pc, &compile_result);
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
			if (s_iop_a32_executor.ExecuteCompiledBlockAtPc(pc, &legacy_result, false))
			{
				dispatch_flags = VitaIOP::ProviderDispatchSuccess |
					(legacy_result.cache_hit ? VitaIOP::ProviderDispatchCacheHit : 0) |
					(legacy_result.lookup_hit ? VitaIOP::ProviderDispatchLookupHit : 0) |
					(legacy_result.fast_dispatch_hit ? VitaIOP::ProviderDispatchFastHit : 0) |
					(legacy_result.wait_loop_fast_forward ?
						VitaIOP::ProviderDispatchWaitForward : 0) |
					(legacy_result.isolate_mode_switched ?
						VitaIOP::ProviderDispatchIsolateSwitch : 0);
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
		dispatch_flags = s_iop_a32_executor.ExecuteProviderBlockAtPc(pc, nullptr);
#endif
		if ((dispatch_flags & VitaIOP::ProviderDispatchSuccess) == 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			// Keep the rare fallback sentinel live even when hot-path statistics
			// are disabled, so QEMU can still prove that product-equivalent
			// execution never entered the interpreter.
			const u32 opcode = iopMemRead32(pc);
			VitaIOP::BlockScanResult scan;
			if (VitaIOP::BlockExecutor::ScanStraightLineBlock(
					pc, VitaIOP::BlockExecutor::MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &scan) &&
				scan.instruction_count != 0)
			{
				s_iop_a32_stats.failed_blocks++;
			}
			if (s_iop_a32_stats.interpreter_blocks == 0)
			{
				s_iop_a32_stats.first_interpreter_pc = pc;
				s_iop_a32_stats.first_interpreter_opcode = opcode;
			}
			s_iop_a32_stats.last_interpreter_pc = pc;
			s_iop_a32_stats.last_interpreter_opcode = opcode;
			s_iop_a32_stats.interpreter_blocks++;
#endif
			const s32 fallback_result = psxInt.ExecuteBlock(psxRegs.iopCycleEE);
			return fallback_result;
		}

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
	s_iop_a32_executor.Reset();
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
	if (IsSaving())
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
}

void VitaSelectA32EeCpuProviders()
{
	Cpu = &recCpu;
	psxCpu = &psxInt;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
}

void VitaSelectA32IopCpuProviders()
{
	Cpu = &intCpu;
	psxCpu = &psxRec;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
}

void VitaSelectA32EeIopCpuProviders()
{
	Cpu = &recCpu;
	psxCpu = &psxRec;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
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
}

VitaA32EeProviderStats VitaGetA32EeProviderStats()
{
	return s_ee_a32_stats;
}

void VitaResetA32IopProviderStats()
{
	s_iop_a32_stats = {};
#if defined(VITASX2_QEMU_VALIDATION)
	s_iop_a32_dispatch_profile.clear();
	s_iop_a32_dispatch_edge_profile.clear();
	s_iop_a32_compact_provider_dispatch_entries = 0;
	s_iop_a32_compact_provider_cache_hit_entries = 0;
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
#endif

VitaA32IopProviderStats VitaGetA32IopProviderStats()
{
	s_iop_a32_stats.code_cache_resets = s_iop_a32_executor.GetCodeCacheResetCount();
#if defined(VITASX2_QEMU_VALIDATION)
	VitaIOP::BlockExecutionResult snapshot{};
	s_iop_a32_executor.SnapshotInstrumentation(&snapshot);
	s_iop_a32_stats.hot_dispatch_cache_hits = snapshot.hot_dispatch_cache_hits;
	s_iop_a32_stats.hot_dispatch_cache_misses = snapshot.hot_dispatch_cache_misses;
	s_iop_a32_stats.hot_dispatch_trusted_raw_hits = snapshot.hot_dispatch_trusted_raw_hits;
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
		static_cast<u64>(s_iop_a32_stats.cache_hits) * 20u +
		ordinary_cache_hits * 6u +
		s_iop_a32_stats.wait_loop_fast_forwards * 20u;
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
