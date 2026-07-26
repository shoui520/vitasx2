// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuProgram.h"

#include "VUmicroFast.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace VitaGpuVu {
namespace {
using LowerKind = VUInterpFast::LowerFastKind;

constexpr u32 PairBytes = 8;
constexpr u32 MaxVu1Pairs = VU1_PROGSIZE / PairBytes;

struct PendingBlock {
  BasicBlock block;
  std::vector<std::pair<ControlEdgeKind, u32>> targets;
};

struct IndirectResolution {
  std::set<u32> targets;
  bool unresolved = false;
};

using IndirectResolutionMap = std::map<u32, IndirectResolution>;

struct ViValue {
  std::set<u16> values;
  bool unknown = false;
};

using ViState = std::array<ViValue, 16>;

bool Fail(std::string *error, std::string message) {
  if (error)
    *error = std::move(message);
  return false;
}

bool IsConditionalImmediateBranch(LowerKind kind) {
  switch (kind) {
  case LowerKind::IBEQ:
  case LowerKind::IBNE:
  case LowerKind::IBLTZ:
  case LowerKind::IBGTZ:
  case LowerKind::IBLEZ:
  case LowerKind::IBGEZ:
    return true;
  default:
    return false;
  }
}

bool IsUnconditionalImmediateBranch(LowerKind kind) {
  return kind == LowerKind::B || kind == LowerKind::BAL;
}

bool IsImmediateBranch(LowerKind kind) {
  return IsConditionalImmediateBranch(kind) ||
         IsUnconditionalImmediateBranch(kind);
}

bool IsIndirectBranch(LowerKind kind) {
  return kind == LowerKind::JR || kind == LowerKind::JALR;
}

bool IsBranch(LowerKind kind) {
  return IsImmediateBranch(kind) || IsIndirectBranch(kind);
}

u32 ReadWord(const u8 *micro, u32 address) {
  u32 result = 0;
  std::memcpy(&result, micro + address, sizeof(result));
  return result;
}

bool DecodePair(const u8 *micro, u32 mask, u32 pc, bool delay_slot,
                ProgramPair *pair, std::string *error) {
  const u32 address = pc & mask;
  const u32 lower = ReadWord(micro, address);
  const u32 upper = ReadWord(micro, address + sizeof(u32));
  pair->delayed_pair = delay_slot;
  if (!VitaVU::AnalyzeGpuVu1Pair(address, upper, lower, &pair->plan)) {
    return Fail(error, "PairPlan rejected a reachable VU1 pair at pc=" +
                           std::to_string(address));
  }
  return true;
}

u32 BranchTarget(u32 pc, u32 lower, u32 mask) {
  return (pc + PairBytes +
          static_cast<s32>(VUInterpFast::Imm11(lower)) * PairBytes) &
         mask;
}

bool ScanBlock(const u8 *micro, u32 mask, u32 start_pc,
               const std::set<u32> &leaders,
               const IndirectResolutionMap &indirect_resolutions,
               PendingBlock *pending, std::set<u32> *discovered_leaders,
               std::string *error) {
  pending->block = {};
  pending->targets.clear();
  pending->block.start_pc = start_pc;
  std::set<u32> local_pcs;
  u32 pc = start_pc;

  for (u32 decoded = 0; decoded < MaxVu1Pairs; decoded++) {
    if (pc != start_pc && leaders.contains(pc)) {
      pending->targets.emplace_back(ControlEdgeKind::Fallthrough, pc);
      return true;
    }
    if (!local_pcs.insert(pc).second) {
      discovered_leaders->insert(pc);
      pending->targets.emplace_back(ControlEdgeKind::Fallthrough, pc);
      return true;
    }

    ProgramPair pair;
    if (!DecodePair(micro, mask, pc, false, &pair, error))
      return false;
    pending->block.pairs.push_back(pair);
    pending->block.has_external_exit |= pair.plan.dflag || pair.plan.tflag;

    const LowerKind kind = static_cast<LowerKind>(pair.plan.lower_kind);
    const bool branch = pair.plan.exec_lower && IsBranch(kind);
    const bool ends_after_delay = pair.plan.ebit;
    if (!branch && !ends_after_delay) {
      pc = (pc + PairBytes) & mask;
      continue;
    }

    pending->block.has_branch = branch;
    pending->block.branch_pc = pc;
    pending->block.branch_kind = pair.plan.lower_kind;
    pending->block.conditional_branch =
        branch && IsConditionalImmediateBranch(kind);
    pending->block.indirect_branch = branch && IsIndirectBranch(kind);

    const u32 delay_pc = (pc + PairBytes) & mask;
    ProgramPair delay;
    if (!DecodePair(micro, mask, delay_pc, true, &delay, error))
      return false;
    pending->block.pairs.push_back(delay);
    pending->block.has_external_exit |= delay.plan.dflag || delay.plan.tflag;
    const LowerKind delay_kind = static_cast<LowerKind>(delay.plan.lower_kind);
    pending->block.branch_in_delay_slot =
        delay.plan.exec_lower && IsBranch(delay_kind);

    if (ends_after_delay) {
      pending->block.ends_program = true;
      pending->targets.emplace_back(ControlEdgeKind::ProgramExit, 0);
      return true;
    }

    if (IsIndirectBranch(kind)) {
      const auto resolution = indirect_resolutions.find(pc);
      if (resolution != indirect_resolutions.end()) {
        for (const u32 target : resolution->second.targets) {
          discovered_leaders->insert(target);
          pending->targets.emplace_back(ControlEdgeKind::ResolvedIndirect,
                                        target);
        }
      }
      if (resolution == indirect_resolutions.end() ||
          resolution->second.targets.empty() || resolution->second.unresolved) {
        pending->targets.emplace_back(ControlEdgeKind::ExternalExit, 0);
      }
      return true;
    }

    const u32 taken = BranchTarget(pc, pair.plan.lower, mask);
    discovered_leaders->insert(taken);
    pending->targets.emplace_back(ControlEdgeKind::BranchTaken, taken);
    if (IsConditionalImmediateBranch(kind)) {
      const u32 not_taken = (pc + 2 * PairBytes) & mask;
      discovered_leaders->insert(not_taken);
      pending->targets.emplace_back(ControlEdgeKind::BranchNotTaken, not_taken);
    }
    return true;
  }

  return Fail(error,
              "reachable VU1 block exceeded the complete micro-memory bound");
}

bool BuildBlocksForIndirectResolutions(
    const u8 *micro, u32 mask, u32 start_pc,
    const IndirectResolutionMap &indirect_resolutions,
    std::vector<BasicBlock> *blocks, std::string *error) {
  std::set<u32> leaders{start_pc};
  std::map<u32, PendingBlock> pending_blocks;

  for (u32 split_pass = 0; split_pass < MaxVu1Pairs; split_pass++) {
    bool changed = false;
    pending_blocks.clear();
    const std::vector<u32> pass_leaders(leaders.begin(), leaders.end());
    for (const u32 leader : pass_leaders) {
      PendingBlock pending;
      std::set<u32> discovered;
      if (!ScanBlock(micro, mask, leader, leaders, indirect_resolutions,
                     &pending, &discovered, error)) {
        return false;
      }
      pending_blocks.emplace(leader, std::move(pending));
      for (const u32 target : discovered)
        changed |= leaders.insert(target).second;
    }
    if (!changed)
      break;
    if (split_pass + 1 == MaxVu1Pairs)
      return Fail(error, "VU1 CFG leader splitting did not converge");
  }

  // Rebuild once with the stable leader set if the final discovery pass
  // was followed by a split.
  pending_blocks.clear();
  for (const u32 leader : leaders) {
    PendingBlock pending;
    std::set<u32> discovered;
    if (!ScanBlock(micro, mask, leader, leaders, indirect_resolutions, &pending,
                   &discovered, error)) {
      return false;
    }
    if (!std::includes(leaders.begin(), leaders.end(), discovered.begin(),
                       discovered.end())) {
      return Fail(error, "VU1 CFG changed after leader convergence");
    }
    pending_blocks.emplace(leader, std::move(pending));
  }

  std::map<u32, u32> block_by_pc;
  blocks->clear();
  blocks->reserve(pending_blocks.size());
  for (auto &[pc, pending] : pending_blocks) {
    block_by_pc.emplace(pc, static_cast<u32>(blocks->size()));
    blocks->push_back(std::move(pending.block));
  }

  for (auto &[pc, pending] : pending_blocks) {
    const u32 source = block_by_pc.at(pc);
    BasicBlock &block = (*blocks)[source];
    for (const auto &[kind, target_pc] : pending.targets) {
      ControlEdge edge;
      edge.kind = kind;
      edge.target_pc = target_pc;
      if (kind != ControlEdgeKind::ProgramExit &&
          kind != ControlEdgeKind::ExternalExit) {
        const auto target = block_by_pc.find(target_pc);
        if (target == block_by_pc.end())
          return Fail(error, "VU1 CFG edge has no target block");
        edge.has_target = true;
        edge.target_block = target->second;
        (*blocks)[target->second].predecessors.push_back(source);
      }
      block.successors.push_back(edge);
    }
  }
  return true;
}

void SetKnownViValue(ViValue *value, u16 known) {
  value->unknown = false;
  value->values.clear();
  value->values.insert(known);
}

// PCSX2 owner: VUops.cpp::_vuBAL()/_vuJALR() write the post-delay return
// pair index, while _vuJR()/_vuJALR() snapshot Is before the delay pair.
// PairPlan supplies the effective simultaneous-pair writes; every non-link
// write deliberately becomes unknown instead of inventing integer semantics.
void ApplyPairViWrites(const ProgramPair &pair, u32 mask, ViState *state) {
  const VitaVU::GpuPairPlan &plan = pair.plan;
  const u32 upper_writes = plan.exec_upper ? plan.upper_vi_write : 0;
  const u32 lower_writes = plan.exec_lower ? plan.lower_vi_write : 0;
  const u32 writes = upper_writes | lower_writes;
  for (u32 reg = 1; reg < state->size(); reg++) {
    if ((writes & (1u << reg)) == 0)
      continue;
    (*state)[reg].unknown = true;
    (*state)[reg].values.clear();
  }

  const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
  // A BAL/JALR in another branch's delay slot has PCSX2's branchpc-derived
  // link behavior. The CFG already rejects that control shape, so retain an
  // unknown value here rather than applying the ordinary static-PC rule.
  if (!pair.delayed_pair && plan.exec_lower &&
      (kind == LowerKind::BAL || kind == LowerKind::JALR)) {
    const u32 link = VUInterpFast::It(plan.lower);
    const u32 link_mask = 1u << link;
    if (link != 0 && (lower_writes & link_mask) != 0 &&
        (upper_writes & link_mask) == 0) {
      const u32 return_pc = (plan.pc + 2 * PairBytes) & mask;
      SetKnownViValue(&(*state)[link], static_cast<u16>(return_pc / PairBytes));
    }
  }

  SetKnownViValue(&(*state)[0], 0);
}

bool JoinViValue(ViValue *destination, const ViValue &source) {
  if (destination->unknown)
    return false;
  if (source.unknown) {
    destination->unknown = true;
    destination->values.clear();
    return true;
  }

  bool changed = false;
  for (const u16 value : source.values)
    changed |= destination->values.insert(value).second;
  return changed;
}

bool JoinViState(ViState *destination, const ViState &source) {
  bool changed = false;
  for (u32 reg = 0; reg < destination->size(); reg++)
    changed |= JoinViValue(&(*destination)[reg], source[reg]);
  return changed;
}

std::map<u32, IndirectResolution>
InferIndirectResolutions(const std::vector<BasicBlock> &blocks, u32 mask,
                         u32 start_pc) {
  std::vector<ViState> input_states(blocks.size());
  std::vector<bool> reachable(blocks.size(), false);
  std::vector<bool> queued(blocks.size(), false);
  std::vector<u32> work;

  const auto entry_iterator =
      std::find_if(blocks.begin(), blocks.end(), [start_pc](const auto &block) {
        return block.start_pc == start_pc;
      });
  if (entry_iterator == blocks.end())
    return {};
  const u32 entry_block = static_cast<u32>(entry_iterator - blocks.begin());

  ViState entry_state;
  for (u32 reg = 1; reg < entry_state.size(); reg++)
    entry_state[reg].unknown = true;
  SetKnownViValue(&entry_state[0], 0);
  input_states[entry_block] = std::move(entry_state);
  reachable[entry_block] = true;
  queued[entry_block] = true;
  work.push_back(entry_block);

  while (!work.empty()) {
    const u32 block_index = work.back();
    work.pop_back();
    queued[block_index] = false;

    ViState output = input_states[block_index];
    for (const ProgramPair &pair : blocks[block_index].pairs)
      ApplyPairViWrites(pair, mask, &output);

    for (const ControlEdge &edge : blocks[block_index].successors) {
      if (!edge.has_target)
        continue;
      bool changed = false;
      if (!reachable[edge.target_block]) {
        input_states[edge.target_block] = output;
        reachable[edge.target_block] = true;
        changed = true;
      } else {
        changed = JoinViState(&input_states[edge.target_block], output);
      }
      if (changed && !queued[edge.target_block]) {
        queued[edge.target_block] = true;
        work.push_back(edge.target_block);
      }
    }
  }

  std::map<u32, IndirectResolution> result;
  for (u32 block_index = 0; block_index < blocks.size(); block_index++) {
    const BasicBlock &block = blocks[block_index];
    if (!reachable[block_index] || !block.indirect_branch ||
        block.pairs.size() < 2) {
      continue;
    }

    ViState before_branch = input_states[block_index];
    for (size_t pair_index = 0; pair_index + 2 < block.pairs.size();
         pair_index++) {
      ApplyPairViWrites(block.pairs[pair_index], mask, &before_branch);
    }

    const ProgramPair &branch_pair = block.pairs[block.pairs.size() - 2];
    const u32 source = VUInterpFast::Is(branch_pair.plan.lower);
    const ViValue &target_value = before_branch[source];
    IndirectResolution &resolution = result[block.branch_pc];
    if (target_value.unknown || target_value.values.empty()) {
      resolution.unresolved = true;
      continue;
    }
    for (const u16 value : target_value.values) {
      resolution.targets.insert((static_cast<u32>(value) * PairBytes) & mask);
    }
  }
  return result;
}

bool UpdateIndirectResolutions(
    const std::map<u32, IndirectResolution> &inferred,
    IndirectResolutionMap *resolutions) {
  bool changed = false;
  for (const auto &[branch_pc, inference] : inferred) {
    const auto current = resolutions->find(branch_pc);
    if (current == resolutions->end()) {
      resolutions->emplace(branch_pc, inference);
      changed = true;
      continue;
    }

    for (const u32 target : inference.targets)
      changed |= current->second.targets.insert(target).second;
    if (inference.unresolved && !current->second.unresolved) {
      current->second.unresolved = true;
      changed = true;
    }
  }
  return changed;
}

bool BuildBlocks(const u8 *micro, u32 mask, u32 start_pc,
                 std::vector<BasicBlock> *blocks, std::string *error) {
  IndirectResolutionMap indirect_resolutions;
  for (u32 resolution_pass = 0; resolution_pass <= MaxVu1Pairs;
       resolution_pass++) {
    if (!BuildBlocksForIndirectResolutions(
            micro, mask, start_pc, indirect_resolutions, blocks, error)) {
      return false;
    }

    const auto inferred = InferIndirectResolutions(*blocks, mask, start_pc);
    if (!UpdateIndirectResolutions(inferred, &indirect_resolutions))
      return true;
    if (resolution_pass == MaxVu1Pairs) {
      return Fail(error,
                  "VU1 static indirect-control recovery did not converge");
    }
  }
  return false;
}

std::vector<std::vector<bool>>
ComputeDominators(const std::vector<BasicBlock> &blocks, u32 entry) {
  const size_t count = blocks.size();
  std::vector<std::vector<bool>> dominators(count,
                                            std::vector<bool>(count, true));
  dominators[entry].assign(count, false);
  dominators[entry][entry] = true;

  for (bool changed = true; changed;) {
    changed = false;
    for (u32 block = 0; block < count; block++) {
      if (block == entry)
        continue;
      std::vector<bool> next(count, true);
      if (blocks[block].predecessors.empty())
        next.assign(count, false);
      for (const u32 predecessor : blocks[block].predecessors) {
        for (u32 candidate = 0; candidate < count; candidate++)
          next[candidate] =
              next[candidate] && dominators[predecessor][candidate];
      }
      next[block] = true;
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  return dominators;
}

std::vector<u32> CollectNaturalLoop(const std::vector<BasicBlock> &blocks,
                                    u32 header, u32 latch) {
  std::set<u32> members{header, latch};
  std::vector<u32> work;
  if (latch != header)
    work.push_back(latch);
  while (!work.empty()) {
    const u32 current = work.back();
    work.pop_back();
    for (const u32 predecessor : blocks[current].predecessors) {
      if (members.insert(predecessor).second && predecessor != header) {
        work.push_back(predecessor);
      }
    }
  }
  return {members.begin(), members.end()};
}

bool BranchCounter(const BasicBlock &latch, u32 *counter_reg) {
  if (!latch.has_branch || !latch.conditional_branch ||
      latch.pairs.size() < 2) {
    return false;
  }
  const u32 code = latch.pairs[latch.pairs.size() - 2].plan.lower;
  const LowerKind kind = static_cast<LowerKind>(latch.branch_kind);
  const u32 is = VUInterpFast::Is(code);
  const u32 it = VUInterpFast::It(code);
  if (kind == LowerKind::IBEQ || kind == LowerKind::IBNE) {
    if ((is == 0) == (it == 0))
      return false;
    *counter_reg = is == 0 ? it : is;
    return true;
  }
  if (kind == LowerKind::IBLTZ || kind == LowerKind::IBGTZ ||
      kind == LowerKind::IBLEZ || kind == LowerKind::IBGEZ) {
    if (is == 0)
      return false;
    *counter_reg = is;
    return true;
  }
  return false;
}

bool CounterUpdate(const ProgramPair &pair, u32 counter_reg, s32 *step) {
  if (!pair.plan.exec_lower ||
      (pair.plan.lower_vi_write & (1u << counter_reg)) == 0) {
    return false;
  }

  const u32 code = pair.plan.lower;
  const LowerKind kind = static_cast<LowerKind>(pair.plan.lower_kind);
  switch (kind) {
  case LowerKind::IADDI:
    if (VUInterpFast::It(code) == counter_reg &&
        VUInterpFast::Is(code) == counter_reg) {
      *step = VUInterpFast::Imm5(code);
      return true;
    }
    break;
  case LowerKind::IADDIU:
    if (VUInterpFast::It(code) == counter_reg &&
        VUInterpFast::Is(code) == counter_reg) {
      *step = VUInterpFast::Imm15(code);
      return true;
    }
    break;
  case LowerKind::ISUBIU:
    if (VUInterpFast::It(code) == counter_reg &&
        VUInterpFast::Is(code) == counter_reg) {
      *step = -VUInterpFast::Imm15(code);
      return true;
    }
    break;
  default:
    break;
  }
  return false;
}

void AnalyzeLoopCounter(const std::vector<BasicBlock> &blocks,
                        NaturalLoop *loop) {
  const BasicBlock &latch = blocks[loop->latch_block];
  u32 counter_reg = 0;
  if (!BranchCounter(latch, &counter_reg))
    return;

  u32 writes = 0;
  s32 counter_step = 0;
  for (const u32 block_index : loop->blocks) {
    const BasicBlock &block = blocks[block_index];
    for (const ProgramPair &pair : block.pairs) {
      if ((pair.plan.lower_vi_write & (1u << counter_reg)) == 0) {
        continue;
      }
      writes++;
      s32 update = 0;
      if (!CounterUpdate(pair, counter_reg, &update))
        return;
      counter_step = update;
    }
  }
  if (writes != 1 || counter_step == 0)
    return;

  loop->counter_reg = counter_reg;
  loop->counter_step = counter_step;
  loop->branch_kind = latch.branch_kind;
  loop->affine_counter = true;
  for (const ControlEdge &edge : latch.successors) {
    if (edge.has_target && edge.target_block == loop->header_block) {
      loop->branch_taken_repeats = edge.kind == ControlEdgeKind::BranchTaken;
      break;
    }
  }
}

void FindNaturalLoops(ProgramAnalysis *analysis, u32 entry_block) {
  const auto dominators = ComputeDominators(analysis->blocks, entry_block);
  for (u32 source = 0; source < analysis->blocks.size(); source++) {
    for (const ControlEdge &edge : analysis->blocks[source].successors) {
      if (!edge.has_target || !dominators[source][edge.target_block]) {
        continue;
      }
      NaturalLoop loop;
      loop.header_block = edge.target_block;
      loop.latch_block = source;
      loop.blocks =
          CollectNaturalLoop(analysis->blocks, edge.target_block, source);
      AnalyzeLoopCounter(analysis->blocks, &loop);
      analysis->natural_loops.push_back(std::move(loop));
    }
  }
}

void AnalyzeExitReachability(ProgramAnalysis *analysis) {
  std::vector<bool> can_reach_exit(analysis->blocks.size(), false);
  std::vector<u32> work;
  for (u32 block_index = 0; block_index < analysis->blocks.size();
       block_index++) {
    for (const ControlEdge &edge : analysis->blocks[block_index].successors) {
      if (edge.kind == ControlEdgeKind::ProgramExit) {
        analysis->has_program_exit = true;
        can_reach_exit[block_index] = true;
        work.push_back(block_index);
        break;
      }
    }
  }

  while (!work.empty()) {
    const u32 block_index = work.back();
    work.pop_back();
    for (const u32 predecessor : analysis->blocks[block_index].predecessors) {
      if (!can_reach_exit[predecessor]) {
        can_reach_exit[predecessor] = true;
        work.push_back(predecessor);
      }
    }
  }
  analysis->every_block_can_reach_program_exit =
      analysis->has_program_exit &&
      std::all_of(can_reach_exit.begin(), can_reach_exit.end(),
                  [](bool reaches_exit) { return reaches_exit; });
}
} // namespace

bool AnalyzeGpuVu1Program(const u8 *micro, u32 micro_size, u32 start_pc,
                          ProgramAnalysis *analysis, std::string *error) {
  if (!micro || !analysis)
    return Fail(error, "null VU1 program analysis input");
  if (micro_size != VU1_PROGSIZE || !std::has_single_bit(micro_size) ||
      (start_pc & (PairBytes - 1)) != 0) {
    return Fail(error,
                "VU1 program analysis requires one aligned 16 KiB image");
  }

  *analysis = {};
  analysis->start_pc = start_pc & (micro_size - 1);
  if (!BuildBlocks(micro, micro_size - 1, analysis->start_pc, &analysis->blocks,
                   error)) {
    return false;
  }
  if (analysis->blocks.empty())
    return Fail(error, "VU1 program analysis produced no blocks");

  u32 entry_block = std::numeric_limits<u32>::max();
  for (u32 i = 0; i < analysis->blocks.size(); i++) {
    const BasicBlock &block = analysis->blocks[i];
    if (block.start_pc == analysis->start_pc)
      entry_block = i;
    analysis->has_branch_in_delay_slot |= block.branch_in_delay_slot;
    analysis->has_external_exit |= block.has_external_exit;
    for (const ControlEdge &edge : block.successors) {
      analysis->resolved_indirect_edges +=
          edge.kind == ControlEdgeKind::ResolvedIndirect ? 1u : 0u;
      analysis->has_unresolved_indirect_control |=
          block.indirect_branch && edge.kind == ControlEdgeKind::ExternalExit;
    }
  }
  if (entry_block == std::numeric_limits<u32>::max())
    return Fail(error, "VU1 CFG has no entry block");

  FindNaturalLoops(analysis, entry_block);
  AnalyzeExitReachability(analysis);
  analysis->complete_cfg = !analysis->has_unresolved_indirect_control &&
                           !analysis->has_branch_in_delay_slot &&
                           !analysis->has_external_exit;
  if (error)
    error->clear();
  return true;
}
} // namespace VitaGpuVu
