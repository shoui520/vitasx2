// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuGifContract.h"

#include "GS/GSRegs.h"

#include <algorithm>
#include <cstring>
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
  if (node.kind != ExpressionKind::Add)
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
  if (product.kind != ExpressionKind::Multiply)
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
  std::sort(stores.begin(), stores.end(),
            [](const LoopStore *left, const LoopStore *right) {
              return left->address.qword_offset < right->address.qword_offset;
            });

  const AffineQwordAddress &first = stores.front()->address;
  if (first.invocation_coefficient != static_cast<s32>(nreg) ||
      first.invocation_coefficient <= 0) {
    return Fail(error, "GIF output stride differs from the loop store stride");
  }
  for (u32 i = 0; i < stores.size(); i++) {
    const AffineQwordAddress &address = stores[i]->address;
    if (address.base_vi != first.base_vi ||
        address.invocation_coefficient != first.invocation_coefficient ||
        address.qword_offset != first.qword_offset + static_cast<s32>(i)) {
      return Fail(error, "GIF output qwords are not one affine vertex record");
    }
  }

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

  if (!contract->has_position || !contract->adc_always_clear)
    return Fail(error, "direct TFX cannot prove an unconditional vertex kick");
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

} // namespace VitaGpuVu
