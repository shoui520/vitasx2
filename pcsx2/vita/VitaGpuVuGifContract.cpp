// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuGifContract.h"

#include "GS/GSRegs.h"
#include "VUmicroFast.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace VitaGpuVu {
namespace {

constexpr u32 InvalidNode = 0;

bool Fail(std::string *error, std::string message) {
  if (error)
    *error = std::move(message);
  return false;
}

bool FullStore(const LoopStore &store) { return store.write_mask == 0x0f; }

bool IsFloatConstant(const ParallelLoopKernel &kernel, u32 node_id, u32 bits) {
  return node_id != InvalidNode && node_id < kernel.expressions.size() &&
         kernel.expressions[node_id].kind == ExpressionKind::ConstantFloat &&
         kernel.expressions[node_id].immediate == bits;
}

u32 StripAdditiveZero(const ParallelLoopKernel &kernel, u32 node_id) {
  if (node_id == InvalidNode || node_id >= kernel.expressions.size())
    return node_id;
  const ExpressionNode &node = kernel.expressions[node_id];
  if (node.kind != ExpressionKind::Add &&
      node.kind != ExpressionKind::RoundedAdd)
    return node_id;
  if (IsFloatConstant(kernel, node.operands[0], 0))
    return node.operands[1];
  if (IsFloatConstant(kernel, node.operands[1], 0))
    return node.operands[0];
  return node_id;
}

bool ProvesPerspectiveAdcClear(const ParallelLoopKernel &kernel,
                               u32 node_id) {
  if (node_id == InvalidNode || node_id >= kernel.expressions.size())
    return false;
  const ExpressionNode &conversion = kernel.expressions[node_id];
  if (conversion.kind != ExpressionKind::FloatToInt ||
      conversion.immediate > 14 ||
      conversion.operands[0] == InvalidNode ||
      conversion.operands[0] >= kernel.expressions.size()) {
    return false;
  }

  const ExpressionNode &product =
      kernel.expressions[conversion.operands[0]];
  if (product.kind != ExpressionKind::Multiply &&
      product.kind != ExpressionKind::RoundedMultiply)
    return false;
  for (u32 reciprocal_side = 0; reciprocal_side < 2; reciprocal_side++) {
    const u32 divide_id = product.operands[reciprocal_side];
    const u32 value_id =
        StripAdditiveZero(kernel, product.operands[reciprocal_side ^ 1]);
    if (divide_id == InvalidNode || divide_id >= kernel.expressions.size())
      continue;
    const ExpressionNode &divide = kernel.expressions[divide_id];
    if (divide.kind == ExpressionKind::Divide &&
        IsFloatConstant(kernel, divide.operands[0], 0x3f800000u) &&
        StripAdditiveZero(kernel, divide.operands[1]) == value_id) {
      // VU DIV returns signed max finite for a zero denominator, so x*(1/x)
      // is zero for x==0. For a finite normalized nonzero x, native nearest
      // reciprocal/multiply remains in (-2,2). Scaling by at most 2^14 cannot
      // set packed XYZ ADC bit 15.
      return true;
    }
  }
  return false;
}

bool ProvesAdcClear(const ParallelLoopKernel &kernel, u32 node_id) {
  if (node_id == InvalidNode || node_id >= kernel.expressions.size())
    return false;
  const ExpressionNode &node = kernel.expressions[node_id];
  switch (node.kind) {
  case ExpressionKind::ConstantSigned:
  case ExpressionKind::ConstantUnsigned:
    return (node.immediate & 0x8000u) == 0;
  default:
    return ProvesPerspectiveAdcClear(kernel, node_id);
  }
}

PackedIntegerExpression Packed(u32 expression, u32 mask,
                               u8 right_shift = 0) {
  return {expression, mask, right_shift};
}

bool MatchesExactNestedOuterStride(s32 actual, u32 outer_iterations,
                                   u32 child_iterations,
                                   u32 record_qwords) {
  if ((outer_iterations == 0u) != (child_iterations == 0u))
    return false;
  if (outer_iterations == 0u)
    return actual == 0;
  const u64 expected =
      static_cast<u64>(record_qwords) * child_iterations;
  if (expected > static_cast<u64>(std::numeric_limits<s32>::max()))
    return false;
  const s32 canonical = static_cast<s32>(expected);
  // A one-row grid has no observable outer-address transition: INDEX always
  // decodes to outer zero. The finite expression composer can therefore
  // canonicalize this unused coefficient to zero. A multi-row root must keep
  // the exact packed-record stride.
  return outer_iterations == 1u ? actual == 0 || actual == canonical
                                : actual == canonical;
}

} // namespace

