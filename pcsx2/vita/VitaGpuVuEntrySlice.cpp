// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuLoopKernel.h"

#include "VU.h"
#include "VUmicroFast.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#endif

namespace VitaGpuVu {
namespace {

using LowerKind = VUInterpFast::LowerFastKind;
using UpperKind = VUInterpFast::UpperFastKind;

constexpr u32 InvalidNode = 0;
constexpr u32 MaximumEntryTransfers = 4096;
constexpr u32 MaximumClosedFormOuterIterations = 64;
// Must remain equal to VitaGpuVuDraw.h's process-lifetime deferred evaluator
// workspace bound.
constexpr u32 MaximumClosedFormExpressions = 32768;
constexpr u32 MaximumClosedFormCompositionDepth = 1024;
constexpr u32 MaximumCompactOuterInputQwords = 64;
constexpr u32 VuMemoryQwordMask = (VU1_MEMSIZE / 16u) - 1u;

bool Fail(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
  return false;
}

bool LaneEnabled(u8 mask, u32 lane) {
  return (mask & (0x8u >> lane)) != 0;
}

bool IsQwordStore(LowerKind kind) {
  return kind == LowerKind::SQ || kind == LowerKind::SQI ||
         kind == LowerKind::SQD;
}

bool IsArchitecturalOutput(LowerKind kind) {
  return IsQwordStore(kind) || kind == LowerKind::ISW ||
         kind == LowerKind::ISWR || kind == LowerKind::XGKICK;
}

bool IsExactSelfCounterUpdate(const VitaVU::GpuPairPlan& plan, u32 counter,
                              s32 expected_step) {
  if (!plan.exec_lower || plan.lower_discarded_by_upper || counter == 0 ||
      (plan.lower_vi_write & (1u << counter)) == 0)
    return false;
  const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
  if (VUInterpFast::It(plan.lower) != counter ||
      VUInterpFast::Is(plan.lower) != counter)
    return false;
  s32 step = 0;
  switch (kind) {
  case LowerKind::IADDIU:
    step = VUInterpFast::Imm15(plan.lower);
    break;
  case LowerKind::ISUBIU:
    step = -VUInterpFast::Imm15(plan.lower);
    break;
  case LowerKind::IADDI:
    step = VUInterpFast::Imm5(plan.lower);
    break;
  default:
    return false;
  }
  return step == expected_step;
}

struct ViValue {
  u8 base_vi = 0;
  s32 offset = 0;
  bool known = false;

  bool operator==(const ViValue& other) const {
    return base_vi == other.base_vi && offset == other.offset &&
           known == other.known;
  }
};

struct EntryState {
  std::array<std::array<u32, 4>, 32> vf{};
  std::array<u32, 4> acc{};
  std::array<ViValue, 16> vi{};
  u32 q = InvalidNode;
  u32 p = InvalidNode;
  u32 i = InvalidNode;
};

struct EntryCountedLoopSummary {
  u32 loop_index = std::numeric_limits<u32>::max();
  u32 iteration_count = 0;
  u32 dynamic_pair_count = 0;
  u32 exact_prefix_pair_count = 0;
  ViValue counter_entry_value;
  ViValue counter_limit_value;
  bool requires_runtime_trip_attestation = false;
  bool runtime_control_captured = false;

  bool Valid() const {
    return loop_index != std::numeric_limits<u32>::max();
  }
};

using NodeCacheKey =
    std::tuple<ExpressionKind, ScalarDomain, std::array<u32, 3>, u8, s32,
               s32, bool, s32, u32, u8, u8>;

NodeCacheKey ExpressionKey(const ExpressionNode& node) {
  return {node.kind, node.domain, node.operands,
          node.memory_address.base_vi,
          node.memory_address.invocation_coefficient,
          node.memory_address.qword_offset, node.memory_address.valid,
          node.memory_address.outer_invocation_coefficient,
          node.immediate, node.reg, node.lane};
}

u32 ExpressionHash(const ExpressionNode& node) {
  u32 hash = 2166136261u;
  const auto mix = [&](u32 value) { hash = (hash ^ value) * 16777619u; };
  mix(static_cast<u32>(node.kind));
  mix(static_cast<u32>(node.domain));
  for (const u32 operand : node.operands)
    mix(operand);
  mix(node.memory_address.base_vi);
  mix(static_cast<u32>(node.memory_address.invocation_coefficient));
  mix(static_cast<u32>(node.memory_address.qword_offset));
  mix(node.memory_address.valid);
  mix(static_cast<u32>(node.memory_address.outer_invocation_coefficient));
  mix(node.immediate);
  mix(node.reg);
  mix(node.lane);
  // Fold high bits into the power-of-two bucket index too.
  return hash ^ (hash >> 16u);
}

// Refine only a failing allocation's label. Literal names survive unwinding;
// success leaves the enclosing semantic phase intact. No logging, allocation,
// retry or exception replacement occurs while the heap is exhausted.
template <typename Allocate>
decltype(auto) WithConstructionAllocationFailureStage(
    const char** stage, const char* failure_stage, Allocate&& allocate) {
  try {
    return std::forward<Allocate>(allocate)();
  } catch (const std::bad_alloc&) {
    if (stage)
      *stage = failure_stage;
    throw;
  }
}

// The finite outer composer only appends immutable nodes. Index those nodes
// by ID instead of duplicating every complete key in an individually allocated
// tree node. Hashes locate candidates only: equality still checks every field
// of the original key, preserving PCSX2 VUops-derived graph identity and IDs.
// Do not use this for entry/output editors which rewrite existing operands or
// memory addresses while retaining their pre-rewrite key snapshots.
class AppendOnlyExpressionGraphEditor {
public:
  explicit AppendOnlyExpressionGraphEditor(ParallelLoopKernel* kernel,
                                           const char** allocation_stage = nullptr)
      : m_kernel(kernel), m_allocation_stage(allocation_stage) {
    if (!m_kernel)
      return;
    size_t capacity = 64u;
    while (capacity < m_kernel->expressions.size() * 2u)
      capacity *= 2u;
    WithConstructionAllocationFailureStage(m_allocation_stage,
        "nested-expression-index-init", [&]() {
          m_slots.resize(capacity, InvalidNode);
        });
    for (u32 id = 1u; id < m_kernel->expressions.size(); id++) {
      const size_t slot = FindSlot(m_kernel->expressions[id], m_slots);
      if (m_slots[slot] == InvalidNode) {
        m_slots[slot] = id;
        m_entries++;
      }
    }
  }

  u32 Add(ExpressionNode node) {
    if (!m_kernel || m_overflow)
      return InvalidNode;
    size_t slot = FindSlot(node, m_slots);
    if (m_slots[slot] != InvalidNode)
      return m_slots[slot];
    if (m_kernel->expressions.size() >= MaximumClosedFormExpressions) {
      m_overflow = true;
      return InvalidNode;
    }
    // Keep at least half the slots empty; every linear probe terminates.
    // Only IDs survive graph-vector reallocations, never element pointers.
    if ((m_entries + 1u) * 2u > m_slots.size()) {
      std::vector<u32> grown = WithConstructionAllocationFailureStage(
          m_allocation_stage, "nested-expression-index-growth", [&]() {
            return std::vector<u32>(m_slots.size() * 2u, InvalidNode);
          });
      for (const u32 id : m_slots) {
        if (id != InvalidNode)
          grown[FindSlot(m_kernel->expressions[id], grown)] = id;
      }
      m_slots.swap(grown);
      slot = FindSlot(node, m_slots);
    }
    const u32 id = static_cast<u32>(m_kernel->expressions.size());
    WithConstructionAllocationFailureStage(m_allocation_stage,
        "nested-expression-node-growth", [&]() {
          auto& expressions = m_kernel->expressions;
          if (expressions.size() == expressions.capacity()) {
            // Vita libstdc++ vector::_M_realloc_append doubles a full vector
            // while its old allocation is still live. Physical R continuation
            // construction fails at this exact allocation. A 3/2 geometric
            // reserve reduces both the contiguous request and old+new peak;
            // it retains amortized append cost and the same semantic bound.
            // Only IDs index this append-only graph, and node was copied by
            // value before growth, so all PCSX2-derived roots stay unchanged.
            const size_t capacity = expressions.capacity();
            const size_t next_capacity = std::min<size_t>(
                MaximumClosedFormExpressions,
                capacity + std::max<size_t>(capacity / 2u, 1u));
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
            if (std::getenv("VITASX2_GENERATED_VU_CONSTRUCTION_MEMORY")) {
              std::printf("vu-jit-validation: expression-node-growth "
                          "nodes=%zu old_capacity=%zu new_capacity=%zu "
                          "element_bytes=%zu overlap_payload_bytes=%zu\n",
                          expressions.size(), capacity, next_capacity,
                          sizeof(ExpressionNode),
                          (capacity + next_capacity) * sizeof(ExpressionNode));
            }
#endif
            expressions.reserve(next_capacity);
          }
          m_kernel->expressions.push_back(node);
        });
    m_slots[slot] = id;
    m_entries++;
    return id;
  }

  bool Overflowed() const { return m_overflow; }

private:
  size_t FindSlot(const ExpressionNode& node,
                  const std::vector<u32>& slots) const {
    const size_t mask = slots.size() - 1u;
    size_t slot = ExpressionHash(node) & mask;
    while (slots[slot] != InvalidNode &&
           ExpressionKey(m_kernel->expressions[slots[slot]]) !=
               ExpressionKey(node)) {
      slot = (slot + 1u) & mask;
    }
    return slot;
  }

  ParallelLoopKernel* m_kernel = nullptr;
  const char** m_allocation_stage = nullptr;
  std::vector<u32> m_slots;
  size_t m_entries = 0u;
  bool m_overflow = false;
};

class EntrySliceBuilder {
public:
  EntrySliceBuilder(const ProgramAnalysis& program, const NaturalLoop& loop,
                    ParallelLoopKernel* kernel)
      : m_program(program), m_loop(loop), m_kernel(kernel),
        m_loop_expression_count(kernel->expressions.size()) {
    for (u32 id = 1; id < m_kernel->expressions.size(); id++)
      CacheNode(id);
  }

  bool Build(std::string* error) {
    ScanStableInitialState();
    EntryState header{};
    if (!BuildHeaderState(false, &header, error))
      return false;

    if (!ApplyEntryState(header, error))
      return false;
    if (m_overflow)
      return Fail(error, "acyclic entry expression slice exceeded its budget");
    m_kernel->acyclic_entry_inlined = true;
    if (error)
      error->clear();
    return true;
  }

  // Produces the state at this loop's first header visit.  `symbolic_vi`
  // retains each architectural entry VI as an affine base, which is required
  // by the closed-form enclosing-loop reducer.  This does not rewrite kernel
  // roots and is therefore safe to use as an input to a transactional proof.
  bool BuildHeaderState(bool symbolic_vi, EntryState* header,
                        std::string* error) {
    if (!BuildHeaderStateImpl(symbolic_vi, 0u, header, nullptr, error))
      return false;
    return !m_overflow ||
           Fail(error, "acyclic entry expression slice exceeded its budget");
  }

  // Extends the acyclic entry reduction with one finite, canonical counted
  // setup loop which dominates the selected parent header.  The loop is
  // unrolled only in the compile-time expression graph.  Its dynamic count is
  // retained for an O(1) immutable-input attestation before any future draw
  // can own the epoch.
  bool BuildHeaderStateWithCountedPrelude(
      bool symbolic_vi, u32 expected_iteration_count, EntryState* header,
      EntryCountedLoopSummary* summary, std::string* error) {
    if (!summary)
      return Fail(error, "null counted entry-loop summary");
    *summary = {};
    if (!BuildHeaderStateImpl(symbolic_vi, expected_iteration_count, header,
                              summary, error)) {
      return false;
    }
    return !m_overflow ||
           Fail(error, "acyclic entry expression slice exceeded its budget");
  }

  bool BuildHeaderStateImpl(bool symbolic_vi, u32 expected_iteration_count,
                            EntryState* header,
                            EntryCountedLoopSummary* summary,
                            std::string* error) {
    if (!header)
      return Fail(error, "null acyclic loop-header state");
    const u32 entry_block = FindEntryBlock();
    if (entry_block == std::numeric_limits<u32>::max())
      return Fail(error, "acyclic entry slice has no external entry block");
    if (m_loop.header_block >= m_program.blocks.size())
      return Fail(error, "acyclic entry slice has no loop header block");

    const EntryState initial = InitialState(symbolic_vi);
    EntryState candidate{};
    bool header_reached = false;
    EntryCountedLoopSummary selected_summary;
    if (expected_iteration_count != 0u &&
        !FindCountedEntryLoop(entry_block, expected_iteration_count,
                              &selected_summary, error)) {
      return false;
    }
    u32 exact_prefix_pairs = 0u;
    if (entry_block == m_loop.header_block) {
      candidate = initial;
      header_reached = true;
    } else if (!Propagate(entry_block, initial, &candidate, &header_reached,
                          selected_summary.Valid() ? &selected_summary : nullptr,
                          summary ? &exact_prefix_pairs : nullptr, error)) {
      return false;
    }
    if (!header_reached)
      return Fail(error, "acyclic entry slice did not reach the loop header");
    *header = std::move(candidate);
    if (summary) {
      selected_summary.exact_prefix_pair_count = exact_prefix_pairs;
      *summary = selected_summary;
    }
    if (error)
      error->clear();
    return true;
  }

  bool BuildEnclosingPrefix(
      const EnclosingLoopEntryIndependence& proof, std::string* error) {
    if (proof.child_loop != m_kernel->loop_index ||
        proof.parent_loop >= m_program.natural_loops.size()) {
      return Fail(error, "enclosing-prefix proof does not match kernel");
    }
    const NaturalLoop& parent = m_program.natural_loops[proof.parent_loop];
    if (proof.prefix_blocks.empty() ||
        proof.prefix_blocks.front() != parent.header_block) {
      return Fail(error, "enclosing-prefix proof has no parent header");
    }

    EntryState state = InitialState(true);
    for (u32 prefix_index = 0; prefix_index < proof.prefix_blocks.size();
         prefix_index++) {
      const u32 block_index = proof.prefix_blocks[prefix_index];
      if (block_index >= m_program.blocks.size() ||
          !std::binary_search(parent.blocks.begin(), parent.blocks.end(),
                              block_index)) {
        return Fail(error, "enclosing-prefix block is outside parent loop");
      }
      for (const ProgramPair& pair : m_program.blocks[block_index].pairs)
        TransferPair(pair.plan, &state);

      const u32 expected_successor =
          prefix_index + 1u < proof.prefix_blocks.size()
              ? proof.prefix_blocks[prefix_index + 1u]
              : m_loop.header_block;
      const bool has_successor = std::any_of(
          m_program.blocks[block_index].successors.begin(),
          m_program.blocks[block_index].successors.end(),
          [expected_successor](const ControlEdge& edge) {
            return edge.has_target && edge.target_block == expected_successor;
          });
      if (!has_successor)
        return Fail(error, "enclosing-prefix proof has a broken edge");
    }

    if (!PrepareChildEntryRoots(proof, state, error))
      return false;
    m_kernel->enclosing_final_state_pass_through =
        FinalStatePassesThrough();
    if (!ProveFinalViStatePassesThrough(parent, proof, error))
      return false;
    if (!ApplyEntryState(state, error))
      return false;
    if (m_overflow)
      return Fail(error, "enclosing-prefix expression slice exceeded its budget");
    m_kernel->enclosing_prefix_inlined = true;
    if (error)
      error->clear();
    return true;
  }

  bool BuildEnclosingSuffix(
      const EnclosingLoopEntryIndependence& boundary,
      const StructuredLoopTailProof& tail,
      EntryState* complete_exit_state, std::string* error) {
    if (!m_kernel->enclosing_prefix_inlined ||
        tail.child_loop != m_kernel->loop_index ||
        tail.parent_loop != boundary.parent_loop ||
        !tail.intermediate_suffix_proven || tail.suffix_blocks.empty()) {
      return Fail(error, "enclosing-suffix proof does not match kernel");
    }
    for (u32 reg = 1; reg < 32; reg++) {
      if (m_kernel->final_vf_lanes[reg] != 0x0f)
        return Fail(error, "enclosing suffix requires canonical child VF exit");
    }
    if (m_kernel->final_acc_lanes != 0x0f || !m_kernel->final_q ||
        !m_kernel->final_p || !m_kernel->final_i) {
      return Fail(error,
                  "enclosing suffix requires canonical child scalar exit");
    }

    EntryState state = InitialState(true);
    for (u32 reg = 1; reg < 32; reg++) {
      for (u32 lane = 0; lane < 4; lane++)
        state.vf[reg][lane] = m_kernel->final_vf_values[reg][lane];
    }
    state.acc = m_kernel->final_acc_values;
    state.q = m_kernel->final_q_value;
    state.p = m_kernel->final_p_value;
    state.i = m_kernel->final_i_value;
    for (const u32 block_index : tail.suffix_blocks) {
      if (block_index >= m_program.blocks.size())
        return Fail(error, "enclosing suffix block is out of range");
      for (const ProgramPair& pair : m_program.blocks[block_index].pairs)
        TransferPair(pair.plan, &state);
    }

    if (complete_exit_state) {
      // These are roots in m_kernel's graph, not an independently evaluated
      // kernel. Copying its expressions/stores retained a second graph which
      // no consumer read while composing the complete architectural exit.
      for (u32 reg = 1; reg < 32; reg++) {
        for (u32 lane = 0; lane < 4; lane++) {
          if (state.vf[reg][lane] == InvalidNode) {
            return Fail(error,
                        "enclosing suffix has unsupported architectural VF value");
          }
        }
      }
      for (u32 lane = 0; lane < 4; lane++) {
        if (state.acc[lane] == InvalidNode) {
          return Fail(error,
                      "enclosing suffix has unsupported architectural ACC value");
        }
      }
      if (state.q == InvalidNode || state.p == InvalidNode ||
          state.i == InvalidNode) {
        return Fail(error,
                    "enclosing suffix has unsupported architectural scalar value");
      }
      *complete_exit_state = state;
    }

    for (u32 reg = 1; reg < 32; reg++) {
      m_kernel->final_vf_lanes[reg] = boundary.parent_live_vf_lanes[reg];
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(m_kernel->final_vf_lanes[reg], lane)) {
          if (state.vf[reg][lane] == InvalidNode)
            return Fail(error, "enclosing suffix has unsupported live VF value");
          m_kernel->final_vf_values[reg][lane] = state.vf[reg][lane];
        }
      }
    }
    m_kernel->final_acc_lanes = boundary.parent_live_acc_lanes;
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(m_kernel->final_acc_lanes, lane)) {
        if (state.acc[lane] == InvalidNode)
          return Fail(error, "enclosing suffix has unsupported live ACC value");
        m_kernel->final_acc_values[lane] = state.acc[lane];
      }
    }
    m_kernel->final_q = boundary.parent_live_q;
    m_kernel->final_p = boundary.parent_live_p;
    m_kernel->final_i = boundary.parent_live_i;
    if ((m_kernel->final_q && state.q == InvalidNode) ||
        (m_kernel->final_p && state.p == InvalidNode) ||
        (m_kernel->final_i && state.i == InvalidNode)) {
      return Fail(error,
                  "enclosing suffix has unsupported live scalar value");
    }
    m_kernel->final_q_value = state.q;
    m_kernel->final_p_value = state.p;
    m_kernel->final_i_value = state.i;
    if (m_overflow)
      return Fail(error, "enclosing-suffix expression slice exceeded its budget");
    m_kernel->enclosing_suffix_inlined = true;
    if (error)
      error->clear();
    return true;
  }

