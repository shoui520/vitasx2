// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuInvocationPlan.h"

#include "VUmicroFast.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace VitaGpuVu {
namespace {

using LowerKind = VUInterpFast::LowerFastKind;

constexpr u32 InvalidValue = 0;
constexpr size_t MaximumAlternatives = 8;

bool Fail(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
  return false;
}

bool LaneEnabled(u8 mask, u32 lane) {
  return (mask & (0x8u >> lane)) != 0;
}

InvocationValueSet UnknownValue() {
  return {};
}

InvocationValueSet KnownValue(u32 value) {
  InvocationValueSet result;
  if (value != InvalidValue) {
    result.unknown = false;
    result.alternatives.push_back(value);
  }
  return result;
}

void NormalizeAlternatives(InvocationValueSet* value) {
  if (value->unknown) {
    value->alternatives.clear();
    return;
  }
  std::sort(value->alternatives.begin(), value->alternatives.end());
  value->alternatives.erase(
      std::unique(value->alternatives.begin(), value->alternatives.end()),
      value->alternatives.end());
  if (value->alternatives.empty() ||
      value->alternatives.size() > MaximumAlternatives) {
    *value = UnknownValue();
  }
}

bool JoinValue(InvocationValueSet* destination,
               const InvocationValueSet& source) {
  if (*destination == source)
    return false;
  if (destination->unknown)
    return false;
  if (source.unknown) {
    *destination = UnknownValue();
    return true;
  }
  destination->alternatives.insert(destination->alternatives.end(),
                                   source.alternatives.begin(),
                                   source.alternatives.end());
  NormalizeAlternatives(destination);
  return true;
}

struct SymbolicState {
  std::array<InvocationValueSet, 16> vi;
  std::array<std::array<InvocationValueSet, 4>, 32> vf;
};

struct QwordCopy {
  InvocationValueSet destination;
  InvocationValueSet source;
};

class InvocationPlanBuilder {
public:
  InvocationPlanBuilder(const ProgramAnalysis& program,
                        const ParallelLoopKernel& kernel,
                        ParallelInvocationPlan* plan)
      : m_program(program), m_kernel(kernel), m_plan(plan) {
    // Value zero is an explicit unknown sentinel.
    m_plan->values.emplace_back();
  }

  bool Build(std::string* error) {
    if (m_kernel.loop_index >= m_program.natural_loops.size())
      return Fail(error, "invocation plan loop index is out of range");
    const NaturalLoop& loop =
        m_program.natural_loops[m_kernel.loop_index];
    if (loop.header_block >= m_program.blocks.size())
      return Fail(error, "invocation plan has no loop header");

    const u32 entry_block = FindEntryBlock();
    if (entry_block == std::numeric_limits<u32>::max())
      return Fail(error, "invocation plan has no program entry block");
    if (!ValidateEntryRegion(entry_block, loop, error))
      return false;

    SymbolicState entry = InitialState();
    SymbolicState header;
    bool header_reached = false;
    if (entry_block == loop.header_block) {
      header = entry;
      header_reached = true;
    } else if (!PropagateEntryRegion(entry_block, loop, entry, &header,
                                     &header_reached, error)) {
      return false;
    }
    if (!header_reached)
      return Fail(error, "invocation entry region cannot reach the loop");

    m_plan->loop_entry_vi = header.vi;
    m_plan->loop_entry_vf = header.vf;
    if (loop.counter_reg >= header.vi.size())
      return Fail(error, "invocation loop counter is out of range");
    m_plan->loop_counter = header.vi[loop.counter_reg];

    if (!FindGifTagSource(header, error))
      return false;
    if (error)
      error->clear();
    return true;
  }

private:
  using NodeKey =
      std::tuple<InvocationValueKind, std::array<u32, 2>, u32, u8, u8>;

  u32 AddNode(InvocationValueNode node) {
    const NodeKey key{node.kind, node.operands, node.immediate, node.reg,
                      node.lane};
    const auto found = m_node_cache.find(key);
    if (found != m_node_cache.end())
      return found->second;
    const u32 id = static_cast<u32>(m_plan->values.size());
    m_plan->values.push_back(node);
    m_node_cache.emplace(key, id);
    return id;
  }