bool BuildDirectTfxContract(const ParallelLoopKernel &kernel,
                            const void *gif_tag_qword,
                            DirectTfxContract *contract,
                            std::string *error) {
  if (!contract)
    return Fail(error, "null direct TFX contract output");
  *contract = {};
  if (!kernel.independent_store_values)
    return Fail(error, "direct TFX requires an independent loop slice");
  if (!gif_tag_qword)
    return Fail(error, "direct TFX requires a static GIF tag");

  GIFTag tag{};
  std::memcpy(&tag, gif_tag_qword, sizeof(tag));
  const u32 nreg = tag.NREG == 0 ? 16 : tag.NREG;
  if (tag.NLOOP == 0 || tag.FLG != GIF_FLG_PACKED)
    return Fail(error, "direct TFX first lowering requires a packed GIF tag");
  if (!tag.PRE)
    return Fail(error, "direct TFX first lowering requires static PRE/PRIM");
  if (nreg != kernel.stores.size())
    return Fail(error, "GIF register count does not match loop qword stores");

  std::vector<const LoopStore *> stores;
  stores.reserve(kernel.stores.size());
  for (const LoopStore &store : kernel.stores) {
    if (!FullStore(store) || !store.address.valid)
      return Fail(error, "direct TFX requires full affine qword stores");
    stores.push_back(&store);
  }
  const AffineQwordAddress& exemplar = stores.front()->address;
  if (exemplar.invocation_coefficient != static_cast<s32>(nreg) ||
      exemplar.invocation_coefficient <= 0) {
    return Fail(error, "GIF output stride differs from the loop store stride");
  }
  if (!MatchesExactNestedOuterStride(
          exemplar.outer_invocation_coefficient,
          kernel.outer_iteration_count, kernel.child_iteration_count,
          nreg)) {
    return Fail(error,
                "GIF output outer stride differs from the exact loop grid");
  }
  for (const LoopStore* store : stores) {
    if (store->address.base_vi != exemplar.base_vi ||
        store->address.invocation_coefficient !=
            exemplar.invocation_coefficient ||
        store->address.outer_invocation_coefficient !=
            exemplar.outer_invocation_coefficient) {
      return Fail(error, "GIF output qwords are not one affine vertex record");
    }
  }

  // VU memory wraps at 1,024 qwords. Find the unique cyclicly contiguous
  // record rather than sorting signed offsets, which misorders a legitimate
  // record spanning qwords 1023, 0, and 1.
  std::vector<const LoopStore*> ordered(stores.size(), nullptr);
  std::vector<const LoopStore*> selected_order;
  bool found_order = false;
  for (const LoopStore* candidate : stores) {
    std::fill(ordered.begin(), ordered.end(), nullptr);
    const u32 first_qword =
        static_cast<u16>(candidate->address.qword_offset) & 1023u;
    bool complete = true;
    for (u32 index = 0; index < stores.size(); index++) {
      const u32 expected = (first_qword + index) & 1023u;
      for (const LoopStore* store : stores) {
        const u32 actual =
            static_cast<u16>(store->address.qword_offset) & 1023u;
        if (actual != expected)
          continue;
        if (ordered[index] != nullptr) {
          complete = false;
          break;
        }
        ordered[index] = store;
      }
      complete &= ordered[index] != nullptr;
      if (!complete)
        break;
    }
    if (!complete)
      continue;
    if (found_order)
      return Fail(error, "GIF output qword order is ambiguous");
    selected_order = ordered;
    found_order = true;
  }
  if (!found_order)
    return Fail(error, "GIF output qwords are not one affine vertex record");
  stores = std::move(selected_order);

  const AffineQwordAddress& first = stores.front()->address;

  GIFRegPRIM prim{};
  prim.U32[0] = tag.PRIM;
  contract->vertex_count = tag.NLOOP;
  contract->primitive = prim.PRIM;
  contract->gouraud = prim.IIP;
  contract->textured = prim.TME;
  contract->fog_enabled = prim.FGE;
  contract->fixed_texture_coordinates = prim.FST;
  contract->output_base_vi = first.base_vi;
  contract->output_base_qword = first.qword_offset;
  contract->output_source_address = first;

  for (u32 i = 0; i < stores.size(); i++) {
    const LoopStore &store = *stores[i];
    const u32 reg = (tag.REGS >> (i * 4)) & 0x0f;
    switch (reg) {
    case GIF_REG_RGBA:
      for (u32 lane = 0; lane < 4; lane++)
        contract->color[lane] = Packed(store.values[lane], 0xffu);
      contract->has_color = true;
      break;
    case GIF_REG_STQ:
      contract->st[0] = store.values[0];
      contract->st[1] = store.values[1];
      contract->q = store.values[2];
      contract->has_stq = true;
      break;
    case GIF_REG_UV:
      contract->uv[0] = Packed(store.values[0], 0x3fffu);
      contract->uv[1] = Packed(store.values[1], 0x3fffu);
      contract->has_uv = true;
      break;
    case GIF_REG_XYZF2:
      if (i + 1 != stores.size())
        return Fail(error, "packed XYZF2 is not the vertex-record terminator");
      contract->position[0] = Packed(store.values[0], 0xffffu);
      contract->position[1] = Packed(store.values[1], 0xffffu);
      contract->depth = Packed(store.values[2], 0x00ffffffu, 4);
      contract->fog = Packed(store.values[3], 0xffu, 4);
      contract->adc_expression = store.values[3];
      contract->position_source_address = store.address;
      contract->position_record_qword = static_cast<u8>(i);
      contract->adc_always_clear =
          ProvesAdcClear(kernel, contract->adc_expression);
      contract->has_position = true;
      break;
    case GIF_REG_XYZ2:
      if (i + 1 != stores.size())
        return Fail(error, "packed XYZ2 is not the vertex-record terminator");
      contract->position[0] = Packed(store.values[0], 0xffffu);
      contract->position[1] = Packed(store.values[1], 0xffffu);
      contract->depth = Packed(store.values[2], 0xffffffffu);
      contract->adc_expression = store.values[3];
      contract->position_source_address = store.address;
      contract->position_record_qword = static_cast<u8>(i);
      contract->adc_always_clear =
          ProvesAdcClear(kernel, contract->adc_expression);
      contract->has_position = true;
      break;
    case GIF_REG_NOP:
      break;
    default:
      return Fail(error, "packed GIF register requires a GPU export lowering");
    }
  }

  if (!contract->has_position)
    return Fail(error, "direct TFX has no packed position output");
  if (!contract->adc_always_clear) {
    const u32 expression = contract->adc_expression;
    const ExpressionNode* node = expression < kernel.expressions.size()
                                     ? &kernel.expressions[expression]
                                     : nullptr;
    const ExpressionNode* input =
        node && node->operands[0] < kernel.expressions.size()
            ? &kernel.expressions[node->operands[0]]
            : nullptr;
    const u32 kind = node ? static_cast<u32>(node->kind)
                          : std::numeric_limits<u32>::max();
    return Fail(error,
                "direct TFX cannot prove an unconditional vertex kick "
                "(adc=" +
                    std::to_string(expression) + " kind=" +
                    std::to_string(kind) + " immediate=" +
                    std::to_string(node ? node->immediate : 0u) +
                    " operands=" +
                    std::to_string(node ? node->operands[0] : 0u) + "/" +
                    std::to_string(node ? node->operands[1] : 0u) + "/" +
                    std::to_string(node ? node->operands[2] : 0u) +
                    " input_kind=" +
                    std::to_string(input ? static_cast<u32>(input->kind)
                                         : std::numeric_limits<u32>::max()) +
                    " input_operands=" +
                    std::to_string(input ? input->operands[0] : 0u) + "/" +
                    std::to_string(input ? input->operands[1] : 0u) + "/" +
                    std::to_string(input ? input->operands[2] : 0u) + ")");
  }
  if (!contract->has_color)
    return Fail(error, "direct TFX first lowering requires per-vertex RGBA");
  if (contract->textured &&
      (contract->fixed_texture_coordinates ? !contract->has_uv
                                           : !contract->has_stq)) {
    return Fail(error, "direct TFX texture coordinate register is absent");
  }
  if (contract->fog_enabled && contract->fog.expression == InvalidNode)
    return Fail(error, "direct TFX fog value is absent");

  if (error)
    error->clear();
  return true;
}