private:
  bool ProveFinalViStatePassesThrough(
      const NaturalLoop& parent,
      const EnclosingLoopEntryIndependence& proof,
      std::string* error) {
    const u16 live = proof.parent_live_vi_mask;
    for (u32 reg = 1; reg < 16; reg++) {
      if (reg != parent.counter_reg && (live & (1u << reg)) != 0 &&
          (m_kernel->vi.affine_mask & (1u << reg)) == 0) {
        return Fail(error,
                    "child VI exit state is not an affine transition");
      }
    }

    const std::set<u32> prefix(proof.prefix_blocks.begin(),
                               proof.prefix_blocks.end());
    const std::set<u32> child_blocks(m_loop.blocks.begin(),
                                     m_loop.blocks.end());
    for (const u32 block_index : parent.blocks) {
      if (prefix.contains(block_index) || child_blocks.contains(block_index))
        continue;
      for (const ProgramPair& pair : m_program.blocks[block_index].pairs) {
        if (pair.plan.exec_lower && !pair.plan.lower_discarded_by_upper &&
            (pair.plan.lower_vi_write & live) != 0) {
          return Fail(error,
                      "enclosing-loop suffix mutates child-carried VI state");
        }
      }
    }
    m_kernel->enclosing_final_vi_state_pass_through = true;
    return true;
  }

  u32 FindInvariant(ExpressionKind kind, u32 reg, u32 lane) const {
    for (u32 id = 1; id < m_loop_expression_count; id++) {
      const ExpressionNode& node = m_kernel->expressions[id];
      if (node.kind == kind && node.reg == reg && node.lane == lane)
        return id;
    }
    return InvalidNode;
  }

  bool PrepareChildEntryRoots(
      const EnclosingLoopEntryIndependence& proof, const EntryState& state,
      std::string* error) {
    m_kernel->child_entry_vf_lanes = proof.demanded_vf_lanes;
    m_kernel->child_entry_acc_lanes = proof.demanded_acc_lanes;
    m_kernel->child_entry_q = proof.demanded_q;
    m_kernel->child_entry_p = proof.demanded_p;
    m_kernel->child_entry_i = proof.demanded_i;
    bool complete = true;
    for (u32 reg = 1; reg < proof.demanded_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (!LaneEnabled(proof.demanded_vf_lanes[reg], lane))
          continue;
        const u32 value =
            FindInvariant(ExpressionKind::InvariantVf, reg, lane);
        m_kernel->child_entry_vf_values[reg][lane] = value;
        complete &= value != InvalidNode;
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if (!LaneEnabled(proof.demanded_acc_lanes, lane))
        continue;
      const u32 value =
          FindInvariant(ExpressionKind::InvariantAcc, 0, lane);
      m_kernel->child_entry_acc_values[lane] = value;
      complete &= value != InvalidNode;
    }
    const auto scalar = [this, &complete](bool requested,
                                         ExpressionKind kind,
                                         u32* destination) {
      if (!requested)
        return;
      *destination = FindInvariant(kind, 0, 0);
      complete &= *destination != InvalidNode;
    };
    scalar(proof.demanded_q, ExpressionKind::InvariantQ,
           &m_kernel->child_entry_q_value);
    scalar(proof.demanded_p, ExpressionKind::InvariantP,
           &m_kernel->child_entry_p_value);
    scalar(proof.demanded_i, ExpressionKind::InvariantI,
           &m_kernel->child_entry_i_value);
    m_kernel->independent_child_entry_state = complete;
    if (!complete)
      return Fail(error, "enclosing prefix cannot represent child entry state");
    m_kernel->child_entry_vi_mask = static_cast<u16>(
        proof.demanded_vi_mask | proof.parent_live_vi_mask);
    for (u32 reg = 1; reg < state.vi.size(); reg++) {
      if ((m_kernel->child_entry_vi_mask & (1u << reg)) == 0)
        continue;
      const ViValue& value = state.vi[reg];
      if (!value.known) {
        return Fail(error,
                    "enclosing prefix cannot represent child VI entry state");
      }
      m_kernel->child_entry_vi_values[reg] = {
          value.base_vi, value.offset, true};
    }
    m_kernel->independent_child_entry_vi = true;
    return true;
  }

  bool FinalStatePassesThrough() const {
    const auto matches = [this](u32 value, ExpressionKind kind, u32 reg,
                                u32 lane) {
      if (value == InvalidNode || value >= m_loop_expression_count)
        return false;
      const ExpressionNode& node = m_kernel->expressions[value];
      return node.kind == kind && node.reg == reg && node.lane == lane;
    };
    for (u32 reg = 1; reg < m_kernel->final_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(m_kernel->final_vf_lanes[reg], lane) &&
            !matches(m_kernel->final_vf_values[reg][lane],
                     ExpressionKind::InvariantVf, reg, lane)) {
          return false;
        }
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(m_kernel->final_acc_lanes, lane) &&
          !matches(m_kernel->final_acc_values[lane],
                   ExpressionKind::InvariantAcc, 0, lane)) {
        return false;
      }
    }
    if (m_kernel->final_q &&
        !matches(m_kernel->final_q_value, ExpressionKind::InvariantQ, 0, 0))
      return false;
    if (m_kernel->final_p &&
        !matches(m_kernel->final_p_value, ExpressionKind::InvariantP, 0, 0))
      return false;
    return !m_kernel->final_i ||
           matches(m_kernel->final_i_value, ExpressionKind::InvariantI, 0, 0);
  }

  bool ApplyEntryState(const EntryState& header, std::string* error) {
    const u32 loop_expression_count =
        static_cast<u32>(m_loop_expression_count);
    for (u32 id = 1; id < loop_expression_count; id++) {
      ExpressionNode& node = m_kernel->expressions[id];
      u32 replacement = InvalidNode;
      switch (node.kind) {
      case ExpressionKind::InvariantVf:
        replacement = header.vf[node.reg][node.lane];
        break;
      case ExpressionKind::InvariantAcc:
        replacement = header.acc[node.lane];
        break;
      case ExpressionKind::InvariantQ:
        replacement = header.q;
        break;
      case ExpressionKind::InvariantP:
        replacement = header.p;
        break;
      case ExpressionKind::InvariantI:
        replacement = header.i;
        break;
      default:
        break;
      }
      if (replacement != InvalidNode)
        m_replacements[id] = replacement;
      else if (node.kind == ExpressionKind::InvariantVf ||
               node.kind == ExpressionKind::InvariantAcc ||
               node.kind == ExpressionKind::InvariantQ ||
               node.kind == ExpressionKind::InvariantP ||
               node.kind == ExpressionKind::InvariantI) {
        m_kernel->requires_dynamic_entry_state = true;
      }
    }

    for (u32 id = 1; id < loop_expression_count; id++) {
      ExpressionNode& node = m_kernel->expressions[id];
      for (u32& operand : node.operands) {
        const auto replacement = m_replacements.find(operand);
        if (replacement != m_replacements.end())
          operand = replacement->second;
      }
    }
    for (LoopStore& store : m_kernel->stores) {
      for (u32& value : store.values) {
        const auto replacement = m_replacements.find(value);
        if (replacement != m_replacements.end())
          value = replacement->second;
      }
    }
    const auto replace_root = [this](u32* value) {
      const auto replacement = m_replacements.find(*value);
      if (replacement != m_replacements.end())
        *value = replacement->second;
    };
    for (u32 reg = 1; reg < m_kernel->final_vf_values.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++)
        replace_root(&m_kernel->final_vf_values[reg][lane]);
    }
    for (u32& value : m_kernel->final_acc_values)
      replace_root(&value);
    replace_root(&m_kernel->final_q_value);
    replace_root(&m_kernel->final_p_value);
    replace_root(&m_kernel->final_i_value);
    for (u32 reg = 1; reg < m_kernel->child_entry_vf_values.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++)
        replace_root(&m_kernel->child_entry_vf_values[reg][lane]);
    }
    for (u32& value : m_kernel->child_entry_acc_values)
      replace_root(&value);
    replace_root(&m_kernel->child_entry_q_value);
    replace_root(&m_kernel->child_entry_p_value);
    replace_root(&m_kernel->child_entry_i_value);
    if (error && !error->empty())
      return false;
    return true;
  }
  u32 FindEntryBlock() const {
    for (u32 block = 0; block < m_program.blocks.size(); block++) {
      if (m_program.blocks[block].start_pc == m_program.start_pc)
        return block;
    }
    return std::numeric_limits<u32>::max();
  }

  NodeCacheKey Key(const ExpressionNode& node) const {
    return ExpressionKey(node);
  }

  void CacheNode(u32 id) {
    m_node_cache.emplace(Key(m_kernel->expressions[id]), id);
  }

  u32 AddNode(ExpressionNode node) {
    if (m_overflow)
      return InvalidNode;
    const NodeCacheKey key = Key(node);
    const auto existing = m_node_cache.find(key);
    if (existing != m_node_cache.end())
      return existing->second;
    if (m_kernel->expressions.size() >= MaximumClosedFormExpressions) {
      m_overflow = true;
      return InvalidNode;
    }
    const u32 id = static_cast<u32>(m_kernel->expressions.size());
    m_kernel->expressions.push_back(node);
    m_node_cache.emplace(key, id);
    return id;
  }

  u32 Unary(ExpressionKind kind, ScalarDomain domain, u32 operand,
            u32 immediate = 0) {
    if (operand == InvalidNode)
      return InvalidNode;
    ExpressionNode node{};
    node.kind = kind;
    node.domain = domain;
    node.operands[0] = operand;
    node.immediate = immediate;
    return AddNode(node);
  }

  u32 Binary(ExpressionKind kind, ScalarDomain domain, u32 left, u32 right) {
    if (left == InvalidNode || right == InvalidNode)
      return InvalidNode;
    ExpressionNode node{};
    node.kind = kind;
    node.domain = domain;
    node.operands[0] = left;
    node.operands[1] = right;
    return AddNode(node);
  }

  u32 FloatConstant(u32 bits) {
    ExpressionNode node{};
    node.kind = ExpressionKind::ConstantFloat;
    node.domain = ScalarDomain::Float;
    node.immediate = bits;
    return AddNode(node);
  }

  u32 Initial(ExpressionKind kind, u32 reg = 0, u32 lane = 0) {
    ExpressionNode node{};
    node.kind = kind;
    node.domain = ScalarDomain::Float;
    node.reg = static_cast<u8>(reg);
    node.lane = static_cast<u8>(lane);
    return AddNode(node);
  }

  EntryState InitialState(bool symbolic_vi = false) {
    EntryState state;
    for (u32 lane = 0; lane < 4; lane++) {
      state.vf[0][lane] =
          FloatConstant(lane == 3 ? 0x3f800000u : 0u);
      state.acc[lane] = Initial(ExpressionKind::InitialAcc, 0, lane);
    }
    for (u32 reg = 1; reg < state.vf.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++)
        state.vf[reg][lane] =
            Initial(ExpressionKind::InitialVf, reg, lane);
    }
    state.vi[0] = {0, 0, true};
    if (symbolic_vi) {
      for (u32 reg = 1; reg < state.vi.size(); reg++)
        state.vi[reg] = {static_cast<u8>(reg), 0, true};
    }
    state.q = Initial(ExpressionKind::InitialQ);
    state.p = Initial(ExpressionKind::InitialP);
    state.i = Initial(ExpressionKind::InitialI);
    return state;
  }

  // A lane is a usable entry uniform only when nothing this entry can execute
  // writes it. The invariant being protected is stale-seed reuse: once a draw
  // is accepted the CPU's VU state is deliberately not advanced, so any lane
  // the accepted region writes must never be re-read as an Initial* leaf on a
  // later invocation. Blocks unreachable from this entry cannot run and so
  // cannot invalidate the seed; an MSCNT resume which branches past its
  // prologue legitimately inherits that prologue's registers from the CPU
  // snapshot taken at the explicit MSCAL boundary.
  // VF00 is architecturally hardwired to (0, 0, 0, 1).
  static u32 Vf00LaneBits(u32 lane) {
    return lane == 3 ? 0x3f800000u : 0u;
  }

  // Lane mask of writes by this pair which are idempotent self-clamps against
  // a VF00 lane. `maxx.xyzw vfN, vfN, vf00x` is the canonical PS2 clamp: the
  // destination is also the first source, so each written lane is
  // `max(lane, constant)`. Broadcast forms still read the destination lane
  // from Fs, so the per-lane mapping is preserved. The `i`/`q` forms are
  // excluded because their bound is not a hardwired constant.
  static u8 SelfClampLanes(const VitaVU::GpuPairPlan& plan, bool* minimum,
                           u32* bound_lane) {
    if (!plan.exec_upper || plan.upper_vf_write == 0)
      return 0;
    if ((plan.upper_vi_write & (1u << REG_ACC_FLAG)) != 0)
      return 0;
    const UpperKind kind = static_cast<UpperKind>(plan.upper_kind);
    const bool is_minimum = IsMinimum(kind);
    if (!is_minimum && !IsMaximum(kind))
      return 0;
    if (UsesI(kind) || UsesQ(kind))
      return 0;
    if (VUInterpFast::Fs(plan.upper) != plan.upper_vf_write)
      return 0;
    if (VUInterpFast::Ft(plan.upper) != 0)
      return 0;
    *minimum = is_minimum;
    const s32 broadcast = BroadcastLane(kind);
    *bound_lane = broadcast >= 0 ? static_cast<u32>(broadcast)
                                 : std::numeric_limits<u32>::max();
    return plan.upper_vf_write_mask;
  }

  void ScanStableInitialState() {
    std::array<u8, 32> vf_writes{};
    std::array<u8, 32> clamp_writes{};
    std::array<u8, 32> other_writes{};
    std::array<std::array<ClampStableLane, 4>, 32> clamps{};
    u8 acc_writes = 0;
    bool q_write = false;
    bool p_write = false;
    bool i_write = false;
    for (const BasicBlock& block : m_program.blocks) {
      if (!block.reachable_from_entry)
        continue;
      for (const ProgramPair& pair : block.pairs) {
        const VitaVU::GpuPairPlan& plan = pair.plan;
        if (plan.exec_upper) {
          if (plan.upper_vf_write != 0) {
            vf_writes[plan.upper_vf_write] |= plan.upper_vf_write_mask;
            bool minimum = false;
            u32 bound_lane = 0;
            const u8 clamped = SelfClampLanes(plan, &minimum, &bound_lane);
            other_writes[plan.upper_vf_write] |=
                static_cast<u8>(plan.upper_vf_write_mask & ~clamped);
            for (u32 lane = 0; lane < 4; lane++) {
              if (!LaneEnabled(clamped, lane))
                continue;
              ClampStableLane candidate;
              candidate.reg = static_cast<u8>(plan.upper_vf_write);
              candidate.lane = static_cast<u8>(lane);
              candidate.minimum = minimum;
              candidate.bound_bits = Vf00LaneBits(
                  bound_lane == std::numeric_limits<u32>::max() ? lane
                                                                : bound_lane);
              ClampStableLane& recorded = clamps[plan.upper_vf_write][lane];
              // Repeated identical clamps stay idempotent; a differing clamp
              // of the same lane does not, so drop the whole lane.
              if (!LaneEnabled(clamp_writes[plan.upper_vf_write], lane))
                recorded = candidate;
              else if (!(recorded == candidate))
                continue;
              clamp_writes[plan.upper_vf_write] |=
                  static_cast<u8>(0x8u >> lane);
            }
          }
          if ((plan.upper_vi_write & (1u << REG_ACC_FLAG)) != 0)
            acc_writes |= plan.upper_vf_write_mask;
        }
        if (plan.exec_lower && !plan.lower_discarded_by_upper) {
          if (plan.lower_vf_write != 0) {
            vf_writes[plan.lower_vf_write] |= plan.lower_vf_write_mask;
            other_writes[plan.lower_vf_write] |= plan.lower_vf_write_mask;
          }
          q_write |= (plan.lower_vi_write & (1u << REG_Q)) != 0;
          p_write |= (plan.lower_vi_write & (1u << REG_P)) != 0;
          i_write |= plan.immediate_lower;
        }
      }
    }
    for (u32 reg = 1; reg < vf_writes.size(); reg++) {
      // A lane is recoverable from the entry seed only when every write to it
      // is an identical self-clamp. One ordinary write anywhere in the region
      // makes the lane unstable regardless of how many clamps also touch it.
      const u8 recoverable =
          static_cast<u8>(clamp_writes[reg] & ~other_writes[reg]);
      const u8 unrecoverable = static_cast<u8>(vf_writes[reg] & ~recoverable);
      m_kernel->stable_initial_vf_lanes[reg] =
          static_cast<u8>((~unrecoverable) & 0x0fu);
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(recoverable, lane))
          m_kernel->clamp_stable_lanes.push_back(clamps[reg][lane]);
      }
    }
    m_kernel->stable_initial_acc_lanes =
        static_cast<u8>((~acc_writes) & 0x0fu);
    m_kernel->stable_initial_q = !q_write;
    m_kernel->stable_initial_p = !p_write;
    m_kernel->stable_initial_i = !i_write;
  }

  static bool JoinNode(u32* destination, u32 source) {
    if (*destination == source)
      return false;
    if (*destination == InvalidNode)
      return false;
    *destination = InvalidNode;
    return true;
  }

  static bool JoinVi(ViValue* destination, const ViValue& source) {
    if (*destination == source)
      return false;
    if (!destination->known)
      return false;
    destination->known = false;
    destination->base_vi = 0;
    destination->offset = 0;
    return true;
  }

  static bool JoinState(EntryState* destination, const EntryState& source) {
    bool changed = false;
    for (u32 reg = 0; reg < destination->vf.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++)
        changed |= JoinNode(&destination->vf[reg][lane],
                            source.vf[reg][lane]);
    }
    for (u32 lane = 0; lane < 4; lane++)
      changed |= JoinNode(&destination->acc[lane], source.acc[lane]);
    for (u32 reg = 0; reg < destination->vi.size(); reg++)
      changed |= JoinVi(&destination->vi[reg], source.vi[reg]);
    changed |= JoinNode(&destination->q, source.q);
    changed |= JoinNode(&destination->p, source.p);
    changed |= JoinNode(&destination->i, source.i);
    return changed;
  }

  bool CanReachBlock(u32 start, u32 target, u32 excluded) const {
    if (start >= m_program.blocks.size() || target >= m_program.blocks.size() ||
        start == excluded)
      return false;
    std::vector<bool> visited(m_program.blocks.size(), false);
    std::vector<u32> work{start};
    visited[start] = true;
    while (!work.empty()) {
      const u32 block = work.back();
      work.pop_back();
      if (block == target)
        return true;
      for (const ControlEdge& edge : m_program.blocks[block].successors) {
        if (!edge.has_target || edge.target_block == excluded ||
            edge.target_block >= visited.size() ||
            visited[edge.target_block]) {
          continue;
        }
        visited[edge.target_block] = true;
        work.push_back(edge.target_block);
      }
    }
    return false;
  }

  bool FindCountedEntryLoop(u32 entry_block, u32 expected_iteration_count,
                            EntryCountedLoopSummary* summary,
                            std::string* error) const {
    if (!summary || expected_iteration_count == 0u ||
        expected_iteration_count > MaximumClosedFormOuterIterations) {
      return Fail(error, "counted entry-loop bound is invalid");
    }
    *summary = {};
    u32 candidate_count = 0u;
    for (u32 loop_index = 0u;
         loop_index < m_program.natural_loops.size(); loop_index++) {
      const NaturalLoop& loop = m_program.natural_loops[loop_index];
      if (loop.header_block >= m_program.blocks.size() ||
          loop.header_block == m_loop.header_block ||
          loop.parent_loop < m_program.natural_loops.size() ||
          !loop.single_entry || !loop.affine_counter ||
          !loop.branch_taken_repeats || loop.counter_reg == 0u ||
          loop.counter_reg >= 16u || loop.counter_limit_reg >= 16u ||
          loop.counter_step == 0 ||
          loop.blocks.size() != 1u ||
          loop.header_block != loop.latch_block ||
          static_cast<LowerKind>(loop.branch_kind) != LowerKind::IBNE) {
        continue;
      }
      const BasicBlock& block = m_program.blocks[loop.header_block];
      if (!block.reachable_from_entry || !block.has_branch ||
          !block.conditional_branch || block.branch_in_delay_slot ||
          block.ends_program || block.has_external_exit) {
        continue;
      }

      u32 backedge_count = 0u;
      u32 exit_count = 0u;
      u32 exit_block = std::numeric_limits<u32>::max();
      for (const ControlEdge& edge : block.successors) {
        if (!edge.has_target)
          continue;
        if (edge.target_block == loop.header_block) {
          backedge_count++;
        } else {
          exit_count++;
          exit_block = edge.target_block;
        }
      }
      if (backedge_count != 1u || exit_count != 1u ||
          exit_block >= m_program.blocks.size() ||
          !CanReachBlock(exit_block, m_loop.header_block,
                         std::numeric_limits<u32>::max()) ||
          (entry_block != loop.header_block &&
           CanReachBlock(entry_block, m_loop.header_block,
                         loop.header_block))) {
        continue;
      }

      u32 counter_updates = 0u;
      bool safe = true;
      for (const ProgramPair& pair : block.pairs) {
        const VitaVU::GpuPairPlan& plan = pair.plan;
        const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
        if (plan.ebit || plan.mflag || plan.dflag || plan.tflag ||
            plan.vi_backup_write ||
            (plan.exec_lower && !plan.lower_discarded_by_upper &&
             IsArchitecturalOutput(kind))) {
          safe = false;
          break;
        }
        if ((plan.lower_vi_write & (1u << loop.counter_limit_reg)) != 0u &&
            loop.counter_limit_reg != 0u) {
          safe = false;
          break;
        }
        if ((plan.lower_vi_write & (1u << loop.counter_reg)) != 0u) {
          if (!IsExactSelfCounterUpdate(plan, loop.counter_reg,
                                        loop.counter_step)) {
            safe = false;
            break;
          }
          counter_updates++;
        }
      }
      if (!safe || counter_updates != 1u)
        continue;

      candidate_count++;
      summary->loop_index = loop_index;
      summary->iteration_count = expected_iteration_count;
      const u64 dynamic_pairs =
          static_cast<u64>(block.pairs.size()) * expected_iteration_count;
      if (dynamic_pairs > std::numeric_limits<u32>::max())
        return Fail(error, "counted entry-loop pair total overflowed");
      summary->dynamic_pair_count = static_cast<u32>(dynamic_pairs);
      summary->requires_runtime_trip_attestation = true;
    }
    if (candidate_count > 1u)
      return Fail(error, "multiple counted entry loops dominate the parent");
    return true;
  }

  bool Propagate(u32 entry_block, const EntryState& initial,
                 EntryState* header, bool* header_reached,
                 EntryCountedLoopSummary* counted_loop,
                 u32* exact_prefix_pair_count, std::string* error) {
    std::vector<EntryState> inputs(m_program.blocks.size());
    std::vector<bool> reachable(m_program.blocks.size(), false);
    std::vector<bool> queued(m_program.blocks.size(), false);
    std::vector<u32> input_pair_counts(m_program.blocks.size(), 0u);
    std::vector<u32> work{entry_block};
    const std::set<u32> loop_blocks(m_loop.blocks.begin(),
                                    m_loop.blocks.end());
    inputs[entry_block] = initial;
    reachable[entry_block] = true;
    queued[entry_block] = true;
    u32 transfers = 0;

    while (!work.empty()) {
      const u32 block_index = work.back();
      work.pop_back();
      queued[block_index] = false;
      if (++transfers > MaximumEntryTransfers)
        return Fail(error, "acyclic entry expression slice did not converge");

      EntryState output = inputs[block_index];
      const BasicBlock& block = m_program.blocks[block_index];
      const bool summarize = counted_loop && counted_loop->Valid() &&
                             block_index == m_program.natural_loops[
                                                counted_loop->loop_index]
                                                .header_block;
      if (summarize && !counted_loop->runtime_control_captured) {
        const NaturalLoop& loop =
            m_program.natural_loops[counted_loop->loop_index];
        counted_loop->counter_entry_value =
            inputs[block_index].vi[loop.counter_reg];
        counted_loop->counter_limit_value =
            loop.counter_limit_reg == 0u
                ? ViValue{0u, 0, true}
                : inputs[block_index].vi[loop.counter_limit_reg];
        // A resumed VU program can derive this finite counter through ILW or
        // ILWR in its acyclic setup prefix.  The expression graph may still
        // be unrolled exactly because the output contract supplies the finite
        // count, but AffineViValue cannot encode the memory-loaded control.
        // Retain that distinction so product admission performs a bounded
        // PairPlan control replay against immutable epoch memory.
        counted_loop->runtime_control_captured =
            counted_loop->counter_entry_value.known &&
            counted_loop->counter_limit_value.known;
      }
      const u32 repetitions = summarize ? counted_loop->iteration_count : 1u;
      for (u32 repetition = 0u; repetition < repetitions; repetition++) {
        for (const ProgramPair& pair : block.pairs)
          TransferPair(pair.plan, &output);
      }
      if (summarize) {
        const NaturalLoop& loop =
            m_program.natural_loops[counted_loop->loop_index];
        const ViValue limit = loop.counter_limit_reg == 0u
                                  ? ViValue{0u, 0, true}
                                  : output.vi[loop.counter_limit_reg];
        if (!limit.known)
          return Fail(error, "counted entry-loop limit is unresolved");
        // The immutable-input trip attestation proves that this is the first
        // non-repeating branch result.  Publishing the exit value here avoids
        // retaining an unknown ILW-derived counter in the expression graph.
        output.vi[loop.counter_reg] = limit;
      }
      const u64 output_pair_count =
          static_cast<u64>(input_pair_counts[block_index]) +
          static_cast<u64>(block.pairs.size()) * repetitions;
      if (exact_prefix_pair_count &&
          output_pair_count > std::numeric_limits<u32>::max()) {
        return Fail(error, "acyclic entry pair count overflowed");
      }

      for (const ControlEdge& edge :
           block.successors) {
        if (!edge.has_target)
          continue;
        if (summarize && edge.target_block == block_index)
          continue;
        if (edge.target_block == m_loop.header_block) {
          if (!*header_reached) {
            *header = output;
            *header_reached = true;
            if (exact_prefix_pair_count)
              *exact_prefix_pair_count = static_cast<u32>(output_pair_count);
          } else {
            if (exact_prefix_pair_count &&
                *exact_prefix_pair_count != output_pair_count) {
              return Fail(error,
                          "entry paths have unequal exact pair counts");
            }
            JoinState(header, output);
          }
          continue;
        }
        if (loop_blocks.contains(edge.target_block))
          continue;
        bool changed = false;
        if (!reachable[edge.target_block]) {
          inputs[edge.target_block] = output;
          input_pair_counts[edge.target_block] =
              static_cast<u32>(output_pair_count);
          reachable[edge.target_block] = true;
          changed = true;
        } else {
          if (exact_prefix_pair_count &&
              input_pair_counts[edge.target_block] != output_pair_count) {
            return Fail(error, "entry join has unequal exact pair counts");
          }
          changed = JoinState(&inputs[edge.target_block], output);
        }
        if (changed && !queued[edge.target_block]) {
          queued[edge.target_block] = true;
          work.push_back(edge.target_block);
        }
      }
    }
    return true;
  }

  static ViValue AddVi(ViValue value, s32 delta) {
    if (value.known)
      value.offset += delta;
    return value;
  }

  static ViValue BinaryVi(const ViValue& left, const ViValue& right,
                          LowerKind kind) {
    if (!left.known || !right.known)
      return {};
    const bool left_constant = left.base_vi == 0;
    const bool right_constant = right.base_vi == 0;
    switch (kind) {
    case LowerKind::IADD:
      if (left_constant)
        return {right.base_vi, left.offset + right.offset, true};
      if (right_constant)
        return {left.base_vi, left.offset + right.offset, true};
      return {};
    case LowerKind::ISUB:
      if (right_constant)
        return {left.base_vi, left.offset - right.offset, true};
      return {};
    case LowerKind::IAND:
      if (left_constant && right_constant) {
        return {0, static_cast<u16>(left.offset) &
                       static_cast<u16>(right.offset), true};
      }
      return {};
    case LowerKind::IOR:
      if (left_constant && right_constant) {
        return {0, static_cast<u16>(left.offset) |
                       static_cast<u16>(right.offset), true};
      }
      return {};
    default:
      return {};
    }
  }

  u32 Memory(const ViValue& address, u32 lane) {
    if (!address.known)
      return InvalidNode;
    ExpressionNode node{};
    node.kind = ExpressionKind::Memory;
    node.domain = ScalarDomain::Float;
    node.memory_address.base_vi = address.base_vi;
    node.memory_address.invocation_coefficient = 0;
    node.memory_address.qword_offset = address.offset;
    node.memory_address.valid = true;
    node.lane = static_cast<u8>(lane);
    return AddNode(node);
  }

  static ViValue LoadAddress(const VitaVU::GpuPairPlan& plan,
                             const EntryState& old) {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    const u32 base = VUInterpFast::Is(code);
    switch (kind) {
    case LowerKind::LQ:
      return AddVi(old.vi[base], VUInterpFast::Imm11(code));
    case LowerKind::LQI:
      return old.vi[base];
    case LowerKind::LQD:
      return AddVi(old.vi[base], -1);
    default:
      return {};
    }
  }

  void AssignLowerVi(const VitaVU::GpuPairPlan& plan,
                     const EntryState& old, EntryState* state) {
    const u32 writes = plan.lower_vi_write & 0xffffu;
    for (u32 reg = 1; reg < state->vi.size(); reg++) {
      if ((writes & (1u << reg)) != 0)
        state->vi[reg] = {};
    }
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    const u32 is = VUInterpFast::Is(code);
    const u32 it = VUInterpFast::It(code);
    const u32 id = VUInterpFast::Id(code);
    switch (kind) {
    case LowerKind::IADDIU:
      if (it != 0)
        state->vi[it] = AddVi(old.vi[is], VUInterpFast::Imm15(code));
      break;
    case LowerKind::ISUBIU:
      if (it != 0)
        state->vi[it] = AddVi(old.vi[is], -VUInterpFast::Imm15(code));
      break;
    case LowerKind::IADDI:
      if (it != 0)
        state->vi[it] = AddVi(old.vi[is], VUInterpFast::Imm5(code));
      break;
    case LowerKind::IADD:
    case LowerKind::ISUB:
    case LowerKind::IAND:
    case LowerKind::IOR:
      if (id != 0)
        state->vi[id] = BinaryVi(old.vi[is], old.vi[it], kind);
      break;
    case LowerKind::LQI:
      if (is != 0)
        state->vi[is] = AddVi(old.vi[is], 1);
      break;
    case LowerKind::LQD:
      if (is != 0)
        state->vi[is] = AddVi(old.vi[is], -1);
      break;
    case LowerKind::SQI:
      if (it != 0)
        state->vi[it] = AddVi(old.vi[it], 1);
      break;
    case LowerKind::SQD:
      if (it != 0)
        state->vi[it] = AddVi(old.vi[it], -1);
      break;
    case LowerKind::XTOP:
      if (it != 0)
        state->vi[it] = {AffineViBaseVifTop, 0, true};
      break;
    case LowerKind::XITOP:
      if (it != 0)
        state->vi[it] = {AffineViBaseVifItop, 0, true};
      break;
    case LowerKind::BAL:
    case LowerKind::JALR:
      if (it != 0)
        state->vi[it] =
            {0, static_cast<s32>((plan.pc + 16u) / 8u), true};
      break;
    default:
      break;
    }
    state->vi[0] = {0, 0, true};
  }

  void AssignLowerVf(const VitaVU::GpuPairPlan& plan,
                     const EntryState& old, EntryState* state) {
    const u32 destination = plan.lower_vf_write;
    if (destination == 0)
      return;
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    const ViValue address = LoadAddress(plan, old);
    for (u32 lane = 0; lane < 4; lane++) {
      if (!LaneEnabled(plan.lower_vf_write_mask, lane))
        continue;
      switch (kind) {
      case LowerKind::LQ:
      case LowerKind::LQI:
      case LowerKind::LQD:
        state->vf[destination][lane] = Memory(address, lane);
        break;
      case LowerKind::MOVE:
        state->vf[destination][lane] =
            old.vf[VUInterpFast::Fs(code)][lane];
        break;
      case LowerKind::MR32:
        state->vf[destination][lane] =
            old.vf[VUInterpFast::Fs(code)][(lane + 1u) & 3u];
        break;
      case LowerKind::MFP:
        state->vf[destination][lane] = old.p;
        break;
      default:
        state->vf[destination][lane] = InvalidNode;
        break;
      }
    }
  }

  void AssignLowerScalar(const VitaVU::GpuPairPlan& plan,
                         const EntryState& old, EntryState* state) {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    if (plan.immediate_lower)
      state->i = FloatConstant(plan.lower);
    if ((plan.lower_vi_write & (1u << REG_Q)) != 0) {
      const u32 fs_lane = VUInterpFast::Fsf(code);
      const u32 ft_lane = (code >> 23) & 3u;
      if (kind == LowerKind::DIV) {
        state->q = Binary(ExpressionKind::Divide, ScalarDomain::Float,
                          old.vf[VUInterpFast::Fs(code)][fs_lane],
                          old.vf[VUInterpFast::Ft(code)][ft_lane]);
      } else if (kind == LowerKind::SQRT) {
        state->q = Unary(
            ExpressionKind::SquareRoot, ScalarDomain::Float,
            Unary(ExpressionKind::Absolute, ScalarDomain::Float,
                  old.vf[VUInterpFast::Ft(code)][ft_lane]));
      } else if (kind == LowerKind::RSQRT) {
        state->q = Binary(
            ExpressionKind::Divide, ScalarDomain::Float,
            old.vf[VUInterpFast::Fs(code)][fs_lane],
            Unary(ExpressionKind::SquareRoot, ScalarDomain::Float,
                  Unary(ExpressionKind::Absolute, ScalarDomain::Float,
                        old.vf[VUInterpFast::Ft(code)][ft_lane])));
      } else {
        state->q = InvalidNode;
      }
    }
    if ((plan.lower_vi_write & (1u << REG_P)) != 0)
      state->p = InvalidNode;
  }

  static bool IsAdd(UpperKind kind) {
    switch (kind) {
    case UpperKind::ADD:
    case UpperKind::ADDi:
    case UpperKind::ADDq:
    case UpperKind::ADDx:
    case UpperKind::ADDy:
    case UpperKind::ADDz:
    case UpperKind::ADDw:
    case UpperKind::ADDA:
    case UpperKind::ADDAi:
    case UpperKind::ADDAq:
    case UpperKind::ADDAx:
    case UpperKind::ADDAy:
    case UpperKind::ADDAz:
    case UpperKind::ADDAw:
      return true;
    default:
      return false;
    }
  }

  static bool IsSubtract(UpperKind kind) {
    switch (kind) {
    case UpperKind::SUB:
    case UpperKind::SUBi:
    case UpperKind::SUBq:
    case UpperKind::SUBx:
    case UpperKind::SUBy:
    case UpperKind::SUBz:
    case UpperKind::SUBw:
    case UpperKind::SUBA:
    case UpperKind::SUBAi:
    case UpperKind::SUBAq:
    case UpperKind::SUBAx:
    case UpperKind::SUBAy:
    case UpperKind::SUBAz:
    case UpperKind::SUBAw:
    case UpperKind::MSUB:
    case UpperKind::MSUBi:
    case UpperKind::MSUBq:
    case UpperKind::MSUBx:
    case UpperKind::MSUBy:
    case UpperKind::MSUBz:
    case UpperKind::MSUBw:
    case UpperKind::MSUBA:
    case UpperKind::MSUBAi:
    case UpperKind::MSUBAq:
    case UpperKind::MSUBAx:
    case UpperKind::MSUBAy:
    case UpperKind::MSUBAz:
    case UpperKind::MSUBAw:
    case UpperKind::OPMSUB:
      return true;
    default:
      return false;
    }
  }

  static bool IsMultiply(UpperKind kind) {
    switch (kind) {
    case UpperKind::MUL:
    case UpperKind::MULi:
    case UpperKind::MULq:
    case UpperKind::MULx:
    case UpperKind::MULy:
    case UpperKind::MULz:
    case UpperKind::MULw:
    case UpperKind::MULA:
    case UpperKind::MULAi:
    case UpperKind::MULAq:
    case UpperKind::MULAx:
    case UpperKind::MULAy:
    case UpperKind::MULAz:
    case UpperKind::MULAw:
      return true;
    default:
      return false;
    }
  }

  static bool ReadsAcc(UpperKind kind) {
    switch (kind) {
    case UpperKind::MADD:
    case UpperKind::MADDi:
    case UpperKind::MADDq:
    case UpperKind::MADDx:
    case UpperKind::MADDy:
    case UpperKind::MADDz:
    case UpperKind::MADDw:
    case UpperKind::MSUB:
    case UpperKind::MSUBi:
    case UpperKind::MSUBq:
    case UpperKind::MSUBx:
    case UpperKind::MSUBy:
    case UpperKind::MSUBz:
    case UpperKind::MSUBw:
    case UpperKind::MADDA:
    case UpperKind::MADDAi:
    case UpperKind::MADDAq:
    case UpperKind::MADDAx:
    case UpperKind::MADDAy:
    case UpperKind::MADDAz:
    case UpperKind::MADDAw:
    case UpperKind::MSUBA:
    case UpperKind::MSUBAi:
    case UpperKind::MSUBAq:
    case UpperKind::MSUBAx:
    case UpperKind::MSUBAy:
    case UpperKind::MSUBAz:
    case UpperKind::MSUBAw:
    case UpperKind::OPMSUB:
      return true;
    default:
      return false;
    }
  }

  static bool IsMadd(UpperKind kind) {
    switch (kind) {
    case UpperKind::MADD:
    case UpperKind::MADDi:
    case UpperKind::MADDq:
    case UpperKind::MADDx:
    case UpperKind::MADDy:
    case UpperKind::MADDz:
    case UpperKind::MADDw:
    case UpperKind::MADDA:
    case UpperKind::MADDAi:
    case UpperKind::MADDAq:
    case UpperKind::MADDAx:
    case UpperKind::MADDAy:
    case UpperKind::MADDAz:
    case UpperKind::MADDAw:
      return true;
    default:
      return false;
    }
  }

  static bool IsMinimum(UpperKind kind) {
    return kind == UpperKind::MINI || kind == UpperKind::MINIi ||
           kind == UpperKind::MINIx || kind == UpperKind::MINIy ||
           kind == UpperKind::MINIz || kind == UpperKind::MINIw;
  }

  static bool IsMaximum(UpperKind kind) {
    return kind == UpperKind::MAX || kind == UpperKind::MAXi ||
           kind == UpperKind::MAXx || kind == UpperKind::MAXy ||
           kind == UpperKind::MAXz || kind == UpperKind::MAXw;
  }

  static s32 BroadcastLane(UpperKind kind) {
    switch (kind) {
    case UpperKind::ADDx:
    case UpperKind::ADDAx:
    case UpperKind::SUBx:
    case UpperKind::SUBAx:
    case UpperKind::MAXx:
    case UpperKind::MINIx:
    case UpperKind::MULx:
    case UpperKind::MULAx:
    case UpperKind::MADDx:
    case UpperKind::MADDAx:
    case UpperKind::MSUBx:
    case UpperKind::MSUBAx:
      return 0;
    case UpperKind::ADDy:
    case UpperKind::ADDAy:
    case UpperKind::SUBy:
    case UpperKind::SUBAy:
    case UpperKind::MAXy:
    case UpperKind::MINIy:
    case UpperKind::MULy:
    case UpperKind::MULAy:
    case UpperKind::MADDy:
    case UpperKind::MADDAy:
    case UpperKind::MSUBy:
    case UpperKind::MSUBAy:
      return 1;
    case UpperKind::ADDz:
    case UpperKind::ADDAz:
    case UpperKind::SUBz:
    case UpperKind::SUBAz:
    case UpperKind::MAXz:
    case UpperKind::MINIz:
    case UpperKind::MULz:
    case UpperKind::MULAz:
    case UpperKind::MADDz:
    case UpperKind::MADDAz:
    case UpperKind::MSUBz:
    case UpperKind::MSUBAz:
      return 2;
    case UpperKind::ADDw:
    case UpperKind::ADDAw:
    case UpperKind::SUBw:
    case UpperKind::SUBAw:
    case UpperKind::MAXw:
    case UpperKind::MINIw:
    case UpperKind::MULw:
    case UpperKind::MULAw:
    case UpperKind::MADDw:
    case UpperKind::MADDAw:
    case UpperKind::MSUBw:
    case UpperKind::MSUBAw:
      return 3;
    default:
      return -1;
    }
  }

  static bool UsesI(UpperKind kind) {
    return kind == UpperKind::ADDi || kind == UpperKind::ADDAi ||
           kind == UpperKind::SUBi || kind == UpperKind::SUBAi ||
           kind == UpperKind::MAXi || kind == UpperKind::MINIi ||
           kind == UpperKind::MULi || kind == UpperKind::MULAi ||
           kind == UpperKind::MADDi || kind == UpperKind::MADDAi ||
           kind == UpperKind::MSUBi || kind == UpperKind::MSUBAi;
  }

  static bool UsesQ(UpperKind kind) {
    return kind == UpperKind::ADDq || kind == UpperKind::ADDAq ||
           kind == UpperKind::SUBq || kind == UpperKind::SUBAq ||
           kind == UpperKind::MULq || kind == UpperKind::MULAq ||
           kind == UpperKind::MADDq || kind == UpperKind::MADDAq ||
           kind == UpperKind::MSUBq || kind == UpperKind::MSUBAq;
  }

  u32 UpperRight(const VitaVU::GpuPairPlan& plan, UpperKind kind,
                 const EntryState& old, u32 lane) {
    if (UsesI(kind))
      return old.i;
    if (UsesQ(kind))
      return old.q;
    const s32 broadcast = BroadcastLane(kind);
    const u32 source_lane =
        broadcast >= 0 ? static_cast<u32>(broadcast) : lane;
    return old.vf[VUInterpFast::Ft(plan.upper)][source_lane];
  }

  u32 UpperValue(const VitaVU::GpuPairPlan& plan, const EntryState& old,
                 bool acc_destination, u32 lane) {
    const UpperKind kind = static_cast<UpperKind>(plan.upper_kind);
    const u32 fs = VUInterpFast::Fs(plan.upper);
    if (kind == UpperKind::ABS) {
      return Unary(ExpressionKind::Absolute, ScalarDomain::Float,
                   old.vf[fs][lane]);
    }
    if (kind == UpperKind::FTOI0 || kind == UpperKind::FTOI4 ||
        kind == UpperKind::FTOI12 || kind == UpperKind::FTOI15) {
      const u32 shift = kind == UpperKind::FTOI0
                            ? 0
                            : kind == UpperKind::FTOI4
                                  ? 4
                                  : kind == UpperKind::FTOI12 ? 12 : 15;
      return Unary(ExpressionKind::FloatToInt, ScalarDomain::SignedInt,
                   old.vf[fs][lane], shift);
    }
    if (kind == UpperKind::ITOF0 || kind == UpperKind::ITOF4 ||
        kind == UpperKind::ITOF12 || kind == UpperKind::ITOF15) {
      const u32 shift = kind == UpperKind::ITOF0
                            ? 0
                            : kind == UpperKind::ITOF4
                                  ? 4
                                  : kind == UpperKind::ITOF12 ? 12 : 15;
      return Unary(ExpressionKind::IntToFloat, ScalarDomain::Float,
                   old.vf[fs][lane], shift);
    }
    if (kind == UpperKind::OPMULA || kind == UpperKind::OPMSUB) {
      if (lane >= 3)
        return acc_destination ? old.acc[lane] : old.vf[0][lane];
      constexpr std::array<u8, 3> fs_lane = {1, 2, 0};
      constexpr std::array<u8, 3> ft_lane = {2, 0, 1};
      const u32 product = Binary(
          ExpressionKind::RoundedMultiply, ScalarDomain::Float,
          old.vf[fs][fs_lane[lane]],
          old.vf[VUInterpFast::Ft(plan.upper)][ft_lane[lane]]);
      return kind == UpperKind::OPMULA
                 ? product
                 : Binary(ExpressionKind::RoundedSubtract,
                          ScalarDomain::Float,
                          old.acc[lane], product);
    }

    const bool arithmetic =
        IsAdd(kind) || IsSubtract(kind) || IsMultiply(kind) ||
        ReadsAcc(kind) || IsMinimum(kind) || IsMaximum(kind);
    if (!arithmetic)
      return InvalidNode;
    const u32 left = old.vf[fs][lane];
    const u32 right = UpperRight(plan, kind, old, lane);
    if (IsMinimum(kind))
      return Binary(ExpressionKind::Minimum, ScalarDomain::Float, left, right);
    if (IsMaximum(kind))
      return Binary(ExpressionKind::Maximum, ScalarDomain::Float, left, right);
    if (IsMadd(kind) || ReadsAcc(kind)) {
      const u32 product = Binary(ExpressionKind::RoundedMultiply,
                                 ScalarDomain::Float, left, right);
      return Binary(IsSubtract(kind) ? ExpressionKind::RoundedSubtract
                                     : ExpressionKind::RoundedAdd,
                    ScalarDomain::Float, old.acc[lane], product);
    }
    if (IsMultiply(kind))
      return Binary(ExpressionKind::RoundedMultiply, ScalarDomain::Float,
                    left, right);
    return Binary(IsSubtract(kind) ? ExpressionKind::RoundedSubtract
                                   : ExpressionKind::RoundedAdd,
                  ScalarDomain::Float, left, right);
  }

  void AssignUpper(const VitaVU::GpuPairPlan& plan,
                   const EntryState& old, EntryState* state) {
    const bool acc_destination =
        (plan.upper_vi_write & (1u << REG_ACC_FLAG)) != 0;
    for (u32 lane = 0; lane < 4; lane++) {
      if (!LaneEnabled(plan.upper_vf_write_mask, lane))
        continue;
      const u32 value = UpperValue(plan, old, acc_destination, lane);
      if (acc_destination)
        state->acc[lane] = value;
      else if (plan.upper_vf_write != 0)
        state->vf[plan.upper_vf_write][lane] = value;
    }
  }

  void TransferPair(const VitaVU::GpuPairPlan& plan, EntryState* state) {
    const EntryState old = *state;
    if (plan.exec_lower && !plan.lower_discarded_by_upper) {
      AssignLowerVi(plan, old, state);
      AssignLowerVf(plan, old, state);
      AssignLowerScalar(plan, old, state);
    }
    if (plan.exec_upper)
      AssignUpper(plan, old, state);
  }

  const ProgramAnalysis& m_program;
  const NaturalLoop& m_loop;
  ParallelLoopKernel* m_kernel;
  const size_t m_loop_expression_count;
  std::map<NodeCacheKey, u32> m_node_cache;
  std::map<u32, u32> m_replacements;
  bool m_overflow = false;
};

