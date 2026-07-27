// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuCgGenerator.h"

#include "GS/GSRegs.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace VitaGpuVu {
namespace {

constexpr u32 InvalidNode = 0;
constexpr u32 MaximumMemoryInputs = 16;
constexpr u32 MaximumConstantInputs = 32;
constexpr u32 MaximumVertexAttributes = 16;
constexpr u32 MaximumValidationStores = 8;
constexpr size_t MaximumGeneratedSourceBytes = 512 * 1024;

bool Fail(std::string *error, std::string message) {
  if (error)
    *error = std::move(message);
  return false;
}

const char *LaneName(u32 lane) {
  constexpr std::array<const char *, 4> names = {"x", "y", "z", "w"};
  return lane < names.size() ? names[lane] : "x";
}

const char *ScalarType(ScalarDomain domain) {
  switch (domain) {
  case ScalarDomain::Float:
    return "float";
  case ScalarDomain::UnsignedInt:
    return "unsigned int";
  case ScalarDomain::Raw:
  case ScalarDomain::SignedInt:
    return "int";
  }
  return "int";
}

using MemoryKey = std::tuple<u8, s32, s32, bool>;

MemoryKey MakeMemoryKey(const AffineQwordAddress &address) {
  return {address.base_vi, address.invocation_coefficient,
          address.qword_offset, address.valid};
}

class CgEmitter {
public:
  CgEmitter(const ParallelLoopKernel &kernel,
            const DirectTfxContract *direct_contract,
            GeneratedCgProgram *program)
      : m_kernel(kernel), m_direct_contract(direct_contract),
        m_program(program) {}

  bool Generate(std::string *error) {
    if (!m_kernel.independent_store_values)
      return Fail(error, "parallel Cg root requires an independent loop slice");
    if (m_kernel.stores.empty())
      return Fail(error, "parallel Cg root has no VU stores");
    if (!m_direct_contract &&
        m_kernel.stores.size() > MaximumValidationStores) {
      return Fail(error, "parallel validation root exceeds varying capacity");
    }

    m_reachable.resize(m_kernel.expressions.size());
    m_flat_color_reachable.resize(m_kernel.expressions.size());
    if (m_direct_contract) {
      if (!ConfigureDirectInputLowering(error))
        return false;
      if (!MarkDirectRoots(error))
        return false;
      m_program->uses_tfx_uniforms = true;
      m_program->uses_tfx_point_size =
          m_direct_contract->primitive == GS_POINTLIST;
      m_program->uses_tfx_uv_no_fog_interface =
          m_direct_contract->textured &&
          m_direct_contract->fixed_texture_coordinates &&
          !m_direct_contract->fog_enabled;
      m_program->uses_gif_q_uniform =
          m_direct_contract->textured &&
          m_direct_contract->fixed_texture_coordinates &&
          !m_program->uses_tfx_uv_no_fog_interface;
    } else {
      for (const LoopStore &store : m_kernel.stores) {
        for (u32 lane = 0; lane < 4; lane++) {
          if ((store.write_mask & (0x8u >> lane)) != 0 &&
              !MarkReachable(store.values[lane], error)) {
            return false;
          }
        }
      }
    }

    CollectResources();
    if (m_program->uses_buffered_batch_inputs &&
        m_program->memory_inputs.empty()) {
      return Fail(error,
                  "parallel buffered Cg root has no raw VIF inputs");
    }
    if (m_program->memory_inputs.size() > MaximumMemoryInputs)
      return Fail(error, "parallel Cg root exceeds GXM vertex input capacity");
    if (m_program->constant_inputs.size() > MaximumConstantInputs)
      return Fail(error, "parallel Cg root exceeds constant-input capacity");
    u32 attribute_count = 0;
    for (const CgMemoryInput &input : m_program->memory_inputs) {
      if (!m_program->uses_flat_instance_inputs) {
        attribute_count++;
        continue;
      }
      for (u32 vertex = 0;
           vertex < m_program->flat_vertices_per_primitive; vertex++) {
        if ((input.flat_attribute_vertex_mask & (1u << vertex)) != 0)
          attribute_count++;
      }
    }
    if (!m_program->uses_buffered_batch_inputs &&
        attribute_count > MaximumVertexAttributes) {
      return Fail(error,
                  "parallel flat Cg root exceeds GXM attribute capacity");
    }

    AppendBindingContract();
    AppendHelpers();
    AppendEntrySignature();
    AppendExpressions(error);
    if (error && !error->empty())
      return false;
    AppendOutputs();
    m_source += "}\n";

    if (m_source.size() > MaximumGeneratedSourceBytes)
      return Fail(error, "generated parallel Cg source exceeds compiler bound");
    m_program->source = std::move(m_source);
    if (error)
      error->clear();
    return true;
  }

private:
  bool ConfigureDirectInputLowering(std::string *error) {
    if (!m_direct_contract || m_direct_contract->gouraud ||
        m_direct_contract->primitive == GS_POINTLIST) {
      return true;
    }

    switch (m_direct_contract->primitive) {
    case GS_LINELIST:
      if ((m_direct_contract->vertex_count & 1u) != 0)
        return Fail(error, "flat line list has an odd vertex count");
      m_program->flat_vertices_per_primitive = 2;
      m_program->flat_instance_vertex_step = 2;
      m_program->batch_primitives_per_draw =
          static_cast<u16>(m_direct_contract->vertex_count / 2);
      break;
    case GS_LINESTRIP:
      if (m_direct_contract->vertex_count < 2)
        return Fail(error, "flat line strip is too short");
      m_program->flat_vertices_per_primitive = 2;
      m_program->flat_instance_vertex_step = 1;
      m_program->batch_primitives_per_draw =
          static_cast<u16>(m_direct_contract->vertex_count - 1);
      break;
    case GS_TRIANGLELIST:
      if ((m_direct_contract->vertex_count % 3u) != 0)
        return Fail(error, "flat triangle list has a partial primitive");
      m_program->flat_vertices_per_primitive = 3;
      m_program->flat_instance_vertex_step = 3;
      m_program->batch_primitives_per_draw =
          static_cast<u16>(m_direct_contract->vertex_count / 3);
      break;
    case GS_TRIANGLESTRIP:
      if (m_direct_contract->vertex_count < 3)
        return Fail(error, "flat triangle strip is too short");
      m_program->flat_vertices_per_primitive = 3;
      m_program->flat_instance_vertex_step = 1;
      m_program->batch_primitives_per_draw =
          static_cast<u16>(m_direct_contract->vertex_count - 2);
      m_program->flat_strip_winding = true;
      break;
    default:
      return Fail(error,
                  "flat primitive requires a GPU export output lowering");
    }
    if (m_program->batch_primitives_per_draw == 0)
      return Fail(error, "flat primitive batch is empty");
    m_program->uses_flat_instance_inputs = true;
    // Sony's skinning sample establishes dynamically indexed vertex uniform
    // buffers as the native way to read mapped arrays. One descriptor record
    // per VU dispatch lets a single instance range cross immutable VIF payload
    // boundaries without copying or CPU-unpacking any vertex.
    m_program->uses_buffered_batch_inputs = true;
    return true;
  }