bool BuildResolvedDirectTfxContract(
    const ParallelLoopKernel& kernel, const void* gif_tag_qword,
    const std::array<u16, 16>& child_entry_vi,
    DirectTfxContract* contract, std::string* error) {
  return BuildResolvedDirectTfxContract(
      kernel, gif_tag_qword, child_entry_vi, 0u, 0u, contract, error);
}

bool BuildResolvedDirectTfxContract(
    const ParallelLoopKernel& kernel, const void* gif_tag_qword,
    const std::array<u16, 16>& child_entry_vi, u16 vif_top, u16 vif_itop,
    DirectTfxContract* contract, std::string* error) {
  const auto resolve_base = [&](u8 base_vi, u16* result) {
    if (!result)
      return false;
    if (base_vi < child_entry_vi.size()) {
      *result = base_vi == 0u ? 0u : child_entry_vi[base_vi];
      return true;
    }
    return EvaluateAffineViRuntimeValue(
        {base_vi, 0, true}, child_entry_vi, vif_top, vif_itop, result);
  };
  ParallelLoopKernel resolved = kernel;
  for (LoopStore& store : resolved.stores) {
    u16 base = 0u;
    if (!store.address.valid ||
        !resolve_base(store.address.base_vi, &base)) {
      return Fail(error, "resolved GIF output has an invalid VI base");
    }
    store.address.qword_offset = static_cast<s32>(
        (base + static_cast<u16>(store.address.qword_offset)) & 1023u);
    store.address.base_vi = 0u;
  }
  DirectTfxContract built;
  if (!BuildDirectTfxContract(
          resolved, gif_tag_qword, &built, error)) {
    return false;
  }

  const u32 output_qword =
      static_cast<u16>(built.output_base_qword) & 1023u;
  const u32 position_qword =
      (output_qword + built.position_record_qword) & 1023u;
  const auto resolved_qword = [&resolve_base](
                                  const AffineQwordAddress& address,
                                  u32* qword) {
    u16 base = 0u;
    if (!qword || !resolve_base(address.base_vi, &base))
      return false;
    *qword = (base + static_cast<u16>(address.qword_offset)) & 1023u;
    return true;
  };
  bool output_found = false;
  bool position_found = false;
  for (const LoopStore& store : kernel.stores) {
    u32 qword = 0u;
    if (!resolved_qword(store.address, &qword))
      return Fail(error, "resolved GIF source has an invalid VI base");
    if (qword == output_qword) {
      if (output_found)
        return Fail(error, "resolved GIF output source is ambiguous");
      built.output_source_address = store.address;
      output_found = true;
    }
    if (qword == position_qword) {
      if (position_found)
        return Fail(error, "resolved GIF position source is ambiguous");
      built.position_source_address = store.address;
      position_found = true;
    }
  }
  if (!output_found || !position_found)
    return Fail(error, "resolved GIF source identity was lost");

  *contract = std::move(built);
  if (error)
    error->clear();
  return true;
}

