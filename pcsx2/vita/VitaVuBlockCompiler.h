// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Vita VU micro-mode block providers. VU1 compiles straight-line upper/lower
// instruction pairs from VU1 micro memory into A32 code that reproduces
// PCSX2's VU1microInterp.cpp::_vu1Exec() step semantics exactly: per-step
// cycle/budget accounting, VUops.cpp stall/pipe bookkeeping, hazard
// backup/discard rules, branch-delay countdown, and E-bit termination.
// VU0 currently uses the same pair emitter for scan-proven bodies, including
// lower LSU with VU0's data-memory/register-window map, VU0 XGKICK no-op
// behavior, VU0 branch-delay continuations, and VU0 M/D/T/E-bit stop handling.
// It falls back to PCSX2's VU0 interpreter for COP2 macro-visibility or other
// unsupported pairs.
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
		u32 budget_countdown_blocks = 0;
		u32 budget_countdown_pairs = 0;
		u32 test_pipes_fast_guard_pairs = 0;
		u32 fmac_clear_inline_pairs = 0;
		u32 upper_fmac_stall_test_inline_pairs = 0;
		u32 lower_fmac_stall_test_inline_pairs = 0;
		u32 upper_fmac_stall_inline_pairs = 0;
		u32 lower_fmac_stall_inline_pairs = 0;
		u32 upper_addsub_inline_pairs = 0;
		u32 upper_mul_inline_pairs = 0;
		u32 upper_maddmsub_inline_pairs = 0;
		u32 upper_outer_inline_pairs = 0;
		u32 upper_nop_inline_pairs = 0;
		u32 upper_unary_inline_pairs = 0;
		u32 upper_clip_inline_pairs = 0;
		u32 upper_minmax_inline_pairs = 0;
		u32 lower_ialu_inline_pairs = 0;
		u32 lower_flag_inline_pairs = 0;
		u32 lower_move_inline_pairs = 0;
		u32 lower_lsu_inline_pairs = 0;
		u32 lower_control_inline_pairs = 0;
		u32 lower_branch_inline_pairs = 0;
		u32 lower_fdiv_inline_pairs = 0;
		u32 lower_efu_inline_pairs = 0;
		u32 lower_xgkick_inline_pairs = 0;
		u32 lower_fdiv_stall_test_inline_pairs = 0;
		u32 lower_efu_stall_test_inline_pairs = 0;
		u32 lower_branch_stall_test_inline_pairs = 0;
		u32 lower_stall_inline_pairs = 0;
		u32 dt_flag_inline_pairs = 0;
		u32 branch_continuation_blocks = 0;
		u32 branch_continuation_pairs = 0;
		u32 ebit_continuation_blocks = 0;
		u32 ebit_continuation_pairs = 0;
		u32 invalidate_alls = 0;
		u32 content_cache_hits = 0;
		u32 direct_link_exits = 0;
		u32 direct_link_patches = 0;
		u32 direct_link_runtime_patches = 0;
		u32 direct_link_runtime_observed_slots = 0;
		u32 code_cache_resets = 0;
		size_t code_cache_used = 0;
		size_t code_cache_capacity = 0;
	};

	// recMicroVU1::Execute() body: mirrors InterpVU1::Execute()'s loop while
	// routing eligible windows through compiled blocks.
	void ExecuteVu1Blocks(u32 cycles);

	// recMicroVU1::Clear() hook. VU1 micro writes clear the affected quick
	// block-map slots while retaining compiled blocks for content-matched
	// reuse, mirroring x86 microVU's Clear()+program-search split.
	void InvalidateVu1Blocks(u32 addr, u32 size);

	// recMicroVU1::Reset()/Shutdown() hooks.
	void ResetVu1Blocks();
	void ShutdownVu1Blocks();

	Vu1ProviderStats GetVu1ProviderStats();
	void ResetVu1ProviderStats();

	// recMicroVU0::Execute() body: mirrors InterpVU0::Execute()'s loop while
	// routing scan-proven VU0 micro bodies through compiled A32 blocks.
	void ExecuteVu0Blocks(u32 cycles);

	// recMicroVU0::Clear()/Reset()/Shutdown() hooks.
	void InvalidateVu0Blocks(u32 addr, u32 size);
	void ResetVu0Blocks();
	void ShutdownVu0Blocks();

	Vu1ProviderStats GetVu0ProviderStats();
	void ResetVu0ProviderStats();
}
