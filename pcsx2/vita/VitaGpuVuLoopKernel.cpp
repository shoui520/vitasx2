// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuLoopKernel.h"

#include "VU.h"
#include "VUmicroFast.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace VitaGpuVu {
namespace {

using LowerKind = VUInterpFast::LowerFastKind;
using UpperKind = VUInterpFast::UpperFastKind;

constexpr u32 InvalidNode = 0;
constexpr u32 MaximumSliceBackedges = 16;

bool Fail(std::string *error, std::string message) {
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

bool IsQwordLoad(LowerKind kind) {
  return kind == LowerKind::LQ || kind == LowerKind::LQI ||
         kind == LowerKind::LQD;
}

s32 LowerImmediateDelta(LowerKind kind, u32 code) {
  switch (kind) {
  case LowerKind::IADDIU:
    return VUInterpFast::Imm15(code);
  case LowerKind::ISUBIU:
    return -VUInterpFast::Imm15(code);
  case LowerKind::IADDI:
    return VUInterpFast::Imm5(code);
  default:
    return 0;
  }
}

bool LowerSelfAffineWrite(const VitaVU::GpuPairPlan &plan, u32 reg,
                          s32 *delta) {
  if (!plan.exec_lower || plan.lower_discarded_by_upper ||
      (plan.lower_vi_write & (1u << reg)) == 0) {
    return false;
  }

  const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
  const u32 code = plan.lower;
  switch (kind) {
  case LowerKind::IADDIU:
  case LowerKind::ISUBIU:
  case LowerKind::IADDI:
    if (VUInterpFast::It(code) != reg || VUInterpFast::Is(code) != reg)
      return false;
    *delta = LowerImmediateDelta(kind, code);
    return true;
  case LowerKind::LQI:
    if (VUInterpFast::Is(code) != reg)
      return false;
    *delta = 1;
    return true;
  case LowerKind::LQD:
    if (VUInterpFast::Is(code) != reg)
      return false;
    *delta = -1;
    return true;
  case LowerKind::SQI:
    if (VUInterpFast::It(code) != reg)
      return false;
    *delta = 1;
    return true;
  case LowerKind::SQD:
    if (VUInterpFast::It(code) != reg)
      return false;
    *delta = -1;
    return true;
  default:
    return false;
  }
}

bool AnalyzeViEvolution(const BasicBlock &block, ViEvolution *evolution,
                        std::string *error) {
  *evolution = {};
  evolution->affine_mask = 0xffffu;
  for (u32 reg = 0; reg < 16; reg++) {
    evolution->prefix[reg].resize(block.pairs.size() + 1);
    evolution->prefix[reg][0] = 0;
  }

  for (u32 pair_index = 0; pair_index < block.pairs.size(); pair_index++) {
    const VitaVU::GpuPairPlan &plan = block.pairs[pair_index].plan;
    for (u32 reg = 0; reg < 16; reg++)
      evolution->prefix[reg][pair_index + 1] =
          evolution->prefix[reg][pair_index];

    if (!plan.exec_lower || plan.lower_discarded_by_upper)
      continue;
    const u32 writes = plan.lower_vi_write & 0xffffu;
    for (u32 reg = 1; reg < 16; reg++) {
      if ((writes & (1u << reg)) == 0)
        continue;
      s32 delta = 0;
      if (!LowerSelfAffineWrite(plan, reg, &delta)) {
        evolution->affine_mask &= ~(1u << reg);
        continue;
      }
      if ((evolution->affine_mask & (1u << reg)) != 0)
        evolution->prefix[reg][pair_index + 1] += delta;
    }
  }

  for (u32 reg = 1; reg < 16; reg++) {
    if ((evolution->affine_mask & (1u << reg)) != 0) {
      evolution->step[reg] = evolution->prefix[reg].back();
      continue;
    }
    std::fill(evolution->prefix[reg].begin(), evolution->prefix[reg].end(),
              0);
  }

  // VI0 is architectural zero even though malformed encodings can advertise
  // it as a destination in register metadata.
  evolution->step[0] = 0;
  std::fill(evolution->prefix[0].begin(), evolution->prefix[0].end(), 0);
  if (error)
    error->clear();
  return true;
}

enum class SlotKind : u8 {
  Vf,
  Acc,
  Q,
  P,
  I,
};

struct ResolveKey {
  SlotKind kind = SlotKind::Vf;
  u8 reg = 0;
  u8 lane = 0;
  u16 cursor = 0;
  s16 iteration = 0;
  ScalarDomain requested = ScalarDomain::Raw;

  auto AsTuple() const {
    return std::tie(kind, reg, lane, cursor, iteration, requested);
  }

  bool operator<(const ResolveKey &other) const {
    return AsTuple() < other.AsTuple();
  }
};

class KernelBuilder {
public:
  KernelBuilder(const ProgramAnalysis &program, const NaturalLoop &loop,
                const BasicBlock &block, ParallelLoopKernel *kernel)
      : m_program(program), m_loop(loop), m_block(block), m_kernel(kernel) {
    // Node zero is an explicit invalid sentinel.
    m_kernel->expressions.emplace_back();
  }

  bool Build(std::string *error) {
    if (!AnalyzeViEvolution(m_block, &m_kernel->vi, error))
      return false;

    m_kernel->pair_count = static_cast<u32>(m_block.pairs.size());
    m_kernel->header_pc = m_block.start_pc;
    m_kernel->latch_pc = m_program.blocks[m_loop.latch_block].start_pc;

    for (u32 pair_index = 0; pair_index < m_block.pairs.size();
         pair_index++) {
      const VitaVU::GpuPairPlan &plan = m_block.pairs[pair_index].plan;
      if (!plan.exec_lower || plan.lower_discarded_by_upper)
        continue;
      const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
      if (!IsQwordStore(kind))
        continue;

      LoopStore store{};
      store.pair_pc = plan.pc;
      store.write_mask = static_cast<u8>(VUInterpFast::XYZW(plan.lower));
      store.source_vf = static_cast<u8>(VUInterpFast::Fs(plan.lower));
      if (!MemoryAddress(plan, pair_index, 0, &store.address)) {
        m_kernel->has_unsupported_expression = true;
        continue;
      }

      for (u32 lane = 0; lane < 4; lane++) {
        if (!LaneEnabled(store.write_mask, lane))
          continue;
        m_current_store_pc = store.pair_pc;
        m_current_store_lane = lane;
        u32 value =
            Resolve({SlotKind::Vf, store.source_vf, static_cast<u8>(lane),
                     static_cast<u16>(pair_index), 0, ScalarDomain::Raw});
        if (value != InvalidNode &&
            m_kernel->expressions[value].domain == ScalarDomain::Float) {
          value = Unary(ExpressionKind::Normalize, ScalarDomain::Float, value);
        }
        store.values[lane] = value;
      }
      m_kernel->stores.push_back(store);
    }

    if (m_kernel->stores.empty()) {
      std::string detail = "natural loop has no qword output store (";
      for (u32 i = 0; i < m_block.pairs.size(); i++) {
        if (i != 0)
          detail += ",";
        const VitaVU::GpuPairPlan &plan = m_block.pairs[i].plan;
        detail += std::to_string(plan.lower_kind);
        detail += plan.exec_lower ? "e" : "x";
        detail += plan.lower_discarded_by_upper ? "d" : "k";
      }
      detail += ")";
      return Fail(error, std::move(detail));
    }

    bool complete = true;
    for (const LoopStore &store : m_kernel->stores) {
      complete &= store.address.valid;
      for (u32 lane = 0; lane < 4; lane++) {
        if (LaneEnabled(store.write_mask, lane))
          complete &= store.values[lane] != InvalidNode;
      }
    }
    m_kernel->independent_store_values =
        complete && !m_kernel->has_true_recurrence &&
        !m_kernel->has_unsupported_expression;
    if (!m_kernel->independent_store_values) {
      std::string reason =
          m_kernel->has_true_recurrence
              ? "loop output has an unbounded carried value"
              : "loop output uses an unsupported semantic expression";
      if (!m_rejection_detail.empty())
        reason += ": " + m_rejection_detail;
      return Fail(error, std::move(reason));
    }
    if (error)
      error->clear();
    return true;
  }

private:
  u32 AddNode(ExpressionNode node) {
    const auto key =
        std::tuple(node.kind, node.domain, node.operands, node.memory_address.base_vi,
                   node.memory_address.invocation_coefficient,
                   node.memory_address.qword_offset,
                   node.memory_address.valid, node.immediate, node.reg,
                   node.lane);
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

  u32 Invariant(SlotKind kind, u32 reg, u32 lane,
                ScalarDomain requested) {
    ExpressionNode node{};
    node.domain = requested;
    node.reg = static_cast<u8>(reg);
    node.lane = static_cast<u8>(lane);
    switch (kind) {
    case SlotKind::Vf:
      if (reg == 0) {
        // VF0 is (0,0,0,1) in floating-point representation.
        return FloatConstant(lane == 3 ? 0x3f800000u : 0u);
      }
      node.kind = ExpressionKind::InvariantVf;
      break;
    case SlotKind::Acc:
      node.kind = ExpressionKind::InvariantAcc;
      node.domain = ScalarDomain::Float;
      break;
    case SlotKind::Q:
      node.kind = ExpressionKind::InvariantQ;
      node.domain = ScalarDomain::Float;
      break;
    case SlotKind::P:
      node.kind = ExpressionKind::InvariantP;
      node.domain = ScalarDomain::Float;
      break;
    case SlotKind::I:
      node.kind = ExpressionKind::InvariantI;
      break;
    }
    return AddNode(node);
  }

  bool IsLoopInvariantExpression(u32 node_id) const {
    if (node_id == InvalidNode || node_id >= m_kernel->expressions.size())
      return false;
    const ExpressionNode &node = m_kernel->expressions[node_id];
    switch (node.kind) {
    case ExpressionKind::ConstantFloat:
    case ExpressionKind::ConstantSigned:
    case ExpressionKind::ConstantUnsigned:
    case ExpressionKind::InvariantVf:
    case ExpressionKind::InvariantAcc:
    case ExpressionKind::InvariantQ:
    case ExpressionKind::InvariantP:
    case ExpressionKind::InvariantI:
      return true;
    case ExpressionKind::Memory:
      // Proving that a loop store cannot alias a constant-address load belongs
      // to the later memory-contract analysis. Keep the first lowering strict.
      return false;
    default:
      break;
    }

    for (u32 operand : node.operands) {
      if (operand != InvalidNode && !IsLoopInvariantExpression(operand))
        return false;
    }
    return true;
  }

  bool MemoryAddress(const VitaVU::GpuPairPlan &plan, u32 pair_index,
                     s32 iteration, AffineQwordAddress *address) const {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    u32 base = 0;
    s32 immediate = 0;
    switch (kind) {
    case LowerKind::LQ:
      base = VUInterpFast::Is(plan.lower);
      immediate = VUInterpFast::Imm11(plan.lower);
      break;
    case LowerKind::SQ:
      base = VUInterpFast::It(plan.lower);
      immediate = VUInterpFast::Imm11(plan.lower);
      break;
    case LowerKind::LQI:
      base = VUInterpFast::Is(plan.lower);
      break;
    case LowerKind::LQD:
      base = VUInterpFast::Is(plan.lower);
      immediate = -1;
      break;
    case LowerKind::SQI:
      base = VUInterpFast::It(plan.lower);
      break;
    case LowerKind::SQD:
      base = VUInterpFast::It(plan.lower);
      immediate = -1;
      break;
    default:
      return false;
    }

    if ((m_kernel->vi.affine_mask & (1u << base)) == 0 ||
        pair_index >= m_kernel->vi.prefix[base].size()) {
      return false;
    }
    address->base_vi = static_cast<u8>(base);
    address->invocation_coefficient = m_kernel->vi.step[base];
    address->qword_offset =
        iteration * m_kernel->vi.step[base] +
        m_kernel->vi.prefix[base][pair_index] + immediate;
    address->valid = true;
    return true;
  }

  u32 Memory(const VitaVU::GpuPairPlan &plan, u32 pair_index, s32 iteration,
             u32 lane, ScalarDomain requested) {
    ExpressionNode node{};
    node.kind = ExpressionKind::Memory;
    node.domain = requested;
    node.lane = static_cast<u8>(lane);
    if (!MemoryAddress(plan, pair_index, iteration, &node.memory_address)) {
      m_kernel->has_unsupported_expression = true;
      return InvalidNode;
    }
    return AddNode(node);
  }

  bool PairWritesSlot(const VitaVU::GpuPairPlan &plan, SlotKind kind, u32 reg,
                      u32 lane, bool *upper, bool *lower) const {
    *upper = false;
    *lower = false;
    switch (kind) {
    case SlotKind::Vf:
      // VF0 is architectural constant state. PairPlan uses register zero as
      // the "no VF destination" sentinel for ACC-only upper instructions.
      if (reg == 0)
        return false;
      *upper = plan.exec_upper && plan.upper_vf_write == reg &&
               LaneEnabled(plan.upper_vf_write_mask, lane);
      *lower = plan.exec_lower && !plan.lower_discarded_by_upper &&
               plan.lower_vf_write == reg &&
               LaneEnabled(plan.lower_vf_write_mask, lane);
      return *upper || *lower;
    case SlotKind::Acc:
      *upper = plan.exec_upper &&
               (plan.upper_vi_write & (1u << REG_ACC_FLAG)) != 0 &&
               LaneEnabled(plan.upper_vf_write_mask, lane);
      return *upper;
    case SlotKind::Q:
      *lower = plan.exec_lower && !plan.lower_discarded_by_upper &&
               (plan.lower_vi_write & (1u << REG_Q)) != 0;
      return *lower;
    case SlotKind::P:
      *lower = plan.exec_lower && !plan.lower_discarded_by_upper &&
               (plan.lower_vi_write & (1u << REG_P)) != 0;
      return *lower;
    case SlotKind::I:
      *lower = plan.immediate_lower;
      return *lower;
    }
    return false;
  }

  bool HasWriterBefore(SlotKind kind, u32 reg, u32 lane,
                       u32 pair_index) const {
    for (u32 i = 0; i < pair_index; i++) {
      bool upper = false;
      bool lower = false;
      if (PairWritesSlot(m_block.pairs[i].plan, kind, reg, lane, &upper,
                         &lower)) {
        return true;
      }
    }
    return false;
  }

  u32 Resolve(ResolveKey key) {
    if (key.iteration < -static_cast<s32>(MaximumSliceBackedges)) {
      m_kernel->has_true_recurrence = true;
      RecordRejection(key, "slice limit");
      return InvalidNode;
    }
    m_kernel->maximum_backedge_distance =
        std::max(m_kernel->maximum_backedge_distance,
                 static_cast<u32>(-std::min<s32>(key.iteration, 0)));

    const auto cached = m_resolved.find(key);
    if (cached != m_resolved.end())
      return cached->second;
    if (!m_resolving.insert(key).second) {
      m_kernel->has_true_recurrence = true;
      RecordRejection(key, "cyclic value");
      return InvalidNode;
    }
    m_resolution_stack.push_back(key);

    ResolveKey writer_key = key;
    bool found = false;
    bool upper = false;
    bool lower = false;
    for (;;) {
      while (writer_key.cursor > 0) {
        const u32 pair_index = --writer_key.cursor;
        const VitaVU::GpuPairPlan &plan = m_block.pairs[pair_index].plan;
        if (PairWritesSlot(plan, key.kind, key.reg, key.lane, &upper, &lower)) {
          found = true;
          break;
        }
      }
      if (found)
        break;

      bool written_in_loop = false;
      for (const ProgramPair &pair : m_block.pairs) {
        bool loop_upper = false;
        bool loop_lower = false;
        if (PairWritesSlot(pair.plan, key.kind, key.reg, key.lane,
                           &loop_upper, &loop_lower)) {
          written_in_loop = true;
          break;
        }
      }
      if (!written_in_loop)
        break;
      writer_key.cursor = static_cast<u16>(m_block.pairs.size());
      writer_key.iteration--;
      m_kernel->maximum_backedge_distance =
          std::max(m_kernel->maximum_backedge_distance,
                   static_cast<u32>(-std::min<s32>(writer_key.iteration, 0)));
      if (writer_key.iteration <
          -static_cast<s32>(MaximumSliceBackedges)) {
        m_kernel->has_true_recurrence = true;
        RecordRejection(writer_key, "writer search limit");
        break;
      }
    }

    u32 result = InvalidNode;
    if (!found && !m_kernel->has_true_recurrence) {
      result = Invariant(key.kind, key.reg, key.lane, key.requested);
    } else if (found) {
      const u32 pair_index = writer_key.cursor;
      const VitaVU::GpuPairPlan &plan = m_block.pairs[pair_index].plan;
      // Architectural upper priority makes the upper value authoritative
      // whenever both sides advertise the same destination lane.
      result = upper
                   ? ResolveUpper(plan, pair_index, writer_key.iteration,
                                  key.kind, key.lane, key.requested)
                   : ResolveLower(plan, pair_index, writer_key.iteration,
                                  key.kind, key.lane, key.requested);
    }

    m_resolving.erase(key);
    m_resolved.emplace(key, result);
    m_resolution_stack.pop_back();
    return result;
  }

  u32 ResolveBefore(SlotKind kind, u32 reg, u32 lane, u32 pair_index,
                    s32 iteration, ScalarDomain requested) {
    return Resolve({kind, static_cast<u8>(reg), static_cast<u8>(lane),
                    static_cast<u16>(pair_index),
                    static_cast<s16>(iteration), requested});
  }

  void RecordRejection(const ResolveKey &key, const char *reason) {
    if (!m_rejection_detail.empty())
      return;
    const char *slot = "unknown";
    switch (key.kind) {
    case SlotKind::Vf:
      slot = "VF";
      break;
    case SlotKind::Acc:
      slot = "ACC";
      break;
    case SlotKind::Q:
      slot = "Q";
      break;
    case SlotKind::P:
      slot = "P";
      break;
    case SlotKind::I:
      slot = "I";
      break;
    }
    m_rejection_detail =
        "store_pc=" + std::to_string(m_current_store_pc) +
        " store_lane=" + std::to_string(m_current_store_lane) +
        " reason=" + reason + " slot=" + slot +
        " reg=" + std::to_string(key.reg) +
        " lane=" + std::to_string(key.lane) +
        " cursor=" + std::to_string(key.cursor) +
        " iteration=" + std::to_string(key.iteration);
    const size_t first =
        m_resolution_stack.size() > 20 ? m_resolution_stack.size() - 20 : 0;
    m_rejection_detail += " path=";
    for (size_t i = first; i < m_resolution_stack.size(); i++) {
      const ResolveKey &path_key = m_resolution_stack[i];
      if (i != first)
        m_rejection_detail += ",";
      m_rejection_detail += std::to_string(static_cast<u32>(path_key.kind));
      m_rejection_detail += "/";
      m_rejection_detail += std::to_string(path_key.reg);
      m_rejection_detail += "/";
      m_rejection_detail += std::to_string(path_key.lane);
      m_rejection_detail += "@";
      m_rejection_detail += std::to_string(path_key.cursor);
      m_rejection_detail += "#";
      m_rejection_detail += std::to_string(path_key.iteration);
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
    switch (kind) {
    case UpperKind::ADDi:
    case UpperKind::ADDAi:
    case UpperKind::SUBi:
    case UpperKind::SUBAi:
    case UpperKind::MAXi:
    case UpperKind::MINIi:
    case UpperKind::MULi:
    case UpperKind::MULAi:
    case UpperKind::MADDi:
    case UpperKind::MADDAi:
    case UpperKind::MSUBi:
    case UpperKind::MSUBAi:
      return true;
    default:
      return false;
    }
  }

  static bool UsesQ(UpperKind kind) {
    switch (kind) {
    case UpperKind::ADDq:
    case UpperKind::ADDAq:
    case UpperKind::SUBq:
    case UpperKind::SUBAq:
    case UpperKind::MULq:
    case UpperKind::MULAq:
    case UpperKind::MADDq:
    case UpperKind::MADDAq:
    case UpperKind::MSUBq:
    case UpperKind::MSUBAq:
      return true;
    default:
      return false;
    }
  }

  u32 UpperFtOperand(const VitaVU::GpuPairPlan &plan, UpperKind kind,
                     u32 pair_index, s32 iteration, u32 lane) {
    if (UsesI(kind))
      return ResolveBefore(SlotKind::I, 0, 0, pair_index, iteration,
                           ScalarDomain::Float);
    if (UsesQ(kind))
      return ResolveBefore(SlotKind::Q, 0, 0, pair_index, iteration,
                           ScalarDomain::Float);
    const s32 broadcast = BroadcastLane(kind);
    const u32 source_lane = broadcast >= 0 ? static_cast<u32>(broadcast) : lane;
    return ResolveBefore(SlotKind::Vf, VUInterpFast::Ft(plan.upper),
                         source_lane, pair_index, iteration,
                         ScalarDomain::Float);
  }

  u32 ResolveUpper(const VitaVU::GpuPairPlan &plan, u32 pair_index,
                   s32 iteration, SlotKind destination, u32 lane,
                   ScalarDomain requested) {
    const UpperKind kind = static_cast<UpperKind>(plan.upper_kind);
    const u32 code = plan.upper;
    const u32 fs_reg = VUInterpFast::Fs(code);

    if (kind == UpperKind::ABS) {
      return Unary(ExpressionKind::Absolute, ScalarDomain::Float,
                   ResolveBefore(SlotKind::Vf, fs_reg, lane, pair_index,
                                 iteration, ScalarDomain::Float));
    }
    if (kind == UpperKind::FTOI0 || kind == UpperKind::FTOI4 ||
        kind == UpperKind::FTOI12 || kind == UpperKind::FTOI15) {
      const u32 shift = kind == UpperKind::FTOI0
                            ? 0
                            : kind == UpperKind::FTOI4
                                  ? 4
                                  : kind == UpperKind::FTOI12 ? 12 : 15;
      return Unary(ExpressionKind::FloatToInt, ScalarDomain::SignedInt,
                   ResolveBefore(SlotKind::Vf, fs_reg, lane, pair_index,
                                 iteration, ScalarDomain::Float),
                   shift);
    }
    if (kind == UpperKind::ITOF0 || kind == UpperKind::ITOF4 ||
        kind == UpperKind::ITOF12 || kind == UpperKind::ITOF15) {
      const u32 shift = kind == UpperKind::ITOF0
                            ? 0
                            : kind == UpperKind::ITOF4
                                  ? 4
                                  : kind == UpperKind::ITOF12 ? 12 : 15;
      return Unary(ExpressionKind::IntToFloat, ScalarDomain::Float,
                   ResolveBefore(SlotKind::Vf, fs_reg, lane, pair_index,
                                 iteration, ScalarDomain::SignedInt),
                   shift);
    }
    if (kind == UpperKind::OPMULA || kind == UpperKind::OPMSUB) {
      if (lane >= 3)
        return Invariant(destination, 0, lane, requested);
      constexpr std::array<u8, 3> fs_lane = {1, 2, 0};
      constexpr std::array<u8, 3> ft_lane = {2, 0, 1};
      const u32 product = Binary(
          ExpressionKind::Multiply, ScalarDomain::Float,
          ResolveBefore(SlotKind::Vf, fs_reg, fs_lane[lane], pair_index,
                        iteration, ScalarDomain::Float),
          ResolveBefore(SlotKind::Vf, VUInterpFast::Ft(code), ft_lane[lane],
                        pair_index, iteration, ScalarDomain::Float));
      if (kind == UpperKind::OPMULA)
        return product;
      return Binary(ExpressionKind::Subtract, ScalarDomain::Float,
                    ResolveBefore(SlotKind::Acc, 0, lane, pair_index,
                                  iteration, ScalarDomain::Float),
                    product);
    }

    const bool arithmetic =
        IsAdd(kind) || IsSubtract(kind) || IsMultiply(kind) ||
        IsMadd(kind) || kind == UpperKind::MSUB ||
        kind == UpperKind::MSUBi || kind == UpperKind::MSUBq ||
        kind == UpperKind::MSUBx || kind == UpperKind::MSUBy ||
        kind == UpperKind::MSUBz || kind == UpperKind::MSUBw ||
        kind == UpperKind::MSUBA || kind == UpperKind::MSUBAi ||
        kind == UpperKind::MSUBAq || kind == UpperKind::MSUBAx ||
        kind == UpperKind::MSUBAy || kind == UpperKind::MSUBAz ||
        kind == UpperKind::MSUBAw || kind == UpperKind::MAX ||
        kind == UpperKind::MAXi || kind == UpperKind::MAXx ||
        kind == UpperKind::MAXy || kind == UpperKind::MAXz ||
        kind == UpperKind::MAXw || kind == UpperKind::MINI ||
        kind == UpperKind::MINIi || kind == UpperKind::MINIx ||
        kind == UpperKind::MINIy || kind == UpperKind::MINIz ||
        kind == UpperKind::MINIw;
    if (!arithmetic) {
      m_kernel->has_unsupported_expression = true;
      return InvalidNode;
    }

    const u32 right =
        UpperFtOperand(plan, kind, pair_index, iteration, lane);
    u32 left = InvalidNode;
    const bool min_or_max =
        kind == UpperKind::MAX || kind == UpperKind::MAXi ||
        kind == UpperKind::MAXx || kind == UpperKind::MAXy ||
        kind == UpperKind::MAXz || kind == UpperKind::MAXw ||
        kind == UpperKind::MINI || kind == UpperKind::MINIi ||
        kind == UpperKind::MINIx || kind == UpperKind::MINIy ||
        kind == UpperKind::MINIz || kind == UpperKind::MINIw;
    const bool carried_self =
        destination == SlotKind::Vf && fs_reg == plan.upper_vf_write &&
        !HasWriterBefore(SlotKind::Vf, fs_reg, lane, pair_index);
    if (min_or_max && carried_self && IsLoopInvariantExpression(right)) {
      // max(max(x,c),c) == max(x,c), and likewise for min. Collapse an
      // idempotent carried clamp to one operation on the loop-entry value.
      // This is an algebraic dependence proof, not an instruction-sequence
      // recognition rule.
      left = Invariant(SlotKind::Vf, fs_reg, lane, ScalarDomain::Float);
      m_kernel->collapsed_idempotent_recurrences++;
    } else {
      left = ResolveBefore(SlotKind::Vf, fs_reg, lane, pair_index, iteration,
                           ScalarDomain::Float);
    }
    u32 value = InvalidNode;
    if (kind == UpperKind::MAX || kind == UpperKind::MAXi ||
        kind == UpperKind::MAXx || kind == UpperKind::MAXy ||
        kind == UpperKind::MAXz || kind == UpperKind::MAXw) {
      value = Binary(ExpressionKind::Maximum, ScalarDomain::Float, left,
                     right);
    } else if (kind == UpperKind::MINI || kind == UpperKind::MINIi ||
               kind == UpperKind::MINIx || kind == UpperKind::MINIy ||
               kind == UpperKind::MINIz || kind == UpperKind::MINIw) {
      value = Binary(ExpressionKind::Minimum, ScalarDomain::Float, left,
                     right);
    } else if (IsMadd(kind) || ReadsAcc(kind)) {
      const u32 product = Binary(ExpressionKind::Multiply,
                                 ScalarDomain::Float, left, right);
      const u32 acc =
          ResolveBefore(SlotKind::Acc, 0, lane, pair_index, iteration,
                        ScalarDomain::Float);
      value = Binary(IsSubtract(kind) ? ExpressionKind::Subtract
                                      : ExpressionKind::Add,
                     ScalarDomain::Float, acc, product);
    } else if (IsMultiply(kind)) {
      value = Binary(ExpressionKind::Multiply, ScalarDomain::Float, left,
                     right);
    } else {
      value = Binary(IsSubtract(kind) ? ExpressionKind::Subtract
                                      : ExpressionKind::Add,
                     ScalarDomain::Float, left, right);
    }
    return value;
  }

  u32 ResolveLower(const VitaVU::GpuPairPlan &plan, u32 pair_index,
                   s32 iteration, SlotKind destination, u32 lane,
                   ScalarDomain requested) {
    const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
    const u32 code = plan.lower;
    switch (destination) {
    case SlotKind::Vf:
      if (IsQwordLoad(kind))
        return Memory(plan, pair_index, iteration, lane, requested);
      if (kind == LowerKind::MOVE) {
        return ResolveBefore(SlotKind::Vf, VUInterpFast::Fs(code), lane,
                             pair_index, iteration, requested);
      }
      if (kind == LowerKind::MR32) {
        return ResolveBefore(SlotKind::Vf, VUInterpFast::Fs(code),
                             (lane + 1) & 3, pair_index, iteration,
                             requested);
      }
      if (kind == LowerKind::MFP) {
        return ResolveBefore(SlotKind::P, 0, 0, pair_index, iteration,
                             ScalarDomain::Float);
      }
      m_kernel->has_unsupported_expression = true;
      return InvalidNode;
    case SlotKind::Q: {
      const u32 fs_lane = VUInterpFast::Fsf(code);
      const u32 ft_lane = (code >> 23) & 3;
      if (kind == LowerKind::DIV) {
        return Binary(
            ExpressionKind::Divide, ScalarDomain::Float,
            ResolveBefore(SlotKind::Vf, VUInterpFast::Fs(code), fs_lane,
                          pair_index, iteration, ScalarDomain::Float),
            ResolveBefore(SlotKind::Vf, VUInterpFast::Ft(code), ft_lane,
                          pair_index, iteration, ScalarDomain::Float));
      }
      if (kind == LowerKind::SQRT) {
        return Unary(
            ExpressionKind::SquareRoot, ScalarDomain::Float,
            Unary(ExpressionKind::Absolute, ScalarDomain::Float,
                  ResolveBefore(SlotKind::Vf, VUInterpFast::Ft(code), ft_lane,
                                pair_index, iteration,
                                ScalarDomain::Float)));
      }
      if (kind == LowerKind::RSQRT) {
        return Binary(
            ExpressionKind::Divide, ScalarDomain::Float,
            ResolveBefore(SlotKind::Vf, VUInterpFast::Fs(code), fs_lane,
                          pair_index, iteration, ScalarDomain::Float),
            Unary(
                ExpressionKind::SquareRoot, ScalarDomain::Float,
                Unary(ExpressionKind::Absolute, ScalarDomain::Float,
                      ResolveBefore(SlotKind::Vf, VUInterpFast::Ft(code),
                                    ft_lane, pair_index, iteration,
                                    ScalarDomain::Float))));
      }
      m_kernel->has_unsupported_expression = true;
      return InvalidNode;
    }
    case SlotKind::P:
      m_kernel->has_unsupported_expression = true;
      return InvalidNode;
    case SlotKind::I:
      if (plan.immediate_lower)
        return FloatConstant(plan.lower);
      m_kernel->has_unsupported_expression = true;
      return InvalidNode;
    case SlotKind::Acc:
      m_kernel->has_unsupported_expression = true;
      return InvalidNode;
    }
    return InvalidNode;
  }

  const ProgramAnalysis &m_program;
  const NaturalLoop &m_loop;
  const BasicBlock &m_block;
  ParallelLoopKernel *m_kernel;
  std::map<ResolveKey, u32> m_resolved;
  std::set<ResolveKey> m_resolving;
  std::vector<ResolveKey> m_resolution_stack;
  std::string m_rejection_detail;
  u32 m_current_store_pc = 0;
  u32 m_current_store_lane = 0;
  using NodeCacheKey =
      std::tuple<ExpressionKind, ScalarDomain, std::array<u32, 3>, u8, s32,
                 s32, bool, u32, u8, u8>;
  std::map<NodeCacheKey, u32> m_node_cache;
};

} // namespace

bool BuildParallelLoopKernel(const ProgramAnalysis &program, u32 loop_index,
                             ParallelLoopKernel *kernel, std::string *error) {
  if (!kernel)
    return Fail(error, "null parallel-loop kernel output");
  *kernel = {};
  kernel->loop_index = loop_index;
  if (loop_index >= program.natural_loops.size())
    return Fail(error, "parallel-loop index is out of range");

  const NaturalLoop &loop = program.natural_loops[loop_index];
  if (!loop.affine_counter || !loop.branch_taken_repeats)
    return Fail(error, "natural loop has no affine repeating counter");
  if (loop.header_block != loop.latch_block || loop.blocks.size() != 1)
    return Fail(error,
                "first parallel lowering requires one natural-loop block");
  if (loop.header_block >= program.blocks.size())
    return Fail(error, "natural-loop block index is out of range");

  const BasicBlock &block = program.blocks[loop.header_block];
  if (!block.has_branch || !block.conditional_branch ||
      block.branch_in_delay_slot) {
    return Fail(error, "natural-loop latch control is not lowerable");
  }

  KernelBuilder builder(program, loop, block, kernel);
  return builder.Build(error);
}

} // namespace VitaGpuVu