bool ProveDirectTfxStoreReadDisjoint(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const std::array<u16, 16>& child_entry_vi, u16 vif_top, u16 vif_itop,
    u32 outer_iteration_count, u32 child_iteration_count,
    DirectTfxStoreReadDisjointProof* proof, std::string* error) {
  if (!proof)
    return Fail(error, "null direct-TFX store/read proof");

  DirectTfxStoreReadDisjointProof built;
  const u64 invocation_count =
      static_cast<u64>(outer_iteration_count) * child_iteration_count;
  const u64 store_qword_count = invocation_count * kernel.stores.size();
  if (outer_iteration_count == 0u || child_iteration_count == 0u ||
      outer_iteration_count > kernel.outer_iteration_count ||
      child_iteration_count != kernel.child_iteration_count ||
      invocation_count == 0u ||
      invocation_count != contract.vertex_count || kernel.stores.empty() ||
      !contract.has_position || store_qword_count > 1024u) {
    return Fail(error, "direct-TFX store/read domain is incomplete or wraps");
  }

  std::array<u8, 1024> store_lanes{};
  std::array<u8, 1024> expression_read_lanes{};
  std::array<u8, 1024> compact_read_lanes{};
  for (u32 outer = 0u; outer < outer_iteration_count; outer++) {
    for (u32 child = 0u; child < child_iteration_count; child++) {
      for (const LoopStore& store : kernel.stores) {
        u16 qword = 0u;
        const u8 lane_mask = store.write_mask & 0x0fu;
        if (lane_mask == 0u ||
            !EvaluateAffineQwordRuntimeAddress(
                store.address, child_entry_vi, vif_top, vif_itop, outer,
                child, &qword)) {
          return Fail(error, "direct-TFX output store address is invalid");
        }
        if ((store_lanes[qword] & lane_mask) != 0u) {
          return Fail(error,
                      "direct-TFX output stores alias across invocations");
        }
        store_lanes[qword] |= lane_mask;
      }
    }
  }

  std::vector<u8> reachable(kernel.expressions.size(), 0u);
  std::vector<u32> pending;
  pending.reserve(kernel.stores.size() * 4u);
  for (const LoopStore& store : kernel.stores) {
    for (u32 lane = 0u; lane < 4u; lane++) {
      if ((store.write_mask & (0x8u >> lane)) != 0u)
        pending.push_back(store.values[lane]);
    }
  }

  while (!pending.empty()) {
    const u32 node_id = pending.back();
    pending.pop_back();
    if (node_id == InvalidNode || node_id >= kernel.expressions.size()) {
      return Fail(error,
                  "direct-TFX output references an invalid expression");
    }
    if (reachable[node_id] != 0u)
      continue;
    reachable[node_id] = 1u;
    built.reachable_expression_count++;

    const ExpressionNode& node = kernel.expressions[node_id];
    for (u32 operand : node.operands) {
      if (operand != InvalidNode)
        pending.push_back(operand);
    }

    if (node.kind == ExpressionKind::Memory) {
      if (node.lane >= 4u)
        return Fail(error, "direct-TFX memory input lane is invalid");
      const u8 lane_mask = static_cast<u8>(0x8u >> node.lane);
      for (u32 outer = 0u; outer < outer_iteration_count; outer++) {
        for (u32 child = 0u; child < child_iteration_count; child++) {
          u16 qword = 0u;
          if (!EvaluateAffineQwordRuntimeAddress(
                  node.memory_address, child_entry_vi, vif_top, vif_itop,
                  outer, child, &qword)) {
            return Fail(error,
                        "direct-TFX expression memory address is invalid");
          }
          if ((store_lanes[qword] & lane_mask) != 0u) {
            return Fail(error,
                        "direct-TFX expression reads an output store lane");
          }
          expression_read_lanes[qword] |= lane_mask;
        }
      }
      continue;
    }

    if (node.kind != ExpressionKind::CompactOuterInput)
      continue;
    if (node.immediate >= kernel.compact_outer_inputs.size() ||
        node.lane >= 4u) {
      return Fail(error, "direct-TFX compact input metadata is invalid");
    }
    const CompactOuterInputTable& table =
        kernel.compact_outer_inputs[node.immediate];
    if (table.sources.size() < outer_iteration_count) {
      return Fail(error,
                  "direct-TFX compact input does not cover the outer domain");
    }
    const u8 lane_mask = static_cast<u8>(0x8u >> node.lane);
    for (u32 outer = 0u; outer < outer_iteration_count; outer++) {
      const CompactQwordSource& source = table.sources[outer];
      if (source.kind == CompactQwordSourceKind::InitialVf) {
        if (source.reg == 0u || source.reg >= 32u) {
          return Fail(error,
                      "direct-TFX compact VF input register is invalid");
        }
        continue;
      }
      if (source.kind != CompactQwordSourceKind::Memory) {
        return Fail(error, "direct-TFX compact input kind is invalid");
      }
      u16 qword = 0u;
      if (!EvaluateAffineQwordRuntimeAddress(
              source.memory_address, child_entry_vi, vif_top, vif_itop, 0u,
              0u, &qword)) {
        return Fail(error, "direct-TFX compact memory address is invalid");
      }
      if ((store_lanes[qword] & lane_mask) != 0u) {
        return Fail(error,
                    "direct-TFX compact input reads an output store lane");
      }
      compact_read_lanes[qword] |= lane_mask;
    }
  }

  const auto count_lanes = [](const std::array<u8, 1024>& lanes) {
    u32 count = 0u;
    for (u8 mask : lanes) {
      for (u32 lane = 0u; lane < 4u; lane++)
        count += (mask & (1u << lane)) != 0u;
    }
    return count;
  };
  built.invocation_count = static_cast<u32>(invocation_count);
  built.store_qword_count = static_cast<u32>(store_qword_count);
  built.store_lane_count = count_lanes(store_lanes);
  built.expression_memory_read_lane_count =
      count_lanes(expression_read_lanes);
  built.compact_memory_read_lane_count = count_lanes(compact_read_lanes);
  built.complete = true;
  *proof = built;
  if (error)
    error->clear();
  return true;
}

