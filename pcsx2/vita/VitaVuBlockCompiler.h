// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Vita VU1 micro-mode block provider. Compiles straight-line upper/lower
// instruction pairs from VU1 micro memory into A32 code that reproduces
// PCSX2's VU1microInterp.cpp::_vu1Exec() step semantics exactly: per-step
// cycle/budget accounting, VUops.cpp stall/pipe bookkeeping, hazard
// backup/discard rules, branch-delay countdown, and E-bit termination.
// Pairs the scanner cannot prove compilable fall back to the PCSX2
// interpreter per step through the same dispatch loop.
namespace VitaVU
{
	struct Vu1ProviderStats
	{
		u64 executed_blocks = 0;
		u64 executed_pairs = 0;
		u64 interpreter_steps = 0;
		u32 compiled_blocks = 0;
		u32 compiled_pairs = 0;
		u32 scan_rejects = 0;
		u32 compile_failures = 0;
		u32 invalidate_alls = 0;
		u32 code_cache_resets = 0;
		size_t code_cache_used = 0;
		size_t code_cache_capacity = 0;
	};

	// recMicroVU1::Execute() body: mirrors InterpVU1::Execute()'s loop while
	// routing eligible windows through compiled blocks.
	void ExecuteVu1Blocks(u32 cycles);

	// recMicroVU1::Clear() hook. Any write to VU1 micro memory discards all
	// compiled blocks (whole-program invalidation, like x86 microVU's
	// program-level Clear()) and the shared decoded-op cache range.
	void InvalidateVu1Blocks(u32 addr, u32 size);

	// recMicroVU1::Reset()/Shutdown() hooks.
	void ResetVu1Blocks();
	void ShutdownVu1Blocks();

	Vu1ProviderStats GetVu1ProviderStats();
	void ResetVu1ProviderStats();
}
