// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeExecutor.h"

#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"

namespace
{
	using GeneratedBlock = u32 (*)();

	constexpr size_t STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 4096;

	extern "C" __attribute__((noinline)) u32 VitaEeA32DirectExit()
	{
		return static_cast<u32>(VitaEE::BlockExitKind::Direct);
	}

	extern "C" __attribute__((noinline)) u32 VitaEeA32EventExit()
	{
		return static_cast<u32>(VitaEE::BlockExitKind::Event);
	}

	bool DecodeExitKind(u32 value, VitaEE::BlockExitKind* exit)
	{
		if (value == static_cast<u32>(VitaEE::BlockExitKind::Direct))
		{
			*exit = VitaEE::BlockExitKind::Direct;
			return true;
		}

		if (value == static_cast<u32>(VitaEE::BlockExitKind::Event))
		{
			*exit = VitaEE::BlockExitKind::Event;
			return true;
		}

		return false;
	}
} // namespace

namespace VitaEE
{
	void BlockExecutor::Reset()
	{
		m_code.Release();
	}

	bool BlockExecutor::ExecuteStraightLineBlockOrInterpreterStep(u32 start_pc, u32 instruction_count,
		bool run_event_test_on_event_exit, BlockExecutionResult* result)
	{
		if (!result || instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return false;

		*result = {};

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if (!BlockCompiler::CanCompileOpcode(op))
			{
				cpuRegs.pc = start_pc;
				intCpu.Step(); // PCSX2 interpreter owner: Interpreter.cpp::execI().

				result->path = BlockExecutionPath::InterpreterStep;
				result->exit = BlockExitKind::InterpreterStep;
				result->exit_value = static_cast<u32>(BlockExitKind::InterpreterStep);
				return true;
			}
		}

		if (!m_code.Data())
		{
			if (!m_code.Allocate(STRAIGHT_LINE_BLOCK_CODE_CAPACITY))
				return false;
		}
		else
		{
			m_code.Reset();
		}

		BlockCompiler compiler(m_code);
		u32 scaled_cycles = 0;
		if (!compiler.CompileStraightLineBlock(start_pc, instruction_count,
				reinterpret_cast<const void*>(&VitaEeA32DirectExit),
				reinterpret_cast<const void*>(&VitaEeA32EventExit), &scaled_cycles) ||
			!m_code.Flush())
		{
			return false;
		}

		cpuRegs.pc = start_pc;
		const u32 exit_value = reinterpret_cast<GeneratedBlock>(m_code.EntryPoint())();

		BlockExitKind exit = BlockExitKind::Direct;
		if (!DecodeExitKind(exit_value, &exit))
			return false;

		// Mirrors the x86 provider's DispatcherEvent -> recEventTest() path in
		// x86/ix86-32/iR5900.cpp. Bare generated-code smokes can leave this off
		// until the full VM scheduler/device state is initialized.
		if (exit == BlockExitKind::Event && run_event_test_on_event_exit)
			_cpuEventTest_Shared();

		result->path = BlockExecutionPath::Compiled;
		result->exit = exit;
		result->exit_value = exit_value;
		result->scaled_cycles = scaled_cycles;
		result->code_size = m_code.Size();
		return true;
	}
} // namespace VitaEE