  bool MarkPacked(const PackedIntegerExpression &value,
                  std::vector<bool> *reachable, std::string *error) {
    return value.expression != InvalidNode &&
           MarkReachable(value.expression, reachable, error);
  }

  u8 FullFlatVertexMask() const {
    return static_cast<u8>(
        (1u << m_program->flat_vertices_per_primitive) - 1u);
  }

  bool MarkDirectRoots(std::string *error) {
    if (!m_direct_contract->has_color ||
        !m_direct_contract->has_position ||
        !m_direct_contract->adc_always_clear) {
      return Fail(error, "direct TFX expression contract is incomplete");
    }
    std::vector<bool> *const color_reachable =
        m_program->uses_flat_instance_inputs ? &m_flat_color_reachable
                                             : &m_reachable;
    for (const PackedIntegerExpression &color : m_direct_contract->color) {
      if (!MarkPacked(color, color_reachable, error))
        return false;
    }
    for (const PackedIntegerExpression &position :
         m_direct_contract->position) {
      if (!MarkPacked(position, &m_reachable, error))
        return false;
    }
    if (!MarkPacked(m_direct_contract->depth, &m_reachable, error))
      return false;

    if (m_direct_contract->textured) {
      if (m_direct_contract->fixed_texture_coordinates) {
        for (const PackedIntegerExpression &uv : m_direct_contract->uv) {
          if (!MarkPacked(uv, &m_reachable, error))
            return false;
        }
      } else {
        if (!MarkReachable(m_direct_contract->st[0], &m_reachable, error) ||
            !MarkReachable(m_direct_contract->st[1], &m_reachable, error) ||
            !MarkReachable(m_direct_contract->q, &m_reachable, error)) {
          return false;
        }
      }
    }
    return !m_direct_contract->fog_enabled ||
           MarkPacked(m_direct_contract->fog, &m_reachable, error);
  }

  bool MarkReachable(u32 node_id, std::string *error) {
    return MarkReachable(node_id, &m_reachable, error);
  }

  bool MarkReachable(u32 node_id, std::vector<bool> *reachable,
                     std::string *error) {
    if (node_id == InvalidNode || node_id >= m_kernel.expressions.size())
      return Fail(error, "parallel Cg root references an invalid expression");
    if ((*reachable)[node_id])
      return true;
    (*reachable)[node_id] = true;
    const ExpressionNode &node = m_kernel.expressions[node_id];
    for (u32 operand : node.operands) {
      if (operand != InvalidNode &&
          !MarkReachable(operand, reachable, error)) {
        return false;
      }
    }
    return true;
  }

  // A stable lane which the region nonetheless clamps must be re-verified
  // against its runtime seed by the descriptor path, so carry the bound into
  // the generated program's contract.
  void RecordClampStableLane(u8 reg, u8 lane) {
    for (const ClampStableLane &clamp : m_kernel.clamp_stable_lanes) {
      if (clamp.reg != reg || clamp.lane != lane)
        continue;
      for (const ClampStableLane &recorded : m_program->clamp_stable_lanes) {
        if (recorded == clamp)
          return;
      }
      m_program->clamp_stable_lanes.push_back(clamp);
      return;
    }
  }

  void CollectResources() {
    std::map<MemoryKey, u32> memory_indices;
    std::map<MemoryKey, u32> constant_indices;
    for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
      if (!m_reachable[node_id] && !m_flat_color_reachable[node_id])
        continue;
      const ExpressionNode &node = m_kernel.expressions[node_id];
      switch (node.kind) {
      case ExpressionKind::Memory: {
        const MemoryKey key = MakeMemoryKey(node.memory_address);
        if (node.memory_address.invocation_coefficient == 0) {
          auto [it, inserted] = constant_indices.emplace(
              key, static_cast<u32>(constant_indices.size()));
          if (inserted) {
            m_program->constant_inputs.push_back(
                {node.memory_address, it->second});
          }
          m_constant_node_input[node_id] = it->second;
          break;
        }
        auto [it, inserted] = memory_indices.emplace(
            key, static_cast<u32>(memory_indices.size()));
        if (inserted) {
          m_program->memory_inputs.push_back(
              {node.memory_address, it->second});
        }
        m_memory_node_input[node_id] = it->second;
        if (m_program->uses_flat_instance_inputs) {
          CgMemoryInput &input = m_program->memory_inputs[it->second];
          if (m_reachable[node_id])
            input.flat_attribute_vertex_mask |= FullFlatVertexMask();
          if (m_flat_color_reachable[node_id]) {
            input.flat_attribute_vertex_mask |= static_cast<u8>(
                1u << (m_program->flat_vertices_per_primitive - 1u));
          }
        }
        break;
      }
      case ExpressionKind::InitialVf:
        m_program->vf_uniform_mask |= 1u << node.reg;
        m_program->stable_initial_vf_lanes[node.reg] |=
            static_cast<u8>(0x8u >> node.lane);
        if ((m_kernel.stable_initial_vf_lanes[node.reg] &
             (0x8u >> node.lane)) == 0) {
          m_program->requires_dynamic_entry_state = true;
        } else {
          RecordClampStableLane(node.reg, node.lane);
        }
        break;
      case ExpressionKind::InitialAcc:
        m_program->uses_acc_uniform = true;
        m_program->stable_initial_acc_lanes |=
            static_cast<u8>(0x8u >> node.lane);
        if ((m_kernel.stable_initial_acc_lanes &
             (0x8u >> node.lane)) == 0) {
          m_program->requires_dynamic_entry_state = true;
        }
        break;
      case ExpressionKind::InitialQ:
        m_program->uses_q_uniform = true;
        m_program->stable_initial_q = true;
        m_program->requires_dynamic_entry_state |=
            !m_kernel.stable_initial_q;
        break;
      case ExpressionKind::InitialP:
        m_program->uses_p_uniform = true;
        m_program->stable_initial_p = true;
        m_program->requires_dynamic_entry_state |=
            !m_kernel.stable_initial_p;
        break;
      case ExpressionKind::InitialI:
        m_program->uses_i_uniform = true;
        m_program->stable_initial_i = true;
        m_program->requires_dynamic_entry_state |=
            !m_kernel.stable_initial_i;
        break;
      case ExpressionKind::InvariantVf:
        m_program->vf_uniform_mask |= 1u << node.reg;
        m_program->requires_dynamic_entry_state = true;
        break;
      case ExpressionKind::InvariantAcc:
        m_program->uses_acc_uniform = true;
        m_program->requires_dynamic_entry_state = true;
        break;
      case ExpressionKind::InvariantQ:
        m_program->uses_q_uniform = true;
        m_program->requires_dynamic_entry_state = true;
        break;
      case ExpressionKind::InvariantP:
        m_program->uses_p_uniform = true;
        m_program->requires_dynamic_entry_state = true;
        break;
      case ExpressionKind::InvariantI:
        m_program->uses_i_uniform = true;
        m_program->requires_dynamic_entry_state = true;
        break;
      default:
        break;
      }
    }
  }

