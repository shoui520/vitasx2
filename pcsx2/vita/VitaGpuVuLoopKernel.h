// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "vita/VitaGpuVuProgram.h"

#include <array>
#include <string>
#include <vector>

namespace VitaGpuVu {

enum class ScalarDomain : u8 {
  Raw,
  Float,
  SignedInt,
  UnsignedInt,
};

enum class ExpressionKind : u8 {
  ConstantFloat,
  ConstantSigned,
  ConstantUnsigned,
  InitialVf,
  InitialAcc,
  InitialQ,
  InitialP,
  InitialI,
  InvariantVf,
  InvariantAcc,
  InvariantQ,
  InvariantP,
  InvariantI,
  Memory,
  Add,
  Subtract,
  Multiply,
  Divide,
  Minimum,
  Maximum,
  Absolute,
  Negate,
  Reciprocal,
  SquareRoot,
  ReciprocalSquareRoot,
  FloatToInt,
  IntToFloat,
  Normalize,
};

// One VU-memory qword address expressed at the natural-loop entry. The
// invocation coefficient is in qwords and the complete address is reduced
// modulo VU1_MEMSIZE / 16 by the eventual stream binder.
struct AffineQwordAddress {
  u8 base_vi = 0;
  s32 invocation_coefficient = 0;
  s32 qword_offset = 0;
  bool valid = false;

  bool operator==(const AffineQwordAddress &other) const {
    return base_vi == other.base_vi &&
           invocation_coefficient == other.invocation_coefficient &&
           qword_offset == other.qword_offset && valid == other.valid;
  }
};

struct ExpressionNode {
  ExpressionKind kind = ExpressionKind::ConstantFloat;
  ScalarDomain domain = ScalarDomain::Raw;
  std::array<u32, 3> operands{};
  AffineQwordAddress memory_address;
  u32 immediate = 0;
  u8 reg = 0;
  u8 lane = 0;
};

struct LoopStore {
  u32 pair_pc = 0;
  AffineQwordAddress address;
  std::array<u32, 4> values{};
  u8 write_mask = 0;
  u8 source_vf = 0;
};

struct ViEvolution {
  std::array<s32, 16> step{};
  std::array<std::vector<s32>, 16> prefix;
  u32 affine_mask = 0xffff;
};

// One VF lane whose every write in the reachable region is an idempotent
// self-clamp against a hardwired VF00 lane, i.e. `vfN.l = max(vfN.l, c)` or
// `mini`. Such a lane is a usable entry uniform exactly when the runtime seed
// already satisfies the clamp: the region is then a no-op on that lane, so the
// CPU snapshot can never become stale behind an accepted GPU draw. The
// descriptor path verifies the seed once per draw; nothing is assumed about
// the program's identity.
struct ClampStableLane {
  u8 reg = 0;
  u8 lane = 0;
  u32 bound_bits = 0;
  bool minimum = false;

  bool operator==(const ClampStableLane &other) const {
    return reg == other.reg && lane == other.lane &&
           bound_bits == other.bound_bits && minimum == other.minimum;
  }
};

struct ParallelLoopKernel {
  u32 loop_index = 0;
  u32 header_pc = 0;
  u32 latch_pc = 0;
  u32 pair_count = 0;
  u32 maximum_backedge_distance = 0;
  u32 collapsed_idempotent_recurrences = 0;
  ViEvolution vi;
  std::vector<ExpressionNode> expressions;
  std::vector<LoopStore> stores;
  std::array<u8, 32> stable_initial_vf_lanes{};
  std::vector<ClampStableLane> clamp_stable_lanes;
  u8 stable_initial_acc_lanes = 0;
  bool stable_initial_q = false;
  bool stable_initial_p = false;
  bool stable_initial_i = false;
  bool acyclic_entry_inlined = false;
  bool requires_dynamic_entry_state = false;
  bool has_true_recurrence = false;
  bool has_unsupported_expression = false;
  bool independent_store_values = false;
};

// Builds the steady-state semantic kernel of one natural loop. Values are
// sliced backward through architectural VF/ACC/Q/P/I state and across a
// bounded number of backedges. A slice is parallel only when it terminates in
// affine VU-memory loads or invariant VU state. This is dependence analysis;
// ShaccCg remains the whole-program SSA/register allocator.
bool BuildParallelLoopKernel(const ProgramAnalysis &program, u32 loop_index,
                             ParallelLoopKernel *kernel, std::string *error);

// Replaces loop-entry invariant leaves with the PairPlan-defined arithmetic
// and fixed-address VU-memory loads in the acyclic region from the external
// entry to the natural-loop header. Every pair reads one input snapshot and
// commits lower then upper writes, preserving simultaneous-pair semantics and
// upper priority. Values which genuinely originate outside the program remain
// Initial* leaves; lanes written anywhere in the program are marked unstable
// so direct admission cannot reuse a stale seed across invocations.
bool InlineAcyclicEntrySlice(const ProgramAnalysis &program, u32 loop_index,
                            ParallelLoopKernel *kernel,
                            std::string *error);

} // namespace VitaGpuVu