bool CheckedOffset(s64 value, s32* result) {
  if (!result || value < std::numeric_limits<s32>::min() ||
      value > std::numeric_limits<s32>::max()) {
    return false;
  }
  *result = static_cast<s32>(value);
  return true;
}

bool AddOffset(const ViValue& value, s64 delta, ViValue* result) {
  if (!result || !value.known)
    return false;
  s32 offset = 0;
  if (!CheckedOffset(static_cast<s64>(value.offset) + delta, &offset))
    return false;
  *result = {value.base_vi, offset, true};
  return true;
}

bool ResolveAffineVi(const AffineViValue& value,
                     const std::array<ViValue, 16>& state,
                     ViValue* result) {
  if (!result || !value.valid || value.base_vi >= state.size())
    return false;
  const ViValue base = value.base_vi == 0 ? ViValue{0, 0, true}
                                          : state[value.base_vi];
  return AddOffset(base, value.offset, result);
}

AffineViValue PublicViValue(const ViValue& value) {
  return {value.base_vi, value.offset, value.known};
}

ViValue AddAffineVi(ViValue value, s32 delta) {
  if (value.known)
    value.offset += delta;
  return value;
}

ViValue BinaryAffineVi(const ViValue& left, const ViValue& right,
                       LowerKind kind) {
  if (!left.known || !right.known)
    return {};
  const bool left_constant = left.base_vi == 0;
  const bool right_constant = right.base_vi == 0;
  switch (kind) {
  case LowerKind::IADD:
    if (left_constant)
      return {right.base_vi, left.offset + right.offset, true};
    if (right_constant)
      return {left.base_vi, left.offset + right.offset, true};
    return {};
  case LowerKind::ISUB:
    if (right_constant)
      return {left.base_vi, left.offset - right.offset, true};
    return {};
  case LowerKind::IAND:
    if (left_constant && right_constant) {
      return {0, static_cast<u16>(left.offset) &
                     static_cast<u16>(right.offset), true};
    }
    return {};
  case LowerKind::IOR:
    if (left_constant && right_constant) {
      return {0, static_cast<u16>(left.offset) |
                     static_cast<u16>(right.offset), true};
    }
    return {};
  default:
    return {};
  }
}

