// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Mirrors VMManager.cpp::UpdateCPUImplementations() for the Vita bring-up
// executables before the full Vita VM lifecycle exists.
void VitaSelectInterpreterCpuProviders();

// Mirrors the PCSX2 DebugTools/EeTrace.cpp::RecordEePreInstruction hook point
// in Interpreter.cpp::execI() for Vita bring-up trace executables.
// Return true only from Cpu->Execute() flows; Cpu->Step() users should stop
// externally after recording the needed instruction count.
using VitaEePreInstructionTraceCallback = bool (*)(u32 pc, u32 opcode);
void VitaSetEePreInstructionTraceCallback(VitaEePreInstructionTraceCallback callback);
bool VitaRecordEePreInstruction(u32 pc, u32 opcode);