  bool Uses(ExpressionKind kind) const {
    for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
      if ((m_reachable[node_id] || m_flat_color_reachable[node_id]) &&
          m_kernel.expressions[node_id].kind == kind) {
        return true;
      }
    }
    return false;
  }

  void AppendBindingContract() {
    // The generated GXP is patched together with its vertex stream layout.
    // Keep every patcher-relevant value in the content-keyed source so two
    // otherwise identical arithmetic roots cannot reuse a registration with
    // a different affine stride or flat primitive step.
    m_source += "// GXM binding: coefficients=";
    for (u32 index = 0; index < m_program->memory_inputs.size(); index++) {
      if (index != 0)
        m_source += ",";
      m_source += std::to_string(
          m_program->memory_inputs[index].address.invocation_coefficient);
    }
    m_source += " flatVertices=";
    m_source += std::to_string(m_program->flat_vertices_per_primitive);
    m_source += " flatStep=";
    m_source += std::to_string(m_program->flat_instance_vertex_step);
    m_source += " batchPrimitives=";
    m_source += std::to_string(m_program->batch_primitives_per_draw);
    m_source += " bufferedBatch=";
    m_source += m_program->uses_buffered_batch_inputs ? "1" : "0";
    m_source += " stripWinding=";
    m_source += m_program->flat_strip_winding ? "1\n" : "0\n";
    m_source += "// GXM constant qwords=";
    for (u32 index = 0; index < m_program->constant_inputs.size(); index++) {
      if (index != 0)
        m_source += ",";
      const AffineQwordAddress& address =
          m_program->constant_inputs[index].address;
      m_source += std::to_string(address.base_vi);
      m_source += "/";
      m_source += std::to_string(address.qword_offset);
    }
    m_source += "\n";
    m_source += "// GXM flat attribute masks=";
    for (u32 index = 0; index < m_program->memory_inputs.size(); index++) {
      if (index != 0)
        m_source += ",";
      m_source += std::to_string(
          m_program->memory_inputs[index].flat_attribute_vertex_mask);
    }
    m_source += "\n";
  }

  void AppendHelpers() {
    m_source +=
        "// Generated from PairPlan semantics; ShaccCg owns SSA, allocation, "
        "and scheduling.\n";
    if (Uses(ExpressionKind::Normalize) || Uses(ExpressionKind::Divide)) {
      m_source +=
          "float VitaVuNormalize(float value)\n"
          "{\n"
          "\tconst int bits = floatToRawIntBits(value);\n"
          "\tconst int exponent = bits & 2139095040;\n"
          "\tif (exponent == 0)\n"
          "\t\treturn intBitsToFloat(bits & (-2147483647 - 1));\n"
          "\tif (exponent == 2139095040)\n"
          "\t\treturn intBitsToFloat((bits < 0 ? (-2147483647 - 1) : 0) | "
          "2139095039);\n"
          "\treturn value;\n"
          "}\n\n";
    }
    if (Uses(ExpressionKind::Divide)) {
      m_source +=
          "float VitaVuDivide(float numerator, float denominator)\n"
          "{\n"
          "\tif (denominator == 0.0f)\n"
          "\t{\n"
          "\t\tconst int signBit = (floatToRawIntBits(numerator) ^ "
          "floatToRawIntBits(denominator)) & (-2147483647 - 1);\n"
          "\t\treturn intBitsToFloat(signBit | 2139095039);\n"
          "\t}\n"
          "\treturn VitaVuNormalize(numerator / denominator);\n"
          "}\n\n";
    }
    if (Uses(ExpressionKind::Minimum) || Uses(ExpressionKind::Maximum)) {
      // The PSP2 Cg compiler has no unambiguous integer min/max overload.
      // Explicit signed bit comparisons preserve the existing PS2 raw-float
      // ordering without converting the operands through host floating point.
      m_source +=
          "float VitaVuMinimum(float left, float right)\n"
          "{\n"
          "\tconst int leftBits = floatToRawIntBits(left);\n"
          "\tconst int rightBits = floatToRawIntBits(right);\n"
          "\tconst unsigned int leftKey = unsigned int(leftBits ^ "
          "((leftBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst unsigned int rightKey = unsigned int(rightBits ^ "
          "((rightBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst int orderedBits = leftKey < rightKey ? "
          "leftBits : rightBits;\n"
          "\treturn intBitsToFloat(orderedBits);\n"
          "}\n\n"
          "float VitaVuMaximum(float left, float right)\n"
          "{\n"
          "\tconst int leftBits = floatToRawIntBits(left);\n"
          "\tconst int rightBits = floatToRawIntBits(right);\n"
          "\tconst unsigned int leftKey = unsigned int(leftBits ^ "
          "((leftBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst unsigned int rightKey = unsigned int(rightBits ^ "
          "((rightBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst int orderedBits = leftKey > rightKey ? "
          "leftBits : rightBits;\n"
          "\treturn intBitsToFloat(orderedBits);\n"
          "}\n\n";
    }
    if (Uses(ExpressionKind::FloatToInt)) {
      m_source +=
          "int VitaVuFloatToInt(float value, float scale)\n"
          "{\n"
          "\tconst float scaled = value * scale;\n"
          "\tconst int bits = floatToRawIntBits(scaled);\n"
          "\tif ((bits & 2139095040) >= 1325400064)\n"
          "\t\treturn bits < 0 ? (-2147483647 - 1) : 2147483647;\n"
          "\treturn int(scaled);\n"
          "}\n\n"
          "float VitaVuFloatToMaskedFloat(float value, float scale, "
          "float modulus)\n"
          "{\n"
          "\tconst float scaled = value * scale;\n"
          "\tconst int bits = floatToRawIntBits(scaled);\n"
          "\tif ((bits & 2139095040) >= 1325400064)\n"
          "\t\treturn bits < 0 ? 0.0f : modulus - 1.0f;\n"
          "\tconst float truncatedMagnitude = floor(abs(scaled));\n"
          "\tconst float remainder = "
          "frac(truncatedMagnitude / modulus) * modulus;\n"
          "\treturn bits < 0 && remainder != 0.0f ? "
          "modulus - remainder : remainder;\n"
          "}\n\n";
    }
  }

  void AppendParameter(std::string parameter) {
    if (!m_first_parameter)
      m_source += ",\n";
    m_source += "\t";
    m_source += parameter;
    m_first_parameter = false;
  }

  bool MemoryInputUsedBy(u32 input_index,
                         const std::vector<bool> &reachable) const {
    for (const auto &[node_id, node_input] : m_memory_node_input) {
      if (node_input == input_index && reachable[node_id])
        return true;
    }
    return false;
  }

  void AppendEntrySignature() {
    m_source += "void main(\n";
    u32 attribute_semantic = 0;
    if (m_program->uses_flat_instance_inputs) {
      if (m_program->uses_buffered_batch_inputs) {
        AppendParameter(
            "uniform int4 VuRawQwords[" +
            std::to_string(GeneratedCgProgram::DeclaredBufferVectors) +
            "] : BUFFER[0]");
        AppendParameter(
            "uniform int4 VuBatchBindings[" +
            std::to_string(GeneratedCgProgram::DeclaredBufferVectors) +
            "] : BUFFER[1]");
      } else {
        for (const CgMemoryInput &input : m_program->memory_inputs) {
          for (u32 vertex = 0;
               vertex < m_program->flat_vertices_per_primitive; vertex++) {
            if ((input.flat_attribute_vertex_mask & (1u << vertex)) == 0)
              continue;
            AppendParameter("__regformat int4 VuMemory" +
                            std::to_string(input.attribute_index) + "Vertex" +
                            std::to_string(vertex) + " : TEXCOORD" +
                            std::to_string(attribute_semantic++));
          }
        }
      }
      // PSP2 Shader Compiler User's Guide: INDEX and INSTANCE are special
      // vertex inputs allocated after ordinary parameters. They do not
      // consume a SceGxmVertexAttribute or an additional stream.
      AppendParameter("unsigned int VuLane : INDEX");
      AppendParameter("unsigned int VuPrimitive : INSTANCE");
    } else {
      for (const CgMemoryInput &input : m_program->memory_inputs) {
        AppendParameter("__regformat int4 VuMemory" +
                        std::to_string(input.attribute_index) + " : TEXCOORD" +
                        std::to_string(attribute_semantic++));
      }
    }
    for (const CgConstantInput &input : m_program->constant_inputs) {
      AppendParameter("uniform float4 VuConstant" +
                      std::to_string(input.uniform_index));
    }
    for (u32 reg = 1; reg < 32; reg++) {
      if ((m_program->vf_uniform_mask & (1u << reg)) != 0) {
        AppendParameter("uniform float4 VF" +
                        (reg < 10 ? std::string("0") : std::string()) +
                        std::to_string(reg));
      }
    }
    if (m_program->uses_acc_uniform)
      AppendParameter("uniform float4 ACC");
    if (m_program->uses_q_uniform)
      AppendParameter("uniform float Q");
    if (m_program->uses_p_uniform)
      AppendParameter("uniform float P");
    if (m_program->uses_i_uniform)
      AppendParameter("uniform float I");
    if (m_direct_contract) {
      if (m_program->uses_gif_q_uniform)
        AppendParameter("uniform float GifQ");
      AppendParameter(m_program->uses_tfx_point_size
                          ? "uniform float4 VertexScaleOffset[3]"
                          : "uniform float4 VertexScaleOffset[2]");
      AppendParameter("uniform float MaxDepth");
      AppendParameter("out float4 vPosition : POSITION");
      if (m_program->uses_tfx_point_size)
        AppendParameter("out float vPointSize : PSIZE");
      if (!m_program->uses_tfx_uv_no_fog_interface)
        AppendParameter("out float4 vTexFloat : TEXCOORD0");
      if (m_program->uses_tfx_uv_no_fog_interface)
        AppendParameter("out float2 vTexInt : TEXCOORD0");
      else
        AppendParameter("out float4 vTexInt : TEXCOORD1");
      AppendParameter(m_program->uses_tfx_uv_no_fog_interface
                          ? "out float4 vColor : TEXCOORD1"
                          : "out float4 vColor : TEXCOORD2");
    } else {
      AppendParameter("out float4 VuPosition : POSITION");
      for (u32 i = 0; i < m_kernel.stores.size(); i++) {
        AppendParameter("out float4 VuStore" + std::to_string(i) +
                        " : TEXCOORD" + std::to_string(i));
      }
    }
    m_source += ")\n{\n";
    if (m_program->uses_flat_instance_inputs) {
      m_source += "\tconst int VuInputLane = int(VuLane);\n";
      if (m_program->flat_strip_winding) {
        m_source +=
            "\tconst bool VuSwapStripLane = ((VuPrimitive & 1u) != 0u) "
            "&& VuInputLane < 2;\n"
            "\tconst int VuSelectedLane = VuSwapStripLane ? "
            "(1 - VuInputLane) : VuInputLane;\n";
      } else {
        m_source += "\tconst int VuSelectedLane = VuInputLane;\n";
      }
      if (m_program->uses_buffered_batch_inputs) {
        const u32 binding_vectors =
            (static_cast<u32>(m_program->memory_inputs.size()) + 3u) / 4u;
        static constexpr std::array<const char *, 4> components = {
            "x", "y", "z", "w"};
        const u32 primitives_per_draw =
            m_program->batch_primitives_per_draw;
        if ((primitives_per_draw & (primitives_per_draw - 1u)) == 0)
        {
          u32 shift = 0;
          while ((1u << shift) != primitives_per_draw)
            shift++;
          // SGX543 has no general integer divide. Express the overwhelmingly
          // common power-of-two case directly so the installed SDK 3.0
          // runtime compiler does not have to discover this lowering across
          // the complete generated VU expression graph.
          m_source +=
              "\tconst unsigned int VuBatchDraw = VuPrimitive >> ";
          m_source += std::to_string(shift);
          m_source += "u;\n\tconst unsigned int VuLocalPrimitive = "
                      "VuPrimitive & ";
          m_source += std::to_string(primitives_per_draw - 1u);
          m_source += "u;\n";
        }
        else
        {
          m_source +=
              "\tconst unsigned int VuBatchDraw = VuPrimitive / ";
          m_source += std::to_string(primitives_per_draw);
          m_source +=
              "u;\n\tconst unsigned int VuLocalPrimitive = VuPrimitive - "
              "VuBatchDraw * ";
          m_source += std::to_string(primitives_per_draw);
          m_source += "u;\n";
        }
        for (const CgMemoryInput &input : m_program->memory_inputs) {
          const u32 binding_vector = input.attribute_index / 4u;
          const u32 binding_component = input.attribute_index & 3u;
          const auto append_load =
              [this, binding_vectors, binding_vector, binding_component,
               &input](const std::string &suffix,
                       const std::string &vertex) {
            m_source += "\tconst int4 VuMemory";
            m_source += std::to_string(input.attribute_index);
            m_source += suffix;
            m_source += " = VuRawQwords[VuBatchBindings[VuBatchDraw * ";
            m_source += std::to_string(binding_vectors);
            m_source += " + ";
            m_source += std::to_string(binding_vector);
            m_source += "].";
            m_source += components[binding_component];
            m_source += " + (VuLocalPrimitive * ";
            m_source +=
                std::to_string(m_program->flat_instance_vertex_step);
            m_source += " + ";
            m_source += vertex;
            m_source += ") * ";
            m_source += std::to_string(
                input.address.invocation_coefficient);
            m_source += "];\n";
          };
          // Geometry consumes the invocation-selected VU iteration directly.
          // Preloading every primitive lane and selecting afterwards made
          // each flat IGA vertex read six geometry qwords instead of two.
          if (MemoryInputUsedBy(input.attribute_index, m_reachable))
            append_load("", "VuSelectedLane");
          if (MemoryInputUsedBy(input.attribute_index,
                                m_flat_color_reachable)) {
            const u32 provoking =
                m_program->flat_vertices_per_primitive - 1u;
            append_load("Vertex" + std::to_string(provoking),
                        std::to_string(provoking));
          }
        }
      } else {
        for (const CgMemoryInput &input : m_program->memory_inputs) {
          if (input.flat_attribute_vertex_mask != FullFlatVertexMask())
            continue;
          m_source += "\tint4 VuMemory";
          m_source += std::to_string(input.attribute_index);
          m_source += " = VuSelectedLane == 0 ? VuMemory";
          m_source += std::to_string(input.attribute_index);
          m_source += "Vertex0 : ";
          if (m_program->flat_vertices_per_primitive == 3) {
            m_source += "(VuSelectedLane == 1 ? VuMemory";
            m_source += std::to_string(input.attribute_index);
            m_source += "Vertex1 : VuMemory";
            m_source += std::to_string(input.attribute_index);
            m_source += "Vertex2)";
          } else {
            m_source += "VuMemory";
            m_source += std::to_string(input.attribute_index);
            m_source += "Vertex1";
          }
          m_source += ";\n";
        }
      }
    }
  }

  std::string Value(u32 node_id, bool flat_color = false) const {
    const u32 set = flat_color ? 1u : 0u;
    if (node_id < m_vector_memberships[set].size()) {
      const VectorMembership &membership =
          m_vector_memberships[set][node_id];
      if (membership.representative != InvalidNode) {
        return VectorValue(membership.representative, flat_color) + "." +
               LaneName(membership.lane);
      }
    }
    return flat_color ? "VuFlatValue" + std::to_string(node_id)
                      : "VuValue" + std::to_string(node_id);
  }

  std::string VectorValue(u32 representative, bool flat_color) const {
    return flat_color ? "VuFlatVector" + std::to_string(representative)
                      : "VuVector" + std::to_string(representative);
  }

  std::string FloatFromBits(std::string bits) const {
    return "intBitsToFloat(" + std::move(bits) + ")";
  }

  std::string RawFromFloat(std::string value) const {
    return "floatToRawIntBits(" + std::move(value) + ")";
  }

  std::string ReadFloatUniform(const std::string &name,
                               ScalarDomain domain) const {
    return domain == ScalarDomain::Float ? name : RawFromFloat(name);
  }

  std::string ScaleLiteral(u32 shift) const {
    switch (shift) {
    case 0:
      return "1.0f";
    case 4:
      return "16.0f";
    case 12:
      return "4096.0f";
    case 15:
      return "32768.0f";
    default:
      return "1.0f";
    }
  }

  std::string ReciprocalScaleLiteral(u32 shift) const {
    switch (shift) {
    case 0:
      return "1.0f";
    case 4:
      return "0.0625f";
    case 12:
      return "0.000244140625f";
    case 15:
      return "0.000030517578125f";
    default:
      return "1.0f";
    }
  }

  std::string NodeExpression(u32 node_id, bool flat_color,
                             std::string *error) const {
    const ExpressionNode &node = m_kernel.expressions[node_id];
    const auto operand = [this, &node, flat_color](u32 index) {
      return Value(node.operands[index], flat_color);
    };
    switch (node.kind) {
    case ExpressionKind::ConstantFloat:
      return FloatFromBits(
          std::to_string(static_cast<s32>(node.immediate)));
    case ExpressionKind::ConstantSigned:
      return std::to_string(static_cast<s32>(node.immediate));
    case ExpressionKind::ConstantUnsigned:
      return std::to_string(node.immediate) + "u";
    case ExpressionKind::InitialVf:
    case ExpressionKind::InvariantVf: {
      const std::string name =
          "VF" + (node.reg < 10 ? std::string("0") : std::string()) +
          std::to_string(node.reg) + "." + LaneName(node.lane);
      return ReadFloatUniform(name, node.domain);
    }
    case ExpressionKind::InitialAcc:
    case ExpressionKind::InvariantAcc:
      return ReadFloatUniform("ACC." + std::string(LaneName(node.lane)),
                              node.domain);
    case ExpressionKind::InitialQ:
    case ExpressionKind::InvariantQ:
      return ReadFloatUniform("Q", node.domain);
    case ExpressionKind::InitialP:
    case ExpressionKind::InvariantP:
      return ReadFloatUniform("P", node.domain);
    case ExpressionKind::InitialI:
    case ExpressionKind::InvariantI:
      return ReadFloatUniform("I", node.domain);
    case ExpressionKind::Memory: {
      const auto constant = m_constant_node_input.find(node_id);
      if (constant != m_constant_node_input.end()) {
        const std::string value =
            "VuConstant" + std::to_string(constant->second) + "." +
            LaneName(node.lane);
        if (node.domain == ScalarDomain::Float)
          return value;
        const std::string raw = RawFromFloat(value);
        return node.domain == ScalarDomain::UnsignedInt
                   ? "unsigned int(" + raw + ")"
                   : raw;
      }
      const auto it = m_memory_node_input.find(node_id);
      if (it == m_memory_node_input.end()) {
        Fail(error, "parallel Cg memory expression has no input binding");
        return {};
      }
      const std::string raw =
          "VuMemory" + std::to_string(it->second) +
          (flat_color
               ? "Vertex" +
                     std::to_string(
                         m_program->flat_vertices_per_primitive - 1u)
               : std::string()) +
          "." + LaneName(node.lane);
      if (node.domain == ScalarDomain::Float)
        return FloatFromBits(raw);
      if (node.domain == ScalarDomain::UnsignedInt)
        return "unsigned int(" + raw + ")";
      return raw;
    }
    case ExpressionKind::Add:
      return "(" + operand(0) + " + " + operand(1) + ")";
    case ExpressionKind::Subtract:
      return "(" + operand(0) + " - " + operand(1) + ")";
    case ExpressionKind::Multiply:
      return "(" + operand(0) + " * " + operand(1) + ")";
    case ExpressionKind::Divide:
      return "VitaVuDivide(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Minimum:
      return "VitaVuMinimum(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Maximum:
      return "VitaVuMaximum(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Absolute:
      return "abs(" + operand(0) + ")";
    case ExpressionKind::Negate:
      return "(-" + operand(0) + ")";
    case ExpressionKind::Reciprocal:
      return "(1.0f / " + operand(0) + ")";
    case ExpressionKind::SquareRoot:
      return "sqrt(" + operand(0) + ")";
    case ExpressionKind::ReciprocalSquareRoot:
      return "rsqrt(" + operand(0) + ")";
    case ExpressionKind::FloatToInt:
      return "VitaVuFloatToInt(" + operand(0) + ", " +
             ScaleLiteral(node.immediate) + ")";
    case ExpressionKind::IntToFloat:
      return "(float(" + operand(0) + ") * " +
             ReciprocalScaleLiteral(node.immediate) + ")";
    case ExpressionKind::Normalize:
      return "VitaVuNormalize(" + operand(0) + ")";
    }
    Fail(error, "parallel Cg expression kind is not lowerable");
    return {};
  }

  struct VectorMembership {
    u32 representative = InvalidNode;
    u8 lane = 0;
  };

  struct VectorGroup {
    std::array<u32, 4> nodes{};
  };

  bool IsVectorLeaf(const ExpressionNode &node) const {
    if (node.domain != ScalarDomain::Float)
      return false;
    switch (node.kind) {
    case ExpressionKind::InitialVf:
    case ExpressionKind::InvariantVf:
    case ExpressionKind::InitialAcc:
    case ExpressionKind::InvariantAcc:
    case ExpressionKind::Memory:
      return true;
    default:
      return false;
    }
  }

  bool IsVectorOperation(const ExpressionNode &node) const {
    if (node.domain != ScalarDomain::Float)
      return false;
    switch (node.kind) {
    case ExpressionKind::Add:
    case ExpressionKind::Subtract:
    case ExpressionKind::Multiply:
    case ExpressionKind::Absolute:
    case ExpressionKind::Negate:
      return true;
    default:
      return false;
    }
  }

  void AddVectorGroup(u32 set, const std::array<u32, 4> &nodes) {
    const u32 representative = nodes[0];
    m_vector_groups[set].emplace(representative, VectorGroup{nodes});
    for (u32 lane = 0; lane < nodes.size(); lane++) {
      m_vector_memberships[set][nodes[lane]] = {
          representative, static_cast<u8>(lane)};
    }
  }

  void BuildVectorGroups(const std::vector<bool> &reachable,
                         bool flat_color) {
    const u32 set = flat_color ? 1u : 0u;
    m_vector_memberships[set].assign(m_kernel.expressions.size(), {});
    m_vector_groups[set].clear();

    using LeafKey =
        std::tuple<ExpressionKind, ScalarDomain, u8, MemoryKey, u32>;
    std::map<LeafKey, std::array<u32, 4>> leaves;
    for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
      if (!reachable[node_id])
        continue;
      const ExpressionNode &node = m_kernel.expressions[node_id];
      if (!IsVectorLeaf(node))
        continue;
      const LeafKey key{node.kind, node.domain, node.reg,
                        MakeMemoryKey(node.memory_address), node.immediate};
      leaves[key][node.lane] = node_id;
    }
    for (const auto &[key, nodes] : leaves) {
      (void)key;
      if (std::all_of(nodes.begin(), nodes.end(),
                      [](u32 node) { return node != InvalidNode; })) {
        AddVectorGroup(set, nodes);
      }
    }

    // Grow lane-isomorphic float4 trees from the vector leaves. Each
    // operation may consume either a corresponding-lane vector or one scalar
    // broadcast. Trying both forms is important for VU lighting expressions:
    // VF08..VF11 vary across color lanes while each light coefficient is the
    // same scalar in all four lanes. This is local expression formation only;
    // ShaccCg still owns SSA, allocation, and scheduling for the whole shader.
    using OperationKey =
        std::tuple<ExpressionKind, ScalarDomain, u32, std::array<u32, 3>,
                   std::array<u8, 3>>;
    for (;;) {
      std::map<OperationKey, std::array<u32, 4>> candidates;
      for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
        if (!reachable[node_id] ||
            m_vector_memberships[set][node_id].representative != InvalidNode) {
          continue;
        }
        const ExpressionNode &node = m_kernel.expressions[node_id];
        if (!IsVectorOperation(node))
          continue;

        for (u32 anchor = 0; anchor < node.operands.size(); anchor++) {
          const u32 anchor_id = node.operands[anchor];
          if (anchor_id == InvalidNode)
            continue;
          const VectorMembership anchor_membership =
              m_vector_memberships[set][anchor_id];
          if (anchor_membership.representative == InvalidNode)
            continue;

          for (u32 vector_mask = 1;
               vector_mask < (1u << node.operands.size()); vector_mask++) {
            if ((vector_mask & (1u << anchor)) == 0)
              continue;
            std::array<u32, 3> operand_ids{};
            std::array<u8, 3> operand_is_vector{};
            bool valid = true;
            for (u32 operand = 0; operand < node.operands.size(); operand++) {
              const u32 operand_id = node.operands[operand];
              const bool use_vector =
                  (vector_mask & (1u << operand)) != 0;
              if (!use_vector) {
                operand_ids[operand] = operand_id;
                continue;
              }
              if (operand_id == InvalidNode) {
                valid = false;
                break;
              }
              const VectorMembership membership =
                  m_vector_memberships[set][operand_id];
              if (membership.representative == InvalidNode ||
                  membership.lane != anchor_membership.lane) {
                valid = false;
                break;
              }
              operand_ids[operand] = membership.representative;
              operand_is_vector[operand] = 1;
            }
            if (!valid)
              continue;
            const OperationKey key{node.kind, node.domain, node.immediate,
                                   operand_ids, operand_is_vector};
            u32 &slot = candidates[key][anchor_membership.lane];
            if (slot == InvalidNode || node_id < slot)
              slot = node_id;
          }
        }
      }

      bool added = false;
      for (const auto &[key, nodes] : candidates) {
        (void)key;
        if (!std::all_of(nodes.begin(), nodes.end(),
                         [this, set](u32 node) {
                           return node != InvalidNode &&
                                  m_vector_memberships[set][node]
                                          .representative == InvalidNode;
                         })) {
          continue;
        }
        std::set<u32> unique(nodes.begin(), nodes.end());
        if (unique.size() != nodes.size())
          continue;
        AddVectorGroup(set, nodes);
        added = true;
      }
      if (!added)
        break;
    }
  }

  std::string VectorOperand(const VectorGroup &group, u32 operand,
                            bool flat_color, std::string *error) const {
    const u32 set = flat_color ? 1u : 0u;
    const u32 first = m_kernel.expressions[group.nodes[0]].operands[operand];
    bool scalar = true;
    u32 vector_representative = InvalidNode;
    for (u32 lane = 0; lane < group.nodes.size(); lane++) {
      const u32 node_operand =
          m_kernel.expressions[group.nodes[lane]].operands[operand];
      scalar &= node_operand == first;
      if (node_operand == InvalidNode)
        continue;
      const VectorMembership membership =
          m_vector_memberships[set][node_operand];
      if (membership.representative == InvalidNode ||
          membership.lane != lane) {
        vector_representative = InvalidNode;
        break;
      }
      if (vector_representative == InvalidNode)
        vector_representative = membership.representative;
      else if (vector_representative != membership.representative) {
        vector_representative = InvalidNode;
        break;
      }
    }
    if (vector_representative != InvalidNode)
      return VectorValue(vector_representative, flat_color);
    if (scalar)
      return Value(first, flat_color);
    Fail(error, "parallel Cg vector group has mismatched operands");
    return {};
  }

  std::string VectorExpression(const VectorGroup &group, bool flat_color,
                               std::string *error) const {
    const ExpressionNode &node = m_kernel.expressions[group.nodes[0]];
    const auto operand = [this, &group, flat_color, error](u32 index) {
      return VectorOperand(group, index, flat_color, error);
    };
    switch (node.kind) {
    case ExpressionKind::InitialVf:
    case ExpressionKind::InvariantVf:
      return "VF" + (node.reg < 10 ? std::string("0") : std::string()) +
             std::to_string(node.reg);
    case ExpressionKind::InitialAcc:
    case ExpressionKind::InvariantAcc:
      return "ACC";
    case ExpressionKind::Memory: {
      const auto constant = m_constant_node_input.find(group.nodes[0]);
      if (constant != m_constant_node_input.end())
        return "VuConstant" + std::to_string(constant->second);
      const auto input = m_memory_node_input.find(group.nodes[0]);
      if (input == m_memory_node_input.end()) {
        Fail(error, "parallel Cg vector memory has no input binding");
        return {};
      }
      std::string raw = "VuMemory" + std::to_string(input->second);
      if (flat_color) {
        raw += "Vertex";
        raw +=
            std::to_string(m_program->flat_vertices_per_primitive - 1u);
      }
      return FloatFromBits(std::move(raw));
    }
    case ExpressionKind::Add:
      return "(" + operand(0) + " + " + operand(1) + ")";
    case ExpressionKind::Subtract:
      return "(" + operand(0) + " - " + operand(1) + ")";
    case ExpressionKind::Multiply:
      return "(" + operand(0) + " * " + operand(1) + ")";
    case ExpressionKind::Absolute:
      return "abs(" + operand(0) + ")";
    case ExpressionKind::Negate:
      return "(-" + operand(0) + ")";
    default:
      break;
    }
    Fail(error, "parallel Cg vector expression kind is not lowerable");
    return {};
  }

  bool AppendVectorGroup(u32 representative,
                         const std::vector<bool> &reachable, bool flat_color,
                         std::vector<bool> *visiting,
                         std::vector<bool> *emitted, std::string *error) {
    const u32 set = flat_color ? 1u : 0u;
    const auto group_it = m_vector_groups[set].find(representative);
    if (group_it == m_vector_groups[set].end())
      return Fail(error, "parallel Cg vector group is missing");
    const VectorGroup &group = group_it->second;
    if ((*emitted)[group.nodes[0]])
      return true;
    for (u32 node_id : group.nodes) {
      if ((*visiting)[node_id])
        return Fail(error, "parallel Cg root has a cyclic vector slice");
      (*visiting)[node_id] = true;
    }
    for (u32 node_id : group.nodes) {
      for (u32 operand : m_kernel.expressions[node_id].operands) {
        if (!AppendNode(operand, reachable, flat_color, visiting, emitted,
                        error)) {
          return false;
        }
      }
    }
    for (u32 node_id : group.nodes)
      (*visiting)[node_id] = false;

    const std::string expression =
        VectorExpression(group, flat_color, error);
    if (error && !error->empty())
      return false;
    m_source += "\tfloat4 ";
    m_source += VectorValue(representative, flat_color);
    m_source += " = ";
    m_source += expression;
    m_source += ";\n";
    for (u32 node_id : group.nodes)
      (*emitted)[node_id] = true;
    m_program->emitted_expression_count +=
        static_cast<u32>(group.nodes.size());
    return true;
  }

  // Emits one node after its operands. Node ids are not a valid ordering:
  // InlineAcyclicEntrySlice appends entry-region nodes with higher ids than
  // the loop nodes it then rewrites to consume them, so ascending-id emission
  // produces forward references that Cg rejects as undeclared identifiers.
  bool AppendNode(u32 node_id, const std::vector<bool> &reachable,
                  bool flat_color, std::vector<bool> *visiting,
                  std::vector<bool> *emitted, std::string *error) {
    if (node_id == InvalidNode || !reachable[node_id] || (*emitted)[node_id])
      return true;
    const u32 set = flat_color ? 1u : 0u;
    const VectorMembership membership =
        m_vector_memberships[set][node_id];
    if (membership.representative != InvalidNode) {
      return AppendVectorGroup(membership.representative, reachable,
                               flat_color, visiting, emitted, error);
    }
    if ((*visiting)[node_id]) {
      return Fail(error, "parallel Cg root has a cyclic expression slice");
    }
    (*visiting)[node_id] = true;
    for (const u32 operand : m_kernel.expressions[node_id].operands) {
      if (!AppendNode(operand, reachable, flat_color, visiting, emitted,
                      error)) {
        return false;
      }
    }
    (*visiting)[node_id] = false;

    const std::string expression = NodeExpression(node_id, flat_color, error);
    if (error && !error->empty())
      return false;
    m_source += "\t";
    m_source += ScalarType(m_kernel.expressions[node_id].domain);
    m_source += " ";
    m_source += Value(node_id, flat_color);
    m_source += " = ";
    m_source += expression;
    m_source += ";\n";
    (*emitted)[node_id] = true;
    m_program->emitted_expression_count++;
    return true;
  }

  void AppendExpressions(std::string *error) {
    const auto append_set = [this, error](const std::vector<bool> &reachable,
                                          bool flat_color) {
      BuildVectorGroups(reachable, flat_color);
      std::vector<bool> visiting(m_kernel.expressions.size(), false);
      std::vector<bool> emitted(m_kernel.expressions.size(), false);
      for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
        if (!AppendNode(node_id, reachable, flat_color, &visiting, &emitted,
                        error)) {
          return false;
        }
      }
      return true;
    };
    if (!append_set(m_reachable, false))
      return;
    append_set(m_flat_color_reachable, true);
  }

  std::string OutputValue(u32 node_id) const {
    const std::string value = Value(node_id);
    return m_kernel.expressions[node_id].domain == ScalarDomain::Float
               ? value
               : FloatFromBits(value);
  }

  std::string FloatValue(u32 node_id) const {
    const std::string value = Value(node_id);
    return m_kernel.expressions[node_id].domain == ScalarDomain::Float
               ? value
               : FloatFromBits(value);
  }

  std::string PackedValue(const PackedIntegerExpression &field,
                          bool flat_color = false) const {
    const ExpressionNode &root = m_kernel.expressions[field.expression];
    if (field.right_shift == 0 && field.mask != 0xffffffffu &&
        (field.mask & (field.mask + 1u)) == 0 &&
        root.kind == ExpressionKind::FloatToInt &&
        root.operands[0] != InvalidNode) {
      return "VitaVuFloatToMaskedFloat(" +
             Value(root.operands[0], flat_color) + ", " +
             ScaleLiteral(root.immediate) + ", " +
             std::to_string(field.mask + 1u) + ".0f)";
    }
    std::string value = Value(field.expression, flat_color);
    if (m_kernel.expressions[field.expression].domain == ScalarDomain::Float)
      value = RawFromFloat(std::move(value));
    if (field.right_shift != 0) {
      value = "(" + value + " >> " +
              std::to_string(field.right_shift) + ")";
    }
    if (field.mask != 0xffffffffu) {
      value = "(" + value + " & " + std::to_string(field.mask) + ")";
      // The PSP2 compiler expands a full 32-bit integer-to-float conversion
      // into a long pack/reconstruct sequence. A mask no wider than 16 bits
      // is a proof that the unsigned-short conversion has the identical
      // mathematical value, while mapping to SGX's native 16-bit pack path.
      if (field.mask <= 0xffffu)
        return "float(unsigned short(" + value + "))";
      // Every integer through 24 bits is represented exactly by IEEE binary32.
      // Reconstructing it from two proven 16-bit ranges therefore preserves
      // the full conversion result while avoiding SGX's emulated 32-bit
      // integer conversion.
      if (field.mask <= 0xffffffu) {
        return "(float(unsigned short((" + value +
               ") & 65535)) + float(unsigned short((" + value +
               ") >> 16)) * 65536.0f)";
      }
      return "float(" + value + ")";
    }
    return "float(unsigned int(" + value + "))";
  }

  void AppendDirectTfxOutputs() {
    const auto &contract = *m_direct_contract;
    const bool uv_no_fog_interface =
        m_program->uses_tfx_uv_no_fog_interface;
    const std::string st_x =
        contract.textured && !contract.fixed_texture_coordinates
            ? FloatValue(contract.st[0])
            : "0.0f";
    const std::string st_y =
        contract.textured && !contract.fixed_texture_coordinates
            ? FloatValue(contract.st[1])
            : "0.0f";
    const std::string q =
        contract.textured
            ? (contract.fixed_texture_coordinates ? "GifQ"
                                                  : FloatValue(contract.q))
            : "1.0f";
    const std::string uv_x =
        contract.textured && contract.fixed_texture_coordinates
            ? PackedValue(contract.uv[0])
            : "0.0f";
    const std::string uv_y =
        contract.textured && contract.fixed_texture_coordinates
            ? PackedValue(contract.uv[1])
            : "0.0f";
    const std::string fog =
        contract.fog_enabled ? "(" + PackedValue(contract.fog) + " / 255.0f)"
                             : "0.0f";
    const bool flat_color = m_program->uses_flat_instance_inputs;

    m_source +=
        "\tconst float2 vertexScale = VertexScaleOffset[0].xy;\n"
        "\tconst float2 vertexOffset = VertexScaleOffset[0].zw;\n"
        "\tconst float2 VuXY = float2(" + PackedValue(contract.position[0]) +
        ", " + PackedValue(contract.position[1]) + ");\n"
        "\tconst float VuZ = min(" + PackedValue(contract.depth) +
        ", MaxDepth);\n"
        "\tvPosition.xy = (VuXY - float2(0.05f, 0.05f)) * vertexScale - "
        "vertexOffset;\n"
        "\tvPosition.z = VuZ * (1.0f / 4294967296.0f);\n"
        "\tvPosition.w = 1.0f;\n";
    if (m_program->uses_tfx_point_size)
      m_source += "\tvPointSize = VertexScaleOffset[2].x;\n";
    m_source +=
        "\tconst float2 textureOffset = VertexScaleOffset[1].zw;\n";
    if (!uv_no_fog_interface) {
      m_source +=
          "\tconst float2 VuST = float2(" + st_x + ", " + st_y + ");\n"
          "\tconst float2 st = VuST - textureOffset;\n"
          "\tvTexFloat = float4(st.x, st.y, " + fog + ", " + q + ");\n";
    }
    m_source +=
        "\tconst float2 textureScale = VertexScaleOffset[1].xy;\n"
        "\tconst float2 VuUV = float2(" + uv_x + ", " + uv_y + ");\n"
        "\tconst float2 uv = VuUV - textureOffset;\n";
    if (uv_no_fog_interface)
      m_source += "\tvTexInt = uv * textureScale;\n";
    else
      m_source +=
          "\tvTexInt.xy = uv * textureScale;\n"
          "\tvTexInt.zw = uv;\n";
    m_source += "\tvColor = float4(" +
        PackedValue(contract.color[0], flat_color) + ", " +
        PackedValue(contract.color[1], flat_color) + ", " +
        PackedValue(contract.color[2], flat_color) + ", " +
        PackedValue(contract.color[3], flat_color) + ");\n";
  }

  void AppendOutputs() {
    if (m_direct_contract) {
      AppendDirectTfxOutputs();
      return;
    }
    m_source += "\tVuPosition = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
    for (u32 store_index = 0; store_index < m_kernel.stores.size();
         store_index++) {
      const LoopStore &store = m_kernel.stores[store_index];
      m_source += "\tVuStore";
      m_source += std::to_string(store_index);
      m_source += " = float4(";
      for (u32 lane = 0; lane < 4; lane++) {
        if (lane != 0)
          m_source += ", ";
        if ((store.write_mask & (0x8u >> lane)) != 0)
          m_source += OutputValue(store.values[lane]);
        else
          m_source += "0.0f";
      }
      m_source += ");\n";
    }
  }

  const ParallelLoopKernel &m_kernel;
  const DirectTfxContract *m_direct_contract;
  GeneratedCgProgram *m_program;
  std::vector<bool> m_reachable;
  std::vector<bool> m_flat_color_reachable;
  std::array<std::vector<VectorMembership>, 2> m_vector_memberships;
  std::array<std::map<u32, VectorGroup>, 2> m_vector_groups;
  std::map<u32, u32> m_memory_node_input;
  std::map<u32, u32> m_constant_node_input;
  std::string m_source;
  bool m_first_parameter = true;
};

} // namespace

bool GenerateParallelStoreValidationCg(const ParallelLoopKernel &kernel,
                                       GeneratedCgProgram *program,
                                       std::string *error) {
  if (!program)
    return Fail(error, "null generated Cg output");
  *program = {};
  CgEmitter emitter(kernel, nullptr, program);
  return emitter.Generate(error);
}

bool GenerateParallelTfxCg(const ParallelLoopKernel &kernel,
                           const DirectTfxContract &contract,
                           GeneratedCgProgram *program,
                           std::string *error) {
  if (!program)
    return Fail(error, "null generated TFX Cg output");
  *program = {};
  CgEmitter emitter(kernel, &contract, program);
  return emitter.Generate(error);
}

} // namespace VitaGpuVu