// Compile-time-only VI transfer for a statically selected PairPlan path.  It
// executes no guest arithmetic: each value remains one initial VI symbol plus
// an integer offset.  This is the complete-state counterpart to the liveness-
// pruned transition used by the expression composer below.  A form which
// cannot remain affine is rejected before a generated root can own the epoch.
bool TransferAffineViPair(const VitaVU::GpuPairPlan& plan,
                          std::array<ViValue, 16>* state,
                          u16* written_mask, std::string* error) {
  if (!state)
    return Fail(error, "null affine VI state");
  if ((plan.upper_vi_write & 0xfffeu) != 0u)
    return Fail(error, "closed-form upper pair writes architectural VI state");
  if (!plan.exec_lower || plan.lower_discarded_by_upper)
    return true;

  const u16 writes = static_cast<u16>(plan.lower_vi_write & 0xfffeu);
  if (written_mask)
    *written_mask |= writes;
  if (writes == 0u)
    return true;
  if (plan.vi_backup_write)
    return Fail(error, "closed-form VI transfer retains a backup window");

  const std::array<ViValue, 16> old = *state;
  for (u32 reg = 1; reg < state->size(); reg++) {
    if ((writes & (1u << reg)) != 0u)
      (*state)[reg] = {};
  }

  const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
  const u32 code = plan.lower;
  const u32 is = VUInterpFast::Is(code);
  const u32 it = VUInterpFast::It(code);
  const u32 id = VUInterpFast::Id(code);
  switch (kind) {
  case LowerKind::IADDIU:
    if (it != 0u)
      (*state)[it] = AddAffineVi(old[is], VUInterpFast::Imm15(code));
    break;
  case LowerKind::ISUBIU:
    if (it != 0u)
      (*state)[it] = AddAffineVi(old[is], -VUInterpFast::Imm15(code));
    break;
  case LowerKind::IADDI:
    if (it != 0u)
      (*state)[it] = AddAffineVi(old[is], VUInterpFast::Imm5(code));
    break;
  case LowerKind::IADD:
  case LowerKind::ISUB:
  case LowerKind::IAND:
  case LowerKind::IOR:
    if (id != 0u)
      (*state)[id] = BinaryAffineVi(old[is], old[it], kind);
    break;
  case LowerKind::LQI:
    if (is != 0u)
      (*state)[is] = AddAffineVi(old[is], 1);
    break;
  case LowerKind::LQD:
    if (is != 0u)
      (*state)[is] = AddAffineVi(old[is], -1);
    break;
  case LowerKind::SQI:
    if (it != 0u)
      (*state)[it] = AddAffineVi(old[it], 1);
    break;
  case LowerKind::SQD:
    if (it != 0u)
      (*state)[it] = AddAffineVi(old[it], -1);
    break;
  default:
    break;
  }

  for (u32 reg = 1; reg < state->size(); reg++) {
    if ((writes & (1u << reg)) != 0u && !(*state)[reg].known) {
      return Fail(error, "closed-form VI write is not affine");
    }
  }
  return true;
}

bool TransferAffineViBlock(const BasicBlock& block,
                           std::array<ViValue, 16>* state,
                           u16* written_mask, std::string* error) {
  for (const ProgramPair& pair : block.pairs) {
    if (!TransferAffineViPair(pair.plan, state, written_mask, error))
      return false;
  }
  return true;
}

bool BuildExactClosedFormParentViState(
    const ProgramAnalysis& program, const NaturalLoop& child,
    const EnclosingLoopEntryIndependence& boundary,
    const StructuredLoopTailProof& tail, u32 outer_iteration_count,
    u32 child_iteration_count,
    const std::array<ViValue, 16>& parent_entry,
    std::array<ViValue, 16>* final_state, u16* written_mask,
    std::string* error) {
  if (!final_state || !written_mask ||
      child.header_block >= program.blocks.size() ||
      child.latch_block != child.header_block || child.blocks.size() != 1u ||
      boundary.prefix_blocks.empty() || tail.suffix_blocks.empty()) {
    return Fail(error,
                "closed-form complete VI path is not one canonical child body");
  }

  std::array<ViValue, 16> state = parent_entry;
  u16 writes = 0u;
  for (u32 outer = 0; outer < outer_iteration_count; outer++) {
    for (const u32 block : boundary.prefix_blocks) {
      if (block >= program.blocks.size() ||
          !TransferAffineViBlock(program.blocks[block], &state, &writes,
                                 error)) {
        return false;
      }
    }
    for (u32 iteration = 0; iteration < child_iteration_count; iteration++) {
      if (!TransferAffineViBlock(program.blocks[child.header_block], &state,
                                 &writes, error)) {
        return false;
      }
    }
    for (const u32 block : tail.suffix_blocks) {
      if (block >= program.blocks.size() ||
          !TransferAffineViBlock(program.blocks[block], &state, &writes,
                                 error)) {
        return false;
      }
    }
  }

  *final_state = state;
  *written_mask = writes;
  return true;
}

class ExpressionGraphEditor {
public:
  explicit ExpressionGraphEditor(ParallelLoopKernel* kernel)
      : m_kernel(kernel) {
    if (!m_kernel)
      return;
    for (u32 id = 1; id < m_kernel->expressions.size(); id++)
      m_cache.emplace(Key(m_kernel->expressions[id]), id);
  }

  u32 Add(ExpressionNode node) {
    if (!m_kernel || m_overflow)
      return InvalidNode;
    const NodeCacheKey key = Key(node);
    const auto found = m_cache.find(key);
    if (found != m_cache.end())
      return found->second;
    if (m_kernel->expressions.size() >= MaximumClosedFormExpressions) {
      m_overflow = true;
      return InvalidNode;
    }
    const u32 id = static_cast<u32>(m_kernel->expressions.size());
    m_kernel->expressions.push_back(node);
    m_cache.emplace(key, id);
    return id;
  }

  bool Overflowed() const { return m_overflow; }

private:
  static NodeCacheKey Key(const ExpressionNode& node) {
    return ExpressionKey(node);
  }

  ParallelLoopKernel* m_kernel = nullptr;
  std::map<NodeCacheKey, u32> m_cache;
  bool m_overflow = false;
};

struct OuterAffineVi {
  u8 base_vi = 0;
  s32 offset = 0;
  s32 outer_step = 0;
  bool valid = false;
};

class ClosedFormExpressionComposer {
public:
  ClosedFormExpressionComposer(ParallelLoopKernel* transition,
                               size_t child_expression_count,
                               size_t suffix_expression_begin,
                               u32 outer_iteration_count,
                               u32 child_iteration_count,
                               const char** allocation_stage)
      : m_transition(transition),
        m_child_expression_count(child_expression_count),
        m_suffix_expression_begin(suffix_expression_begin),
        m_outer_iteration_count(outer_iteration_count),
        m_child_iteration_count(child_iteration_count),
        m_editor(transition, allocation_stage) {}

  bool Compose(u32 source, const EntryState& parent,
               const std::array<ViValue, 16>& parent_vi,
               u32 selected_child_iteration, u32* result,
               std::string* error) {
    if (!result)
      return Fail(error, "null closed-form expression output");
    std::map<u32, u32> memo;
    const u32 composed = ComposeNode(source, parent, parent_vi,
                                    selected_child_iteration, 0u, &memo,
                                    error);
    if (composed == InvalidNode) {
      if (error && error->empty())
        *error = "closed-form expression composition failed";
      return false;
    }
    *result = composed;
    return true;
  }

  bool Collapse(const std::vector<u32>& sequence, u32* result,
                std::string* error) {
    if (!result)
      return Fail(error, "null closed-form collapse output");
    const u32 collapsed = CollapseSequence(sequence, error);
    if (collapsed == InvalidNode) {
      if (error && error->empty())
        *error = "enclosing recurrence has no finite closed form";
      return false;
    }
    *result = collapsed;
    return true;
  }

  bool Overflowed() const { return m_editor.Overflowed(); }

  bool RegisterMemoryReadDemands(
      const std::array<ViValue, 16>& parent_vi, std::string* error) {
    for (u32 source_id = 1u;
         source_id < m_transition->expressions.size(); source_id++) {
      const ExpressionNode& node = m_transition->expressions[source_id];
      if (node.kind != ExpressionKind::Memory)
        continue;
      const bool child_expression = source_id < m_child_expression_count;
      const u32 invocation_count =
          child_expression ? m_child_iteration_count : 1u;
      for (u32 child = 0u; child < invocation_count; child++) {
        AffineQwordAddress address;
        if (!ResolveMemoryAddress(source_id, node.memory_address, parent_vi,
                                  child, &address, error)) {
          return false;
        }
        m_memory_read_demands.emplace(MemoryKey(address, node.lane));
      }
    }
    return true;
  }

  bool PrepareOuterStores(const EntryState& parent,
                          const std::array<ViValue, 16>& parent_vi,
                          std::string* error) {
    m_current_outer_memory.clear();
    for (u32 child = 0; child < m_child_iteration_count; child++) {
      for (const LoopStore& store : m_transition->stores) {
        AffineQwordAddress address;
        if (!ResolveStoreAddress(store.address, parent_vi, child, &address,
                                 error)) {
          return false;
        }
        for (u32 lane = 0; lane < store.values.size(); lane++) {
          if (!LaneEnabled(store.write_mask, lane))
            continue;
          const SymbolicMemoryKey key = MemoryKey(address, lane);
          if (!m_memory_read_demands.contains(key))
            continue;
          u32 value = InvalidNode;
          if (!Compose(store.values[lane], parent, parent_vi, child, &value,
                       error)) {
            return false;
          }
          m_current_outer_memory[key] = value;
        }
      }
    }
    return true;
  }

  void CommitOuterStores() {
    for (const auto& [address, value] : m_current_outer_memory)
      m_prior_outer_memory[address] = value;
    m_current_outer_memory.clear();
  }

private:
  struct SymbolicMemoryKey {
    u8 base_vi = 0;
    u16 qword_offset = 0;
    u8 lane = 0;

    bool operator<(const SymbolicMemoryKey& other) const {
      return std::tie(base_vi, qword_offset, lane) <
             std::tie(other.base_vi, other.qword_offset, other.lane);
    }
  };

  static SymbolicMemoryKey MemoryKey(const AffineQwordAddress& address,
                                     u32 lane) {
    return {address.base_vi,
            static_cast<u16>(static_cast<u32>(address.qword_offset) &
                             VuMemoryQwordMask),
            static_cast<u8>(lane)};
  }

  u32 InitialReplacement(const ExpressionNode& node,
                         const EntryState& parent) const {
    switch (node.kind) {
    case ExpressionKind::InitialVf:
      return node.reg < parent.vf.size() && node.lane < 4
                 ? parent.vf[node.reg][node.lane]
                 : InvalidNode;
    case ExpressionKind::InitialAcc:
      return node.lane < parent.acc.size() ? parent.acc[node.lane]
                                           : InvalidNode;
    case ExpressionKind::InitialQ:
      return parent.q;
    case ExpressionKind::InitialP:
      return parent.p;
    case ExpressionKind::InitialI:
      return parent.i;
    default:
      return InvalidNode;
    }
  }

  bool ResolveMemoryAddress(u32 source_id, const AffineQwordAddress& source,
                            const std::array<ViValue, 16>& parent_vi,
                            u32 selected_child_iteration,
                            AffineQwordAddress* result,
                            std::string* error) const {
    const bool vif_symbol = source.base_vi == AffineViBaseVifTop ||
                            source.base_vi == AffineViBaseVifItop;
    if (!result || !source.valid ||
        (source.base_vi >= parent_vi.size() && !vif_symbol) ||
        source.outer_invocation_coefficient != 0) {
      return Fail(error, "closed-form memory address is not canonical");
    }

    ViValue base{};
    const bool child_expression = source_id < m_child_expression_count;
    if (child_expression) {
      if (source.base_vi == 0) {
        base = {0, 0, true};
      } else if (!ResolveAffineVi(
                     m_transition->child_entry_vi_values[source.base_vi],
                     parent_vi, &base)) {
        return Fail(error,
                    "closed-form child memory base is not affine");
      }
    } else {
      if (source.invocation_coefficient != 0) {
        return Fail(error,
                    "enclosing prefix/suffix memory unexpectedly depends on "
                    "the child invocation");
      }
      if (source.base_vi == 0) {
        base = {0, 0, true};
      } else if (vif_symbol) {
        // EntrySliceBuilder has already resolved XTOP/XITOP into immutable
        // epoch symbols.  They are no longer parent-register indices.
        base = {source.base_vi, 0, true};
      } else {
        base = parent_vi[source.base_vi];
      }
    }
    if (!base.known)
      return Fail(error, "closed-form memory base is unresolved");

    const s64 selected_offset =
        static_cast<s64>(source.qword_offset) +
        (child_expression
             ? static_cast<s64>(selected_child_iteration) *
                   source.invocation_coefficient
             : 0);
    s32 qword_offset = 0;
    if (!CheckedOffset(static_cast<s64>(base.offset) + selected_offset,
                       &qword_offset)) {
      return Fail(error, "closed-form memory offset overflowed");
    }
    *result = {base.base_vi, 0, qword_offset, true, 0};
    return true;
  }

  bool ResolveStoreAddress(const AffineQwordAddress& source,
                           const std::array<ViValue, 16>& parent_vi,
                           u32 selected_child_iteration,
                           AffineQwordAddress* result,
                           std::string* error) const {
    if (!result || !source.valid || source.base_vi >= parent_vi.size() ||
        source.outer_invocation_coefficient != 0) {
      return Fail(error, "closed-form store address is not canonical");
    }
    ViValue base{};
    if (source.base_vi == 0) {
      base = {0, 0, true};
    } else if (!ResolveAffineVi(
                   m_transition->child_entry_vi_values[source.base_vi],
                   parent_vi, &base)) {
      return Fail(error, "closed-form store base is not affine");
    }
    s32 qword_offset = 0;
    if (!CheckedOffset(
            static_cast<s64>(base.offset) + source.qword_offset +
                static_cast<s64>(selected_child_iteration) *
                    source.invocation_coefficient,
            &qword_offset)) {
      return Fail(error, "closed-form store offset overflowed");
    }
    *result = {base.base_vi, 0, qword_offset, true, 0};
    return true;
  }

  u32 PublishedMemoryReplacement(u32 source_id,
                                 const AffineQwordAddress& address,
                                 u32 lane) const {
    const SymbolicMemoryKey key = MemoryKey(address, lane);
    // A direct suffix load executes after every store in the current child
    // grid. Child-body and enclosing-prefix loads retain the generation which
    // existed at this outer iteration's entry. This is the precise memory-SSA
    // seam needed by in-place VU algorithms: the next enclosing iteration
    // observes prior stores without making concurrently lowered child stores
    // visible too early.
    if (source_id >= m_suffix_expression_begin) {
      const auto current = m_current_outer_memory.find(key);
      if (current != m_current_outer_memory.end())
        return current->second;
    }
    const auto prior = m_prior_outer_memory.find(key);
    return prior != m_prior_outer_memory.end() ? prior->second : InvalidNode;
  }

  u32 ComposeNode(u32 source, const EntryState& parent,
                  const std::array<ViValue, 16>& parent_vi,
                  u32 selected_child_iteration, u32 depth,
                  std::map<u32, u32>* memo, std::string* error) {
    if (source == InvalidNode || source >= m_transition->expressions.size()) {
      Fail(error, "closed-form expression root is out of range");
      return InvalidNode;
    }
    if (depth >= MaximumClosedFormCompositionDepth) {
      Fail(error, "closed-form expression composition depth exceeded");
      return InvalidNode;
    }
    const auto cached = memo->find(source);
    if (cached != memo->end()) {
      if (cached->second == InvalidNode) {
        Fail(error, "closed-form expression graph contains a cycle");
        return InvalidNode;
      }
      return cached->second;
    }

    // Reserve an in-progress marker before descending.  The transition is
    // produced internally, but rejecting a malformed cycle is still required:
    // runtime compilation must never turn corrupt metadata into a host stack
    // overflow.
    (*memo)[source] = InvalidNode;

    // Recursive children can append to m_transition->expressions through the
    // editor.  Keep a value copy: retaining a vector element reference across
    // those appends follows freed storage after a reallocation.
    const ExpressionNode original = m_transition->expressions[source];
    const u32 replacement = InitialReplacement(original, parent);
    if (replacement != InvalidNode) {
      (*memo)[source] = replacement;
      return replacement;
    }
    switch (original.kind) {
    case ExpressionKind::InvariantVf:
    case ExpressionKind::InvariantAcc:
    case ExpressionKind::InvariantQ:
    case ExpressionKind::InvariantP:
    case ExpressionKind::InvariantI:
    case ExpressionKind::StructuredScratch:
      Fail(error, "closed-form transition retains an unresolved entry leaf");
      return InvalidNode;
    default:
      break;
    }

    ExpressionNode node = original;
    if (node.kind == ExpressionKind::Memory) {
      if (!ResolveMemoryAddress(source, original.memory_address, parent_vi,
                                selected_child_iteration,
                                &node.memory_address, error)) {
        return InvalidNode;
      }
      const u32 published = PublishedMemoryReplacement(
          source, node.memory_address, node.lane);
      if (published != InvalidNode) {
        (*memo)[source] = published;
        return published;
      }
    }
    for (u32 operand_index = 0; operand_index < node.operands.size();
         operand_index++) {
      if (node.operands[operand_index] == InvalidNode)
        continue;
      node.operands[operand_index] = ComposeNode(
          original.operands[operand_index], parent, parent_vi,
          selected_child_iteration, depth + 1u, memo, error);
      if (node.operands[operand_index] == InvalidNode)
        return InvalidNode;
    }

    const u32 composed = m_editor.Add(node);
    if (composed == InvalidNode) {
      Fail(error, "closed-form expression capacity exceeded");
      return InvalidNode;
    }
    (*memo)[source] = composed;
    return composed;
  }

  bool ContainsExpression(u32 root, u32 target,
                          std::vector<u8>* visited) const {
    if (root == target)
      return true;
    if (root == InvalidNode || root >= m_transition->expressions.size() ||
        (*visited)[root] != 0) {
      return false;
    }
    (*visited)[root] = 1;
    for (const u32 operand : m_transition->expressions[root].operands) {
      if (ContainsExpression(operand, target, visited))
        return true;
    }
    return false;
  }

  bool ExtractRepeatedAddTransition(u32 previous, u32 current,
                                    u32* increment, u32* add_count,
                                    u8* carried_side) const {
    if (!increment || !add_count || !carried_side)
      return false;
    u32 cursor = current;
    u32 found_increment = InvalidNode;
    u32 found_count = 0;
    u8 found_side = 0;
    while (cursor != previous) {
      if (cursor == InvalidNode || cursor >= m_transition->expressions.size() ||
          found_count >= MaximumClosedFormOuterIterations) {
        return false;
      }
      const ExpressionNode& node = m_transition->expressions[cursor];
      if (node.kind != ExpressionKind::RoundedAdd)
        return false;
      std::vector<u8> left_visited(m_transition->expressions.size());
      std::vector<u8> right_visited(m_transition->expressions.size());
      const bool left_contains =
          ContainsExpression(node.operands[0], previous, &left_visited);
      const bool right_contains =
          ContainsExpression(node.operands[1], previous, &right_visited);
      if (left_contains == right_contains)
        return false;
      const u8 side = right_contains ? 1u : 0u;
      const u32 next = node.operands[side];
      const u32 candidate_increment = node.operands[side ^ 1u];
      if (candidate_increment == InvalidNode ||
          (found_increment != InvalidNode &&
           candidate_increment != found_increment) ||
          (found_count != 0 && side != found_side)) {
        return false;
      }
      found_increment = candidate_increment;
      found_side = side;
      found_count++;
      cursor = next;
    }
    if (found_count == 0 || found_increment == InvalidNode)
      return false;
    *increment = found_increment;
    *add_count = found_count;
    *carried_side = found_side;
    return true;
  }

  bool TryCompactOuterInput(const std::vector<u32>& sequence,
                            u32* result, bool* recognized,
                            std::string* error) {
    if (!result || !recognized)
      return Fail(error, "null compact outer-input result");
    *result = InvalidNode;
    *recognized = false;
    CompactOuterInputTable table;
    table.sources.reserve(sequence.size());
    u8 lane = 0xffu;
    for (const u32 id : sequence) {
      if (id == InvalidNode || id >= m_transition->expressions.size())
        return true;
      const ExpressionNode& node = m_transition->expressions[id];
      if (node.domain != ScalarDomain::Float || node.lane >= 4u)
        return true;
      if (lane == 0xffu)
        lane = node.lane;
      else if (node.lane != lane)
        return true;

      CompactQwordSource source;
      if (node.kind == ExpressionKind::InitialVf && node.reg != 0u &&
          node.reg < 32u) {
        source.kind = CompactQwordSourceKind::InitialVf;
        source.reg = node.reg;
      } else if (node.kind == ExpressionKind::Memory &&
                 node.memory_address.valid &&
                 node.memory_address.invocation_coefficient == 0 &&
                 node.memory_address.outer_invocation_coefficient == 0) {
        source.kind = CompactQwordSourceKind::Memory;
        source.memory_address = node.memory_address;
      } else {
        return true;
      }
      table.sources.push_back(source);
    }
    if (lane == 0xffu || table.sources.size() != m_outer_iteration_count)
      return true;
    *recognized = true;

    u32 table_index = 0u;
    const auto found = std::find(m_transition->compact_outer_inputs.begin(),
                                 m_transition->compact_outer_inputs.end(),
                                 table);
    if (found != m_transition->compact_outer_inputs.end()) {
      table_index = static_cast<u32>(
          found - m_transition->compact_outer_inputs.begin());
    } else {
      u64 qword_count = table.sources.size();
      for (const CompactOuterInputTable& existing :
           m_transition->compact_outer_inputs) {
        qword_count += existing.sources.size();
      }
      if (qword_count > MaximumCompactOuterInputQwords)
        return Fail(error, "closed-form compact outer input exceeds 64 qwords");
      table_index = static_cast<u32>(
          m_transition->compact_outer_inputs.size());
      m_transition->compact_outer_inputs.push_back(std::move(table));
    }

    ExpressionNode compact{};
    compact.kind = ExpressionKind::CompactOuterInput;
    compact.domain = ScalarDomain::Float;
    compact.immediate = table_index;
    compact.lane = lane;
    *result = m_editor.Add(compact);
    if (*result == InvalidNode)
      return Fail(error, "closed-form expression capacity exceeded");
    return true;
  }

