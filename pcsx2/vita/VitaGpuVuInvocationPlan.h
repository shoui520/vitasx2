// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuLoopKernel.h"
#include "vita/VitaGpuVuVifInput.h"

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
  EqualU16,
  NotEqualU16,
  LessThanZeroS16,
  GreaterThanZeroS16,
  LessEqualZeroS16,
  GreaterEqualZeroS16,
  BooleanAnd,
  BooleanNot,
  CountUntilZeroU16,
  ScaleAddU16,
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

struct FinalViAlternative {
  u32 predicate = 0;
  std::array<u32, 16> values{};
};

struct ParallelInvocationPlan {
  u32 loop_index = 0;
  std::vector<InvocationValueNode> values;
  std::array<InvocationValueSet, 16> loop_entry_vi;
  std::array<std::array<InvocationValueSet, 4>, 32> loop_entry_vf;
  InvocationValueSet loop_counter;
  // The packed tag may have been loaded before the current MSCAL/MSCNT entry
  // and therefore exist only in the architectural VF snapshot.  Preserve the
  // four proven words rather than requiring every valid copy to be reducible
  // to a current VU-memory address.
  std::array<InvocationValueSet, 4> gif_tag_words;
  // Optional provenance retained for diagnostics and memory-backed fixtures.
  InvocationValueSet gif_tag_qword_address;
  std::vector<FinalViAlternative> final_vi_alternatives;
  u32 final_vi_write_mask = 0;
  bool has_static_gif_source = false;
  bool has_final_vi_state = false;
};

struct InvocationEvaluationContext {
  u16 vif_top = 0;
  u16 vif_itop = 0;
  const u16* initial_vi = nullptr;
  const u32* initial_vf_words = nullptr;
  const u32* initial_acc_words = nullptr;
  // A speculative generated transaction may deliberately leave persistent
  // values unevaluated until a real architectural observer.  The backing
  // arrays still contain the preceding known value, so every consumer must
  // reject a lane marked unavailable instead of accidentally treating that
  // stale word as an input.  A null VF mask means canonical/all-available
  // state for existing callers and validation fixtures.
  const u8* unavailable_initial_vf_lanes = nullptr;
  u8 unavailable_initial_acc_lanes = 0;
  u32 initial_q = 0;
  u32 initial_p = 0;
  u32 initial_i = 0;
  bool initial_q_available = true;
  bool initial_p_available = true;
  bool initial_i_available = true;
  void* memory_user = nullptr;
  bool (*read_memory_u16)(void* user, u16 qword_address, u8 lane,
                          u16* value) = nullptr;
  bool (*read_memory_u32)(void* user, u16 qword_address, u8 lane,
                          u32* value) = nullptr;
  // Copies complete consecutive VU-memory qwords, wrapping in the 16 KiB
  // architectural window. Product callers use this to resolve immutable VIF
  // overlays once per range instead of paying four layered callbacks per
  // qword while assembling a generated root's compact BUFFER0 input.
  bool (*read_memory_qwords)(void* user, u16 first_qword, u32 qword_count,
                             u32* values) = nullptr;
  // True only when a complete non-wrapping qword range still belongs to the
  // CPU-visible generation which GXM maps once for direct read access. This is
  // provenance, not a value comparison: a later write of identical bits does
  // not restore canonical ownership.
  bool (*memory_qwords_have_canonical_owner)(void* user, u16 first_qword,
                                             u32 qword_count) = nullptr;
  // O(1) provenance query for memory which no longer belongs to canonical
  // VU1.Mem but is still backed by one complete immutable VIF span. This is
  // deliberately separate from read_memory_qwords: equal copied values do not
  // establish a GPU-readable owner.
  bool (*bind_memory_qwords_to_raw_payload)(
      void* user, u16 first_qword, s32 outer_invocation_coefficient,
      u32 outer_invocation_count, s32 child_invocation_coefficient,
      u32 child_invocation_count, RawVifPayloadRef* payload,
      RawQwordBinding* binding) = nullptr;

  bool InitialVfLaneAvailable(u32 reg, u32 lane) const {
    return reg < 32u && lane < 4u &&
           (!unavailable_initial_vf_lanes ||
            (unavailable_initial_vf_lanes[reg] & (0x8u >> lane)) == 0u);
  }
  bool InitialVfRegisterAvailable(u32 reg) const {
    return reg < 32u &&
           (!unavailable_initial_vf_lanes ||
            (unavailable_initial_vf_lanes[reg] & 0x0fu) == 0u);
  }
  bool InitialAccLaneAvailable(u32 lane) const {
    return lane < 4u &&
           (unavailable_initial_acc_lanes & (0x8u >> lane)) == 0u;
  }
};

// Descriptor formulas refer to nodes by their dense ParallelInvocationPlan
// index. Reuse one indexed cache for every value needed by a descriptor rather
// than constructing std::map/std::set nodes for each query. Begin() preserves
// capacity, so the steady-state MTVU path performs no evaluator allocation.
struct InvocationEvaluationWorkspace {
  void Begin(size_t node_count);

  std::vector<u32> values;
  // 0 = unseen, 1 = active (cycle guard), 2 = cached.
  std::vector<u8> states;
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
bool EvaluateInvocationValue(const ParallelInvocationPlan& plan,
                             const InvocationValueSet& value,
                             const InvocationEvaluationContext& context,
                             InvocationEvaluationWorkspace* workspace,
                             u32* result);

// Evaluates the PairPlan-derived exit formulas for one accepted invocation.
// The formulas are built once with the program analysis; this performs no
// runtime instruction decoding, VU-pair execution, or per-vertex work.
bool EvaluateFinalViState(const ParallelInvocationPlan& plan,
                          const InvocationEvaluationContext& context,
                          std::array<u16, 16>* values, u32* write_mask);
bool EvaluateFinalViState(const ParallelInvocationPlan& plan,
                          const InvocationEvaluationContext& context,
                          InvocationEvaluationWorkspace* workspace,
                          std::array<u16, 16>* values, u32* write_mask);

} // namespace VitaGpuVu
