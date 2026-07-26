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

} // namespace VitaGpuVu