  bool TryCompactOuterRepeatedAdd(const std::vector<u32>& sequence,
                                  u32* result, bool* recognized,
                                  std::string* error) {
    if (!result || !recognized)
      return Fail(error, "null compact repeated-ADD result");
    *result = InvalidNode;
    *recognized = false;
    if (sequence.size() < 2u)
      return true;

    // A sliding parent window commonly publishes the selected source through
    // one additional FMAC ADD on every enclosing iteration.  The raw source
    // may therefore change (initial VF lanes followed by VU-memory qwords),
    // while the exact publication depth is affine in the outer index.  Keep
    // those sources in a compact immutable table and execute every rounded ADD
    // in the generated invocation; no CPU-side semantic precompute is used.
    for (u8 carried_side = 0u; carried_side < 2u; carried_side++) {
      const ExpressionNode& first_step =
          m_transition->expressions[sequence[1]];
      if (first_step.kind != ExpressionKind::RoundedAdd)
        continue;
      const u32 increment = first_step.operands[carried_side ^ 1u];
      if (increment == InvalidNode)
        continue;

      std::vector<u32> sources;
      std::vector<u32> depths;
      sources.reserve(sequence.size());
      depths.reserve(sequence.size());
      bool valid = true;
      for (const u32 value : sequence) {
        u32 cursor = value;
        u32 depth = 0u;
        while (cursor != InvalidNode &&
               cursor < m_transition->expressions.size()) {
          const ExpressionNode& node = m_transition->expressions[cursor];
          if (node.kind != ExpressionKind::RoundedAdd ||
              node.operands[carried_side ^ 1u] != increment) {
            break;
          }
          cursor = node.operands[carried_side];
          if (++depth > MaximumClosedFormOuterIterations) {
            valid = false;
            break;
          }
        }
        if (!valid || cursor == InvalidNode ||
            cursor >= m_transition->expressions.size()) {
          valid = false;
          break;
        }
        sources.push_back(cursor);
        depths.push_back(depth);
      }
      if (!valid)
        continue;

      const u32 maximum_depth =
          *std::max_element(depths.begin(), depths.end());
      if (maximum_depth == 0u || maximum_depth > 0xffu)
        continue;

      u32 compact_source = InvalidNode;
      bool compact_recognized = false;
      if (std::all_of(sources.begin() + 1u, sources.end(),
                      [&](u32 source) { return source == sources.front(); })) {
        compact_source = sources.front();
        compact_recognized = true;
      } else if (!TryCompactOuterInput(sources, &compact_source,
                                       &compact_recognized, error)) {
        return false;
      }
      if (!compact_recognized)
        continue;

      OuterRepeatedAddSchedule schedule;
      schedule.add_counts.reserve(depths.size());
      for (const u32 depth : depths)
        schedule.add_counts.push_back(static_cast<u8>(depth));
      u32 schedule_index = 0u;
      const auto found = std::find(
          m_transition->outer_repeated_add_schedules.begin(),
          m_transition->outer_repeated_add_schedules.end(), schedule);
      if (found != m_transition->outer_repeated_add_schedules.end()) {
        schedule_index = static_cast<u32>(
            found - m_transition->outer_repeated_add_schedules.begin());
      } else {
        if (m_transition->outer_repeated_add_schedules.size() >=
            OuterRepeatedAddScheduledBit) {
          return Fail(error,
                      "closed-form repeated-ADD schedule capacity exceeded");
        }
        schedule_index = static_cast<u32>(
            m_transition->outer_repeated_add_schedules.size());
        m_transition->outer_repeated_add_schedules.push_back(
            std::move(schedule));
      }

      ExpressionNode publication{};
      publication.kind = ExpressionKind::OuterRepeatedAdd;
      publication.domain = ScalarDomain::Float;
      publication.operands = {compact_source, increment, InvalidNode};
      publication.immediate =
          OuterRepeatedAddScheduledBit | schedule_index;
      publication.reg = carried_side;
      *result = m_editor.Add(publication);
      if (*result == InvalidNode)
        return Fail(error, "closed-form expression capacity exceeded");
      *recognized = true;
      return true;
    }
    return true;
  }

  u32 CollapseSequence(const std::vector<u32>& sequence,
                       std::string* error) {
    if (sequence.empty()) {
      Fail(error, "closed-form recurrence sequence is empty");
      return InvalidNode;
    }
    if (std::any_of(sequence.begin(), sequence.end(), [this](u32 id) {
          return id == InvalidNode || id >= m_transition->expressions.size();
        })) {
      Fail(error, "closed-form recurrence contains an invalid expression");
      return InvalidNode;
    }
    const auto cached = m_collapsed.find(sequence);
    if (cached != m_collapsed.end())
      return cached->second;
    if (std::all_of(sequence.begin() + 1, sequence.end(),
                    [&](u32 id) { return id == sequence.front(); })) {
      m_collapsed.emplace(sequence, sequence.front());
      return sequence.front();
    }

    const ExpressionNode& first = m_transition->expressions[sequence.front()];
    const bool all_memory =
        first.kind == ExpressionKind::Memory &&
        std::all_of(sequence.begin() + 1u, sequence.end(),
                    [this](u32 id) {
                      return m_transition->expressions[id].kind ==
                             ExpressionKind::Memory;
                    });
    if (all_memory) {
      if (!first.memory_address.valid ||
          first.memory_address.outer_invocation_coefficient != 0) {
        Fail(error, "closed-form memory recurrence is not canonical");
        return InvalidNode;
      }
      const s64 delta = static_cast<s64>(
                            m_transition->expressions[sequence[1]]
                                .memory_address.qword_offset) -
                        first.memory_address.qword_offset;
      s32 outer_step = 0;
      if (!CheckedOffset(delta, &outer_step)) {
        Fail(error, "closed-form memory recurrence stride overflowed");
        return InvalidNode;
      }
      for (u32 iteration = 0; iteration < sequence.size(); iteration++) {
        const ExpressionNode& node = m_transition->expressions[sequence[iteration]];
        if (node.kind != ExpressionKind::Memory ||
            node.domain != first.domain || node.lane != first.lane ||
            node.memory_address.base_vi != first.memory_address.base_vi ||
            node.memory_address.invocation_coefficient != 0 ||
            node.memory_address.outer_invocation_coefficient != 0 ||
            !node.memory_address.valid ||
            static_cast<s64>(node.memory_address.qword_offset) !=
                static_cast<s64>(first.memory_address.qword_offset) +
                    static_cast<s64>(outer_step) * iteration) {
          Fail(error,
               "enclosing memory recurrence is not affine at " +
                   std::to_string(iteration) + " (first=" +
                   std::to_string(first.memory_address.base_vi) + "/" +
                   std::to_string(first.memory_address.qword_offset) + "/" +
                   std::to_string(first.memory_address.invocation_coefficient) +
                   "/" +
                   std::to_string(
                       first.memory_address.outer_invocation_coefficient) +
                   " current=" +
                   std::to_string(node.memory_address.base_vi) + "/" +
                   std::to_string(node.memory_address.qword_offset) + "/" +
                   std::to_string(node.memory_address.invocation_coefficient) +
                   "/" +
                   std::to_string(
                       node.memory_address.outer_invocation_coefficient) +
                   ")");
          return InvalidNode;
        }
      }
      ExpressionNode collapsed = first;
      collapsed.memory_address.outer_invocation_coefficient = outer_step;
      const u32 id = m_editor.Add(collapsed);
      if (id == InvalidNode) {
        Fail(error, "closed-form expression capacity exceeded");
        return InvalidNode;
      }
      m_collapsed.emplace(sequence, id);
      return id;
    }

    u32 compact_outer_input = InvalidNode;
    bool compact_recognized = false;
    if (!TryCompactOuterInput(sequence, &compact_outer_input,
                              &compact_recognized, error)) {
      return InvalidNode;
    }
    if (compact_recognized) {
      if (compact_outer_input == InvalidNode)
        return InvalidNode;
      m_collapsed.emplace(sequence, compact_outer_input);
      return compact_outer_input;
    }

    u32 compact_repeated_add = InvalidNode;
    bool compact_repeated_add_recognized = false;
    if (!TryCompactOuterRepeatedAdd(sequence, &compact_repeated_add,
                                    &compact_repeated_add_recognized,
                                    error)) {
      return InvalidNode;
    }
    if (compact_repeated_add_recognized) {
      if (compact_repeated_add == InvalidNode)
        return InvalidNode;
      m_collapsed.emplace(sequence, compact_repeated_add);
      return compact_repeated_add;
    }

    // Recognize the exact PairPlan recurrence before structural recursion.  A
    // finite rounded ADD chain is not algebraically replaced by multiplication:
    // preserving every binary32 publication boundary is required for the named
    // playable numeric profile and for oracle diagnostics.
    u32 increment = InvalidNode;
    u32 adds_per_outer = 0;
    u8 carried_side = 0;
    bool repeated_add = sequence.size() == m_outer_iteration_count;
    for (u32 iteration = 1; repeated_add && iteration < sequence.size();
         iteration++) {
      u32 candidate_increment = InvalidNode;
      u32 candidate_count = 0;
      u8 candidate_side = 0;
      repeated_add = ExtractRepeatedAddTransition(
          sequence[iteration - 1], sequence[iteration],
          &candidate_increment, &candidate_count, &candidate_side);
      if (repeated_add && increment == InvalidNode) {
        increment = candidate_increment;
        adds_per_outer = candidate_count;
        carried_side = candidate_side;
      } else if (repeated_add &&
                 (candidate_increment != increment ||
                  candidate_count != adds_per_outer ||
                  candidate_side != carried_side)) {
        repeated_add = false;
      }
    }
    if (repeated_add && increment != InvalidNode && adds_per_outer != 0) {
      u32 value = sequence.front();
      for (u32 add = 0; add < adds_per_outer; add++) {
        ExpressionNode collapsed{};
        collapsed.kind = ExpressionKind::OuterRepeatedAdd;
        collapsed.domain = ScalarDomain::Float;
        collapsed.operands = {value, increment, InvalidNode};
        collapsed.immediate = m_outer_iteration_count;
        collapsed.reg = carried_side;
        value = m_editor.Add(collapsed);
        if (value == InvalidNode) {
          Fail(error, "closed-form expression capacity exceeded");
          return InvalidNode;
        }
      }
      m_collapsed.emplace(sequence, value);
      return value;
    }

    for (u32 iteration = 0; iteration < sequence.size(); iteration++) {
      const u32 id = sequence[iteration];
      const ExpressionNode& node = m_transition->expressions[id];
      if (node.kind != first.kind || node.domain != first.domain ||
          node.immediate != first.immediate || node.reg != first.reg ||
          node.lane != first.lane ||
          node.memory_address != first.memory_address) {
        const auto describe = [this](u32 id) {
          const ExpressionNode& value = m_transition->expressions[id];
          return std::to_string(id) + ":" +
                 std::to_string(static_cast<u32>(value.kind)) + "[" +
                 std::to_string(value.operands[0]) + "," +
                 std::to_string(value.operands[1]) + "," +
                 std::to_string(value.operands[2]) + "]r" +
                 std::to_string(value.reg) + "l" +
                 std::to_string(value.lane);
        };
        std::string detail;
        for (u32 sequence_index = 0; sequence_index < sequence.size();
             sequence_index++) {
          if (!detail.empty())
            detail += ",";
          detail += describe(sequence[sequence_index]);
        }
        Fail(error, "enclosing recurrence changes expression shape at " +
                        std::to_string(iteration) + " (" +
                        std::to_string(static_cast<u32>(first.kind)) + "/" +
                        std::to_string(static_cast<u32>(node.kind)) +
                        "; sequence=" + detail + ")");
        return InvalidNode;
      }
    }
    switch (first.kind) {
    case ExpressionKind::ConstantFloat:
    case ExpressionKind::ConstantSigned:
    case ExpressionKind::ConstantUnsigned:
    case ExpressionKind::InitialVf:
    case ExpressionKind::InitialAcc:
    case ExpressionKind::InitialQ:
    case ExpressionKind::InitialP:
    case ExpressionKind::InitialI:
    case ExpressionKind::InvariantVf:
    case ExpressionKind::InvariantAcc:
    case ExpressionKind::InvariantQ:
    case ExpressionKind::InvariantP:
    case ExpressionKind::InvariantI:
    case ExpressionKind::StructuredScratch:
    case ExpressionKind::OuterRepeatedAdd:
      Fail(error, "enclosing recurrence changes a non-composable leaf");
      return InvalidNode;
    default:
      break;
    }

    ExpressionNode collapsed = first;
    for (u32 operand_index = 0; operand_index < collapsed.operands.size();
         operand_index++) {
      std::vector<u32> operands;
      operands.reserve(sequence.size());
      bool any = false;
      bool all = true;
      for (u32 id : sequence) {
        const u32 operand =
            m_transition->expressions[id].operands[operand_index];
        operands.push_back(operand);
        any |= operand != InvalidNode;
        all &= operand != InvalidNode;
      }
      if (any != all) {
        Fail(error, "enclosing recurrence changes expression arity");
        return InvalidNode;
      }
      if (!any) {
        collapsed.operands[operand_index] = InvalidNode;
        continue;
      }
      collapsed.operands[operand_index] = CollapseSequence(operands, error);
      if (collapsed.operands[operand_index] == InvalidNode)
        return InvalidNode;
    }
    const u32 id = m_editor.Add(collapsed);
    if (id == InvalidNode) {
      Fail(error, "closed-form expression capacity exceeded");
      return InvalidNode;
    }
    m_collapsed.emplace(sequence, id);
    return id;
  }

  ParallelLoopKernel* m_transition = nullptr;
  size_t m_child_expression_count = 0;
  size_t m_suffix_expression_begin = 0;
  u32 m_outer_iteration_count = 0;
  u32 m_child_iteration_count = 0;
  AppendOnlyExpressionGraphEditor m_editor;
  std::map<std::vector<u32>, u32> m_collapsed;
  std::set<SymbolicMemoryKey> m_memory_read_demands;
  std::map<SymbolicMemoryKey, u32> m_prior_outer_memory;
  std::map<SymbolicMemoryKey, u32> m_current_outer_memory;
};

bool CollapseViSequence(const std::vector<ViValue>& sequence,
                        OuterAffineVi* result, std::string* error) {
  if (!result || sequence.empty() || !sequence.front().known)
    return Fail(error, "nested child VI sequence is unresolved");
  s64 delta = 0;
  if (sequence.size() > 1) {
    delta = static_cast<s64>(sequence[1].offset) - sequence[0].offset;
  }
  s32 outer_step = 0;
  if (!CheckedOffset(delta, &outer_step))
    return Fail(error, "nested child VI outer stride overflowed");
  for (u32 iteration = 0; iteration < sequence.size(); iteration++) {
    if (!sequence[iteration].known ||
        sequence[iteration].base_vi != sequence.front().base_vi ||
        static_cast<s64>(sequence[iteration].offset) !=
            static_cast<s64>(sequence.front().offset) +
                static_cast<s64>(outer_step) * iteration) {
      return Fail(error, "nested child VI state is not outer-affine");
    }
  }
  *result = {sequence.front().base_vi, sequence.front().offset, outer_step,
             true};
  return true;
}

bool NormalizeChildAddress(const AffineQwordAddress& source,
                           const std::array<OuterAffineVi, 16>& child_vi,
                           AffineQwordAddress* result,
                           std::string* error) {
  if (!result || !source.valid || source.base_vi >= child_vi.size() ||
      source.outer_invocation_coefficient != 0)
    return Fail(error, "nested child address is not canonical");
  const OuterAffineVi base = source.base_vi == 0
                                 ? OuterAffineVi{0, 0, 0, true}
                                 : child_vi[source.base_vi];
  if (!base.valid)
    return Fail(error, "nested child address has no affine VI entry");
  s32 offset = 0;
  if (!CheckedOffset(static_cast<s64>(base.offset) + source.qword_offset,
                     &offset)) {
    return Fail(error, "nested child address offset overflowed");
  }
  *result = {base.base_vi, source.invocation_coefficient, offset, true,
             base.outer_step};
  return true;
}

u32 ImportExpression(const ParallelLoopKernel& source, u32 source_id,
                     ExpressionGraphEditor* destination,
                     std::map<u32, u32>* memo, std::string* error) {
  if (!destination || !memo || source_id == InvalidNode ||
      source_id >= source.expressions.size()) {
    Fail(error, "closed-form imported expression is out of range");
    return InvalidNode;
  }
  const auto found = memo->find(source_id);
  if (found != memo->end())
    return found->second;
  ExpressionNode node = source.expressions[source_id];
  for (u32& operand : node.operands) {
    if (operand == InvalidNode)
      continue;
    operand = ImportExpression(source, operand, destination, memo, error);
    if (operand == InvalidNode)
      return InvalidNode;
  }
  const u32 imported = destination->Add(node);
  if (imported == InvalidNode) {
    Fail(error, "closed-form imported expression capacity exceeded");
    return InvalidNode;
  }
  memo->emplace(source_id, imported);
  return imported;
}

bool ValidateConstantChildTrip(const NaturalLoop& child,
                               const std::array<ViValue, 16>& child_vi,
                               u32 expected, std::string* error) {
  if (child.counter_reg == 0 || child.counter_reg >= child_vi.size() ||
      child.counter_limit_reg >= child_vi.size() ||
      static_cast<LowerKind>(child.branch_kind) != LowerKind::IBNE ||
      !child.branch_taken_repeats ||
      (child.counter_step != 1 && child.counter_step != -1)) {
    return Fail(error, "nested child has no exact counted IBNE loop");
  }
  const ViValue counter = child_vi[child.counter_reg];
  const ViValue limit = child.counter_limit_reg == 0
                            ? ViValue{0, 0, true}
                            : child_vi[child.counter_limit_reg];
  if (!counter.known || !limit.known || counter.base_vi != limit.base_vi)
    return Fail(error, "nested child trip count depends on unrelated VI bases");
  const u16 counter_value = static_cast<u16>(counter.offset);
  const u16 limit_value = static_cast<u16>(limit.offset);
  const u32 trip = child.counter_step > 0
                       ? static_cast<u16>(limit_value - counter_value)
                       : static_cast<u16>(counter_value - limit_value);
  if (trip == 0 || trip != expected)
    return Fail(error, "nested child trip count differs from the exact grid");
  return true;
}

bool IsReachableInvariant(const ParallelLoopKernel& kernel, u32 root,
                          std::vector<u8>* visited) {
  if (root == InvalidNode || root >= kernel.expressions.size())
    return true;
  if ((*visited)[root] != 0)
    return false;
  (*visited)[root] = 1;
  const ExpressionNode& node = kernel.expressions[root];
  switch (node.kind) {
  case ExpressionKind::InvariantVf:
  case ExpressionKind::InvariantAcc:
  case ExpressionKind::InvariantQ:
  case ExpressionKind::InvariantP:
  case ExpressionKind::InvariantI:
  case ExpressionKind::StructuredScratch:
    return true;
  default:
    break;
  }
  for (u32 operand : node.operands) {
    if (operand != InvalidNode && IsReachableInvariant(kernel, operand, visited))
      return true;
  }
  return false;
}

} // namespace

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
bool ValidateAppendOnlyExpressionGraphIndex() {
  const AffineQwordAddress compact_address{17u, -3, -1025, true, 11};
  const AffineQwordAddress default_address;
  if (compact_address.base_vi != 17u || !compact_address.valid ||
      compact_address.invocation_coefficient != -3 ||
      compact_address.qword_offset != -1025 ||
      compact_address.outer_invocation_coefficient != 11 ||
      default_address.base_vi != 0u || default_address.valid ||
      default_address.invocation_coefficient != 0 ||
      default_address.qword_offset != 0 ||
      default_address.outer_invocation_coefficient != 0) {
    return false;
  }
  std::printf("vu-jit-validation: compact-expression-layout ok address_bytes=%zu "
              "node_bytes=%zu address_alignment=%zu node_alignment=%zu "
              "coefficients=full-width constructor_order=preserved\n",
              sizeof(AffineQwordAddress), sizeof(ExpressionNode),
              alignof(AffineQwordAddress), alignof(ExpressionNode));
  const char* allocation_stage = "enclosing-phase";
  if (WithConstructionAllocationFailureStage(&allocation_stage, "unused",
          []() { return 42u; }) != 42u ||
      std::string_view(allocation_stage) != "enclosing-phase") {
    return false;
  }
  for (const char* failure_stage : {"nested-expression-index-init",
                                   "nested-expression-index-growth",
                                   "nested-expression-node-growth"}) {
    u32 calls = 0u;
    try {
      WithConstructionAllocationFailureStage(&allocation_stage, failure_stage,
          [&]() { ++calls; throw std::bad_alloc(); });
      return false;
    } catch (const std::bad_alloc&) {
      if (calls != 1u || allocation_stage != failure_stage)
        return false;
    }
  }
  try {
    WithConstructionAllocationFailureStage(nullptr, "unused",
        []() { throw std::bad_alloc(); });
    return false;
  } catch (const std::bad_alloc&) {
  }
  allocation_stage = "enclosing-phase";
  try {
    WithConstructionAllocationFailureStage(&allocation_stage, "unused",
        []() { throw 42u; });
    return false;
  } catch (u32 value) {
    if (value != 42u || std::string_view(allocation_stage) != "enclosing-phase")
      return false;
  }
  std::printf("vu-jit-validation: construction-allocation-stage ok "
              "success=unchanged bad_alloc=rethrown other_exception=unchanged "
              "null_sink=valid retries=0\n");
  ParallelLoopKernel indexed;
  indexed.expressions.resize(1u);
  ExpressionNode seed;
  seed.immediate = 0x12345678u;
  indexed.expressions.push_back(seed);
  indexed.expressions.push_back(seed); // First existing ID must win.
  ParallelLoopKernel reference = indexed;
  AppendOnlyExpressionGraphEditor editor(&indexed);
  ExpressionGraphEditor oracle(&reference);
  const auto check = [&](const ExpressionNode& node) {
    const size_t previous_size = indexed.expressions.size();
    const size_t previous_capacity = indexed.expressions.capacity();
    const u32 id = editor.Add(node);
    if (id != oracle.Add(node) ||
        indexed.expressions.size() != reference.expressions.size()) {
      return false;
    }
    const bool grew = indexed.expressions.capacity() != previous_capacity;
    if (grew) {
      const size_t expected = std::min<size_t>(MaximumClosedFormExpressions,
          previous_capacity + std::max<size_t>(previous_capacity / 2u, 1u));
      if (previous_size != previous_capacity ||
          indexed.expressions.capacity() != expected ||
          indexed.expressions.size() != previous_size + 1u) {
        return false;
      }
    }
    return true;
  };
  if (!check(seed) || editor.Add(seed) != 1u)
    return false;

  // Deliberately populate one initial bucket with distinct keys. A hash match
  // must never merge two expressions. Reinsert after several index growths.
  std::vector<ExpressionNode> collisions;
  const u32 bucket = ExpressionHash(seed) & 63u;
  for (u32 value = 0u; collisions.size() < 48u; value++) {
    ExpressionNode node = seed;
    node.immediate = value;
    if ((ExpressionHash(node) & 63u) == bucket) {
      collisions.push_back(node);
      if (!check(node))
        return false;
    }
  }
  // Distinguish every key field even when the node kind would not consume it.
  // This is an index-identity test, not a claim that arbitrary IR is executable.
  for (u32 field = 0u; field < 13u; field++) {
    ExpressionNode node = seed;
    switch (field) {
    case 0: node.kind = ExpressionKind::InitialVf; break;
    case 1: node.domain = ScalarDomain::Float; break;
    case 2: node.operands[0] = 1u; break;
    case 3: node.operands[1] = 2u; break;
    case 4: node.operands[2] = 3u; break;
    case 5: node.memory_address.base_vi = 17u; break;
    case 6: node.memory_address.invocation_coefficient = -3; break;
    case 7: node.memory_address.qword_offset = -1025; break;
    case 8: node.memory_address.valid = true; break;
    case 9: node.memory_address.outer_invocation_coefficient = 11; break;
    case 10: node.immediate ^= 0x80000000u; break;
    case 11: node.reg = 31u; break;
    case 12: node.lane = 3u; break;
    }
    if (!check(node) || editor.Add(node) == 1u)
      return false;
  }
  for (u32 value = 0u; value < 4096u; value++) {
    ExpressionNode node = seed;
    node.immediate = value ^ 0xa5a5a5a5u;
    node.operands = {value, value / 2u, value / 3u};
    node.memory_address.qword_offset = static_cast<s32>(value) - 2048;
    if (!check(node) || !check(node))
      return false;
  }
  for (const ExpressionNode& node : collisions) {
    if (!check(node))
      return false;
  }
  for (size_t id = 0u; id < indexed.expressions.size(); id++) {
    if (ExpressionKey(indexed.expressions[id]) !=
        ExpressionKey(reference.expressions[id])) {
      return false;
    }
  }
  std::printf("vu-jit-validation: expression-node-growth-policy ok "
              "nodes=%zu capacity=%zu doubling_capacity=%zu element_bytes=%zu "
              "ids=unchanged collisions=exact growth=three-halves\n",
              indexed.expressions.size(), indexed.expressions.capacity(),
              reference.expressions.capacity(), sizeof(ExpressionNode));
  // Exact existing IDs are usable at the bound, but a new node trips the same
  // sticky overflow contract as the original editor. No larger IR is admitted.
  ParallelLoopKernel full;
  full.expressions.resize(MaximumClosedFormExpressions);
  for (u32 id = 1u; id < full.expressions.size(); id++)
    full.expressions[id].immediate = id;
  AppendOnlyExpressionGraphEditor bounded(&full);
  if (bounded.Add(full.expressions.back()) != MaximumClosedFormExpressions - 1u)
    return false;
  ExpressionNode extra;
  extra.immediate = MaximumClosedFormExpressions;
  if (bounded.Add(extra) != InvalidNode || !bounded.Overflowed() ||
      full.expressions.size() != MaximumClosedFormExpressions ||
      bounded.Add(full.expressions.back()) != InvalidNode) {
    return false;
  }
  AppendOnlyExpressionGraphEditor missing(nullptr);
  return missing.Add(seed) == InvalidNode;
}
#endif