namespace {

using LowerKind = VUInterpFast::LowerFastKind;

AffineViValue AddViOffset(AffineViValue value, s32 offset) {
  if (!value.valid)
    return value;
  const s64 sum = static_cast<s64>(value.offset) + offset;
  if (sum < std::numeric_limits<s32>::min() ||
      sum > std::numeric_limits<s32>::max()) {
    value.valid = false;
    return value;
  }
  value.offset = static_cast<s32>(sum);
  return value;
}

AffineViValue BinaryViValue(const AffineViValue& left,
                            const AffineViValue& right,
                            LowerKind kind) {
  if (!left.valid || !right.valid)
    return {};
  const bool left_constant = left.base_vi == 0u;
  const bool right_constant = right.base_vi == 0u;
  switch (kind) {
  case LowerKind::IADD:
    if (left_constant)
      return {right.base_vi, left.offset + right.offset, true};
    if (right_constant)
      return {left.base_vi, left.offset + right.offset, true};
    break;
  case LowerKind::ISUB:
    if (right_constant)
      return {left.base_vi, left.offset - right.offset, true};
    break;
  case LowerKind::IAND:
    if (left_constant && right_constant) {
      return {0u, static_cast<u16>(left.offset) &
                       static_cast<u16>(right.offset), true};
    }
    break;
  case LowerKind::IOR:
    if (left_constant && right_constant) {
      return {0u, static_cast<u16>(left.offset) |
                       static_cast<u16>(right.offset), true};
    }
    break;
  default:
    break;
  }
  return {};
}

bool TransferTailVi(const VitaVU::GpuPairPlan& plan,
                    std::array<AffineViValue, 16>* state,
                    u16* written_mask, std::string* error) {
  if (!state)
    return Fail(error, "null post-loop VI state");
  if (plan.mflag || plan.dflag || plan.tflag)
    return Fail(error, "post-loop contains an architectural M/D/T observer");
  if (plan.exec_upper &&
      (plan.upper_kind != 0u || (plan.upper_vi_write & 0xfffeu) != 0u ||
       plan.upper_vf_write != 0u)) {
    return Fail(error, "post-loop contains a non-NOP upper operation");
  }
  if (!plan.exec_lower || plan.lower_discarded_by_upper)
    return true;

  const u16 writes = static_cast<u16>(plan.lower_vi_write & 0xfffeu);
  if (written_mask)
    *written_mask |= writes;
  if (writes == 0u)
    return true;
  if (plan.vi_backup_write)
    return Fail(error, "post-loop VI write retains a backup window");

  const std::array<AffineViValue, 16> old = *state;
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
      (*state)[it] = AddViOffset(old[is], VUInterpFast::Imm15(code));
    break;
  case LowerKind::ISUBIU:
    if (it != 0u)
      (*state)[it] = AddViOffset(old[is], -VUInterpFast::Imm15(code));
    break;
  case LowerKind::IADDI:
    if (it != 0u)
      (*state)[it] = AddViOffset(old[is], VUInterpFast::Imm5(code));
    break;
  case LowerKind::IADD:
  case LowerKind::ISUB:
  case LowerKind::IAND:
  case LowerKind::IOR:
    if (id != 0u)
      (*state)[id] = BinaryViValue(old[is], old[it], kind);
    break;
  case LowerKind::LQI:
    if (is != 0u)
      (*state)[is] = AddViOffset(old[is], 1);
    break;
  case LowerKind::LQD:
    if (is != 0u)
      (*state)[is] = AddViOffset(old[is], -1);
    break;
  case LowerKind::SQI:
    if (it != 0u)
      (*state)[it] = AddViOffset(old[it], 1);
    break;
  case LowerKind::SQD:
    if (it != 0u)
      (*state)[it] = AddViOffset(old[it], -1);
    break;
  default:
    break;
  }
  for (u32 reg = 1; reg < state->size(); reg++) {
    if ((writes & (1u << reg)) != 0u && !(*state)[reg].valid)
      return Fail(error, "post-loop VI write is not affine");
  }
  return true;
}

bool IsTailControlKind(LowerKind kind) {
  switch (kind) {
  case LowerKind::None:
  case LowerKind::IADDIU:
  case LowerKind::ISUBIU:
  case LowerKind::IADDI:
  case LowerKind::IADD:
  case LowerKind::ISUB:
  case LowerKind::IAND:
  case LowerKind::IOR:
  case LowerKind::IBGTZ:
  case LowerKind::ISW:
  case LowerKind::XGKICK:
    return true;
  default:
    return false;
  }
}

bool AppendEquality(const AffineViValue& left, const AffineViValue& right,
                    bool qword_wrapped, DirectTfxPostLoopProof* proof,
                    std::string* error) {
  if (!left.valid || !right.valid)
    return Fail(error, "post-loop runtime equality is not affine");
  if (left.base_vi == right.base_vi) {
    const u32 mask = qword_wrapped ? 1023u : 0xffffu;
    if ((static_cast<u32>(left.offset - right.offset) & mask) != 0u)
      return Fail(error, "post-loop static VI equality is false");
    return true;
  }
  proof->runtime_vi_equalities.push_back({left, right, qword_wrapped});
  return true;
}

} // namespace

bool BuildDirectTfxStripIndices(u32 primitive, std::span<const u8> adc,
                               std::vector<u16>* indices, std::string* error) {
  const u32 width = primitive == GS_LINESTRIP ? 2u :
                    primitive == GS_TRIANGLESTRIP ? 3u : 0u;
  if (!indices || width == 0u || adc.size() > (1u << 16u) ||
      std::any_of(adc.begin(), adc.end(), [](u8 value) { return value > 1u; })) {
    return Fail(error, "direct-TFX strip topology input is invalid");
  }

  std::vector<u16> built;
  if (adc.size() >= width) {
    built.reserve((adc.size() - width + 1u) * width);
    // PCSX2 GSState::VertexKick<GS_LINESTRIP/GS_TRIANGLESTRIP>: every kick
    // queues a vertex, and every complete primitive advances head by one,
    // whether ADC suppresses its indices or not. The GSState buffer may
    // compact gaps; these indices retain the original source vertex identity.
    for (u32 last = width - 1u; last < adc.size(); last++) {
      if (adc[last] != 0u)
        continue;
      for (u32 lane = 0u; lane < width; lane++)
        built.push_back(static_cast<u16>(last - (width - 1u) + lane));
    }
  }
  *indices = std::move(built);
  if (error)
    error->clear();
  return true;
}

bool BuildDirectTfxFlatVertexMap(u32 primitive, u32 vertex_count,
                                std::span<const u16> exact_indices,
                                std::vector<DirectTfxFlatVertex>* vertices,
                                std::string* error) {
  const u32 width = primitive == GS_LINESTRIP ? 2u :
                    primitive == GS_TRIANGLESTRIP ? 3u : 0u;
  if (!vertices || width == 0u || vertex_count > (1u << 16u) ||
      (exact_indices.size() % width) != 0u ||
      std::any_of(exact_indices.begin(), exact_indices.end(),
                  [vertex_count](u16 index) { return index >= vertex_count; })) {
    return Fail(error, "direct-TFX flat vertex map input is invalid");
  }
  std::vector<DirectTfxFlatVertex> built;
  built.reserve(exact_indices.size());
  for (size_t first = 0u; first < exact_indices.size(); first += width) {
    const u16 provoking = exact_indices[first + width - 1u];
    for (u32 lane = 0u; lane < width; lane++)
      built.push_back({exact_indices[first + lane], provoking});
  }
  *vertices = std::move(built);
  if (error)
    error->clear();
  return true;
}

