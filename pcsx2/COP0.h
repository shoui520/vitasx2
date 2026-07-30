// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

struct tlbs;

extern void WriteCP0Status(u32 value);
extern void WriteCP0Config(u32 value);
extern void cpuUpdateOperationMode();
extern void WriteTLB(int i);
extern void UnmapTLB(const tlbs& t, int i);
extern void MapTLB(const tlbs& t, int i);

// Materialize the architectural Count value at cpuRegs.cycle. Count is
// otherwise represented by CP0.Count plus the cycle-lastCOP0Cycle delta.
extern void COP0_UpdateCount();
extern void COP0_UpdatePCCR();
extern void COP0_DiagnosticPCCR();
