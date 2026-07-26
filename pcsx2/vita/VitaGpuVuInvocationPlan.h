// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuLoopKernel.h"

#include <array>
#include <string>
#include <vector>

namespace VitaGpuVu {

enum class InvocationValueKind : u8 {
  Constant,
  InitialVi,
  InitialVfWord,
  VifTop,
  VifItop,
  AddU16,
  AddConstantU16,
  SubtractU16,
  AndU16,
  OrU16,
  SignExtendU16,
  MemoryU16,
  MemoryU32,
};

struct InvocationValueNode {
  InvocationValueKind kind = InvocationValueKind::Constant;
  std::array<u32, 2> operands{};
  u32 immediate = 0;
  u8 reg = 0;
  u8 lane = 0;
};

// A small finite set represents control-flow joins. It is descriptor
// provenance, not shader SSA: every alternative must evaluate to the same
// runtime value before a direct draw may use it.
struct InvocationValueSet {
  std::vector<u32> alternatives;
  bool unknown = true;

  bool operator==(const InvocationValueSet& other) const {
    return unknown == other.unknown && alternatives == other.alternatives;
  }
};

struct ParallelInvocationPlan {
  u32 loop_index = 0;
  std::vector<InvocationValueNode> values;
  std::array<InvocationValueSet, 16> loop_entry_vi;
  std::array<std::array<InvocationValueSet, 4>, 32> loop_entry_vf;
  InvocationValueSet loop_counter;
  InvocationValueSet gif_tag_qword_address;
  bool has_static_gif_source = false;
};

struct InvocationEvaluationContext {
  u16 vif_top = 0;
  u16 vif_itop = 0;
  const u16* initial_vi = nullptr;
  const u32* initial_vf_words = nullptr;
  void* memory_user = nullptr;
  bool (*read_memory_u16)(void* user, u16 qword_address, u8 lane,
                          u16* value) = nullptr;
  bool (*read_memory_u32)(void* user, u16 qword_address, u8 lane,
                          u32* value) = nullptr;
};

// Recovers loop-entry VI/VF provenance and the packed GIF-tag source through
// the acyclic entry region. This executes no VU arithmetic and does no
// per-vertex work; it is the once-per-program descriptor contract.
bool BuildParallelInvocationPlan(const ProgramAnalysis& program,
                                 const ParallelLoopKernel& kernel,
                                 ParallelInvocationPlan* plan,
                                 std::string* error);

// Evaluates a descriptor-scale symbolic value. Direct binding requires all
// control-flow alternatives to agree after 16-bit VU integer wrapping.
bool EvaluateInvocationValue(const ParallelInvocationPlan& plan,
                             const InvocationValueSet& value,
                             const InvocationEvaluationContext& context,
                             u32* result);

} // namespace VitaGpuVu