bool BuildDirectTfxFlatLineIndices(u32 vertex_count,
                                 std::span<const u16> exact_indices,
                                 std::vector<u16>* expanded_indices,
                                 std::string* error) {
  const u32 domain = DirectTfxFlatLineIndexDomain(vertex_count);
  if (!expanded_indices || domain == 0u || (exact_indices.size() & 1u) != 0u)
    return Fail(error, "flat line index domain is invalid");
  // PCSX2 GSState::VertexKick<GS_LINESTRIP> emits each surviving consecutive
  // pair in guest order. The generated endpoint INDEX is 2*head + lane, and
  // both endpoints select head+1 for RGBA (the GS last/provoking vertex).
  for (size_t first = 0u; first < exact_indices.size(); first += 2u) {
    const u32 head = exact_indices[first];
    if (head + 1u >= vertex_count || exact_indices[first + 1u] != head + 1u ||
        (first != 0u && head <= exact_indices[first - 2u])) {
      return Fail(error, "flat line source pairs are not ordered strip edges");
    }
  }
  std::vector<u16> built;
  built.reserve(exact_indices.size());
  for (size_t first = 0u; first < exact_indices.size(); first += 2u) {
    const u32 endpoint = 2u * exact_indices[first];
    built.push_back(static_cast<u16>(endpoint));
    built.push_back(static_cast<u16>(endpoint + 1u));
  }
  *expanded_indices = std::move(built);
  if (error)
    error->clear();
  return true;
}