bool InlineAcyclicEntrySlice(const ProgramAnalysis& program, u32 loop_index,
                            ParallelLoopKernel* kernel,
                            std::string* error) {
  if (!kernel)
    return Fail(error, "null acyclic entry kernel");
  if (loop_index >= program.natural_loops.size())
    return Fail(error, "acyclic entry loop index is out of range");
  EntrySliceBuilder builder(program, program.natural_loops[loop_index],
                            kernel);
  return builder.Build(error);
}

bool InlineEnclosingLoopPrefixSlice(
    const ProgramAnalysis& program,
    const EnclosingLoopEntryIndependence& proof,
    ParallelLoopKernel* kernel, std::string* error) {
  if (!kernel)
    return Fail(error, "null enclosing-prefix kernel");
  if (kernel->loop_index >= program.natural_loops.size())
    return Fail(error, "enclosing-prefix loop index is out of range");
  EntrySliceBuilder builder(program,
                            program.natural_loops[kernel->loop_index],
                            kernel);
  return builder.BuildEnclosingPrefix(proof, error);
}

bool InlineEnclosingLoopSuffixSlice(
    const ProgramAnalysis& program,
    const EnclosingLoopEntryIndependence& boundary,
    const StructuredLoopTailProof& tail,
    ParallelLoopKernel* kernel, std::string* error) {
  if (!kernel)
    return Fail(error, "null enclosing-suffix kernel");
  if (kernel->loop_index >= program.natural_loops.size())
    return Fail(error, "enclosing-suffix loop index is out of range");
  EntrySliceBuilder builder(program,
                            program.natural_loops[kernel->loop_index],
                            kernel);
  return builder.BuildEnclosingSuffix(boundary, tail, nullptr, error);
}

bool BuildClosedFormNestedLoopKernelAndSuccessorForConfiguration(
    const ProgramAnalysis& program, u32 child_loop_index,
    u32 configuration_bits, u32 outer_iteration_count,
    u32 child_iteration_count, ParallelLoopKernel* kernel,
    ParallelLoopKernel* successor_kernel,
    ClosedFormNestedLoopProof* proof, std::string* error,
    const char** allocation_stage) {
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
  const bool trace_memory =
      std::getenv("VITASX2_GENERATED_VU_CONSTRUCTION_MEMORY") != nullptr;
#endif
  const auto stage = [&](const char* name) {
    if (allocation_stage)
      *allocation_stage = name;
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
    if (trace_memory) {
      const struct mallinfo2 heap = mallinfo2();
      std::printf("vu-jit-validation: compiler-memory phase=%s used=%zu "
                  "mapped=%zu arena=%zu free=%zu\n", name,
                  static_cast<size_t>(heap.uordblks),
                  static_cast<size_t>(heap.hblkhd),
                  static_cast<size_t>(heap.arena),
                  static_cast<size_t>(heap.fordblks));
    }
#endif
  };
  const auto fail = [error](std::string detail) {
    return Fail(error, std::move(detail));
  };
  if (!kernel || !successor_kernel || !proof)
    return fail("null closed-form nested-loop output");
  if (child_loop_index >= program.natural_loops.size() ||
      outer_iteration_count == 0 ||
      outer_iteration_count > MaximumClosedFormOuterIterations ||
      child_iteration_count == 0 ||
      child_iteration_count > std::numeric_limits<u16>::max()) {
    return fail("closed-form nested-loop dimensions are out of range");
  }
  const NaturalLoop& child = program.natural_loops[child_loop_index];
  if (child.parent_loop >= program.natural_loops.size())
    return fail("closed-form child has no enclosing natural loop");
  const NaturalLoop& parent = program.natural_loops[child.parent_loop];
  if (!parent.affine_counter || !parent.branch_taken_repeats ||
      parent.counter_reg == 0 || parent.counter_reg >= 16 ||
      parent.counter_limit_reg >= 16 ||
      (parent.counter_step != 1 && parent.counter_step != -1)) {
    return fail("closed-form parent has no affine counted control");
  }

  stage("nested-child-kernel");
  ParallelLoopKernel child_kernel;
  std::string local_error;
  if (!BuildParallelLoopKernelForConfiguration(
          program, child_loop_index, configuration_bits, &child_kernel,
          &local_error) ||
      !child_kernel.independent_store_values) {
    return fail(local_error.empty()
                    ? "closed-form child store slice is not independent"
                    : std::move(local_error));
  }

  stage("nested-child-entry-proof");
  EnclosingLoopEntryIndependence initial_boundary;
  if (!ProveEnclosingLoopEntryIndependence(
          program, child_kernel, &initial_boundary, &local_error)) {
    return fail(local_error.empty()
                    ? "closed-form child has no enclosing dependence proof"
                    : std::move(local_error));
  }
  // The suffix transfer consumes a canonical child-exit generation before
  // pruning it to the values live at the next parent header.  Requesting the
  // complete architectural value state here is deliberate: it prevents a
  // dead-looking suffix instruction from silently acquiring an Initial* seed
  // when its result later becomes live through the enclosing prefix.  The
  // finite composer below collapses unchanged roots instead of publishing a
  // serial state snapshot.
  std::array<u8, 32> complete_final_vf_lanes{};
  for (u32 reg = 1; reg < complete_final_vf_lanes.size(); reg++)
    complete_final_vf_lanes[reg] = 0x0fu;
  stage("nested-complete-transition");
  ParallelLoopKernel transition;
  if (!BuildParallelLoopKernelWithFinalStateForConfiguration(
          program, child_loop_index, configuration_bits,
          complete_final_vf_lanes, 0x0fu, true, true, true, &transition,
          &local_error) ||
      !transition.independent_final_state) {
    return fail(local_error.empty()
                    ? "closed-form parent-carried child state is unresolved"
                    : std::move(local_error));
  }

  stage("nested-transition-boundary");
  EnclosingLoopEntryIndependence boundary;
  StructuredLoopTailProof tail;
  if (!ProveEnclosingLoopEntryIndependence(
          program, transition, &boundary, &local_error) ||
      boundary.parent_loop != child.parent_loop ||
      !ProveStructuredLoopTailResume(
          program, transition, boundary, &tail, &local_error)) {
    return fail(local_error.empty()
                    ? "closed-form parent transition seam is unresolved"
                    : std::move(local_error));
  }

  const size_t child_expression_count = transition.expressions.size();
  stage("nested-prefix-slice");
  if (!InlineEnclosingLoopPrefixSlice(
          program, boundary, &transition, &local_error)) {
    return fail(local_error.empty()
                    ? "closed-form enclosing prefix could not be sliced"
                    : std::move(local_error));
  }
  const size_t suffix_expression_begin = transition.expressions.size();
  EntryState complete_exit_transition;
  {
    stage("nested-suffix-slice");
    EntrySliceBuilder suffix_builder(program, child, &transition);
    if (!suffix_builder.BuildEnclosingSuffix(
            boundary, tail, &complete_exit_transition, &local_error) ||
        !transition.enclosing_prefix_inlined ||
        !transition.enclosing_suffix_inlined ||
        !transition.independent_child_entry_state ||
        !transition.independent_child_entry_vi) {
      return fail(local_error.empty()
                      ? "closed-form enclosing transition could not be sliced"
                      : std::move(local_error));
    }
  }

  EntryState parent_state;
  EntryCountedLoopSummary entry_summary;
  {
    stage("nested-parent-entry");
    EntrySliceBuilder parent_entry_builder(program, parent, &transition);
    if (!parent_entry_builder.BuildHeaderStateWithCountedPrelude(
            true, outer_iteration_count, &parent_state, &entry_summary,
            &local_error)) {
      return fail(local_error.empty()
                      ? "closed-form parent entry state is unresolved"
                      : std::move(local_error));
    }
  }
  std::array<ViValue, 16> parent_vi = parent_state.vi;
  if (!parent_vi[parent.counter_reg].known ||
      (parent.counter_limit_reg != 0 &&
       !parent_vi[parent.counter_limit_reg].known)) {
    return fail("closed-form parent counter entry is unresolved");
  }
  const std::array<ViValue, 16> parent_entry_vi = parent_vi;

  ClosedFormNestedLoopProof built_proof;
  built_proof.child_loop = child_loop_index;
  built_proof.parent_loop = child.parent_loop;
  built_proof.outer_iteration_count = outer_iteration_count;
  built_proof.child_iteration_count = child_iteration_count;
  built_proof.entry_prefix_pair_count =
      entry_summary.exact_prefix_pair_count;
  built_proof.summarized_entry_loop = entry_summary.loop_index;
  built_proof.summarized_entry_loop_iterations =
      entry_summary.iteration_count;
  built_proof.entry_loop_trip_count_requires_runtime_attestation =
      entry_summary.requires_runtime_trip_attestation;
  if (entry_summary.Valid()) {
    const NaturalLoop& entry_loop =
        program.natural_loops[entry_summary.loop_index];
    built_proof.entry_loop_control_requires_pairplan_preflight =
        !entry_summary.runtime_control_captured ||
        (entry_loop.counter_step != 1 && entry_loop.counter_step != -1);
    if (entry_summary.runtime_control_captured) {
      built_proof.summarized_entry_counter_value =
          PublicViValue(entry_summary.counter_entry_value);
      built_proof.summarized_entry_limit_value =
          PublicViValue(entry_summary.counter_limit_value);
    }
    built_proof.summarized_entry_counter_reg =
        static_cast<u8>(entry_loop.counter_reg);
    built_proof.summarized_entry_limit_reg =
        static_cast<u8>(entry_loop.counter_limit_reg);
    built_proof.summarized_entry_branch_kind = entry_loop.branch_kind;
    built_proof.summarized_entry_counter_step = entry_loop.counter_step;
  }
  built_proof.tail = tail;
  built_proof.parent_counter_reg = static_cast<u8>(parent.counter_reg);
  built_proof.parent_counter_limit_reg =
      static_cast<u8>(parent.counter_limit_reg);
  built_proof.parent_branch_kind = parent.branch_kind;
  built_proof.parent_counter_step = parent.counter_step;
  built_proof.parent_counter_entry_value =
      PublicViValue(parent_vi[parent.counter_reg]);
  const ViValue parent_limit =
      parent.counter_limit_reg == 0 ? ViValue{0, 0, true}
                                    : parent_vi[parent.counter_limit_reg];
  built_proof.parent_counter_limit_value = PublicViValue(parent_limit);
  built_proof.parent_trip_count_requires_runtime_attestation =
      entry_summary.requires_runtime_trip_attestation ||
      parent_vi[parent.counter_reg].base_vi != parent_limit.base_vi;
  if (!built_proof.parent_trip_count_requires_runtime_attestation) {
    const u16 counter = static_cast<u16>(parent_vi[parent.counter_reg].offset);
    const u16 limit = static_cast<u16>(parent_limit.offset);
    u32 static_trip = 0;
    const LowerKind branch = static_cast<LowerKind>(parent.branch_kind);
    if ((branch == LowerKind::IBNE && parent.branch_taken_repeats) ||
        (branch == LowerKind::IBEQ && !parent.branch_taken_repeats)) {
      static_trip = parent.counter_step > 0
                        ? static_cast<u16>(limit - counter)
                        : static_cast<u16>(counter - limit);
    } else if (branch == LowerKind::IBGTZ &&
               parent.branch_taken_repeats &&
               parent.counter_step == -1 && limit == 0) {
      static_trip = counter;
    } else {
      return fail("closed-form parent branch is not a supported exact count");
    }
    if (static_trip != outer_iteration_count)
      return fail("closed-form parent trip count differs from the exact grid");
  }

  stage("nested-outer-composition");
  using VfSequences =
      std::array<std::array<std::vector<u32>, 4>, 32>;
  VfSequences child_vf_sequences;
  std::array<std::vector<u32>, 4> child_acc_sequences;
  std::vector<u32> child_q_sequence;
  std::vector<u32> child_p_sequence;
  std::vector<u32> child_i_sequence;
  std::array<std::vector<ViValue>, 16> child_vi_sequences;
  EntryState complete_final_state;
  bool complete_final_state_ready = false;
  std::optional<ClosedFormExpressionComposer> composer(std::in_place,
      &transition, child_expression_count, suffix_expression_begin,
      outer_iteration_count, child_iteration_count, allocation_stage);

  const auto advance_parent_vi = [&](const std::array<ViValue, 16>& current,
                                     std::array<ViValue, 16>* next) {
    *next = current;
    for (u32 reg = 1; reg < next->size(); reg++) {
      if ((boundary.parent_live_vi_mask & (1u << reg)) == 0 ||
          reg == parent.counter_reg) {
        continue;
      }
      ViValue child_entry{};
      if (!ResolveAffineVi(transition.child_entry_vi_values[reg], current,
                           &child_entry) ||
          !AddOffset(child_entry,
                     static_cast<s64>(child_iteration_count) *
                         transition.vi.step[reg],
                     &(*next)[reg])) {
        return false;
      }
    }
    return AddOffset(current[parent.counter_reg], parent.counter_step,
                     &(*next)[parent.counter_reg]);
  };

  // Slice store publication to addresses which some expression can actually
  // read in this exact finite grid.  Register every generation before the
  // first store is composed, since a write in outer zero may first become
  // visible several enclosing iterations later.
  std::array<ViValue, 16> demand_parent_vi = parent_vi;
  stage("nested-memory-read-demands");
  for (u32 outer = 0; outer < outer_iteration_count; outer++) {
    if (!composer->RegisterMemoryReadDemands(demand_parent_vi, &local_error))
      return fail(std::move(local_error));
    if (outer + 1u < outer_iteration_count) {
      std::array<ViValue, 16> next_demand_parent_vi{};
      if (!advance_parent_vi(demand_parent_vi, &next_demand_parent_vi)) {
        return fail("closed-form parent VI demand transition is not affine");
      }
      demand_parent_vi = next_demand_parent_vi;
    }
  }

  for (u32 outer = 0; outer < outer_iteration_count; outer++) {
    stage("nested-child-entry-composition");
    std::array<ViValue, 16> child_vi{};
    child_vi[0] = {0, 0, true};
    for (u32 reg = 1; reg < child_vi.size(); reg++) {
      if ((transition.child_entry_vi_mask & (1u << reg)) == 0)
        continue;
      if (!ResolveAffineVi(transition.child_entry_vi_values[reg], parent_vi,
                           &child_vi[reg])) {
        return fail("closed-form child VI entry is unresolved");
      }
      child_vi_sequences[reg].push_back(child_vi[reg]);
    }
    if (!ValidateConstantChildTrip(child, child_vi, child_iteration_count,
                                   &local_error)) {
      return fail(std::move(local_error));
    }

    for (u32 reg = 1; reg < initial_boundary.demanded_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (!LaneEnabled(initial_boundary.demanded_vf_lanes[reg], lane))
          continue;
        u32 value = InvalidNode;
        if (!composer->Compose(transition.child_entry_vf_values[reg][lane],
                              parent_state, parent_vi, 0, &value,
                              &local_error)) {
          return fail(std::move(local_error));
        }
        child_vf_sequences[reg][lane].push_back(value);
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if (!LaneEnabled(initial_boundary.demanded_acc_lanes, lane))
        continue;
      u32 value = InvalidNode;
      if (!composer->Compose(transition.child_entry_acc_values[lane],
                            parent_state, parent_vi, 0, &value,
                            &local_error)) {
        return fail(std::move(local_error));
      }
      child_acc_sequences[lane].push_back(value);
    }
    const auto append_scalar = [&](bool requested, u32 root,
                                   std::vector<u32>* sequence) {
      if (!requested)
        return true;
      u32 value = InvalidNode;
      if (!composer->Compose(root, parent_state, parent_vi, 0, &value,
                            &local_error)) {
        return false;
      }
      sequence->push_back(value);
      return true;
    };
    if (!append_scalar(initial_boundary.demanded_q,
                       transition.child_entry_q_value, &child_q_sequence) ||
        !append_scalar(initial_boundary.demanded_p,
                       transition.child_entry_p_value, &child_p_sequence) ||
        !append_scalar(initial_boundary.demanded_i,
                       transition.child_entry_i_value, &child_i_sequence)) {
      return fail(std::move(local_error));
    }

    // The child body can overwrite qwords consumed by the next enclosing
    // iteration. Preserve those stores as expression-graph memory SSA before
    // composing the parent suffix/final state; treating every later load as
    // an immutable epoch input is incorrect for in-place VU algorithms.
    stage("nested-store-generation");
    if (!composer->PrepareOuterStores(parent_state, parent_vi, &local_error))
      return fail(std::move(local_error));

    stage("nested-parent-state-composition");
    EntryState next_parent = parent_state;
    const u32 final_child_iteration = child_iteration_count - 1u;
    for (u32 reg = 1; reg < transition.final_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (!LaneEnabled(transition.final_vf_lanes[reg], lane))
          continue;
        if (!composer->Compose(transition.final_vf_values[reg][lane],
                              parent_state, parent_vi,
                              final_child_iteration,
                              &next_parent.vf[reg][lane], &local_error)) {
          return fail(std::move(local_error));
        }
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(transition.final_acc_lanes, lane) &&
          !composer->Compose(transition.final_acc_values[lane], parent_state,
                            parent_vi, final_child_iteration,
                            &next_parent.acc[lane], &local_error)) {
        return fail(std::move(local_error));
      }
    }
    const auto update_scalar = [&](bool requested, u32 root,
                                   u32* destination) {
      return !requested || composer->Compose(
                               root, parent_state, parent_vi,
                               final_child_iteration, destination,
                               &local_error);
    };
    if (!update_scalar(transition.final_q, transition.final_q_value,
                       &next_parent.q) ||
        !update_scalar(transition.final_p, transition.final_p_value,
                       &next_parent.p) ||
        !update_scalar(transition.final_i, transition.final_i_value,
                       &next_parent.i)) {
      return fail(std::move(local_error));
    }

    if (outer + 1u == outer_iteration_count) {
      stage("nested-final-state-composition");
      EntryState complete = parent_state;
      for (u32 reg = 1; reg < complete_exit_transition.vf.size();
           reg++) {
        for (u32 lane = 0; lane < 4; lane++) {
          if (!composer->Compose(
                  complete_exit_transition.vf[reg][lane],
                  parent_state, parent_vi, final_child_iteration,
                  &complete.vf[reg][lane], &local_error)) {
            return fail(std::move(local_error));
          }
        }
      }
      for (u32 lane = 0; lane < 4; lane++) {
        if (!composer->Compose(
                complete_exit_transition.acc[lane],
                parent_state, parent_vi, final_child_iteration,
                &complete.acc[lane], &local_error)) {
          return fail(std::move(local_error));
        }
      }
      if (!composer->Compose(
              complete_exit_transition.q, parent_state, parent_vi,
              final_child_iteration, &complete.q, &local_error) ||
          !composer->Compose(
              complete_exit_transition.p, parent_state, parent_vi,
              final_child_iteration, &complete.p, &local_error) ||
          !composer->Compose(
              complete_exit_transition.i, parent_state, parent_vi,
              final_child_iteration, &complete.i, &local_error)) {
        return fail(std::move(local_error));
      }
      complete_final_state = std::move(complete);
      complete_final_state_ready = true;
    }

    std::array<ViValue, 16> next_parent_vi{};
    if (!advance_parent_vi(parent_vi, &next_parent_vi)) {
      return fail("closed-form parent counter transition overflowed");
    }
    stage("nested-store-generation-commit");
    composer->CommitOuterStores();
    parent_state = std::move(next_parent);
    parent_vi = next_parent_vi;
  }
  if (composer->Overflowed())
    return fail("closed-form recurrence exceeded its expression budget");
  if (!complete_final_state_ready)
    return fail("closed-form architectural successor was not produced");

  stage("nested-recurrence-collapse");
  std::array<std::array<u32, 4>, 32> collapsed_child_vf{};
  std::array<u32, 4> collapsed_child_acc{};
  u32 collapsed_child_q = InvalidNode;
  u32 collapsed_child_p = InvalidNode;
  u32 collapsed_child_i = InvalidNode;
  for (u32 reg = 1; reg < initial_boundary.demanded_vf_lanes.size(); reg++) {
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(initial_boundary.demanded_vf_lanes[reg], lane) &&
          !composer->Collapse(child_vf_sequences[reg][lane],
                             &collapsed_child_vf[reg][lane], &local_error)) {
        const u32 transition_root =
            transition.child_entry_vf_values[reg][lane];
        const ExpressionNode* transition_node =
            transition_root < transition.expressions.size()
                ? &transition.expressions[transition_root]
                : nullptr;
        return fail("closed-form child VF" + std::to_string(reg) + "." +
                    std::to_string(lane) + " recurrence root=" +
                    std::to_string(transition_root) + ":" +
                    (transition_node
                         ? std::to_string(static_cast<u32>(
                               transition_node->kind)) + "[" +
                               std::to_string(transition_node->operands[0]) +
                               "," +
                               std::to_string(transition_node->operands[1]) +
                               "," +
                               std::to_string(transition_node->operands[2]) +
                               "]"
                         : std::string("invalid")) +
                    ": " + local_error);
      }
    }
  }
  for (u32 lane = 0; lane < 4; lane++) {
    if (LaneEnabled(initial_boundary.demanded_acc_lanes, lane) &&
        !composer->Collapse(child_acc_sequences[lane],
                           &collapsed_child_acc[lane], &local_error)) {
      return fail("closed-form child ACC." + std::to_string(lane) +
                  " recurrence: " + local_error);
    }
  }
  const auto collapse_scalar = [&](bool requested,
                                   const std::vector<u32>& sequence,
                                   u32* destination) {
    return !requested || composer->Collapse(sequence, destination, &local_error);
  };
  if (!collapse_scalar(initial_boundary.demanded_q, child_q_sequence,
                       &collapsed_child_q) ||
      !collapse_scalar(initial_boundary.demanded_p, child_p_sequence,
                       &collapsed_child_p) ||
      !collapse_scalar(initial_boundary.demanded_i, child_i_sequence,
                       &collapsed_child_i)) {
    return fail("closed-form child scalar recurrence: " + local_error);
  }

  std::array<OuterAffineVi, 16> collapsed_child_vi{};
  collapsed_child_vi[0] = {0, 0, 0, true};
  for (u32 reg = 1; reg < collapsed_child_vi.size(); reg++) {
    if ((transition.child_entry_vi_mask & (1u << reg)) != 0 &&
        !CollapseViSequence(child_vi_sequences[reg],
                            &collapsed_child_vi[reg], &local_error)) {
      return fail(std::move(local_error));
    }
  }

  // Root IDs now belong to transition. Its composer only owns memoization
  // maps; release those before allocating the two output graph editors.
  composer.reset();
  stage("nested-output-graph");
  ParallelLoopKernel flattened = std::move(child_kernel);
  flattened.compact_outer_inputs = transition.compact_outer_inputs;
  flattened.outer_repeated_add_schedules =
      transition.outer_repeated_add_schedules;
  const size_t original_child_expressions = flattened.expressions.size();
  {
    ExpressionGraphEditor flattened_editor(&flattened);
    std::map<u32, u32> imported;
    std::array<std::array<u32, 4>, 32> imported_child_vf{};
    std::array<u32, 4> imported_child_acc{};
    const auto import = [&](u32 source, u32 *destination) {
      if (source == InvalidNode)
        return false;
      *destination = ImportExpression(transition, source, &flattened_editor,
                                      &imported, &local_error);
      return *destination != InvalidNode;
    };
    for (u32 reg = 1; reg < initial_boundary.demanded_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(initial_boundary.demanded_vf_lanes[reg], lane) &&
            !import(collapsed_child_vf[reg][lane],
                    &imported_child_vf[reg][lane])) {
          return fail(std::move(local_error));
        }
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(initial_boundary.demanded_acc_lanes, lane) &&
          !import(collapsed_child_acc[lane], &imported_child_acc[lane])) {
        return fail(std::move(local_error));
      }
    }
    u32 imported_child_q = InvalidNode;
    u32 imported_child_p = InvalidNode;
    u32 imported_child_i = InvalidNode;
    if ((initial_boundary.demanded_q &&
         !import(collapsed_child_q, &imported_child_q)) ||
        (initial_boundary.demanded_p &&
         !import(collapsed_child_p, &imported_child_p)) ||
        (initial_boundary.demanded_i &&
         !import(collapsed_child_i, &imported_child_i))) {
      return fail(std::move(local_error));
    }

    std::map<u32, u32> replacements;
    for (u32 id = 1; id < original_child_expressions; id++) {
      ExpressionNode &node = flattened.expressions[id];
      u32 replacement = InvalidNode;
      switch (node.kind) {
      case ExpressionKind::InvariantVf:
        replacement = imported_child_vf[node.reg][node.lane];
        break;
      case ExpressionKind::InvariantAcc:
        replacement = imported_child_acc[node.lane];
        break;
      case ExpressionKind::InvariantQ:
        replacement = imported_child_q;
        break;
      case ExpressionKind::InvariantP:
        replacement = imported_child_p;
        break;
      case ExpressionKind::InvariantI:
        replacement = imported_child_i;
        break;
      default:
        break;
      }
      if (replacement != InvalidNode)
        replacements.emplace(id, replacement);
      if (node.kind == ExpressionKind::Memory &&
          !NormalizeChildAddress(node.memory_address, collapsed_child_vi,
                                 &node.memory_address, &local_error)) {
        return fail(std::move(local_error));
      }
    }
    for (u32 id = 1; id < original_child_expressions; id++) {
      for (u32 &operand : flattened.expressions[id].operands) {
        const auto replacement = replacements.find(operand);
        if (replacement != replacements.end())
          operand = replacement->second;
      }
    }
    for (LoopStore &store : flattened.stores) {
      if (!NormalizeChildAddress(store.address, collapsed_child_vi,
                                 &store.address, &local_error)) {
        return fail(std::move(local_error));
      }
      for (u32 &value : store.values) {
        const auto replacement = replacements.find(value);
        if (replacement != replacements.end())
          value = replacement->second;
      }
    }

    flattened.child_entry_vf_lanes = initial_boundary.demanded_vf_lanes;
    flattened.child_entry_vf_values = imported_child_vf;
    flattened.child_entry_acc_lanes = initial_boundary.demanded_acc_lanes;
    flattened.child_entry_acc_values = imported_child_acc;
    flattened.child_entry_q = initial_boundary.demanded_q;
    flattened.child_entry_p = initial_boundary.demanded_p;
    flattened.child_entry_i = initial_boundary.demanded_i;
    flattened.child_entry_q_value = imported_child_q;
    flattened.child_entry_p_value = imported_child_p;
    flattened.child_entry_i_value = imported_child_i;
    flattened.child_entry_vi_mask = initial_boundary.demanded_vi_mask;
    for (u32 reg = 0; reg < collapsed_child_vi.size(); reg++) {
      if (collapsed_child_vi[reg].valid) {
        flattened.child_entry_vi_values[reg] = {collapsed_child_vi[reg].base_vi,
                                                collapsed_child_vi[reg].offset,
                                                true};
      }
    }

    flattened.final_vf_lanes = transition.final_vf_lanes;
    flattened.final_acc_lanes = transition.final_acc_lanes;
    flattened.final_q = transition.final_q;
    flattened.final_p = transition.final_p;
    flattened.final_i = transition.final_i;
    bool final_state_complete = true;
    for (u32 reg = 1; reg < flattened.final_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(flattened.final_vf_lanes[reg], lane) &&
            !import(parent_state.vf[reg][lane],
                    &flattened.final_vf_values[reg][lane])) {
          final_state_complete = false;
        }
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(flattened.final_acc_lanes, lane) &&
          !import(parent_state.acc[lane], &flattened.final_acc_values[lane])) {
        final_state_complete = false;
      }
    }
    if ((flattened.final_q &&
         !import(parent_state.q, &flattened.final_q_value)) ||
        (flattened.final_p &&
         !import(parent_state.p, &flattened.final_p_value)) ||
        (flattened.final_i &&
         !import(parent_state.i, &flattened.final_i_value))) {
      final_state_complete = false;
    }
    if (!final_state_complete || flattened_editor.Overflowed())
      return fail("closed-form final parent state is not representable");
  }

  stage("nested-successor-graph");
  // This owner evaluates final architectural registers only. The old copy
  // retained the raster/store graph until a later final-state compaction,
  // overlapping both graphs and their interning maps at Vita's cold-build
  // heap peak. Import the same complete_exit_transition-derived roots into
  // an empty graph instead; VUops.cpp still defines every value and store,
  // and the separate flattened kernel retains the entire store contract.
  // Keep table indices stable, as in CompactLoopKernelFinalStateGraph.
  ParallelLoopKernel complete_successor;
  complete_successor.loop_index = flattened.loop_index;
  complete_successor.header_pc = flattened.header_pc;
  complete_successor.latch_pc = flattened.latch_pc;
  complete_successor.pair_count = flattened.pair_count;
  complete_successor.maximum_backedge_distance = flattened.maximum_backedge_distance;
  complete_successor.configuration_bits = flattened.configuration_bits;
  complete_successor.outer_iteration_count = flattened.outer_iteration_count;
  complete_successor.child_iteration_count = flattened.child_iteration_count;
  complete_successor.vi = flattened.vi;
  complete_successor.compact_outer_inputs = flattened.compact_outer_inputs;
  complete_successor.outer_repeated_add_schedules = flattened.outer_repeated_add_schedules;
  complete_successor.expressions.push_back(transition.expressions.front());
  {
    ExpressionGraphEditor successor_editor(&complete_successor);
    std::map<u32, u32> successor_imported;
    const auto import_successor = [&](u32 source, u32 *destination) {
      if (source == InvalidNode)
        return false;
      *destination = ImportExpression(transition, source, &successor_editor,
                                      &successor_imported, &local_error);
      return *destination != InvalidNode;
    };
    complete_successor.final_vf_lanes.fill(0u);
    for (u32 reg = 1; reg < complete_successor.final_vf_lanes.size(); reg++) {
      complete_successor.final_vf_lanes[reg] = 0x0fu;
      for (u32 lane = 0; lane < 4; lane++) {
        if (!import_successor(complete_final_state.vf[reg][lane],
                              &complete_successor.final_vf_values[reg][lane])) {
          return fail(local_error.empty()
                          ? "closed-form complete VF successor is unresolved"
                          : std::move(local_error));
        }
      }
    }
    complete_successor.final_acc_lanes = 0x0fu;
    for (u32 lane = 0; lane < 4; lane++) {
      if (!import_successor(complete_final_state.acc[lane],
                            &complete_successor.final_acc_values[lane])) {
        return fail(local_error.empty()
                        ? "closed-form complete ACC successor is unresolved"
                        : std::move(local_error));
      }
    }
    complete_successor.final_q = true;
    complete_successor.final_p = true;
    complete_successor.final_i = true;
    if (!import_successor(complete_final_state.q,
                          &complete_successor.final_q_value) ||
        !import_successor(complete_final_state.p,
                          &complete_successor.final_p_value) ||
        !import_successor(complete_final_state.i,
                          &complete_successor.final_i_value) ||
        successor_editor.Overflowed()) {
      return fail(local_error.empty()
                      ? "closed-form complete scalar successor is unresolved"
                      : std::move(local_error));
    }
    complete_successor.independent_final_state = true;
  }

  std::vector<u8> visited(flattened.expressions.size());
  for (const LoopStore& store : flattened.stores) {
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(store.write_mask, lane) &&
          IsReachableInvariant(flattened, store.values[lane], &visited)) {
        return fail("closed-form child output retains a dynamic invariant");
      }
    }
  }

  flattened.outer_iteration_count = outer_iteration_count;
  flattened.child_iteration_count = child_iteration_count;
  flattened.configuration_bits = configuration_bits;
  flattened.acyclic_entry_inlined = true;
  flattened.enclosing_prefix_inlined = true;
  flattened.enclosing_suffix_inlined = true;
  flattened.enclosing_final_state_pass_through =
      transition.enclosing_final_state_pass_through;
  flattened.enclosing_final_vi_state_pass_through =
      transition.enclosing_final_vi_state_pass_through;
  flattened.independent_child_entry_state = true;
  flattened.independent_child_entry_vi = true;
  flattened.independent_final_state = true;
  flattened.requires_dynamic_entry_state = false;
  flattened.has_true_recurrence = false;
  flattened.has_unsupported_expression = false;
  flattened.independent_store_values = true;

  stage("nested-final-vi-proof");
  std::array<ViValue, 16> exact_final_parent_vi{};
  u16 exact_parent_vi_writes = 0u;
  if (!BuildExactClosedFormParentViState(
          program, child, boundary, tail, outer_iteration_count,
          child_iteration_count, parent_entry_vi, &exact_final_parent_vi,
          &exact_parent_vi_writes, &local_error)) {
    return fail(local_error.empty()
                    ? "closed-form complete parent VI state is unresolved"
                    : std::move(local_error));
  }
  const u16 live_parent_vi = static_cast<u16>(
      boundary.parent_live_vi_mask | (1u << parent.counter_reg));
  for (u32 reg = 1; reg < parent_vi.size(); reg++) {
    if ((live_parent_vi & (1u << reg)) != 0u &&
        (!(exact_final_parent_vi[reg] == parent_vi[reg]))) {
      return fail("closed-form complete VI transfer differs from the "
                  "liveness-pruned transition");
    }
    built_proof.final_parent_vi_values[reg] =
        PublicViValue(exact_final_parent_vi[reg]);
  }
  built_proof.final_parent_vi_mask = exact_parent_vi_writes;
  built_proof.parent_entry_reduced = true;
  built_proof.child_entry_reduced = true;
  built_proof.final_parent_state_reduced = true;

  stage("nested-construction-complete");
  *kernel = std::move(flattened);
  *successor_kernel = std::move(complete_successor);
  *proof = std::move(built_proof);
  if (error)
    error->clear();
  return true;
}