  u32 Constant(u32 value) {
    InvocationValueNode node;
    node.kind = InvocationValueKind::Constant;
    node.immediate = value;
    return AddNode(node);
  }

  u32 InitialVi(u32 reg) {
    if (reg == 0)
      return Constant(0);
    InvocationValueNode node;
    node.kind = InvocationValueKind::InitialVi;
    node.reg = static_cast<u8>(reg);
    return AddNode(node);
  }

  u32 InitialVfWord(u32 reg, u32 lane) {
    if (reg == 0)
      return Constant(lane == 3 ? 0x3f800000u : 0u);
    InvocationValueNode node;
    node.kind = InvocationValueKind::InitialVfWord;
    node.reg = static_cast<u8>(reg);
    node.lane = static_cast<u8>(lane);
    return AddNode(node);
  }

  u32 VifTop() {
    InvocationValueNode node;
    node.kind = InvocationValueKind::VifTop;
    return AddNode(node);
  }

  u32 VifItop() {
    InvocationValueNode node;
    node.kind = InvocationValueKind::VifItop;
    return AddNode(node);
  }

  bool IsConstant(u32 id, u32* value = nullptr) const {
    if (id == InvalidValue || id >= m_plan->values.size() ||
        m_plan->values[id].kind != InvocationValueKind::Constant) {
      return false;
    }
    if (value)
      *value = m_plan->values[id].immediate;
    return true;
  }

  u32 AddConstant(u32 input, s32 delta) {
    if (input == InvalidValue)
      return InvalidValue;
    u32 constant = 0;
    if (IsConstant(input, &constant))
      return Constant(static_cast<u16>(constant + delta));
    const InvocationValueNode& source = m_plan->values[input];
    if (source.kind == InvocationValueKind::AddConstantU16) {
      return AddConstant(source.operands[0],
                         static_cast<s32>(source.immediate) + delta);
    }
    if (delta == 0)
      return input;
    InvocationValueNode node;
    node.kind = InvocationValueKind::AddConstantU16;
    node.operands[0] = input;
    node.immediate = static_cast<u32>(delta);
    return AddNode(node);
  }

  u32 Binary(InvocationValueKind kind, u32 left, u32 right) {
    if (left == InvalidValue || right == InvalidValue)
      return InvalidValue;
    u32 left_constant = 0;
    u32 right_constant = 0;
    const bool left_is_constant = IsConstant(left, &left_constant);
    const bool right_is_constant = IsConstant(right, &right_constant);
    if (kind == InvocationValueKind::AddU16) {
      if (left_is_constant)
        return AddConstant(right, static_cast<s16>(left_constant));
      if (right_is_constant)
        return AddConstant(left, static_cast<s16>(right_constant));
    }
    if (kind == InvocationValueKind::SubtractU16 && right_is_constant)
      return AddConstant(left, -static_cast<s32>(right_constant));
    if (left_is_constant && right_is_constant) {
      switch (kind) {
      case InvocationValueKind::AddU16:
        return Constant(static_cast<u16>(left_constant + right_constant));
      case InvocationValueKind::SubtractU16:
        return Constant(static_cast<u16>(left_constant - right_constant));
      case InvocationValueKind::AndU16:
        return Constant(static_cast<u16>(left_constant & right_constant));
      case InvocationValueKind::OrU16:
        return Constant(static_cast<u16>(left_constant | right_constant));
      default:
        break;
      }
    }
    if ((kind == InvocationValueKind::AddU16 ||
         kind == InvocationValueKind::AndU16 ||
         kind == InvocationValueKind::OrU16) &&
        right < left) {
      std::swap(left, right);
    }
    InvocationValueNode node;
    node.kind = kind;
    node.operands = {left, right};
    return AddNode(node);
  }

  u32 Unary(InvocationValueKind kind, u32 operand, u32 immediate = 0,
            u8 lane = 0) {
    if (operand == InvalidValue)
      return InvalidValue;
    InvocationValueNode node;
    node.kind = kind;
    node.operands[0] = operand;
    node.immediate = immediate;
    node.lane = lane;
    return AddNode(node);
  }