bool BuildClosedFormDirectTfxPostLoopProof(
    const ProgramAnalysis& program, const ParallelLoopKernel& kernel,
    const ClosedFormNestedLoopProof& closed_form,
    const DirectTfxContract& contract, DirectTfxPostLoopProof* proof,
    std::string* error) {
  if (!proof)
    return Fail(error, "null direct-TFX post-loop proof");
  DirectTfxPostLoopProof built;
  if (!program.complete_cfg || program.resume_pcs.size() != 1u ||
      closed_form.child_loop >= program.natural_loops.size() ||
      closed_form.parent_loop >= program.natural_loops.size() ||
      closed_form.tail.parent_exit_block >= program.blocks.size() ||
      kernel.outer_iteration_count != closed_form.outer_iteration_count ||
      kernel.child_iteration_count != closed_form.child_iteration_count ||
      contract.vertex_count == 0u ||
      (contract.primitive != GS_TRIANGLESTRIP &&
       contract.primitive != GS_LINESTRIP) || !contract.adc_always_clear ||
      !contract.output_source_address.valid ||
      !contract.position_source_address.valid) {
    return Fail(error, "direct-TFX post-loop input contract is incomplete");
  }

  const NaturalLoop& child =
      program.natural_loops[closed_form.child_loop];
  const NaturalLoop& parent =
      program.natural_loops[closed_form.parent_loop];
  if (parent.child_loops.size() != 1u ||
      parent.child_loops.front() != closed_form.child_loop ||
      parent.pair_count < child.pair_count +
                              closed_form.tail.summarized_pair_count) {
    return Fail(error, "post-loop pair ownership is not one nested loop");
  }

  const u32 no_entry_loop = std::numeric_limits<u32>::max();
  if (closed_form.summarized_entry_loop == no_entry_loop) {
    if (closed_form.summarized_entry_loop_iterations != 0u ||
        closed_form.entry_loop_trip_count_requires_runtime_attestation) {
      return Fail(error, "post-loop entry-loop proof is internally inconsistent");
    }
  } else {
    if (closed_form.summarized_entry_loop >= program.natural_loops.size() ||
        closed_form.summarized_entry_loop == closed_form.child_loop ||
        closed_form.summarized_entry_loop == closed_form.parent_loop ||
        closed_form.summarized_entry_loop_iterations == 0u ||
        !closed_form.entry_loop_trip_count_requires_runtime_attestation) {
      return Fail(error, "post-loop summarized entry loop is invalid");
    }
    const NaturalLoop& entry_loop =
        program.natural_loops[closed_form.summarized_entry_loop];
    const u64 summarized_pairs =
        static_cast<u64>(entry_loop.pair_count) *
        closed_form.summarized_entry_loop_iterations;
    if (entry_loop.parent_loop < program.natural_loops.size() ||
        summarized_pairs == 0u ||
        summarized_pairs > closed_form.entry_prefix_pair_count) {
      return Fail(error, "post-loop summarized entry pair count is invalid");
    }
  }

  std::array<AffineViValue, 16> state =
      closed_form.final_parent_vi_values;
  state[0] = {0u, 0, true};
  for (u32 reg = 1; reg < state.size(); reg++) {
    if (!state[reg].valid)
      return Fail(error, "post-loop parent VI state is incomplete");
  }
  built.final_vi_write_mask = closed_form.final_parent_vi_mask;

  const auto find_loop_at_header = [&](u32 block) {
    for (u32 loop = 0; loop < program.natural_loops.size(); loop++) {
      if (loop != closed_form.child_loop && loop != closed_form.parent_loop &&
          program.natural_loops[loop].header_block == block) {
        return loop;
      }
    }
    return std::numeric_limits<u32>::max();
  };
  const auto unique_target = [&](const BasicBlock& block,
                                 u32 excluded_target,
                                 u32* target) {
    u32 count = 0u;
    u32 selected = 0u;
    for (const ControlEdge& edge : block.successors) {
      if (!edge.has_target || edge.target_block == excluded_target)
        continue;
      selected = edge.target_block;
      count++;
    }
    if (count != 1u)
      return false;
    *target = selected;
    return true;
  };
  const auto transfer_block = [&](const BasicBlock& block,
                                  bool allow_isw, bool allow_xgkick,
                                  std::vector<AffineViValue>* adc_addresses,
                                  AffineViValue* xgkick_address) {
    for (const ProgramPair& pair : block.pairs) {
      const VitaVU::GpuPairPlan& plan = pair.plan;
      const LowerKind kind = static_cast<LowerKind>(plan.lower_kind);
      if (!IsTailControlKind(kind))
        return Fail(error, "post-loop contains an unsupported operation");
      if (plan.exec_lower && !plan.lower_discarded_by_upper &&
          kind == LowerKind::ISW) {
        if (!allow_isw || !adc_addresses || VUInterpFast::XYZW(plan.lower) != 1u) {
          return Fail(error, "post-loop memory write is not an ADC W patch");
        }
        const u32 source = VUInterpFast::It(plan.lower);
        const u32 base = VUInterpFast::Is(plan.lower);
        if (!state[source].valid || state[source].base_vi != 0u ||
            static_cast<u16>(state[source].offset) != 0x8000u) {
          return Fail(error, "post-loop ADC patch value is not 0x8000");
        }
        const AffineViValue address = AddViOffset(
            state[base], VUInterpFast::Imm11(plan.lower));
        if (!address.valid)
          return Fail(error, "post-loop ADC address is not affine");
        adc_addresses->push_back(address);
      }
      if (plan.exec_lower && !plan.lower_discarded_by_upper &&
          kind == LowerKind::XGKICK) {
        if (!allow_xgkick || !xgkick_address || xgkick_address->valid)
          return Fail(error, "post-loop has an unexpected XGKICK");
        *xgkick_address = state[VUInterpFast::Is(plan.lower)];
      }
      if (!TransferTailVi(plan, &state, &built.final_vi_write_mask, error))
        return false;
    }
    built.post_loop_pair_count += static_cast<u32>(block.pairs.size());
    return true;
  };

  u32 block_index = closed_form.tail.parent_exit_block;
  u32 tail_loop_index = find_loop_at_header(block_index);
  std::set<u32> prelude_blocks;
  while (tail_loop_index == std::numeric_limits<u32>::max()) {
    if (block_index >= program.blocks.size() ||
        !prelude_blocks.insert(block_index).second) {
      return Fail(error, "post-loop prelude is cyclic");
    }
    const BasicBlock& block = program.blocks[block_index];
    if (block.has_branch || block.ends_program ||
        !transfer_block(block, false, false, nullptr, nullptr)) {
      return false;
    }
    if (!unique_target(block, std::numeric_limits<u32>::max(), &block_index))
      return Fail(error, "post-loop prelude has no unique successor");
    tail_loop_index = find_loop_at_header(block_index);
  }

  const NaturalLoop& tail_loop = program.natural_loops[tail_loop_index];
  if (!tail_loop.single_entry || !tail_loop.affine_counter ||
      !tail_loop.branch_taken_repeats || tail_loop.counter_reg == 0u ||
      tail_loop.counter_limit_reg != 0u || tail_loop.counter_step != -1 ||
      static_cast<LowerKind>(tail_loop.branch_kind) != LowerKind::IBGTZ ||
      tail_loop.blocks.size() != 1u ||
      tail_loop.header_block != tail_loop.latch_block ||
      tail_loop.header_block != block_index) {
    return Fail(error, "post-loop is not one canonical count-to-zero body");
  }
  if (!AppendEquality(
          state[tail_loop.counter_reg],
          {0u, static_cast<s32>(closed_form.outer_iteration_count), true},
          false, &built, error)) {
    return false;
  }

  std::vector<AffineViValue> adc_addresses;
  for (u32 outer = 0; outer < closed_form.outer_iteration_count; outer++) {
    if (!transfer_block(program.blocks[tail_loop.header_block], true, false,
                        &adc_addresses, nullptr)) {
      return false;
    }
  }
  if (adc_addresses.empty())
    return Fail(error, "post-loop contains no ADC patches");
  built.adc_patch_addresses = adc_addresses;

  const u32 record_qwords = static_cast<u32>(kernel.stores.size());
  if (record_qwords == 0u ||
      contract.position_source_address.invocation_coefficient !=
          static_cast<s32>(record_qwords) ||
      !MatchesExactNestedOuterStride(
          contract.position_source_address.outer_invocation_coefficient,
          closed_form.outer_iteration_count,
          closed_form.child_iteration_count, record_qwords)) {
    return Fail(error, "post-loop XYZ record stride differs from the kernel");
  }
  const AffineViValue position_start{
      contract.position_source_address.base_vi,
      contract.position_source_address.qword_offset, true};
  if (!AppendEquality(adc_addresses.front(), position_start, true, &built,
                      error)) {
    return false;
  }

  std::vector<u8> adc_vertices(contract.vertex_count, 0u);
  const AffineViValue adc_base = adc_addresses.front();
  for (const AffineViValue& address : adc_addresses) {
    if (!address.valid || address.base_vi != adc_base.base_vi) {
      return Fail(error, "post-loop ADC addresses do not share one base");
    }
    const s64 delta = static_cast<s64>(address.offset) - adc_base.offset;
    if (delta < 0 || (delta % record_qwords) != 0) {
      return Fail(error, "post-loop ADC address is not a vertex boundary");
    }
    const u64 vertex = static_cast<u64>(delta / record_qwords);
    if (vertex >= contract.vertex_count)
      return Fail(error, "post-loop ADC address exceeds the GIF vertex count");
    if (!adc_vertices[vertex]) {
      adc_vertices[vertex] = 1u;
      built.adc_vertex_count++;
    }
  }

  built.indices_per_primitive = contract.primitive == GS_LINESTRIP ? 2u : 3u;
  if (!BuildDirectTfxStripIndices(contract.primitive, adc_vertices,
                                  &built.exact_indices, error))
    return false;
  if (built.exact_indices.empty())
    return Fail(error, "post-loop ADC topology emits no complete primitive");
  built.primitive_count = static_cast<u32>(
      built.exact_indices.size() / built.indices_per_primitive);
  if (!contract.gouraud &&
      !BuildDirectTfxFlatVertexMap(contract.primitive, contract.vertex_count,
                                   built.exact_indices, &built.flat_vertices,
                                   error)) {
    return false;
  }
  if (!contract.gouraud && contract.primitive == GS_LINESTRIP &&
      !BuildDirectTfxFlatLineIndices(contract.vertex_count,
          built.exact_indices, &built.expanded_line_indices, error)) {
    return false;
  }

  u32 terminal_block = 0u;
  if (!unique_target(program.blocks[tail_loop.latch_block],
                     tail_loop.header_block, &terminal_block)) {
    return Fail(error, "post-loop latch has no unique terminal successor");
  }
  AffineViValue xgkick_address;
  std::set<u32> terminal_blocks;
  while (true) {
    if (terminal_block >= program.blocks.size() ||
        !terminal_blocks.insert(terminal_block).second) {
      return Fail(error, "post-loop terminal path is cyclic");
    }
    const BasicBlock& block = program.blocks[terminal_block];
    if (block.has_branch ||
        !transfer_block(block, false, true, nullptr, &xgkick_address)) {
      return false;
    }
    if (block.ends_program) {
      if (!block.has_resume_pc || block.resume_pc != program.resume_pcs.front())
        return Fail(error, "post-loop E-bit resume PC is not unique");
      built.unique_resume_pc = block.resume_pc;
      break;
    }
    if (!unique_target(block, std::numeric_limits<u32>::max(),
                       &terminal_block)) {
      return Fail(error, "post-loop terminal path has no unique successor");
    }
  }
  if (!xgkick_address.valid)
    return Fail(error, "post-loop terminal path lacks XGKICK");
  const AffineViValue output_start{
      contract.output_source_address.base_vi,
      contract.output_source_address.qword_offset, true};
  if (!AppendEquality(AddViOffset(xgkick_address, 1), output_start, true,
                      &built, error)) {
    return false;
  }

  for (u32 reg = 1; reg < state.size(); reg++) {
    if (!state[reg].valid)
      return Fail(error, "post-loop final VI state is unresolved");
    built.final_vi_values[reg] = state[reg];
  }
  const u32 prefix_pairs = parent.pair_count - child.pair_count -
                           closed_form.tail.summarized_pair_count;
  const u64 parent_pairs =
      static_cast<u64>(closed_form.outer_iteration_count) *
      (prefix_pairs +
       static_cast<u64>(closed_form.child_iteration_count) * child.pair_count +
       closed_form.tail.summarized_pair_count);
  const u64 exact_pairs = closed_form.entry_prefix_pair_count + parent_pairs +
                          built.post_loop_pair_count;
  if (exact_pairs == 0u || exact_pairs > std::numeric_limits<u32>::max())
    return Fail(error, "post-loop exact pair count overflowed");
  built.exact_pair_count = static_cast<u32>(exact_pairs);
  built.terminal_xgkick_proven = true;
  built.exact_adc_topology_proven = true;

  *proof = std::move(built);
  if (error)
    error->clear();
  return true;
}

