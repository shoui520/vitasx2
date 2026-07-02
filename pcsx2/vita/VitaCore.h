// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Vita CPU provider selection. Bring-up executables can force interpreter or
// A32 EE mode directly; VMManager.cpp::UpdateCPUImplementations() uses the
// configured selector.
void VitaSelectInterpreterCpuProviders();
void VitaSelectA32EeCpuProviders();
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
	u32 invalidated_blocks = 0;
};

void VitaResetA32EeProviderStats();
VitaA32EeProviderStats VitaGetA32EeProviderStats();
void VitaRequestA32EeCacheReset();

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

// Mirrors the PCSX2 DebugTools/IopTrace.cpp::RecordIopPreInstruction hook point
// in R3000AInterpreter.cpp::execI() for Vita bring-up trace executables.
using VitaIopPreInstructionTraceCallback = bool (*)(u32 pc, u32 opcode);
void VitaSetIopPreInstructionTraceCallback(VitaIopPreInstructionTraceCallback callback);
bool VitaRecordIopPreInstruction(u32 pc, u32 opcode);
