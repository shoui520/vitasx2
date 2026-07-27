// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuLoopKernel.h"

#include "VU.h"
#include "VUmicroFast.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace VitaGpuVu {
namespace {

using LowerKind = VUInterpFast::LowerFastKind;
using UpperKind = VUInterpFast::UpperFastKind;

constexpr u32 InvalidNode = 0;
constexpr u32 MaximumEntryTransfers = 4096;

bool Fail(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
  return false;
}

bool LaneEnabled(u8 mask, u32 lane) {
  return (mask & (0x8u >> lane)) != 0;
}

struct ViValue {
  u16 value = 0;
  bool known = false;

  bool operator==(const ViValue& other) const {
    return value == other.value && known == other.known;
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

using NodeCacheKey =
    std::tuple<ExpressionKind, ScalarDomain, std::array<u32, 3>, u8, s32,
               s32, bool, u32, u8, u8>;

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
    const u32 entry_block = FindEntryBlock();
    if (entry_block == std::numeric_limits<u32>::max())
      return Fail(error, "acyclic entry slice has no external entry block");
    if (m_loop.header_block >= m_program.blocks.size())
      return Fail(error, "acyclic entry slice has no loop header block");

    ScanStableInitialState();
    const EntryState initial = InitialState();
    EntryState header{};
    bool header_reached = false;
    if (entry_block == m_loop.header_block) {
      header = initial;
      header_reached = true;
    } else if (!Propagate(entry_block, initial, &header, &header_reached,
                          error)) {
      return false;
    }
    if (!header_reached)
      return Fail(error, "acyclic entry slice did not reach the loop header");

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
    m_kernel->acyclic_entry_inlined = true;
    if (error)
      error->clear();
    return true;
  }

private:
  u32 FindEntryBlock() const {
    for (u32 block = 0; block < m_program.blocks.size(); block++) {
      if (m_program.blocks[block].start_pc == m_program.start_pc)
        return block;
    }
    return std::numeric_limits<u32>::max();
  }

  NodeCacheKey Key(const ExpressionNode& node) const {
    return {node.kind,
            node.domain,
            node.operands,
            node.memory_address.base_vi,
            node.memory_address.invocation_coefficient,
            node.memory_address.qword_offset,
            node.memory_address.valid,
            node.immediate,
            node.reg,
            node.lane};
  }

  void CacheNode(u32 id) {
    m_node_cache.emplace(Key(m_kernel->expressions[id]), id);
  }

  u32 AddNode(ExpressionNode node) {
    const NodeCacheKey key = Key(node);
    const auto existing = m_node_cache.find(key);
    if (existing != m_node_cache.end())
      return existing->second;
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

  EntryState InitialState() {
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
    state.vi[0] = {0, true};
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
    destination->value = 0;
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

  bool Propagate(u32 entry_block, const EntryState& initial,
                 EntryState* header, bool* header_reached,
                 std::string* error) {
    std::vector<EntryState> inputs(m_program.blocks.size());
    std::vector<bool> reachable(m_program.blocks.size(), false);
    std::vector<bool> queued(m_program.blocks.size(), false);
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
      for (const ProgramPair& pair : m_program.blocks[block_index].pairs)
        TransferPair(pair.plan, &output);

      for (const ControlEdge& edge :
           m_program.blocks[block_index].successors) {
        if (!edge.has_target)
          continue;
        if (edge.target_block == m_loop.header_block) {
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

  static ViValue AddVi(ViValue value, s32 delta) {
    if (value.known)
      value.value = static_cast<u16>(value.value + delta);
    return value;
  }

  static ViValue BinaryVi(const ViValue& left, const ViValue& right,
                          LowerKind kind) {
    if (!left.known || !right.known)
      return {};
    switch (kind) {
    case LowerKind::IADD:
      return {static_cast<u16>(left.value + right.value), true};
    case LowerKind::ISUB:
      return {static_cast<u16>(left.value - right.value), true};
    case LowerKind::IAND:
      return {static_cast<u16>(left.value & right.value), true};
    case LowerKind::IOR:
      return {static_cast<u16>(left.value | right.value), true};
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
    node.memory_address.base_vi = 0;
    node.memory_address.invocation_coefficient = 0;
    node.memory_address.qword_offset = address.value & 0x3ffu;
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
    case LowerKind::BAL:
    case LowerKind::JALR:
      if (it != 0)
        state->vi[it] =
            {static_cast<u16>((plan.pc + 16u) / 8u), true};
      break;
    default:
      break;
    }
    state->vi[0] = {0, true};
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
          ExpressionKind::Multiply, ScalarDomain::Float,
          old.vf[fs][fs_lane[lane]],
          old.vf[VUInterpFast::Ft(plan.upper)][ft_lane[lane]]);
      return kind == UpperKind::OPMULA
                 ? product
                 : Binary(ExpressionKind::Subtract, ScalarDomain::Float,
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
      const u32 product =
          Binary(ExpressionKind::Multiply, ScalarDomain::Float, left, right);
      return Binary(IsSubtract(kind) ? ExpressionKind::Subtract
                                     : ExpressionKind::Add,
                    ScalarDomain::Float, old.acc[lane], product);
    }
    if (IsMultiply(kind))
      return Binary(ExpressionKind::Multiply, ScalarDomain::Float, left,
                    right);
    return Binary(IsSubtract(kind) ? ExpressionKind::Subtract
                                   : ExpressionKind::Add,
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
};

} // namespace

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

} // namespace VitaGpuVu