bool EvaluateDirectTfxPostLoopProof(
    const DirectTfxPostLoopProof& proof,
    const std::array<u16, 16>& initial_vi,
    std::array<u16, 16>* final_vi, std::string* error) {
  return EvaluateDirectTfxPostLoopProof(
      proof, initial_vi, 0u, 0u, final_vi, error);
}

bool EvaluateDirectTfxPostLoopProof(
    const DirectTfxPostLoopProof& proof,
    const std::array<u16, 16>& initial_vi, u16 vif_top, u16 vif_itop,
    std::array<u16, 16>* final_vi, std::string* error) {
  if (!final_vi || !proof.exact_adc_topology_proven ||
      !proof.terminal_xgkick_proven || proof.exact_indices.empty() ||
      proof.adc_patch_addresses.empty() ||
      (proof.indices_per_primitive != 2u && proof.indices_per_primitive != 3u) ||
      (proof.exact_indices.size() % proof.indices_per_primitive) != 0u ||
      proof.primitive_count !=
          proof.exact_indices.size() / proof.indices_per_primitive ||
      proof.exact_pair_count < proof.post_loop_pair_count ||
      proof.unique_resume_pc > 0x4000u ||
      (proof.unique_resume_pc & 7u) != 0u) {
    return Fail(error, "direct-TFX post-loop proof is incomplete");
  }
  const auto evaluate = [&initial_vi, vif_top, vif_itop](
                            const AffineViValue& value, u16* result) {
    return EvaluateAffineViRuntimeValue(
        value, initial_vi, vif_top, vif_itop, result);
  };
  for (const DirectTfxViEquality& equality : proof.runtime_vi_equalities) {
    u16 left = 0u;
    u16 right = 0u;
    if (!evaluate(equality.left, &left) ||
        !evaluate(equality.right, &right) ||
        (equality.qword_wrapped ? ((left ^ right) & 1023u) != 0u
                                : left != right)) {
      return Fail(error, "direct-TFX post-loop runtime VI equality failed");
    }
  }

  *final_vi = initial_vi;
  for (u32 reg = 1; reg < final_vi->size(); reg++) {
    if ((proof.final_vi_write_mask & (1u << reg)) == 0u)
      continue;
    if (!evaluate(proof.final_vi_values[reg], &(*final_vi)[reg]))
      return Fail(error, "direct-TFX post-loop final VI formula failed");
  }
  (*final_vi)[0] = 0u;
  if (error)
    error->clear();
  return true;
}

} // namespace VitaGpuVu