  template <typename Operation>
  InvocationValueSet MapUnary(const InvocationValueSet& input,
                              Operation operation) {
    if (input.unknown)
      return UnknownValue();
    InvocationValueSet result;
    result.unknown = false;
    for (const u32 value : input.alternatives)
      result.alternatives.push_back(operation(value));
    NormalizeAlternatives(&result);
    return result;
  }

  template <typename Operation>
  InvocationValueSet MapBinary(const InvocationValueSet& left,
                               const InvocationValueSet& right,
                               Operation operation) {
    if (left.unknown || right.unknown)
      return UnknownValue();
    InvocationValueSet result;
    result.unknown = false;
    for (const u32 left_value : left.alternatives) {
      for (const u32 right_value : right.alternatives)
        result.alternatives.push_back(operation(left_value, right_value));
    }
    NormalizeAlternatives(&result);
    return result;
  }

  InvocationValueSet AddConstant(const InvocationValueSet& input,
                                 s32 delta) {
    return MapUnary(input,
                    [this, delta](u32 value) {
                      return AddConstant(value, delta);
                    });
  }

  InvocationValueSet Binary(InvocationValueKind kind,
                            const InvocationValueSet& left,
                            const InvocationValueSet& right) {
    return MapBinary(left, right,
                     [this, kind](u32 left_value, u32 right_value) {
                       return Binary(kind, left_value, right_value);
                     });
  }

  InvocationValueSet Memory(const InvocationValueSet& address,
                            InvocationValueKind kind, u32 lane) {
    return MapUnary(address,
                    [this, kind, lane](u32 value) {
                      return Unary(kind, value, 0,
                                   static_cast<u8>(lane));
                    });
  }

  SymbolicState InitialState() {
    SymbolicState state;
    for (u32 reg = 0; reg < state.vi.size(); reg++)
      state.vi[reg] = KnownValue(InitialVi(reg));
    for (u32 reg = 0; reg < state.vf.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++)
        state.vf[reg][lane] = KnownValue(InitialVfWord(reg, lane));
    }
    return state;
  }

  u32 FindEntryBlock() const {
    for (u32 block = 0; block < m_program.blocks.size(); block++) {
      if (m_program.blocks[block].start_pc == m_program.start_pc)
        return block;
    }
    return std::numeric_limits<u32>::max();
  }

  bool ValidateEntryRegionBlock(u32 block_index, const NaturalLoop& loop,
                                const std::set<u32>& loop_blocks,
                                std::vector<u8>* state,
                                std::string* error) const {
    if (block_index == loop.header_block)
      return true;
    if (block_index >= m_program.blocks.size() ||
        loop_blocks.contains(block_index)) {
      return Fail(error,
                  "invocation entry reaches the loop below its header");
    }
    if ((*state)[block_index] == 1)
      return Fail(error, "invocation entry region contains a cycle");
    if ((*state)[block_index] == 2)
      return true;

    (*state)[block_index] = 1;
    const BasicBlock& block = m_program.blocks[block_index];
    if (block.successors.empty()) {
      return Fail(error,
                  "invocation entry can terminate before the loop");
    }
    for (const ControlEdge& edge : block.successors) {
      if (!edge.has_target) {
        return Fail(error,
                    "invocation entry has an external exit before the loop");
      }
      if (!ValidateEntryRegionBlock(edge.target_block, loop, loop_blocks,
                                    state, error)) {
        return false;
      }
    }
    (*state)[block_index] = 2;
    return true;
  }

  bool ValidateEntryRegion(u32 entry_block, const NaturalLoop& loop,
                           std::string* error) const {
    if (entry_block == loop.header_block)
      return true;
    const std::set<u32> loop_blocks(loop.blocks.begin(), loop.blocks.end());
    std::vector<u8> state(m_program.blocks.size());
    return ValidateEntryRegionBlock(entry_block, loop, loop_blocks, &state,
                                    error);
  }

