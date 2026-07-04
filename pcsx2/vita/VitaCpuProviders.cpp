// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MTVU.h"
#include "Config.h"
#include "DebugTools/GsTrace.h"
#include "DebugTools/IpuTrace.h"
#include "DebugTools/Spu2Trace.h"
#include "DebugTools/VuTrace.h"
#include "Hw.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SaveState.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "vita/VitaCore.h"
#include "vita/VitaEeBlockCompiler.h"
#include "vita/VitaEeExecutor.h"
#include "vtlb.h"

#include "common/Assertions.h"
#include "common/Console.h"

static VitaEePreInstructionTraceCallback s_ee_pre_instruction_trace_callback = nullptr;
static VitaIopPreInstructionTraceCallback s_iop_pre_instruction_trace_callback = nullptr;
static VitaEE::BlockExecutor s_ee_a32_executor;
static VitaA32EeProviderStats s_ee_a32_stats;
static bool s_ee_a32_exit_execution = false;
static bool s_ee_a32_cache_reset_requested = false;
static bool s_ee_a32_running_compiled_block = false;
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
	s_iop_pre_instruction_trace_callback = callback;
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

static bool recRecordEeWindow(u32 start_pc, u32 instruction_count, u32* executable_instruction_count)
{
	if (!executable_instruction_count)
		return false;

	*executable_instruction_count = 0;
	s_ee_a32_prerecording_window = true;
	for (u32 i = 0; i < instruction_count; i++)
	{
		const u32 pc = start_pc + i * 4;
		if (VitaRecordEePreInstruction(pc, memRead32(pc)))
		{
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

static void recReserve()
{
}

static void recShutdown()
{
	s_ee_a32_executor.Reset();
	s_ee_a32_cache_reset_requested = false;
}

static void recReset()
{
	intCpu.Reset();
	VitaEE::RefreshRawGpr0KnownZero();
	s_ee_a32_executor.Reset();
	VitaResetA32EeProviderStats();
	s_ee_a32_exit_execution = false;
	s_ee_a32_cache_reset_requested = false;
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
		const bool elf_booted = VMManager::Internal::HasBootedELF();
		s_ee_a32_executor.SetDirectLinkingEnabled(
			s_ee_pre_instruction_trace_callback == nullptr && elf_booted);

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

		if (result.path == VitaEE::BlockExecutionPath::Compiled)
		{
			s_ee_a32_stats.compiled_blocks++;
			s_ee_a32_stats.compiled_instructions += result.instruction_count;
		}
		else
		{
			recRecordInterpreterFallback(pc, memRead32(pc), VitaA32EeFallbackReason::InterpreterPath);
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
}

static s32 psxRecExecuteBlock(s32 eeCycles)
{
	return psxInt.ExecuteBlock(eeCycles);
}

static void psxRecClear(u32 addr, u32 size)
{
}

static void psxRecShutdown()
{
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
}

void recMicroVU0::Reset()
{
	CpuIntVU0.Reset();
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
	CpuIntVU0.Execute(cycles);
}

void recMicroVU0::Clear(u32 addr, u32 size)
{
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
}

void recMicroVU1::Reset()
{
	CpuIntVU1.Reset();
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
	CpuIntVU1.Execute(cycles);
}

void recMicroVU1::Clear(u32 addr, u32 size)
{
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

void VitaSelectConfiguredCpuProviders()
{
	// PCSX2 owner: VMManager.cpp::UpdateCPUImplementations(). The Vita fork
	// maps the EE recompiler flag to the A32 EE provider while IOP/VU remain
	// on their PCSX2 interpreters until their Vita providers are ported.
	if (EmuConfig.Cpu.Recompiler.EnableEE)
		VitaSelectA32EeCpuProviders();
	else
		VitaSelectInterpreterCpuProviders();
}

void VitaResetA32EeProviderStats()
{
	s_ee_a32_stats = {};
}

VitaA32EeProviderStats VitaGetA32EeProviderStats()
{
	return s_ee_a32_stats;
}
