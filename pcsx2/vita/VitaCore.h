// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Vita CPU provider selection. Bring-up executables can force interpreter or
// A32 EE mode directly; VMManager.cpp::UpdateCPUImplementations() uses the
// configured selector.
void VitaSelectInterpreterCpuProviders();
void VitaSelectA32EeCpuProviders();
void VitaSelectA32IopCpuProviders();
void VitaSelectA32EeIopCpuProviders();
void VitaSelectConfiguredCpuProviders();

struct VitaA32EeProviderStats
{
	u32 compiled_blocks = 0;
	u32 compiled_instructions = 0;
	u32 interpreter_steps = 0;
	u32 direct_exits = 0;
	u32 event_exits = 0;
	u32 failed_blocks = 0;
	u32 cache_hits = 0;
	u32 cache_misses = 0;
	u32 lookup_hits = 0;
	u32 fast_dispatch_hits = 0;
	u32 invalidated_blocks = 0;
	u32 first_interpreter_pc = 0;
	u32 first_interpreter_opcode = 0;
	u32 first_interpreter_reason = 0;
	u32 last_interpreter_pc = 0;
	u32 last_interpreter_opcode = 0;
	u32 last_interpreter_reason = 0;
	u32 scan_unsupported_fallbacks = 0;
	u32 scan_boundary_fallbacks = 0;
	u32 exact_trace_branch_likely_fallbacks = 0;
	u32 execute_failed_fallbacks = 0;
	u32 interpreter_path_fallbacks = 0;
};

enum class VitaA32EeFallbackReason : u32
{
	None = 0,
	ScanUnsupportedOpcode = 1,
	ScanBoundary = 2,
	ExactTraceBranchLikely = 3,
	ExecuteFailed = 4,
	InterpreterPath = 5,
};

const char* VitaA32EeFallbackReasonName(VitaA32EeFallbackReason reason);
void VitaResetA32EeProviderStats();
VitaA32EeProviderStats VitaGetA32EeProviderStats();
void VitaRequestA32EeCacheReset();

struct VitaA32IopProviderStats
{
	u32 compiled_blocks = 0;
	u32 compiled_instructions = 0;
	u32 native_instructions = 0;
	u32 helper_instructions = 0;
	u32 executed_blocks = 0;
	u32 direct_exits = 0;
	u32 failed_blocks = 0;
	u32 interpreter_blocks = 0;
	u32 cache_hits = 0;
	u32 cache_misses = 0;
	u32 lookup_hits = 0;
	u32 fast_dispatch_hits = 0;
	u32 invalidated_blocks = 0;
	u32 code_cache_resets = 0;
	u32 first_interpreter_pc = 0;
	u32 first_interpreter_opcode = 0;
	u32 last_interpreter_pc = 0;
	u32 last_interpreter_opcode = 0;
};

void VitaResetA32IopProviderStats();
VitaA32IopProviderStats VitaGetA32IopProviderStats();

// Mirrors the fast-boot ELF state that VMManager.cpp::Initialize() seeds for
// R5900.cpp::eeloadHook() in Vita bring-up executables.
void VitaClearVmBootState();
void VitaSetFastBootElfOverride(const char* elf_path);
void VitaSetFastBootDisc();

// Mirrors the PCSX2 DebugTools/EeTrace.cpp::RecordEePreInstruction hook point
// in Interpreter.cpp::execI() for Vita bring-up trace executables.
// Return true only from Cpu->Execute() flows; Cpu->Step() users should stop
// externally after recording the needed instruction count.
using VitaEePreInstructionTraceCallback = bool (*)(u32 pc, u32 opcode);
void VitaSetEePreInstructionTraceCallback(VitaEePreInstructionTraceCallback callback);
bool VitaRecordEePreInstruction(u32 pc, u32 opcode);

// Optional trace-harness fast path for A32 exact-stream recording. The callback
// returns how many leading pre-instruction records in a straight-line native
// window were consumed by an outer skip gate without writing trace state.
using VitaEePreInstructionTraceWindowSkipCallback =
	u32 (*)(u32 start_pc, u32 first_opcode, u32 instruction_count);
void VitaSetEePreInstructionTraceWindowSkipCallback(
	VitaEePreInstructionTraceWindowSkipCallback callback);

enum class VitaA32EeTraceMode
{
	InstructionWindow,
	BlockBoundaryState,
};

// InstructionWindow records the executed pc/opcode stream by pre-recording
// straight-line block windows before native execution. BlockBoundaryState
// records only block starts, where the complete EE state is current and can be
// compared against PCSX2 full-state traces.
void VitaSetA32EeTraceMode(VitaA32EeTraceMode mode);

// Exact-stream trace mode for the A32 EE provider: recorded windows must
// match the executed instruction stream exactly, so branch-likely pairs are
// stepped through Interpreter.cpp::execI() (which records the delay slot only
// when it executes) instead of compiling natively. Requires initialized
// EE hardware because interpreter branches run intEventTest(). Oracle trace
// recorders enable this; synthetic provider validation keeps it off to prove
// native likely-branch blocks.
void VitaSetEeExactTraceStreams(bool enabled);

// Mirrors the PCSX2 DebugTools/IopTrace.cpp::RecordIopPreInstruction hook point
// in R3000AInterpreter.cpp::execI() for Vita bring-up trace executables.
using VitaIopPreInstructionTraceCallback = bool (*)(u32 pc, u32 opcode);
void VitaSetIopPreInstructionTraceCallback(VitaIopPreInstructionTraceCallback callback);
bool VitaIsIopPreInstructionTraceEnabled();
bool VitaRecordIopPreInstruction(u32 pc, u32 opcode);