  bool JoinState(SymbolicState* destination,
                 const SymbolicState& source) {
    bool changed = false;
    for (u32 reg = 0; reg < destination->vi.size(); reg++)
      changed |= JoinValue(&destination->vi[reg], source.vi[reg]);
    for (u32 reg = 0; reg < destination->vf.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++)
        changed |= JoinValue(&destination->vf[reg][lane],
                             source.vf[reg][lane]);
    }
    return changed;
  }

  bool PropagateEntryRegion(u32 entry_block, const NaturalLoop& loop,
                            const SymbolicState& entry,
                            SymbolicState* header, bool* header_reached,
                            std::string* error) {
    std::vector<SymbolicState> inputs(m_program.blocks.size());
    std::vector<bool> reachable(m_program.blocks.size(), false);
    std::vector<bool> queued(m_program.blocks.size(), false);
    std::vector<u32> work{entry_block};
    inputs[entry_block] = entry;
    reachable[entry_block] = true;
    queued[entry_block] = true;
    const std::set<u32> loop_blocks(loop.blocks.begin(), loop.blocks.end());
    u32 transfers = 0;

    while (!work.empty()) {
      const u32 block_index = work.back();
      work.pop_back();
      queued[block_index] = false;
      if (++transfers > m_program.blocks.size() *
                            (MaximumAlternatives + 2)) {
        return Fail(error,
                    "invocation entry provenance did not converge");
      }

      SymbolicState output = inputs[block_index];
      for (const ProgramPair& pair :
           m_program.blocks[block_index].pairs) {
        TransferPair(pair.plan, &output);
      }

      for (const ControlEdge& edge :
           m_program.blocks[block_index].successors) {
        if (!edge.has_target)
          continue;
        if (edge.target_block == loop.header_block) {
          if (!*header_reached) {
            *header = output;
            *header_reached = true;
          } else {
            JoinState(header, output);
          }
          continue;
        }
        if (loop_blocks.contains(edge.target_block))
          continue;
        bool changed = false;
        if (!reachable[edge.target_block]) {
          inputs[edge.target_block] = output;
          reachable[edge.target_block] = true;
          changed = true;
        } else {
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

  InvocationValueSet Address(const SymbolicState& state, u32 base,
                             s32 immediate) {
    if (base >= state.vi.size())
      return UnknownValue();
    return AddConstant(state.vi[base], immediate);
  }

  void AssignLowerVi(const VitaVU::GpuPairPlan& plan,
                     const SymbolicState& old, SymbolicState* state) {
    const u32 writes = plan.lower_vi_write & 0xffffu;
    for (u32 reg = 1; reg < state->vi.size(); reg++) {
      if ((writes & (1u << reg)) != 0)
        state->vi[reg] = UnknownValue();
    }

    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    const u32 is = VUInterpFast::Is(code);
    const u32 it = VUInterpFast::It(code);
    const u32 id = VUInterpFast::Id(code);
    switch (kind) {
    case LowerKind::IADDIU:
      if (it != 0)
        state->vi[it] =
            AddConstant(old.vi[is], VUInterpFast::Imm15(code));
      break;
    case LowerKind::ISUBIU:
      if (it != 0)
        state->vi[it] =
            AddConstant(old.vi[is], -VUInterpFast::Imm15(code));
      break;
    case LowerKind::IADDI:
      if (it != 0)
        state->vi[it] =
            AddConstant(old.vi[is], VUInterpFast::Imm5(code));
      break;
    case LowerKind::IADD:
      if (id != 0)
        state->vi[id] =
            Binary(InvocationValueKind::AddU16, old.vi[is], old.vi[it]);
      break;
    case LowerKind::ISUB:
      if (id != 0)
        state->vi[id] = Binary(InvocationValueKind::SubtractU16,
                               old.vi[is], old.vi[it]);
      break;
    case LowerKind::IAND:
      if (id != 0)
        state->vi[id] =
            Binary(InvocationValueKind::AndU16, old.vi[is], old.vi[it]);
      break;
    case LowerKind::IOR:
      if (id != 0)
        state->vi[id] =
            Binary(InvocationValueKind::OrU16, old.vi[is], old.vi[it]);
      break;
    case LowerKind::ILW:
    case LowerKind::ILWR: {
      if (it == 0)
        break;
      const InvocationValueSet address =
          Address(old, is,
                  kind == LowerKind::ILW ? VUInterpFast::Imm11(code) : 0);
      s32 selected_lane = -1;
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(static_cast<u8>(VUInterpFast::XYZW(code)), lane))
          selected_lane = static_cast<s32>(lane);
      }
      state->vi[it] =
          selected_lane >= 0
              ? Memory(address, InvocationValueKind::MemoryU16,
                       static_cast<u32>(selected_lane))
              : old.vi[it];
      break;
    }
    case LowerKind::LQI:
      if (is != 0)
        state->vi[is] = AddConstant(old.vi[is], 1);
      break;
    case LowerKind::LQD:
      if (is != 0)
        state->vi[is] = AddConstant(old.vi[is], -1);
      break;
    case LowerKind::SQI:
      if (it != 0)
        state->vi[it] = AddConstant(old.vi[it], 1);
      break;
    case LowerKind::SQD:
      if (it != 0)
        state->vi[it] = AddConstant(old.vi[it], -1);
      break;
    case LowerKind::XTOP:
      if (it != 0)
        state->vi[it] = KnownValue(VifTop());
      break;
    case LowerKind::XITOP:
      if (it != 0)
        state->vi[it] = KnownValue(VifItop());
      break;
    case LowerKind::BAL:
    case LowerKind::JALR:
      if (it != 0) {
        state->vi[it] =
            KnownValue(Constant(static_cast<u16>((plan.pc + 16) / 8)));
      }
      break;
    default:
      break;
    }
    state->vi[0] = KnownValue(Constant(0));
  }

  InvocationValueSet LoadAddress(const VitaVU::GpuPairPlan& plan,
                                 const SymbolicState& old,
                                 SymbolicState* state) {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    const u32 is = VUInterpFast::Is(code);
    if (kind == LowerKind::LQ)
      return Address(old, is, VUInterpFast::Imm11(code));
    if (kind == LowerKind::LQI)
      return old.vi[is];
    if (kind == LowerKind::LQD)
      return AddConstant(old.vi[is], -1);
    (void)state;
    return UnknownValue();
  }

  InvocationValueSet StoreAddress(const VitaVU::GpuPairPlan& plan,
                                  const SymbolicState& old) {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    const u32 it = VUInterpFast::It(code);
    if (kind == LowerKind::SQ)
      return Address(old, it, VUInterpFast::Imm11(code));
    if (kind == LowerKind::SQI)
      return old.vi[it];
    if (kind == LowerKind::SQD)
      return AddConstant(old.vi[it], -1);
    return UnknownValue();
  }

  bool ExtractQwordSource(
      const std::array<InvocationValueSet, 4>& vf,
      InvocationValueSet* qword_address) const {
    InvocationValueSet address;
    bool initialized = false;
    for (u32 lane = 0; lane < 4; lane++) {
      if (vf[lane].unknown)
        return false;
      InvocationValueSet lane_addresses;
      lane_addresses.unknown = false;
      for (const u32 value : vf[lane].alternatives) {
        if (value == InvalidValue || value >= m_plan->values.size())
          return false;
        const InvocationValueNode& node = m_plan->values[value];
        if (node.kind != InvocationValueKind::MemoryU32 ||
            node.lane != lane) {
          return false;
        }
        lane_addresses.alternatives.push_back(node.operands[0]);
      }
      NormalizeAlternatives(&lane_addresses);
      if (lane_addresses.unknown)
        return false;
      if (!initialized) {
        address = lane_addresses;
        initialized = true;
      } else if (!(address == lane_addresses)) {
        return false;
      }
    }
    *qword_address = std::move(address);
    return initialized;
  }

  void AssignLowerVf(const VitaVU::GpuPairPlan& plan,
                     const SymbolicState& old, SymbolicState* state) {
    const u32 destination = plan.lower_vf_write;
    if (destination == 0)
      return;
    const u8 mask = plan.lower_vf_write_mask;
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    if (kind == LowerKind::LQ || kind == LowerKind::LQI ||
        kind == LowerKind::LQD) {
      const InvocationValueSet address = LoadAddress(plan, old, state);
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(mask, lane)) {
          state->vf[destination][lane] =
              Memory(address, InvocationValueKind::MemoryU32, lane);
        }
      }
      return;
    }
    if (kind == LowerKind::MOVE || kind == LowerKind::MR32) {
      const u32 source = VUInterpFast::Fs(code);
      for (u32 lane = 0; lane < 4; lane++) {
        if (!LaneEnabled(mask, lane))
          continue;
        const u32 source_lane =
            kind == LowerKind::MR32 ? ((lane + 1) & 3) : lane;
        state->vf[destination][lane] = old.vf[source][source_lane];
      }
      return;
    }
    if (kind == LowerKind::MFIR) {
      const u32 source = VUInterpFast::Is(code);
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(mask, lane)) {
          state->vf[destination][lane] =
              MapUnary(old.vi[source],
                       [this](u32 value) {
                         return Unary(
                             InvocationValueKind::SignExtendU16, value);
                       });
        }
      }
      return;
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if (LaneEnabled(mask, lane))
        state->vf[destination][lane] = UnknownValue();
    }
  }

  void RecordQwordStore(const VitaVU::GpuPairPlan& plan,
                        const SymbolicState& old) {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    if ((kind != LowerKind::SQ && kind != LowerKind::SQI &&
         kind != LowerKind::SQD) ||
        VUInterpFast::XYZW(plan.lower) != 0x0f) {
      return;
    }
    const u32 source = VUInterpFast::Fs(plan.lower);
    InvocationValueSet source_address;
    if (!ExtractQwordSource(old.vf[source], &source_address))
      return;
    QwordCopy copy;
    copy.destination = StoreAddress(plan, old);
    copy.source = std::move(source_address);
    if (!copy.destination.unknown && !copy.source.unknown)
      m_qword_copies.push_back(std::move(copy));
  }

  void TransferPair(const VitaVU::GpuPairPlan& plan,
                    SymbolicState* state) {
    const SymbolicState old = *state;
    if (plan.exec_lower) {
      RecordQwordStore(plan, old);
      AssignLowerVi(plan, old, state);
      AssignLowerVf(plan, old, state);
    }

    // PairPlan has already discarded a conflicting lower instruction. Any
    // remaining upper VF write has architectural priority over the lower
    // result and arithmetic provenance is deliberately not evaluated on ARM.
    if (plan.exec_upper && plan.upper_vf_write != 0) {
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(plan.upper_vf_write_mask, lane))
          state->vf[plan.upper_vf_write][lane] = UnknownValue();
      }
    }
    state->vi[0] = KnownValue(Constant(0));
    for (u32 lane = 0; lane < 4; lane++)
      state->vf[0][lane] =
          KnownValue(Constant(lane == 3 ? 0x3f800000u : 0u));
  }

  bool FindGifTagSource(const SymbolicState& header,
                        std::string* error) {
    if (m_kernel.stores.empty())
      return Fail(error, "invocation plan has no loop stores");
    const LoopStore* first = &m_kernel.stores.front();
    for (const LoopStore& store : m_kernel.stores) {
      if (store.address.qword_offset < first->address.qword_offset)
        first = &store;
    }
    if (!first->address.valid ||
        first->address.base_vi >= header.vi.size()) {
      return Fail(error, "invocation output base is not affine");
    }
    const InvocationValueSet tag_destination =
        AddConstant(header.vi[first->address.base_vi],
                    first->address.qword_offset - 1);
    if (tag_destination.unknown)
      return Fail(error, "invocation output tag address is unknown");

    InvocationValueSet source;
    bool found = false;
    for (const QwordCopy& copy : m_qword_copies) {
      if (!(copy.destination == tag_destination))
        continue;
      if (!found) {
        source = copy.source;
        found = true;
      } else if (!(source == copy.source)) {
        return Fail(error,
                    "invocation output tag has conflicting memory sources");
      }
    }
    if (!found || source.unknown)
      return Fail(error,
                  "invocation cannot prove the copied packed GIF tag source");
    m_plan->gif_tag_qword_address = std::move(source);
    m_plan->has_static_gif_source = true;
    return true;
  }

  const ProgramAnalysis& m_program;
  const ParallelLoopKernel& m_kernel;
  ParallelInvocationPlan* m_plan;
  std::map<NodeKey, u32> m_node_cache;
  std::vector<QwordCopy> m_qword_copies;
};

