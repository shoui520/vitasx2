// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MTVU.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SaveState.h"
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

void VitaSetEePreInstructionTraceCallback(VitaEePreInstructionTraceCallback callback)
{
	s_ee_pre_instruction_trace_callback = callback;
}

bool VitaRecordEePreInstruction(u32 pc, u32 opcode)
{
	const VitaEePreInstructionTraceCallback callback = s_ee_pre_instruction_trace_callback;
	return callback ? callback(pc, opcode) : false;
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

static void recInterpreterStepWithoutProviderTrace()
{
	const VitaEePreInstructionTraceCallback callback = s_ee_pre_instruction_trace_callback;
	s_ee_pre_instruction_trace_callback = nullptr;
	intCpu.Step();
	s_ee_pre_instruction_trace_callback = callback;
}

static void recReserve()
{
}

static void recShutdown()
{
	s_ee_a32_executor.Reset();
}

static void recReset()
{
	intCpu.Reset();
	s_ee_a32_executor.Reset();
	VitaResetA32EeProviderStats();
	s_ee_a32_exit_execution = false;
}

static void recStep()
{
	intCpu.Step();
}

static void recExecute()
{
	s_ee_a32_exit_execution = false;

	while (!s_ee_a32_exit_execution)
	{
		const u32 pc = cpuRegs.pc;
		const u32 op = memRead32(pc);
		if (VitaRecordEePreInstruction(pc, op))
			break;

		if (!VitaEE::BlockCompiler::CanCompileOpcode(op))
		{
			recInterpreterStepWithoutProviderTrace();
			s_ee_a32_stats.interpreter_steps++;
			continue;
		}

		VitaEE::BlockExecutionResult result;
		if (!s_ee_a32_executor.ExecuteStraightLineBlockOrInterpreterStep(pc, 1, true, &result))
		{
			s_ee_a32_stats.failed_blocks++;
			recInterpreterStepWithoutProviderTrace();
			s_ee_a32_stats.interpreter_steps++;
			continue;
		}

		if (result.path == VitaEE::BlockExecutionPath::Compiled)
			s_ee_a32_stats.compiled_blocks++;
		else
			s_ee_a32_stats.interpreter_steps++;

		if (result.exit == VitaEE::BlockExitKind::Direct)
			s_ee_a32_stats.direct_exits++;
		else if (result.exit == VitaEE::BlockExitKind::Event)
			s_ee_a32_stats.event_exits++;
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
	s_ee_a32_executor.Reset();
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

void VitaResetA32EeProviderStats()
{
	s_ee_a32_stats = {};
}

VitaA32EeProviderStats VitaGetA32EeProviderStats()
{
	return s_ee_a32_stats;
}