bool CompactLoopKernelFinalStateGraph(
    ParallelLoopKernel* kernel, u32* original_expression_count,
    std::string* error) {
  if (!kernel || kernel->expressions.empty())
    return Fail(error, "null or empty final-state expression graph");
  if (kernel->final_vf_lanes[0] != 0u ||
      kernel->final_acc_lanes != 0x0fu || !kernel->final_q ||
      !kernel->final_p || !kernel->final_i ||
      !kernel->independent_final_state) {
    return Fail(error, "final-state expression graph is incomplete");
  }
  for (u32 reg = 1u; reg < kernel->final_vf_lanes.size(); reg++) {
    if (kernel->final_vf_lanes[reg] != 0x0fu)
      return Fail(error, "final-state VF expression graph is incomplete");
  }

  ParallelLoopKernel source = std::move(*kernel);
  if (original_expression_count) {
    *original_expression_count =
        static_cast<u32>(source.expressions.size());
  }
  ParallelLoopKernel compacted;
  compacted.expressions.push_back(source.expressions.front());
  ExpressionGraphEditor editor(&compacted);
  std::map<u32, u32> memo;
  const auto restore_and_fail = [&](const char* message) {
    *kernel = std::move(source);
    return Fail(error, message);
  };
  const auto import = [&](u32 source_id, u32* destination) {
    const u32 imported = ImportExpression(
        source, source_id, &editor, &memo, error);
    if (imported == InvalidNode)
      return false;
    *destination = imported;
    return true;
  };

  for (u32 reg = 1u; reg < source.final_vf_values.size(); reg++) {
    for (u32 lane = 0u; lane < 4u; lane++) {
      if (!import(source.final_vf_values[reg][lane],
                  &compacted.final_vf_values[reg][lane])) {
        return restore_and_fail(
            "failed to compact final-state VF expression graph");
      }
    }
  }
  for (u32 lane = 0u; lane < 4u; lane++) {
    if (!import(source.final_acc_values[lane],
                &compacted.final_acc_values[lane])) {
      return restore_and_fail(
          "failed to compact final-state ACC expression graph");
    }
  }
  if (!import(source.final_q_value, &compacted.final_q_value) ||
      !import(source.final_p_value, &compacted.final_p_value) ||
      !import(source.final_i_value, &compacted.final_i_value) ||
      editor.Overflowed()) {
    return restore_and_fail(
        "failed to compact final-state scalar expression graph");
  }

  // Preserve only metadata consumed by the host successor evaluator. Table
  // indices embedded in CompactOuterInput and OuterRepeatedAdd nodes remain
  // stable because their immutable tables move without reordering.
  compacted.loop_index = source.loop_index;
  compacted.header_pc = source.header_pc;
  compacted.latch_pc = source.latch_pc;
  compacted.pair_count = source.pair_count;
  compacted.maximum_backedge_distance = source.maximum_backedge_distance;
  compacted.configuration_bits = source.configuration_bits;
  compacted.outer_iteration_count = source.outer_iteration_count;
  compacted.child_iteration_count = source.child_iteration_count;
  compacted.vi = source.vi;
  compacted.compact_outer_inputs = std::move(source.compact_outer_inputs);
  compacted.outer_repeated_add_schedules =
      std::move(source.outer_repeated_add_schedules);
  compacted.final_vf_lanes = source.final_vf_lanes;
  compacted.final_acc_lanes = source.final_acc_lanes;
  compacted.final_q = source.final_q;
  compacted.final_p = source.final_p;
  compacted.final_i = source.final_i;
  compacted.independent_final_state = true;
  compacted.has_unsupported_expression = false;
  *kernel = std::move(compacted);
  if (error)
    error->clear();
  return true;
}

bool BuildClosedFormNestedLoopKernelForConfiguration(
    const ProgramAnalysis& program, u32 child_loop_index,
    u32 configuration_bits, u32 outer_iteration_count,
    u32 child_iteration_count, ParallelLoopKernel* kernel,
    ClosedFormNestedLoopProof* proof, std::string* error) {
  ParallelLoopKernel successor;
  return BuildClosedFormNestedLoopKernelAndSuccessorForConfiguration(
      program, child_loop_index, configuration_bits, outer_iteration_count,
      child_iteration_count, kernel, &successor, proof, error);
}

namespace {

struct ParentCarriedState {
  std::array<std::array<bool, 4>, 32> vf{};
  std::array<bool, 4> acc{};
  std::array<bool, 16> vi{};
  bool q = true;
  bool p = true;
  bool i = true;
};

ParentCarriedState InitialParentCarriedState() {
  ParentCarriedState state;
  for (u32 reg = 1; reg < state.vf.size(); reg++)
    state.vf[reg].fill(true);
  state.acc.fill(true);
  for (u32 reg = 1; reg < state.vi.size(); reg++)
    state.vi[reg] = true;
  return state;
}

bool AnyVfDependency(const ParentCarriedState& state, u8 reg, u8 mask) {
  if (reg == 0)
    return false;
  for (u32 lane = 0; lane < 4; lane++) {
    if (LaneEnabled(mask, lane) && state.vf[reg][lane])
      return true;
  }
  return false;
}

bool AnyAccDependency(const ParentCarriedState& state) {
  return std::any_of(state.acc.begin(), state.acc.end(),
                     [](bool value) { return value; });
}

bool PairInputDependency(const VitaVU::GpuPairPlan& plan,
                         const ParentCarriedState& old, bool upper) {
  const u8 read0 = upper ? plan.upper_vf_read0 : plan.lower_vf_read0;
  const u8 mask0 =
      upper ? plan.upper_vf_read0_mask : plan.lower_vf_read0_mask;
  const u8 read1 = upper ? plan.upper_vf_read1 : plan.lower_vf_read1;
  const u8 mask1 =
      upper ? plan.upper_vf_read1_mask : plan.lower_vf_read1_mask;
  if (AnyVfDependency(old, read0, mask0) ||
      AnyVfDependency(old, read1, mask1)) {
    return true;
  }
  const u32 vi_reads = upper ? plan.upper_vi_read : plan.lower_vi_read;
  for (u32 reg = 1; reg < old.vi.size(); reg++) {
    if ((vi_reads & (1u << reg)) != 0 && old.vi[reg])
      return true;
  }
  return ((vi_reads & (1u << REG_ACC_FLAG)) != 0 &&
          AnyAccDependency(old)) ||
         ((vi_reads & (1u << REG_I)) != 0 && old.i) ||
         ((vi_reads & (1u << REG_Q)) != 0 && old.q) ||
         ((vi_reads & (1u << REG_P)) != 0 && old.p);
}

void TransferParentPair(const VitaVU::GpuPairPlan& plan,
                        ParentCarriedState* state) {
  const ParentCarriedState old = *state;
  if (plan.exec_lower && !plan.lower_discarded_by_upper) {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    const bool lower_dependency = PairInputDependency(plan, old, false);
    if (plan.lower_vf_write != 0) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (!LaneEnabled(plan.lower_vf_write_mask, lane))
          continue;
        bool dependency = lower_dependency;
        if (kind == LowerKind::LQ || kind == LowerKind::LQI ||
            kind == LowerKind::LQD) {
          dependency = false;
        } else if (kind == LowerKind::MOVE) {
          dependency = old.vf[VUInterpFast::Fs(code)][lane];
        } else if (kind == LowerKind::MR32) {
          dependency = old.vf[VUInterpFast::Fs(code)][(lane + 1u) & 3u];
        } else if (kind == LowerKind::MFIR) {
          dependency = old.vi[VUInterpFast::Is(code)];
        } else if (kind == LowerKind::MFP) {
          dependency = old.p;
        }
        state->vf[plan.lower_vf_write][lane] = dependency;
      }
    }