bool EvaluateNode(const ParallelInvocationPlan& plan, u32 id,
                  const InvocationEvaluationContext& context,
                  std::map<u32, u32>* cache, std::set<u32>* active,
                  u32* result) {
  if (id == InvalidValue || id >= plan.values.size() ||
      !result)
    return false;
  const auto cached = cache->find(id);
  if (cached != cache->end()) {
    *result = cached->second;
    return true;
  }
  if (!active->insert(id).second)
    return false;
  const InvocationValueNode& node = plan.values[id];
  u32 left = 0;
  u32 right = 0;
  const auto operand = [&](u32 index, u32* value) {
    return EvaluateNode(plan, node.operands[index], context, cache,
                        active, value);
  };
  bool ok = true;
  switch (node.kind) {
  case InvocationValueKind::Constant:
    *result = node.immediate;
    break;
  case InvocationValueKind::InitialVi:
    ok = context.initial_vi != nullptr;
    if (ok)
      *result = context.initial_vi[node.reg];
    break;
  case InvocationValueKind::InitialVfWord:
    ok = context.initial_vf_words != nullptr;
    if (ok)
      *result = context.initial_vf_words[node.reg * 4 + node.lane];
    break;
  case InvocationValueKind::VifTop:
    *result = context.vif_top;
    break;
  case InvocationValueKind::VifItop:
    *result = context.vif_itop;
    break;
  case InvocationValueKind::AddU16:
    ok = operand(0, &left) && operand(1, &right);
    if (ok)
      *result = static_cast<u16>(left + right);
    break;
  case InvocationValueKind::AddConstantU16:
    ok = operand(0, &left);
    if (ok)
      *result = static_cast<u16>(
          left + static_cast<s32>(node.immediate));
    break;
  case InvocationValueKind::SubtractU16:
    ok = operand(0, &left) && operand(1, &right);
    if (ok)
      *result = static_cast<u16>(left - right);
    break;
  case InvocationValueKind::AndU16:
    ok = operand(0, &left) && operand(1, &right);
    if (ok)
      *result = static_cast<u16>(left & right);
    break;
  case InvocationValueKind::OrU16:
    ok = operand(0, &left) && operand(1, &right);
    if (ok)
      *result = static_cast<u16>(left | right);
    break;
  case InvocationValueKind::SignExtendU16:
    ok = operand(0, &left);
    if (ok)
      *result = static_cast<u32>(
          static_cast<s32>(static_cast<s16>(left)));
    break;
  case InvocationValueKind::MemoryU16:
    ok = operand(0, &left) && context.read_memory_u16 != nullptr;
    if (ok) {
      u16 value = 0;
      ok = context.read_memory_u16(
          context.memory_user, static_cast<u16>(left & 0x3ffu),
          node.lane, &value);
      *result = value;
    }
    break;
  case InvocationValueKind::MemoryU32:
    ok = operand(0, &left) && context.read_memory_u32 != nullptr;
    if (ok) {
      ok = context.read_memory_u32(
          context.memory_user, static_cast<u16>(left & 0x3ffu),
          node.lane, result);
    }
    break;
  }
  active->erase(id);
  if (!ok)
    return false;
  cache->emplace(id, *result);
  return true;
}

} // namespace

bool BuildParallelInvocationPlan(const ProgramAnalysis& program,
                                 const ParallelLoopKernel& kernel,
                                 ParallelInvocationPlan* plan,
                                 std::string* error) {
  if (!plan)
    return Fail(error, "null parallel invocation plan output");
  *plan = {};
  plan->loop_index = kernel.loop_index;
  InvocationPlanBuilder builder(program, kernel, plan);
  return builder.Build(error);
}

bool EvaluateInvocationValue(const ParallelInvocationPlan& plan,
                             const InvocationValueSet& value,
                             const InvocationEvaluationContext& context,
                             u32* result) {
  if (!result || value.unknown || value.alternatives.empty())
    return false;
  std::map<u32, u32> cache;
  u32 common = 0;
  bool have_common = false;
  for (const u32 alternative : value.alternatives) {
    std::set<u32> active;
    u32 evaluated = 0;
    if (!EvaluateNode(plan, alternative, context, &cache, &active,
                      &evaluated)) {
      return false;
    }
    if (!have_common) {
      common = evaluated;
      have_common = true;
    } else if (common != evaluated) {
      return false;
    }
  }
  *result = common;
  return have_common;
}

} // namespace VitaGpuVu