    const u32 vi_writes = plan.lower_vi_write;
    for (u32 reg = 1; reg < state->vi.size(); reg++) {
      if ((vi_writes & (1u << reg)) != 0)
        state->vi[reg] = lower_dependency;
    }
    const u32 is = VUInterpFast::Is(code);
    const u32 it = VUInterpFast::It(code);
    const u32 id = VUInterpFast::Id(code);
    switch (kind) {
    case LowerKind::IADDIU:
    case LowerKind::ISUBIU:
    case LowerKind::IADDI:
      if (it != 0)
        state->vi[it] = old.vi[is];
      break;
    case LowerKind::IADD:
    case LowerKind::ISUB:
    case LowerKind::IAND:
    case LowerKind::IOR:
      if (id != 0)
        state->vi[id] = old.vi[is] || old.vi[it];
      break;
    case LowerKind::ILW:
    case LowerKind::ILWR:
      if (it != 0)
        state->vi[it] = false;
      break;
    case LowerKind::LQI:
    case LowerKind::LQD:
      if (is != 0)
        state->vi[is] = old.vi[is];
      break;
    case LowerKind::SQI:
    case LowerKind::SQD:
      if (it != 0)
        state->vi[it] = old.vi[it];
      break;
    case LowerKind::BAL:
    case LowerKind::JALR:
      if (it != 0)
        state->vi[it] = false;
      break;
    case LowerKind::XITOP:
    case LowerKind::XTOP:
      if (it != 0)
        state->vi[it] = false;
      break;
    default:
      break;
    }
    if ((vi_writes & (1u << REG_Q)) != 0)
      state->q = lower_dependency;
    if ((vi_writes & (1u << REG_P)) != 0)
      state->p = lower_dependency;
    if (plan.immediate_lower)
      state->i = false;
  }

  if (plan.exec_upper) {
    const bool upper_dependency = PairInputDependency(plan, old, true);
    if ((plan.upper_vi_write & (1u << REG_ACC_FLAG)) != 0) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(plan.upper_vf_write_mask, lane))
          state->acc[lane] = upper_dependency;
      }
    } else if (plan.upper_vf_write != 0) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(plan.upper_vf_write_mask, lane))
          state->vf[plan.upper_vf_write][lane] = upper_dependency;
      }
    }
  }
  state->vi[0] = false;
}

struct ParentLiveState {
  std::array<u8, 32> vf{};
  u16 vi = 0;
  u8 acc = 0;
  bool q = false;
  bool p = false;
  bool i = false;
};

void AddPairLiveInputs(const VitaVU::GpuPairPlan& plan, bool upper,
                       ParentLiveState* live) {
  const u8 read0 = upper ? plan.upper_vf_read0 : plan.lower_vf_read0;
  const u8 mask0 =
      upper ? plan.upper_vf_read0_mask : plan.lower_vf_read0_mask;
  const u8 read1 = upper ? plan.upper_vf_read1 : plan.lower_vf_read1;
  const u8 mask1 =
      upper ? plan.upper_vf_read1_mask : plan.lower_vf_read1_mask;
  if (read0 != 0)
    live->vf[read0] |= mask0;
  if (read1 != 0)
    live->vf[read1] |= mask1;
  const u32 vi_reads = upper ? plan.upper_vi_read : plan.lower_vi_read;
  live->vi |= static_cast<u16>(vi_reads & 0xfffeu);
  if ((vi_reads & (1u << REG_ACC_FLAG)) != 0)
    live->acc |= 0x0fu;
  live->i |= (vi_reads & (1u << REG_I)) != 0;
  live->q |= (vi_reads & (1u << REG_Q)) != 0;
  live->p |= (vi_reads & (1u << REG_P)) != 0;
}

void TransferParentLivenessBackward(const VitaVU::GpuPairPlan& plan,
                                    ParentLiveState* live) {
  if (plan.exec_upper) {
    if ((plan.upper_vi_write & (1u << REG_ACC_FLAG)) != 0) {
      const u8 overwritten = live->acc & plan.upper_vf_write_mask;
      if (overwritten != 0) {
        live->acc &= static_cast<u8>(~plan.upper_vf_write_mask);
        AddPairLiveInputs(plan, true, live);
      }
    } else if (plan.upper_vf_write != 0) {
      const u8 overwritten =
          live->vf[plan.upper_vf_write] & plan.upper_vf_write_mask;
      if (overwritten != 0) {
        live->vf[plan.upper_vf_write] &=
            static_cast<u8>(~plan.upper_vf_write_mask);
        AddPairLiveInputs(plan, true, live);
      }
    }
  }
  if (!plan.exec_lower || plan.lower_discarded_by_upper)
    return;
  bool lower_result_live = false;
  if (plan.lower_vf_write != 0) {
    const u8 overwritten =
        live->vf[plan.lower_vf_write] & plan.lower_vf_write_mask;
    if (overwritten != 0) {
      live->vf[plan.lower_vf_write] &=
          static_cast<u8>(~plan.lower_vf_write_mask);
      lower_result_live = true;
    }
  }
  if ((plan.lower_vi_write & (1u << REG_Q)) != 0 && live->q) {
    live->q = false;
    lower_result_live = true;
  }
  if ((plan.lower_vi_write & (1u << REG_P)) != 0 && live->p) {
    live->p = false;
    lower_result_live = true;
  }
  const u16 overwritten_vi = static_cast<u16>(
      live->vi & plan.lower_vi_write & 0xfffeu);
  if (overwritten_vi != 0) {
    live->vi &= static_cast<u16>(~overwritten_vi);
    lower_result_live = true;
  }
  if (plan.immediate_lower && live->i)
    live->i = false;
  if (lower_result_live)
    AddPairLiveInputs(plan, false, live);
}

} // namespace

bool ProveEnclosingLoopEntryIndependence(
    const ProgramAnalysis& program, const ParallelLoopKernel& child_kernel,
    EnclosingLoopEntryIndependence* proof, std::string* error) {
  if (!proof)
    return Fail(error, "null enclosing-loop entry proof");
  *proof = {};
  if (child_kernel.loop_index >= program.natural_loops.size())
    return Fail(error, "enclosing-loop child index is out of range");
  const NaturalLoop& child = program.natural_loops[child_kernel.loop_index];
  if (child.parent_loop >= program.natural_loops.size())
    return Fail(error, "parallel loop has no enclosing natural loop");
  const NaturalLoop& parent = program.natural_loops[child.parent_loop];
  proof->child_loop = child_kernel.loop_index;
  proof->parent_loop = child.parent_loop;

  for (const ExpressionNode& node : child_kernel.expressions) {
    switch (node.kind) {
    case ExpressionKind::InvariantVf:
      proof->demanded_vf_lanes[node.reg] |=
          static_cast<u8>(0x8u >> node.lane);
      break;
    case ExpressionKind::InvariantAcc:
      proof->demanded_acc_lanes |= static_cast<u8>(0x8u >> node.lane);
      break;
    case ExpressionKind::InvariantQ:
      proof->demanded_q = true;
      break;
    case ExpressionKind::InvariantP:
      proof->demanded_p = true;
      break;
    case ExpressionKind::InvariantI:
      proof->demanded_i = true;
      break;
    default:
      break;
    }
    if (node.kind == ExpressionKind::Memory &&
        node.memory_address.valid && node.memory_address.base_vi != 0) {
      proof->demanded_vi_mask |=
          static_cast<u16>(1u << node.memory_address.base_vi);
    }
  }
  for (const LoopStore& store : child_kernel.stores) {
    if (store.address.valid && store.address.base_vi != 0) {
      proof->demanded_vi_mask |=
          static_cast<u16>(1u << store.address.base_vi);
    }
  }
  if (child.counter_reg != 0)
    proof->demanded_vi_mask |= static_cast<u16>(1u << child.counter_reg);
  if (child.counter_limit_reg != 0) {
    proof->demanded_vi_mask |=
        static_cast<u16>(1u << child.counter_limit_reg);
  }
  // A structured final-state stage publishes every VI changed by the child.
  // Demand only the architectural base of each symbolic exit formula; a VI
  // reset to a constant does not depend on its stale entry value.
  for (u32 reg = 1; reg < 16; reg++) {
    if ((child_kernel.vi.written_mask & (1u << reg)) == 0 ||
        child_kernel.vi.AddressesForRegister(reg).empty()) {
      continue;
    }
    const AffineQwordAddress& final =
        child_kernel.vi.AddressesForRegister(reg).back();
    if (final.valid && final.base_vi != 0) {
      proof->demanded_vi_mask |=
          static_cast<u16>(1u << final.base_vi);
    }
  }

  const std::set<u32> parent_blocks(parent.blocks.begin(),
                                    parent.blocks.end());
  std::vector<bool> can_reach_child(program.blocks.size(), false);
  can_reach_child[child.header_block] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const u32 block_index : parent.blocks) {
      if (can_reach_child[block_index])
        continue;
      for (const ControlEdge& edge : program.blocks[block_index].successors) {
        const bool parent_backedge = block_index == parent.latch_block &&
                                     edge.has_target &&
                                     edge.target_block == parent.header_block;
        if (!parent_backedge && edge.has_target &&
            edge.target_block < can_reach_child.size() &&
            can_reach_child[edge.target_block]) {
          can_reach_child[block_index] = true;
          changed = true;
          break;
        }
      }
    }
  }
  if (!can_reach_child[parent.header_block])
    return Fail(error, "enclosing-loop header cannot reach child header");

  ParentCarriedState state = InitialParentCarriedState();
  std::set<u32> visited;
  u32 block_index = parent.header_block;
  while (block_index != child.header_block) {
    if (!visited.insert(block_index).second)
      return Fail(error, "enclosing-loop entry prefix contains a cycle");
    for (const u32 sibling : parent.child_loops) {
      if (sibling == child_kernel.loop_index)
        continue;
      if (std::binary_search(program.natural_loops[sibling].blocks.begin(),
                             program.natural_loops[sibling].blocks.end(),
                             block_index)) {
        return Fail(error,
                    "enclosing-loop entry prefix crosses a sibling loop");
      }
    }
    proof->prefix_blocks.push_back(block_index);
    for (const ProgramPair& pair : program.blocks[block_index].pairs)
      TransferParentPair(pair.plan, &state);

    u32 successor = std::numeric_limits<u32>::max();
    u32 successor_count = 0;
    for (const ControlEdge& edge : program.blocks[block_index].successors) {
      const bool parent_backedge = block_index == parent.latch_block &&
                                   edge.has_target &&
                                   edge.target_block == parent.header_block;
      if (parent_backedge || !edge.has_target ||
          edge.target_block >= can_reach_child.size() ||
          !can_reach_child[edge.target_block]) {
        continue;
      }
      successor = edge.target_block;
      successor_count++;
    }
    if (successor_count != 1 || !parent_blocks.contains(successor))
      return Fail(error,
                  "enclosing-loop entry prefix is not a unique acyclic path");
    block_index = successor;
  }

  ParentLiveState live;
  live.vf = proof->demanded_vf_lanes;
  live.vi = proof->demanded_vi_mask;
  live.acc = proof->demanded_acc_lanes;
  live.q = proof->demanded_q;
  live.p = proof->demanded_p;
  live.i = proof->demanded_i;
  for (auto block = proof->prefix_blocks.rbegin();
       block != proof->prefix_blocks.rend(); ++block) {
    const auto& pairs = program.blocks[*block].pairs;
    for (auto pair = pairs.rbegin(); pair != pairs.rend(); ++pair)
      TransferParentLivenessBackward(pair->plan, &live);
  }
  proof->parent_live_vf_lanes = live.vf;
  proof->parent_live_vi_mask = live.vi;
  proof->parent_live_acc_lanes = live.acc;
  proof->parent_live_q = live.q;
  proof->parent_live_p = live.p;
  proof->parent_live_i = live.i;

  for (u32 reg = 1; reg < proof->demanded_vf_lanes.size(); reg++) {
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(proof->demanded_vf_lanes[reg], lane) &&
          state.vf[reg][lane]) {
        proof->prior_vf_lanes[reg] |= static_cast<u8>(0x8u >> lane);
      }
    }
  }
  for (u32 lane = 0; lane < 4; lane++) {
    if (LaneEnabled(proof->demanded_acc_lanes, lane) && state.acc[lane])
      proof->prior_acc_lanes |= static_cast<u8>(0x8u >> lane);
  }
  proof->prior_q = proof->demanded_q && state.q;
  proof->prior_p = proof->demanded_p && state.p;
  proof->prior_i = proof->demanded_i && state.i;
  for (u32 reg = 1; reg < state.vi.size(); reg++) {
    if ((proof->demanded_vi_mask & (1u << reg)) != 0 && state.vi[reg])
      proof->prior_vi_mask |= static_cast<u16>(1u << reg);
  }
  proof->independent_of_prior_parent_state =
      std::all_of(proof->prior_vf_lanes.begin(),
                  proof->prior_vf_lanes.end(),
                  [](u8 lanes) { return lanes == 0; }) &&
      proof->prior_acc_lanes == 0 && proof->prior_vi_mask == 0 &&
      !proof->prior_q && !proof->prior_p && !proof->prior_i;
  if (error)
    error->clear();
  return true;
}

bool ProveStructuredLoopTailResume(
    const ProgramAnalysis& program, const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    StructuredLoopTailProof* proof, std::string* error) {
  if (!proof)
    return Fail(error, "null structured-loop tail proof");
  *proof = {};
  if (child_kernel.loop_index >= program.natural_loops.size() ||
      boundary.child_loop != child_kernel.loop_index ||
      boundary.parent_loop >= program.natural_loops.size()) {
    return Fail(error, "structured-loop tail boundary does not match kernel");
  }
  const NaturalLoop& child = program.natural_loops[child_kernel.loop_index];
  const NaturalLoop& parent = program.natural_loops[boundary.parent_loop];
  if (child.parent_loop != boundary.parent_loop ||
      child.latch_block >= program.blocks.size() ||
      parent.header_block >= program.blocks.size() ||
      parent.latch_block >= program.blocks.size()) {
    return Fail(error, "structured-loop tail has invalid loop ownership");
  }
  proof->child_loop = child_kernel.loop_index;
  proof->parent_loop = boundary.parent_loop;

  const auto in_child = [&child](u32 block) {
    return std::binary_search(child.blocks.begin(), child.blocks.end(), block);
  };
  const auto in_parent = [&parent](u32 block) {
    return std::binary_search(parent.blocks.begin(), parent.blocks.end(),
                              block);
  };

  u32 child_exit = std::numeric_limits<u32>::max();
  u32 child_exit_count = 0;
  for (const ControlEdge& edge :
       program.blocks[child.latch_block].successors) {
    if (!edge.has_target || in_child(edge.target_block))
      continue;
    child_exit = edge.target_block;
    child_exit_count++;
  }
  if (child_exit_count != 1 || child_exit >= program.blocks.size() ||
      !in_parent(child_exit)) {
    return Fail(error,
                "structured child has no unique in-parent exit successor");
  }
  proof->final_resume_block = child_exit;
  proof->final_resume_pc = program.blocks[child_exit].start_pc;

  std::vector<bool> can_reach_latch(program.blocks.size(), false);
  can_reach_latch[parent.latch_block] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const u32 block_index : parent.blocks) {
      if (can_reach_latch[block_index] || in_child(block_index))
        continue;
      for (const ControlEdge& edge : program.blocks[block_index].successors) {
        if (edge.has_target && edge.target_block < can_reach_latch.size() &&
            can_reach_latch[edge.target_block]) {
          can_reach_latch[block_index] = true;
          changed = true;
          break;
        }
      }
    }
  }
  if (!can_reach_latch[child_exit])
    return Fail(error, "structured child exit cannot reach parent latch");

  std::set<u32> visited;
  u32 block_index = child_exit;
  while (true) {
    if (!visited.insert(block_index).second)
      return Fail(error, "structured parent suffix contains a cycle");
    proof->suffix_blocks.push_back(block_index);
    proof->summarized_pair_count +=
        static_cast<u32>(program.blocks[block_index].pairs.size());
    if (block_index == parent.latch_block)
      break;
    if (program.blocks[block_index].has_branch)
      return Fail(error, "structured parent suffix branches before its latch");

    u32 successor = std::numeric_limits<u32>::max();
    u32 successor_count = 0;
    for (const ControlEdge& edge : program.blocks[block_index].successors) {
      if (!edge.has_target || edge.target_block >= can_reach_latch.size() ||
          !can_reach_latch[edge.target_block] || in_child(edge.target_block)) {
        continue;
      }
      successor = edge.target_block;
      successor_count++;
    }
    if (successor_count != 1 || !in_parent(successor)) {
      return Fail(error,
                  "structured parent suffix is not a unique acyclic path");
    }
    block_index = successor;
  }

  const BasicBlock& latch = program.blocks[parent.latch_block];
  u32 repeat_count = 0;
  u32 exit_count = 0;
  for (const ControlEdge& edge : latch.successors) {
    if (!edge.has_target)
      continue;
    if (edge.target_block == parent.header_block) {
      repeat_count++;
      continue;
    }
    if (!in_parent(edge.target_block)) {
      proof->parent_exit_block = edge.target_block;
      proof->parent_exit_pc = program.blocks[edge.target_block].start_pc;
      exit_count++;
    }
  }
  if (!latch.conditional_branch || repeat_count != 1 || exit_count != 1)
    return Fail(error, "structured parent latch lacks one repeat and one exit");

  for (const u32 suffix_block : proof->suffix_blocks) {
    for (const ProgramPair& pair : program.blocks[suffix_block].pairs) {
      const VitaVU::GpuPairPlan& plan = pair.plan;
      if (plan.dflag || plan.tflag)
        return Fail(error, "structured parent suffix contains a D/T observer");
      const LowerKind lower = static_cast<LowerKind>(plan.lower_kind);
      if (plan.exec_lower && !plan.lower_discarded_by_upper &&
          IsArchitecturalOutput(lower)) {
        return Fail(error,
                    "structured parent suffix contains memory/PATH1 output");
      }

      if (!plan.exec_lower || plan.lower_discarded_by_upper)
        continue;
      const u16 vi_writes =
          static_cast<u16>(plan.lower_vi_write & 0xfffeu);
      if ((vi_writes & (1u << parent.counter_reg)) != 0) {
        if (suffix_block != parent.latch_block ||
            !IsExactSelfCounterUpdate(plan, parent.counter_reg,
                                      parent.counter_step)) {
          return Fail(error,
                      "structured parent counter update is not the exact latch");
        }
        proof->parent_counter_write_count++;
      }
      const u16 unmodeled_live_writes = static_cast<u16>(
          vi_writes & boundary.parent_live_vi_mask &
          ~(1u << parent.counter_reg));
      if (unmodeled_live_writes != 0)
        return Fail(error, "structured parent suffix mutates live VI state");
    }
  }
  if (proof->parent_counter_write_count != 1)
    return Fail(error, "structured parent suffix lacks one counter update");

  proof->exact_final_resume_proven = true;
  proof->intermediate_suffix_proven = true;
  if (error)
    error->clear();
  return true;
}

bool ProveStructuredLoopMemoryIndependence(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    u32 maximum_outer_iterations, u32 maximum_child_iterations,
    StructuredMemoryDependenceProof* proof, std::string* error) {
  if (!proof)
    return Fail(error, "null structured memory-dependence proof");
  *proof = {};
  if (maximum_outer_iterations == 0 || maximum_child_iterations == 0)
    return Fail(error, "structured memory-dependence bound is zero");
  if (boundary.child_loop != child_kernel.loop_index)
    return Fail(error, "structured memory boundary does not match kernel");

  struct AbsoluteAccess {
    s32 coefficient = 0;
    s32 offset = 0;
    u8 lane_mask = 0;
  };
  const auto absolute = [&child_kernel](const AffineQwordAddress& address,
                                        u8 lane_mask,
                                        AbsoluteAccess* output) {
    if (!address.valid)
      return false;
    s32 base = 0;
    if (address.base_vi != 0) {
      if ((child_kernel.child_entry_vi_mask &
           (1u << address.base_vi)) == 0) {
        return false;
      }
      const AffineViValue& value =
          child_kernel.child_entry_vi_values[address.base_vi];
      if (!value.valid || value.base_vi != 0)
        return false;
      base = value.offset;
    }
    output->coefficient = address.invocation_coefficient;
    output->offset = base + address.qword_offset;
    output->lane_mask = lane_mask;
    return true;
  };

  std::vector<AbsoluteAccess> loads;
  std::vector<AbsoluteAccess> stores;
  bool dynamic_base = false;
  for (const ExpressionNode& node : child_kernel.expressions) {
    if (node.kind != ExpressionKind::Memory)
      continue;
    AbsoluteAccess access;
    if (!absolute(node.memory_address,
                  static_cast<u8>(0x8u >> node.lane), &access)) {
      dynamic_base = true;
      continue;
    }
    loads.push_back(access);
  }
  for (const LoopStore& store : child_kernel.stores) {
    AbsoluteAccess access;
    if (!absolute(store.address, store.write_mask, &access)) {
      dynamic_base = true;
      continue;
    }
    stores.push_back(access);
  }
  proof->load_sites = static_cast<u32>(std::count_if(
      child_kernel.expressions.begin(), child_kernel.expressions.end(),
      [](const ExpressionNode& node) {
        return node.kind == ExpressionKind::Memory;
      }));
  proof->store_sites = static_cast<u32>(child_kernel.stores.size());
  proof->constant_address_bases = !dynamic_base;
  proof->requires_runtime_preflight = dynamic_base;
  if (dynamic_base) {
    if (error)
      error->clear();
    return true;
  }

  for (const AbsoluteAccess& load : loads) {
    for (const AbsoluteAccess& store : stores) {
      if ((load.lane_mask & store.lane_mask) == 0)
        continue;
      for (u32 child_iteration = 0;
           child_iteration < maximum_child_iterations; child_iteration++) {
        const u32 load_address = static_cast<u32>(
            load.offset +
            load.coefficient * static_cast<s32>(child_iteration)) &
            VuMemoryQwordMask;
        proof->compared_addresses++;
        const u32 store_address = static_cast<u32>(
            store.offset +
            store.coefficient * static_cast<s32>(child_iteration)) &
            VuMemoryQwordMask;
        // A parallel child grid cannot preserve any inter-child memory order.
        // This loop catches same-child aliases; the complete static/runtime
        // proof below additionally rejects cross-child and cross-outer aliases.
        if (load_address == store_address) {
          return Fail(error,
                      "structured child iteration has an unordered "
                      "load/store alias");
        }
      }
    }
  }
  proof->intra_iteration_access_order_safe = true;

  // Constant child-entry bases repeat on every outer iteration. Any store
  // therefore needs the concrete snapshot preflight when more than one outer
  // invocation can execute. The runtime root compares complete per-outer
  // access sets and permits arbitrary same-outer child dependencies.
  if (!stores.empty() && maximum_outer_iterations > 1) {
    proof->requires_runtime_preflight = true;
    if (error)
      error->clear();
    return true;
  }
  proof->cross_outer_accesses_independent = true;
  if (error)
    error->clear();
  return true;
}

} // namespace VitaGpuVu
