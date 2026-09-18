// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuCgGenerator.h"

#include "VitaGpuVuStructuredPreflightCgSource.h"

#include "GS/GSRegs.h"
#include "VUmicroFast.h"
#include "vita/VitaGpuVuCommandEpoch.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace VitaGpuVu {
namespace {

using LowerKind = VUInterpFast::LowerFastKind;

constexpr u32 InvalidNode = 0;
constexpr u32 MaximumMemoryInputs = 16;
constexpr u32 MaximumConstantInputs = 32;
constexpr u32 MaximumVertexAttributes = 16;
constexpr u32 MaximumValidationStores = 8;
constexpr u32 MaximumStructuredChildIterations = 1024;
constexpr u32 MaximumStructuredOuterIterations = 64;
constexpr u32 StructuredStoreAddressMask = 0x000003ffu;
constexpr u32 StructuredStoreLaneMask = 0x000f0000u;
constexpr u32 StructuredStorePackedMask =
    StructuredStoreAddressMask | StructuredStoreLaneMask;
constexpr size_t MaximumGeneratedSourceBytes = 512 * 1024;
constexpr u32 GeneratedCgSoftwareF32NumericMask =
    UniversalConfigurationOverflowClamp |
    UniversalConfigurationExtraOverflowClamp |
    UniversalConfigurationSignOverflowClamp |
    UniversalConfigurationUnderflowClamp |
    UniversalConfigurationRoundModeMask |
    UniversalConfigurationDenormalsAreZero |
    UniversalConfigurationFlushToZero;
constexpr u32 GeneratedCgSoftwareF32NumericProfile =
    UniversalConfigurationOverflowClamp |
    UniversalConfigurationDenormalsAreZero |
    UniversalConfigurationFlushToZero;
// ShaderCompiler recognizes these title-neutral markers. The former complete
// 16.8 KiB state root faulted Shacc at O1, while a 10.7 KiB recurrence slice
// which unnecessarily embedded the complete software-add helper faulted at
// O0. Helper pruning plus destination/recurrence partitioning now keeps that
// state class below 9.1 KiB for the captured structural gate; both Sony SDK
// 3.5.0.13276 and 3.5.0.13284 compile those roots at O1, while O0 ICEs. Keep
// these literals synchronized with VitaGpuVuShaderCompiler.cpp.
constexpr const char* StructuredOptimizationMarker =
    "// VitaSX2 generated structured state ceiling O1\n";
constexpr const char* StructuredNativeStateOptimizationMarker =
    "// VitaSX2 generated structured native state ceiling O3\n";
constexpr const char* StructuredExactOptimizationMarker =
    "// VitaSX2 generated structured exact ceiling O0\n";
constexpr const char* LoopKernelExactOptimizationMarker =
    "// VitaSX2 generated loop-kernel exact optimization ceiling O0\n";
constexpr const char* StructuredParallelOptimizationMarker =
    "// VitaSX2 generated structured parallel ceiling O1\n";
constexpr const char* StructuredNativeParallelOptimizationMarker =
    "// VitaSX2 generated structured native parallel ceiling O3\n";

u32 NormalizeGeneratedCgSoftwareF32Input(u32 bits) {
  const u32 exponent = bits & 0x7f800000u;
  if (exponent == 0)
    return bits & 0x80000000u;
  if (exponent == 0x7f800000u)
    return (bits & 0x80000000u) | 0x7f7fffffu;
  return bits;
}

u32 ShiftRightJamGeneratedCgSoftwareF32(u32 value, u32 distance) {
  if (distance == 0)
    return value;
  if (distance < 32) {
    const u32 mask = (1u << distance) - 1u;
    return (value >> distance) | ((value & mask) != 0 ? 1u : 0u);
  }
  return value != 0 ? 1u : 0u;
}

// SGX543's native f32 adder does not retain an unbounded sticky bit when the
// smaller operand is shifted beyond its finite alignment window. Physical
// attestation of the emitted add.f32 instruction shows both behaviours which
// distinguish this from a mathematical round-toward-zero operation:
// 1.0 + 3*2^-24 chops to the lower adjacent float, while FLT_MAX - 1.0 leaves
// FLT_MAX unchanged. Keep this candidate playable-profile model separate from
// the exact software helper; broader graphics and arithmetic gates still own
// whether the native GXP may be promoted.
u32 ShiftRightGeneratedCgNativeF32(u32 value, u32 distance) {
  return distance < 32u ? value >> distance : 0u;
}

u32 AddGeneratedCgF32(u32 left, u32 right, bool subtract,
                      bool nearest_even, bool jam_discarded_bits) {
  left = NormalizeGeneratedCgSoftwareF32Input(left);
  right = NormalizeGeneratedCgSoftwareF32Input(right);
  if (subtract)
    right ^= 0x80000000u;

  u32 left_magnitude = left & 0x7fffffffu;
  u32 right_magnitude = right & 0x7fffffffu;
  const u32 left_sign = left >> 31u;
  const u32 right_sign = right >> 31u;
  if (left_magnitude == 0 && right_magnitude == 0) {
    return left_sign == right_sign ? left_sign << 31u : 0u;
  }
  if (left_magnitude == 0)
    return right;
  if (right_magnitude == 0)
    return left;
  if (left_magnitude < right_magnitude) {
    std::swap(left, right);
    std::swap(left_magnitude, right_magnitude);
  }

  const u32 sign = left & 0x80000000u;
  s32 exponent = static_cast<s32>((left >> 23u) & 0xffu);
  const u32 right_exponent = (right >> 23u) & 0xffu;
  const u32 left_significand =
      ((left & 0x007fffffu) | 0x00800000u) << 3u;
  u32 right_significand =
      ((right & 0x007fffffu) | 0x00800000u) << 3u;
  const u32 exponent_distance =
      static_cast<u32>(exponent) - right_exponent;
  right_significand = jam_discarded_bits
                          ? ShiftRightJamGeneratedCgSoftwareF32(
                                right_significand, exponent_distance)
                          : ShiftRightGeneratedCgNativeF32(
                                right_significand, exponent_distance);

  u32 significand;
  if (((left ^ right) & 0x80000000u) == 0) {
    significand = left_significand + right_significand;
    if ((significand & 0x08000000u) != 0) {
      significand = ShiftRightJamGeneratedCgSoftwareF32(significand, 1);
      exponent++;
    }
  } else {
    significand = left_significand - right_significand;
    if (significand == 0)
      return 0;
    while ((significand & 0x04000000u) == 0) {
      significand <<= 1u;
      exponent--;
    }
  }

  // The supported profile flushes every tiny result. Normal inputs can reach
  // this path only through cancellation; there is no subnormal state to
  // publish into the VU register file.
  if (exponent <= 0)
    return sign;

  u32 rounded = significand >> 3u;
  const u32 round_bits = significand & 7u;
  if (nearest_even &&
      (round_bits > 4u || (round_bits == 4u && (rounded & 1u) != 0)))
    rounded++;
  if ((rounded & 0x01000000u) != 0) {
    rounded >>= 1u;
    exponent++;
  }
  if (exponent >= 255)
    return sign | 0x7f7fffffu;
  return sign | (static_cast<u32>(exponent) << 23u) |
         (rounded & 0x007fffffu);
}

u32 AddGeneratedCgSoftwareF32(u32 left, u32 right, bool subtract) {
  return AddGeneratedCgF32(left, right, subtract, true, true);
}

u32 AddGeneratedCgNativeF32(u32 left, u32 right, bool subtract) {
  return AddGeneratedCgF32(left, right, subtract, false, false);
}

u32 MultiplyGeneratedCgF32(u32 left, u32 right, bool nearest_even) {
  left = NormalizeGeneratedCgSoftwareF32Input(left);
  right = NormalizeGeneratedCgSoftwareF32Input(right);
  const u32 sign = (left ^ right) & 0x80000000u;
  if ((left & 0x7fffffffu) == 0 || (right & 0x7fffffffu) == 0)
    return sign;

  s32 exponent = static_cast<s32>((left >> 23u) & 0xffu) +
                 static_cast<s32>((right >> 23u) & 0xffu) - 127;
  const u32 left_significand =
      (left & 0x007fffffu) | 0x00800000u;
  const u32 right_significand =
      (right & 0x007fffffu) | 0x00800000u;

  // Two 24-bit significands are multiplied as 16-bit limbs. The resulting
  // 48-bit product is retained in product_high:product_low without requiring
  // a shader-language 64-bit type.
  const u32 left_low = left_significand & 0xffffu;
  const u32 left_high = left_significand >> 16u;
  const u32 right_low = right_significand & 0xffffu;
  const u32 right_high = right_significand >> 16u;
  const u32 low_product = left_low * right_low;
  const u32 cross_product =
      left_high * right_low + left_low * right_high;
  const u32 product_low = low_product + (cross_product << 16u);
  const u32 carry = product_low < low_product ? 1u : 0u;
  const u32 product_high =
      left_high * right_high + (cross_product >> 16u) + carry;

  u32 rounded;
  u32 remainder;
  u32 halfway;
  if ((product_high & 0x00008000u) != 0) {
    rounded = (product_high << 8u) | (product_low >> 24u);
    remainder = product_low & 0x00ffffffu;
    halfway = 0x00800000u;
    exponent++;
  } else {
    rounded = (product_high << 9u) | (product_low >> 23u);
    remainder = product_low & 0x007fffffu;
    halfway = 0x00400000u;
  }
  if (nearest_even &&
      (remainder > halfway ||
       (remainder == halfway && (rounded & 1u) != 0))) {
    rounded++;
  }
  if ((rounded & 0x01000000u) != 0) {
    rounded >>= 1u;
    exponent++;
  }
  if (exponent <= 0)
    return sign;
  if (exponent >= 255)
    return sign | 0x7f7fffffu;
  return sign | (static_cast<u32>(exponent) << 23u) |
         (rounded & 0x007fffffu);
}

u32 MultiplyGeneratedCgSoftwareF32(u32 left, u32 right) {
  return MultiplyGeneratedCgF32(left, right, true);
}

u32 MultiplyGeneratedCgNativeF32(u32 left, u32 right) {
  return MultiplyGeneratedCgF32(left, right, false);
}

u32 DivideGeneratedCgExactF32(u32 numerator, u32 denominator) {
  numerator = NormalizeGeneratedCgSoftwareF32Input(numerator);
  denominator = NormalizeGeneratedCgSoftwareF32Input(denominator);
  const u32 sign = (numerator ^ denominator) & 0x80000000u;
  if ((denominator & 0x7fffffffu) == 0u)
    return sign | 0x7f7fffffu;
  if ((numerator & 0x7fffffffu) == 0u)
    return sign;

  const u32 numerator_significand =
      (numerator & 0x007fffffu) | 0x00800000u;
  const u32 denominator_significand =
      (denominator & 0x007fffffu) | 0x00800000u;
  s32 exponent = static_cast<s32>((numerator >> 23u) & 0xffu) -
                 static_cast<s32>((denominator >> 23u) & 0xffu) + 127;
  const u32 ratio_shift =
      numerator_significand < denominator_significand ? 1u : 0u;
  exponent -= static_cast<s32>(ratio_shift);

  // Both significands are 24-bit normalized integers.  Once the ratio is in
  // [1, 2), its leading quotient bit is known.  The remaining 23 bits and the
  // exact remainder are obtained without a 64-bit type or integer divide,
  // matching the operations available on SGX543's Series5XT USE core.
  u32 remainder =
      (numerator_significand << ratio_shift) - denominator_significand;
  u32 quotient = 0x00800000u;
  for (s32 bit = 22; bit >= 0; bit--) {
    remainder <<= 1u;
    if (remainder >= denominator_significand) {
      remainder -= denominator_significand;
      quotient |= 1u << static_cast<u32>(bit);
    }
  }

  const u32 twice_remainder = remainder << 1u;
  if (twice_remainder > denominator_significand ||
      (twice_remainder == denominator_significand &&
       (quotient & 1u) != 0u)) {
    quotient++;
  }
  if ((quotient & 0x01000000u) != 0u) {
    quotient >>= 1u;
    exponent++;
  }
  if (exponent <= 0)
    return sign;
  if (exponent >= 255)
    return sign | 0x7f7fffffu;
  return sign | (static_cast<u32>(exponent) << 23u) |
         (quotient & 0x007fffffu);
}

u32 StructuredArmReciprocalEstimateField(u32 normalized_bits) {
  const u32 scaled = 0x100u | ((normalized_bits >> 15u) & 0xffu);
  const u32 denominator = scaled * 2u + 1u;
  const u32 estimate = (1u << 19u) / denominator;
  return ((estimate + 1u) >> 1u) & 0xffu;
}

u32 StructuredArmReciprocalSqrtEstimateField(u32 normalized_bits) {
  const u32 exponent = (normalized_bits >> 23u) & 0xffu;
  const u32 scaled = (exponent & 1u) != 0u
                         ? 0x80u | ((normalized_bits >> 16u) & 0x7fu)
                         : 0x100u | ((normalized_bits >> 15u) & 0xffu);
  const u32 operand = scaled < 256u
                          ? scaled * 2u + 1u
                          : ((((scaled >> 1u) << 1u) + 1u) * 2u);
  u32 estimate = 512u;
  while (operand * (estimate + 1u) * (estimate + 1u) < (1u << 28u))
    estimate++;
  return ((estimate + 1u) >> 1u) & 0xffu;
}

u32 ApplyStructuredFixedQpNumericBits(
    StructuredFixedQpNumericOperation operation, u32 input_bits) {
  const u32 value_bits = NormalizeGeneratedCgSoftwareF32Input(input_bits);
  const u32 sign_bits = value_bits & 0x80000000u;
  const u32 exponent = (value_bits >> 23u) & 0xffu;
  if (operation ==
      StructuredFixedQpNumericOperation::ArmApproximateReciprocal) {
    if (exponent == 0u)
      return value_bits;
    if (exponent == 0xffu || exponent >= 253u)
      return sign_bits;
    const u32 estimate_field =
        StructuredArmReciprocalEstimateField(value_bits);
    const u32 seed_bits = sign_bits | ((253u - exponent) << 23u) |
                          (estimate_field << 15u);
    const u32 product =
        MultiplyGeneratedCgSoftwareF32(value_bits, seed_bits);
    const u32 refinement = AddGeneratedCgSoftwareF32(
        0x40000000u, product ^ 0x80000000u, false);
    return MultiplyGeneratedCgSoftwareF32(seed_bits, refinement);
  }

  if ((value_bits & 0x80000000u) != 0u ||
      (value_bits & 0x7fffffffu) == 0u || exponent == 0xffu) {
    return value_bits;
  }
  const u32 estimate_field =
      StructuredArmReciprocalSqrtEstimateField(value_bits);
  const u32 seed_bits = (((380u - exponent) >> 1u) << 23u) |
                        (estimate_field << 15u);
  const u32 seed_square =
      MultiplyGeneratedCgSoftwareF32(seed_bits, seed_bits);
  const u32 product =
      MultiplyGeneratedCgSoftwareF32(value_bits, seed_square);
  const u32 difference = AddGeneratedCgSoftwareF32(
      0x40400000u, product ^ 0x80000000u, false);
  const u32 refinement =
      MultiplyGeneratedCgSoftwareF32(difference, 0x3f000000u);
  const u32 refined =
      MultiplyGeneratedCgSoftwareF32(seed_bits, refinement);
  return MultiplyGeneratedCgSoftwareF32(value_bits, refined);
}

enum class CgOutputKind : u8 {
  StoreVaryings,
  DirectTfx,
  LoopKernelDirectTfx,
  FinalStateBuffer,
  StructuredStateSnapshots,
  StructuredExpressionScratch,
  StructuredParallelChildMemoryStore,
  StructuredParallelDirectTfx,
  StructuredFinalState,
};

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

const char *VectorType(ScalarDomain domain) {
  switch (domain) {
  case ScalarDomain::Float:
    return "float4";
  case ScalarDomain::UnsignedInt:
    return "unsigned int4";
  case ScalarDomain::Raw:
  case ScalarDomain::SignedInt:
    return "int4";
  }
  return "int4";
}

using MemoryKey = std::tuple<u8, s32, s32, bool, s32>;

MemoryKey MakeMemoryKey(const AffineQwordAddress &address) {
  return {address.base_vi, address.invocation_coefficient,
          address.qword_offset, address.valid,
          address.outer_invocation_coefficient};
}

bool FindBoundedUnsignedDivisionMagic(u32 divisor, u32 exclusive_maximum,
                                      u32* multiplier, u32* shift) {
  if (!multiplier || !shift || divisor < 2u || exclusive_maximum == 0u)
    return false;
  for (u32 candidate_shift = 1u; candidate_shift < 32u;
       candidate_shift++) {
    const u64 scale = 1ull << candidate_shift;
    const u64 candidate_multiplier = (scale + divisor - 1u) / divisor;
    if (candidate_multiplier > std::numeric_limits<u32>::max())
      continue;
    // The generated Cg multiply is a 32-bit operation. Reject a mathematically
    // valid quotient which would require an unrepresented 64-bit product.
    if (static_cast<u64>(exclusive_maximum - 1u) *
            candidate_multiplier >
        std::numeric_limits<u32>::max()) {
      continue;
    }
    bool exact = true;
    for (u32 value = 0u; value < exclusive_maximum; value++) {
      if ((static_cast<u64>(value) * candidate_multiplier >>
           candidate_shift) != value / divisor) {
        exact = false;
        break;
      }
    }
    if (!exact)
      continue;
    *multiplier = static_cast<u32>(candidate_multiplier);
    *shift = candidate_shift;
    return true;
  }
  return false;
}

class CgEmitter {
public:
  CgEmitter(const ParallelLoopKernel &kernel,
            const DirectTfxContract *direct_contract,
            GeneratedCgProgram *program,
            CgOutputKind output_kind,
            const NaturalLoop* child_loop = nullptr,
            const NaturalLoop* enclosing_loop = nullptr,
            const EnclosingLoopEntryIndependence* enclosing_boundary = nullptr,
            const StructuredLoopTailProof* structured_tail = nullptr,
            u32 maximum_structured_iterations = 0,
            u32 maximum_structured_child_iterations = 0,
            u32 structured_parent_prefix_pairs = 0,
            u32 structured_entry_pc = 0,
            bool structured_final_control = true,
            bool structured_state_control = true,
            const GeneratedBatchVaryingLiveIns* batch_varying_live_ins =
                nullptr,
            bool emit_loop_kernel_private_output = true)
      : m_kernel(kernel), m_direct_contract(direct_contract),
        m_program(program), m_output_kind(output_kind),
        m_child_loop(child_loop),
        m_enclosing_loop(enclosing_loop),
        m_enclosing_boundary(enclosing_boundary),
        m_structured_tail(structured_tail),
        m_maximum_structured_iterations(maximum_structured_iterations),
        m_maximum_structured_child_iterations(
            maximum_structured_child_iterations),
        m_structured_parent_prefix_pairs(
            structured_parent_prefix_pairs),
        m_structured_entry_pc(structured_entry_pc),
        m_structured_final_control(structured_final_control),
        m_structured_state_control(structured_state_control),
        m_batch_varying_live_ins(batch_varying_live_ins),
        m_emit_loop_kernel_private_output(emit_loop_kernel_private_output) {}

  void SetBatchVaryingLiveIns(
      const GeneratedBatchVaryingLiveIns* batch_varying_live_ins,
      const GeneratedCgProgram* input_layout = nullptr) {
    m_batch_varying_live_ins = batch_varying_live_ins;
    m_batch_varying_input_layout = input_layout;
  }

  void SetEmitLoopKernelPrivateOutput(bool enabled) {
    m_emit_loop_kernel_private_output = enabled;
  }

  void SetNativeOutputOnlyProfile(bool enabled) {
    m_native_output_only_profile = enabled;
  }

  void SetStateCanary(bool enabled) {
    m_program->uses_loop_kernel_state_canary = enabled;
  }

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
  void SetExactDepthAnalysis(bool enabled, std::span<const u8> exact_fmac_nodes,
                            bool range_scaled_multiply, bool normalized_fmac_inputs) {
    m_exact_depth_analysis = enabled;
    m_exact_fmac_analysis_nodes = exact_fmac_nodes;
    m_range_scaled_multiply_analysis = enabled && range_scaled_multiply;
    m_normalized_fmac_inputs_analysis = enabled && normalized_fmac_inputs;
  }
#endif

  bool Generate(std::string *error) {
    if (IsStructuredParallelChildOutput()) {
      if (!m_child_loop || !m_enclosing_boundary ||
          (!m_kernel.independent_store_values &&
           !m_kernel.independent_final_state) ||
          m_maximum_structured_iterations == 0 ||
          m_maximum_structured_iterations > 64 ||
          m_maximum_structured_child_iterations == 0 ||
          m_maximum_structured_child_iterations >
              MaximumStructuredChildIterations ||
          (m_output_kind != CgOutputKind::StructuredParallelDirectTfx &&
           m_output_kind != CgOutputKind::StructuredExpressionScratch &&
           (m_maximum_structured_child_iterations &
            (m_maximum_structured_child_iterations - 1u)) != 0)) {
        return Fail(error,
                    "structured generated child module has an invalid "
                    "bounded contract");
      }
      if (m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore &&
          m_kernel.stores.empty())
        return Fail(error, "structured private-memory store has no output");
      if (m_output_kind == CgOutputKind::StructuredParallelDirectTfx &&
          (!m_direct_contract ||
           !m_direct_contract->gouraud ||
           m_direct_contract->vertex_count == 0u ||
           (m_direct_contract->vertex_count %
            m_maximum_structured_child_iterations) != 0u ||
           (m_direct_contract->vertex_count /
            m_maximum_structured_child_iterations) >
               m_maximum_structured_iterations)) {
        return Fail(error,
                    "structured direct TFX has an invalid dense dispatch "
                    "contract");
      }
      if ((m_output_kind == CgOutputKind::StructuredParallelDirectTfx ||
           m_output_kind == CgOutputKind::StructuredExpressionScratch) &&
          (m_maximum_structured_child_iterations &
           (m_maximum_structured_child_iterations - 1u)) != 0u) {
        u32 multiplier = 0u;
        u32 shift = 0u;
        const u32 invocation_count =
            m_output_kind == CgOutputKind::StructuredParallelDirectTfx
                ? m_direct_contract->vertex_count
                : m_maximum_structured_iterations *
                      m_maximum_structured_child_iterations;
        if (!FindBoundedUnsignedDivisionMagic(
                m_maximum_structured_child_iterations,
                invocation_count, &multiplier, &shift)) {
          return Fail(error,
                      "structured child output has no compiler-safe dense "
                      "dispatch mapping");
        }
      }
      if (m_output_kind == CgOutputKind::StructuredExpressionScratch) {
        if (m_kernel.structured_scratch_outputs.empty() ||
            m_kernel.structured_scratch_outputs.size() >
                StructuredGeneratedScratchSlots) {
          return Fail(error,
                      "structured expression module has an invalid output "
                      "count");
        }
        StructuredFixedFmacScratchMask used_slots{};
        for (const StructuredScratchOutput& output :
             m_kernel.structured_scratch_outputs) {
          const bool duplicate =
              output.slot < StructuredGeneratedScratchSlots &&
              (used_slots[output.slot / 32u] &
               (1u << (output.slot % 32u))) != 0u;
          if (output.expression == InvalidNode ||
              output.expression >= m_kernel.expressions.size() ||
              output.slot >= StructuredGeneratedScratchSlots ||
              duplicate) {
            return Fail(error,
                        "structured expression module has invalid scratch "
                        "metadata");
          }
          used_slots[output.slot / 32u] |=
              1u << (output.slot % 32u);
        }
      }
      const LowerKind child_branch_kind =
          static_cast<LowerKind>(m_child_loop->branch_kind);
      if (child_branch_kind != LowerKind::IBNE ||
          !m_child_loop->branch_taken_repeats ||
          (m_child_loop->counter_step != 1 &&
           m_child_loop->counter_step != -1) ||
          m_child_loop->counter_reg == 0 ||
          (m_enclosing_boundary->demanded_vi_mask &
           (1u << m_child_loop->counter_reg)) == 0 ||
          (m_child_loop->counter_limit_reg != 0 &&
           (m_enclosing_boundary->demanded_vi_mask &
            (1u << m_child_loop->counter_limit_reg)) == 0)) {
        return Fail(error,
                    "structured private-memory store requires a canonical "
                    "child IBNE loop");
      }
      m_program->uses_structured_expression_scratch =
          m_output_kind == CgOutputKind::StructuredExpressionScratch;
      m_program->structured_scratch_output_count =
          m_program->uses_structured_expression_scratch
              ? static_cast<u32>(m_kernel.structured_scratch_outputs.size())
              : 0u;
      m_program->uses_structured_parallel_child_memory_store =
          m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore;
      m_program->uses_structured_parallel_direct_vu_tfx =
          m_output_kind == CgOutputKind::StructuredParallelDirectTfx;
      m_program->execution_kind = m_program->uses_structured_expression_scratch
          ? GeneratedCgExecutionKind::StructuredExpressionScratch
          : (m_program->uses_structured_parallel_direct_vu_tfx
                 ? GeneratedCgExecutionKind::StructuredParallelDirectVuTfx
                 : GeneratedCgExecutionKind::
                       StructuredParallelChildMemoryStore);
      m_program->child_entry_vi_mask =
          m_enclosing_boundary->demanded_vi_mask;
      m_program->maximum_structured_iterations =
          m_maximum_structured_iterations;
      m_program->maximum_structured_child_iterations =
          m_maximum_structured_child_iterations;
      m_program->structured_store_count =
          static_cast<u32>(m_kernel.stores.size());
    } else if (m_output_kind == CgOutputKind::StructuredFinalState) {
      if (!m_child_loop || !m_enclosing_loop || !m_enclosing_boundary ||
          !m_structured_tail ||
          !m_structured_tail->exact_final_resume_proven ||
          !m_structured_tail->intermediate_suffix_proven ||
          m_enclosing_loop->counter_reg == 0 ||
          m_maximum_structured_iterations == 0 ||
          m_maximum_structured_iterations > MaximumStructuredOuterIterations ||
          m_maximum_structured_child_iterations == 0 ||
          m_maximum_structured_child_iterations >
              MaximumStructuredChildIterations ||
          (m_maximum_structured_child_iterations &
           (m_maximum_structured_child_iterations - 1u)) != 0 ||
          !m_kernel.independent_final_state) {
        return Fail(error,
                    "structured final state has an invalid bounded contract");
      }
      bool has_output = m_structured_final_control;
      for (u32 reg = 1; reg < 32; reg++) {
        if (m_kernel.final_vf_lanes[reg] != 0 &&
            m_kernel.final_vf_lanes[reg] != 0x0f) {
          return Fail(error,
                      "structured final-state slice has partial VF lanes");
        }
        has_output |= m_kernel.final_vf_lanes[reg] != 0;
      }
      if (!has_output)
        return Fail(error, "structured final-state slice has no output");
      for (u32 reg = 1; reg < 16; reg++) {
        if ((m_kernel.vi.written_mask & (1u << reg)) == 0)
          continue;
        if (m_kernel.vi.AddressesForRegister(reg).empty())
          return Fail(error, "structured final state has absent VI evolution");
        const AffineQwordAddress& final =
            m_kernel.vi.AddressesForRegister(reg).back();
        if (!final.valid ||
            (final.base_vi != 0 &&
             (m_enclosing_boundary->demanded_vi_mask &
              (1u << final.base_vi)) == 0)) {
          return Fail(error,
                      "structured final state has non-affine or absent VI state");
        }
      }
      m_program->execution_kind =
          GeneratedCgExecutionKind::StructuredFinalState;
      m_program->uses_structured_final_state = true;
      m_program->structured_final_control_owner =
          m_structured_final_control;
      m_program->uses_final_state_output = true;
      m_program->final_vf_lanes = m_kernel.final_vf_lanes;
      m_program->final_acc_lanes = m_kernel.final_acc_lanes;
      m_program->final_q = m_kernel.final_q;
      m_program->final_p = m_kernel.final_p;
      m_program->final_i = m_kernel.final_i;
      m_program->child_entry_vi_mask =
          m_enclosing_boundary->demanded_vi_mask;
      m_program->maximum_structured_iterations =
          m_maximum_structured_iterations;
      m_program->maximum_structured_child_iterations =
          m_maximum_structured_child_iterations;
    } else if (m_output_kind == CgOutputKind::FinalStateBuffer ||
        m_output_kind == CgOutputKind::StructuredStateSnapshots) {
      if (!m_kernel.independent_final_state)
        return Fail(error,
                    "parallel final-state Cg root requires an independent "
                    "exit-state slice");
      const bool has_final_state =
          std::any_of(m_kernel.final_vf_lanes.begin(),
                      m_kernel.final_vf_lanes.end(),
                      [](u8 lanes) { return lanes != 0; }) ||
          m_kernel.final_acc_lanes != 0 || m_kernel.final_q ||
          m_kernel.final_p || m_kernel.final_i ||
          (m_output_kind == CgOutputKind::StructuredStateSnapshots &&
           m_enclosing_boundary &&
           (m_enclosing_boundary->parent_live_vi_mask != 0 ||
            std::any_of(m_kernel.child_entry_vf_lanes.begin(),
                        m_kernel.child_entry_vf_lanes.end(),
                        [](u8 lanes) { return lanes != 0; }) ||
            m_kernel.child_entry_acc_lanes != 0 || m_kernel.child_entry_q ||
            m_kernel.child_entry_p || m_kernel.child_entry_i));
      if (!has_final_state)
        return Fail(error, "parallel final-state Cg root has no live output");
      m_program->final_vf_lanes = m_kernel.final_vf_lanes;
      m_program->final_acc_lanes = m_kernel.final_acc_lanes;
      m_program->final_q = m_kernel.final_q;
      m_program->final_p = m_kernel.final_p;
      m_program->final_i = m_kernel.final_i;
      m_program->uses_final_state_output = true;
      if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
        if (!m_child_loop || !m_enclosing_loop || !m_enclosing_boundary ||
            !m_structured_tail ||
            !m_structured_tail->exact_final_resume_proven ||
            !m_structured_tail->intermediate_suffix_proven ||
            m_maximum_structured_iterations == 0 ||
            m_maximum_structured_iterations > 64 ||
            m_maximum_structured_child_iterations == 0 ||
            m_maximum_structured_child_iterations >
                MaximumStructuredChildIterations) {
          return Fail(error,
                      "structured state Cg root has an invalid iteration "
                      "bound");
        }
        if (!m_kernel.enclosing_prefix_inlined ||
            !m_kernel.enclosing_suffix_inlined ||
            !m_kernel.independent_child_entry_state ||
            !m_kernel.independent_child_entry_vi ||
            !m_kernel.enclosing_final_vi_state_pass_through) {
          return Fail(error,
                      "structured state Cg root requires a pass-through "
                      "child transition");
        }
        bool state_slice_valid = true;
        for (u32 reg = 1; reg < 32; reg++) {
          state_slice_valid &=
              (m_kernel.child_entry_vf_lanes[reg] &
               ~m_enclosing_boundary->demanded_vf_lanes[reg]) == 0;
          state_slice_valid &=
              (m_kernel.final_vf_lanes[reg] &
               ~m_enclosing_boundary->parent_live_vf_lanes[reg]) == 0;
        }
        state_slice_valid &=
            (m_kernel.child_entry_acc_lanes &
             ~m_enclosing_boundary->demanded_acc_lanes) == 0;
        state_slice_valid &=
            (m_kernel.final_acc_lanes &
             ~m_enclosing_boundary->parent_live_acc_lanes) == 0;
        state_slice_valid &=
            !m_kernel.child_entry_q || m_enclosing_boundary->demanded_q;
        state_slice_valid &=
            !m_kernel.final_q || m_enclosing_boundary->parent_live_q;
        state_slice_valid &=
            !m_kernel.child_entry_p || m_enclosing_boundary->demanded_p;
        state_slice_valid &=
            !m_kernel.final_p || m_enclosing_boundary->parent_live_p;
        state_slice_valid &=
            !m_kernel.child_entry_i || m_enclosing_boundary->demanded_i;
        state_slice_valid &=
            !m_kernel.final_i || m_enclosing_boundary->parent_live_i;
        if (!state_slice_valid) {
          return Fail(error,
                      "structured state Cg root boundary metadata mismatch");
        }
        if (m_kernel.child_entry_vi_mask !=
            static_cast<u16>(m_enclosing_boundary->demanded_vi_mask |
                             m_enclosing_boundary->parent_live_vi_mask)) {
          return Fail(error,
                      "structured state Cg root VI boundary mismatch");
        }
        const LowerKind child_branch_kind =
            static_cast<LowerKind>(m_child_loop->branch_kind);
        if (child_branch_kind != LowerKind::IBNE ||
            !m_child_loop->branch_taken_repeats ||
            (m_child_loop->counter_step != 1 &&
             m_child_loop->counter_step != -1) ||
            m_child_loop->counter_reg == 0 ||
            (m_kernel.child_entry_vi_mask &
             (1u << m_child_loop->counter_reg)) == 0 ||
            (m_child_loop->counter_limit_reg != 0 &&
             (m_kernel.child_entry_vi_mask &
              (1u << m_child_loop->counter_limit_reg)) == 0)) {
          return Fail(error,
                      "structured state Cg root requires a bounded canonical "
                      "child IBNE loop");
        }
        const LowerKind branch_kind =
            static_cast<LowerKind>(m_enclosing_loop->branch_kind);
        if (branch_kind != LowerKind::IBEQ && branch_kind != LowerKind::IBNE &&
            branch_kind != LowerKind::IBLTZ &&
            branch_kind != LowerKind::IBGTZ &&
            branch_kind != LowerKind::IBLEZ &&
            branch_kind != LowerKind::IBGEZ)
          return Fail(error,
                      "structured state Cg root requires canonical VI control");
        const u64 maximum_pairs_per_outer =
            static_cast<u64>(m_structured_parent_prefix_pairs) +
            static_cast<u64>(m_maximum_structured_child_iterations) *
                m_child_loop->pair_count +
            m_structured_tail->summarized_pair_count;
        const u64 maximum_structured_pairs = maximum_pairs_per_outer *
            m_maximum_structured_iterations;
        if (maximum_structured_pairs == 0 ||
            maximum_structured_pairs > std::numeric_limits<u32>::max()) {
          return Fail(error,
                      "structured state Cg pair bound overflows its ABI");
        }
        m_program->uses_structured_state_snapshots = true;
        m_program->structured_state_control_owner =
            m_structured_state_control;
        m_program->execution_kind =
            GeneratedCgExecutionKind::StructuredStateSnapshots;
        m_program->child_entry_vf_lanes =
            m_kernel.child_entry_vf_lanes;
        m_program->child_entry_vi_mask =
            m_enclosing_boundary->demanded_vi_mask;
        m_program->child_entry_acc_lanes =
            m_kernel.child_entry_acc_lanes;
        m_program->child_entry_q = m_kernel.child_entry_q;
        m_program->child_entry_p = m_kernel.child_entry_p;
        m_program->child_entry_i = m_kernel.child_entry_i;
        m_program->maximum_structured_iterations =
            m_maximum_structured_iterations;
        m_program->maximum_structured_child_iterations =
            m_maximum_structured_child_iterations;
        m_program->structured_entry_pc = m_structured_entry_pc;
        m_program->structured_tail_pc =
            m_structured_tail->final_resume_pc;
        m_program->structured_parent_prefix_pairs =
            m_structured_parent_prefix_pairs;
        m_program->structured_child_pairs = m_child_loop->pair_count;
        m_program->structured_suffix_pairs =
            m_structured_tail->summarized_pair_count;
        m_program->structured_pair_upper_bound =
            static_cast<u32>(maximum_structured_pairs);
        for (u32 reg = 1; reg < m_kernel.final_vf_lanes.size(); reg++) {
          if (m_kernel.final_vf_lanes[reg] != 0)
            m_program->vf_uniform_mask |= 1u << reg;
        }
        m_program->vi_uniform_mask = static_cast<u16>(
            m_enclosing_boundary->parent_live_vi_mask &
            ~(1u << m_enclosing_loop->counter_reg));
        if (m_enclosing_loop->counter_limit_reg != 0) {
          m_program->vi_uniform_mask &= static_cast<u16>(
              ~(1u << m_enclosing_loop->counter_limit_reg));
        }
        for (u32 reg = 1; reg < 16; reg++) {
          const AffineViValue& value = m_kernel.child_entry_vi_values[reg];
          if ((m_kernel.child_entry_vi_mask & (1u << reg)) == 0 ||
              value.base_vi == 0 ||
              value.base_vi == m_enclosing_loop->counter_reg ||
              value.base_vi == m_enclosing_loop->counter_limit_reg) {
            continue;
          }
          if ((m_enclosing_boundary->parent_live_vi_mask &
               (1u << value.base_vi)) == 0) {
            return Fail(error,
                        "structured child VI formula has an unbound parent "
                        "base");
          }
        }
        m_program->uses_acc_uniform |= m_kernel.final_acc_lanes != 0;
        m_program->uses_q_uniform |= m_kernel.final_q;
        m_program->uses_p_uniform |= m_kernel.final_p;
        m_program->uses_i_uniform |= m_kernel.final_i;
      }
    } else if (!m_kernel.independent_store_values) {
      return Fail(error, "parallel Cg root requires an independent loop slice");
    }
    if ((m_output_kind == CgOutputKind::StoreVaryings ||
         m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore ||
         IsDirectTfxOutput()) &&
        m_kernel.stores.empty())
      return Fail(error, "parallel Cg root has no VU stores");
    if (m_output_kind == CgOutputKind::StoreVaryings &&
        m_kernel.stores.size() > MaximumValidationStores) {
      return Fail(error, "parallel validation root exceeds varying capacity");
    }

    m_reachable.resize(m_kernel.expressions.size());
    m_flat_color_reachable.resize(m_kernel.expressions.size());
    m_exact_reachable.resize(m_kernel.expressions.size());
    if (IsDirectTfxOutput()) {
      if (!ConfigureDirectInputLowering(error))
        return false;
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
      if (m_exact_depth_analysis) {
        if (!m_direct_contract->has_position ||
            !MarkPacked(m_direct_contract->depth, &m_exact_reachable, error))
          return Fail(error, "depth precision analysis requires a proven depth cone");
        if (!m_exact_fmac_analysis_nodes.empty()) {
          if (m_exact_fmac_analysis_nodes.size() != m_exact_reachable.size())
            return Fail(error, "depth precision analysis mask has wrong extent");
          for (u32 id = 0u; id < m_exact_reachable.size(); ++id) {
            if (m_exact_fmac_analysis_nodes[id] > 1u ||
                (m_exact_fmac_analysis_nodes[id] && !m_exact_reachable[id]))
              return Fail(error, "depth precision analysis mask escapes depth cone");
            m_exact_reachable[id] = m_exact_fmac_analysis_nodes[id] != 0u;
          }
        }
      }
#endif
      if (!m_program->uses_loop_kernel_state_canary && !MarkDirectRoots(error))
        return false;
      if (m_output_kind == CgOutputKind::LoopKernelDirectTfx) {
        // BUFFER2 canaries must evaluate every PairPlan store cone. A no-write
        // product has no SGX-owned state publication: the exact transactional
        // successor remains the sole VU state/store owner, so retaining those
        // cones would execute dead VU work once per raster vertex.
        if (m_emit_loop_kernel_private_output) {
          for (const LoopStore& store : m_kernel.stores) {
            for (u32 lane = 0; lane < 4; lane++) {
              if ((store.write_mask & (0x8u >> lane)) == 0u)
                continue;
              if (!MarkReachable(store.values[lane], error)) {
                return false;
              }
            }
          }
        }
      }
      m_program->uses_tfx_uniforms = !m_program->uses_loop_kernel_state_canary;
      m_program->uses_tfx_point_size =
          m_program->uses_loop_kernel_state_canary ||
          m_direct_contract->primitive == GS_POINTLIST;
      m_program->uses_tfx_uv_no_fog_interface =
          !m_program->uses_loop_kernel_state_canary && m_direct_contract->textured &&
          m_direct_contract->fixed_texture_coordinates &&
          !m_direct_contract->fog_enabled;
      m_program->uses_gif_q_uniform =
          !m_program->uses_loop_kernel_state_canary && m_direct_contract->textured &&
          m_direct_contract->fixed_texture_coordinates &&
          !m_program->uses_tfx_uv_no_fog_interface;
    } else if (m_output_kind == CgOutputKind::StoreVaryings ||
               m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore) {
      for (const LoopStore &store : m_kernel.stores) {
        for (u32 lane = 0; lane < 4; lane++) {
          if ((store.write_mask & (0x8u >> lane)) != 0 &&
              !MarkReachable(store.values[lane], error)) {
            return false;
          }
        }
      }
    } else if (m_output_kind == CgOutputKind::StructuredExpressionScratch) {
      for (const StructuredScratchOutput& output :
           m_kernel.structured_scratch_outputs) {
        if (!MarkReachable(output.expression, error))
          return false;
      }
    } else if (!MarkFinalStateRoots(error) ||
               (m_output_kind == CgOutputKind::StructuredStateSnapshots &&
                !MarkChildEntryRoots(error))) {
      return false;
    }

    if (!CollectStructuredScratchDependencies(error))
      return false;

    CollectResources();
    if (m_batch_varying_live_ins && m_batch_varying_input_layout) {
      GeneratedBatchInputIdentityMap input_map;
      if (!input_map.Configure(*m_batch_varying_input_layout, *m_program, error) ||
          !input_map.Project(*m_batch_varying_live_ins,
                             &m_projected_batch_varying_live_ins, error)) {
        return false;
      }
      m_batch_varying_live_ins = &m_projected_batch_varying_live_ins;
    }
    // ABI 26 keeps fixed live-ins at record zero. Physical ABI-27 evidence
    // showed that streaming every live-in made the hot root spill 17 KiB, so
    // only bit-identical descriptors may currently share one INDEX draw.
    m_program->uses_dynamic_batch_uniform_index = false;
    m_program->uses_instance_indexed_batch_live_ins = false;
    if (m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
        m_batch_varying_live_ins && m_batch_varying_live_ins->Any()) {
      const u64 valid_constant_mask =
          m_program->constant_inputs.size() >= 64u
              ? std::numeric_limits<u64>::max()
              : ((1ull << m_program->constant_inputs.size()) - 1ull);
      const bool uses_scalars = m_program->uses_q_uniform ||
          m_program->uses_p_uniform || m_program->uses_i_uniform ||
          m_program->uses_gif_q_uniform;
      if ((m_batch_varying_live_ins->constant_mask &
           ~valid_constant_mask) != 0u ||
          (m_batch_varying_live_ins->vf_mask &
           ~m_program->vf_uniform_mask) != 0u ||
          (m_batch_varying_live_ins->vf_mask & 1u) != 0u ||
          (m_batch_varying_live_ins->acc &&
           !m_program->uses_acc_uniform) ||
          (m_batch_varying_live_ins->scalars && !uses_scalars)) {
        return Fail(error,
                    "partial batch live-ins exceed the generated root's "
                    "semantic input set");
      }
      m_program->batch_varying_live_ins = *m_batch_varying_live_ins;
      if (m_program->BatchVaryingLiveInVectorCount() == 0u ||
          m_program->BatchVaryingLiveInVectorCount() > 4u) {
        return Fail(error,
                    "partial batch live-ins exceed the compiler-bounded "
                    "BUFFER4 class");
      }
      m_program->loop_kernel_source_abi =
          GeneratedLoopKernelPartialBatchCgAbiVersion;
    }
    m_program->uses_structured_native_f32 = UsesAnyStructuredNativeF32();
    m_program->emitted_software_f32_operation_count =
        StructuredSoftwareF32OperationCost();
    m_program->uses_software_f32_resource_helpers =
        UsesAnyStructuredExactF32() &&
        (Uses(ExpressionKind::ArmApproximateReciprocal) ||
         Uses(ExpressionKind::ArmApproximateSquareRoot));
    if (UsesStructuredSoftwareF32Output() &&
        (Uses(ExpressionKind::RoundedAdd) ||
         Uses(ExpressionKind::RoundedSubtract) ||
         Uses(ExpressionKind::RoundedMultiply) ||
         Uses(ExpressionKind::EfuSumXyzSquares) ||
         Uses(ExpressionKind::ArmApproximateReciprocal) ||
         Uses(ExpressionKind::ArmApproximateSquareRoot)) &&
        !IsGeneratedCgSoftwareF32ConfigurationSupported(
            m_kernel.configuration_bits)) {
      return Fail(error,
                  "structured generated arithmetic has an unsupported "
                  "binary32 profile");
    }
    if (m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
        Uses(ExpressionKind::Divide) &&
        !UsesLeanNativeOutputProfile() &&
        !IsGeneratedCgExactDivideConfigurationSupported(
            m_kernel.configuration_bits)) {
      return Fail(error,
                  "generated DIV has no exact-Q binary32 profile");
    }
    if (m_emit_loop_kernel_private_output &&
        !m_program->uses_loop_kernel_state_canary &&
        m_kernel.enable_private_ftoi_probe &&
        m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
        Uses(ExpressionKind::Divide) && UsesStructuredNativeF32()) {
      // The exact-Q and branch-free FTOI helpers have independent physical
      // gates. Probe the first architectural FTOI store which consumes a
      // rounded multiply even when that exact helper is selected: BUFFER3
      // then identifies whether a private-journal mismatch begins before the
      // multiply, in native SGX FMAC, or at conversion. This is a title-neutral
      // diagnostic boundary and never publishes GPU state.
      for (u32 store_index = 0u; store_index < m_kernel.stores.size() &&
                                m_ftoi_probe_node == InvalidNode;
           store_index++) {
        const LoopStore& store = m_kernel.stores[store_index];
        for (u32 lane = 0u; lane < store.values.size(); lane++) {
          if ((store.write_mask & (0x8u >> lane)) == 0u)
            continue;
          const u32 node_id = store.values[lane];
          if (node_id == InvalidNode || node_id >= m_kernel.expressions.size() ||
              !m_reachable[node_id]) {
            continue;
          }
          const ExpressionNode& ftoi = m_kernel.expressions[node_id];
          if (ftoi.kind != ExpressionKind::FloatToInt ||
              (ftoi.immediate != 0u && ftoi.immediate != 4u &&
               ftoi.immediate != 12u && ftoi.immediate != 15u) ||
              ftoi.operands[0] == InvalidNode ||
              ftoi.operands[0] >= m_kernel.expressions.size() ||
              m_kernel.expressions[ftoi.operands[0]].kind !=
                  ExpressionKind::RoundedMultiply) {
            continue;
          }
          m_ftoi_probe_node = node_id;
          m_program->uses_loop_kernel_ftoi_probe_output = true;
          m_program->loop_kernel_ftoi_probe_configuration_bits =
              m_kernel.configuration_bits;
          m_program->loop_kernel_ftoi_probe_store_index =
              static_cast<u8>(store_index);
          m_program->loop_kernel_ftoi_probe_lane = static_cast<u8>(lane);
          m_program->loop_kernel_ftoi_probe_scale_offset =
              static_cast<u8>(ftoi.immediate);
          break;
        }
      }
    }
    m_program->uses_sink_scheduled_outputs =
        m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
        m_program->uses_nested_iteration_grid &&
        (m_program->uses_loop_kernel_state_canary || UsesLeanNativeOutputProfile() ||
         m_program->RequiresExactPartialSinkSchedule() ||
         m_program->BatchUniformVectorCount() >
             GeneratedCgProgram::SinkScheduledOutputUniformThreshold) &&
        !m_program->uses_loop_kernel_ftoi_probe_output;
    if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
      for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
        if (!m_reachable[node_id])
          continue;
        const ExpressionNode& node = m_kernel.expressions[node_id];
        if (node.kind != ExpressionKind::Memory)
          continue;
        const AffineQwordAddress& address = node.memory_address;
        if (!address.valid || address.invocation_coefficient != 0) {
          return Fail(error,
                      "structured state prefix has a non-parent memory "
                      "address");
        }
        if (address.base_vi != 0 &&
            address.base_vi != m_enclosing_loop->counter_reg &&
            address.base_vi != m_enclosing_loop->counter_limit_reg &&
            (m_program->vi_uniform_mask & (1u << address.base_vi)) == 0) {
          return Fail(error,
                      "structured state prefix reads an unbound parent VI "
                      "base");
        }
      }
    }
    if (m_output_kind == CgOutputKind::StructuredExpressionScratch ||
        m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore ||
        m_output_kind == CgOutputKind::StructuredParallelDirectTfx ||
        m_output_kind == CgOutputKind::StructuredFinalState) {
      for (u32 reg = 1; reg < 32; reg++) {
        if ((m_program->vf_uniform_mask & (1u << reg)) != 0 &&
            m_enclosing_boundary->demanded_vf_lanes[reg] == 0) {
          return Fail(error,
                      "structured generated stage reads an absent VF "
                      "snapshot");
        }
      }
      if ((m_program->uses_acc_uniform &&
           m_enclosing_boundary->demanded_acc_lanes == 0) ||
          (m_program->uses_q_uniform && !m_enclosing_boundary->demanded_q) ||
          (m_program->uses_p_uniform && !m_enclosing_boundary->demanded_p) ||
          (m_program->uses_i_uniform && !m_enclosing_boundary->demanded_i)) {
        return Fail(error,
                    "structured generated stage reads an absent scalar "
                    "snapshot");
      }
      for (const ExpressionNode& node : m_kernel.expressions) {
        if (node.kind == ExpressionKind::Memory &&
            node.memory_address.base_vi != 0 &&
            (m_enclosing_boundary->demanded_vi_mask &
             (1u << node.memory_address.base_vi)) == 0) {
          return Fail(error,
                      "structured generated stage reads an absent VI base");
        }
      }
      if (m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore) {
        for (const LoopStore& store : m_kernel.stores) {
          if (store.address.base_vi != 0 &&
              (m_enclosing_boundary->demanded_vi_mask &
               (1u << store.address.base_vi)) == 0) {
            return Fail(error,
                        "structured private-memory store writes through an "
                        "absent VI base");
          }
        }
      }
    }
    if (m_program->uses_buffered_batch_inputs &&
        m_program->memory_inputs.empty()) {
      return Fail(error,
                  "parallel buffered Cg root has no raw VIF inputs");
    }
    if (!m_program->uses_buffered_batch_inputs &&
        m_program->memory_inputs.size() > MaximumMemoryInputs)
      return Fail(error, "parallel Cg root exceeds GXM vertex input capacity");
    if (m_program->constant_inputs.size() > MaximumConstantInputs)
      return Fail(error, "parallel Cg root exceeds constant-input capacity");
    u32 attribute_count = 0;
    for (const CgMemoryInput &input : m_program->memory_inputs) {
      if (!UsesExpandedFlatInputs()) {
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

    if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
      m_source += m_program->uses_structured_native_f32
          ? StructuredNativeStateOptimizationMarker
          : StructuredOptimizationMarker;
    } else if (m_output_kind == CgOutputKind::StructuredExpressionScratch ||
               m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore ||
               m_output_kind == CgOutputKind::StructuredParallelDirectTfx ||
               m_output_kind == CgOutputKind::StructuredFinalState) {
      // Offline SDK 3.5 characterization of the dependency-partitioned roots
      // found exact add/multiply modules compile in roughly 0.5--1.8 seconds
      // at O0 but take 8--19+ seconds or time out at O1.  Native/simple
      // structured roots retain O1; exact integer correction roots use their
      // separately bounded O0 compiler class. Native-F32 roots use O3: offline
      // SDK 3.570 characterization reduced the captured recurrence from 461 to
      // 332 primary instructions and its fused store from 1324 to 890 without
      // introducing scratch or per-thread backing.
      m_source += m_program->emitted_software_f32_operation_count != 0u
          ? StructuredExactOptimizationMarker
          : (m_program->uses_structured_native_f32
                 ? StructuredNativeParallelOptimizationMarker
                 : StructuredParallelOptimizationMarker);
    }
    if (UsesCompilerSafeScalarVectorOperations()) {
      m_source +=
          "#define VITASX2_GPU_VU_SCALAR_VECTOR_OPERATIONS 1\n";
    }
    AppendStructuredScratchDependencyContract();
    // Helper emission precedes the expression body. Build the same
    // deterministic lane-isomorphism sets here so optional float4 helpers are
    // present only when a reachable vector expression actually calls them.
    // Leaving an unused branch-heavy helper in source changes Sony O1's GXP
    // shape and has made psp2shaderperf non-terminating on otherwise small
    // final-state roots.
    BuildVectorGroups(m_reachable, false);
    BuildVectorGroups(m_flat_color_reachable, true);
    AppendWritableBufferContract();
    AppendBindingContract();
    AppendHelpers();
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
    if (m_normalized_fmac_inputs_analysis && !PrepareNormalizedFmacHelpers(error))
      return false;
#endif
    AppendEntrySignature();
    AppendLeanOuterRepeatedAddScheduleCounts();
    AppendStructuredStatePrelude();
    AppendStructuredStorePrelude();
    AppendStructuredInactiveGuard();
    if (m_program->uses_sink_scheduled_outputs)
      AppendLoopKernelSinkDeclarations();
    AppendExpressions(error);
    if (error && !error->empty())
      return false;
    if (!ValidateLazyVaryingConstantDeclarations(error))
      return false;
    if (m_program->uses_sink_scheduled_outputs &&
        m_sink_scheduled_assignment_count !=
            LoopKernelSinkAssignmentCount()) {
      return Fail(error,
                  "loop-kernel sink scheduler did not publish every output");
    }
    if (m_output_kind == CgOutputKind::StructuredExpressionScratch ||
        m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore) {
      if (m_output_kind == CgOutputKind::StructuredExpressionScratch)
        AppendStructuredScratchWrites();
      else
        AppendStructuredStoreWrites();
      m_source +=
          "\tVuPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n"
          "\tVuResult = float4(float(VuOuterIteration), "
          "float(VuChildIteration), 1.0f, 0.0f);\n";
    } else {
      AppendOutputs();
    }
    m_source += "}\n";

    if (m_source.size() > MaximumGeneratedSourceBytes)
      return Fail(error, "generated parallel Cg source exceeds compiler bound");
    m_program->source = std::move(m_source);
    if (error)
      error->clear();
    return true;
  }

private:
  bool IsDirectTfxOutput() const {
    return m_output_kind == CgOutputKind::DirectTfx ||
           m_output_kind == CgOutputKind::LoopKernelDirectTfx ||
           m_output_kind == CgOutputKind::StructuredParallelDirectTfx;
  }

  bool UsesExpandedFlatInputs() const {
    return m_program->uses_flat_instance_inputs ||
           m_program->uses_flat_index_inputs;
  }

  bool UsesNestedIterationGrid() const {
    return m_program->uses_nested_iteration_grid;
  }

  static const char* OuterIterationName(bool flat_color) {
    return flat_color ? "VuFlatOuterIteration" : "VuOuterIteration";
  }

  static const char* ChildIterationName(bool flat_color) {
    return flat_color ? "VuFlatChildIteration" : "VuChildIteration";
  }

  bool UsesLazySinkScheduledVaryingConstants() const {
    // The product ABI is assigned after CgEmitter finishes. Its source-time
    // identity is the lean no-private-output profile, not the temporary ABI-51
    // value used to describe the common inline BUFFER1 tail while emitting.
    return m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
           m_native_output_only_profile &&
           !m_emit_loop_kernel_private_output &&
           m_program->uses_sink_scheduled_outputs &&
           m_program->UsesInlineBatchVaryingTail() &&
           m_program->batch_varying_live_ins.constant_mask != 0u;
  }

  bool UsesBranchlessLoopKernelControl() const {
    return m_output_kind == CgOutputKind::LoopKernelDirectTfx;
  }

  bool IsStructuredParallelChildOutput() const {
    return m_output_kind == CgOutputKind::StructuredExpressionScratch ||
           m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore ||
           m_output_kind == CgOutputKind::StructuredParallelDirectTfx;
  }

  bool IsStructuredSnapshotConsumer() const {
    return IsStructuredParallelChildOutput() ||
           m_output_kind == CgOutputKind::StructuredFinalState;
  }

  bool CollectStructuredScratchDependencies(std::string* error) {
    for (u32 node_id = 1u; node_id < m_kernel.expressions.size(); node_id++) {
      if ((!m_reachable[node_id] && !m_flat_color_reachable[node_id]) ||
          m_kernel.expressions[node_id].kind !=
              ExpressionKind::StructuredScratch) {
        continue;
      }
      const u32 slot = m_kernel.expressions[node_id].immediate;
      if (slot >= StructuredGeneratedScratchSlots) {
        return Fail(error,
                    "structured generated stage reads an invalid scratch "
                    "slot");
      }
      m_program->structured_scratch_read_mask[slot / 32u] |=
          1u << (slot % 32u);
    }
    for (const StructuredScratchOutput& output :
         m_kernel.structured_scratch_outputs) {
      if (output.slot >= StructuredGeneratedScratchSlots) {
        return Fail(error,
                    "structured generated stage writes an invalid scratch "
                    "slot");
      }
      m_program->structured_scratch_write_mask[output.slot / 32u] |=
          1u << (output.slot % 32u);
    }
    return true;
  }

  void AppendStructuredScratchDependencyContract() {
    for (u32 slot = 0u; slot < StructuredGeneratedScratchSlots; slot++) {
      const u32 bit = 1u << (slot % 32u);
      if ((m_program->structured_scratch_read_mask[slot / 32u] & bit) != 0u) {
        m_source += "// VitaSX2 generated scratch read slot ";
        m_source += std::to_string(slot);
        m_source += "\n";
      }
      if ((m_program->structured_scratch_write_mask[slot / 32u] & bit) != 0u) {
        m_source += "// VitaSX2 generated scratch write slot ";
        m_source += std::to_string(slot);
        m_source += "\n";
      }
    }
  }

  void AppendWritableBufferContract() {
    if (m_output_kind == CgOutputKind::LoopKernelDirectTfx) {
      if (!m_emit_loop_kernel_private_output)
        return;
      m_source +=
          "#pragma warning (default:7203)\n"
          "#pragma readwrite_buffer BUFFER2\n";
      if (m_program->uses_loop_kernel_ftoi_probe_output)
        m_source += "#pragma readwrite_buffer BUFFER3\n";
      return;
    }
    if (m_output_kind == CgOutputKind::StructuredExpressionScratch) {
      m_source +=
          "#pragma warning (default:7203)\n"
          "#pragma readwrite_buffer BUFFER10\n";
      return;
    }
    if (m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore) {
      m_source +=
          "#pragma warning (default:7203)\n"
          "#pragma readwrite_buffer BUFFER8\n";
      return;
    }
    if (m_output_kind == CgOutputKind::StructuredFinalState) {
      m_source +=
          "#pragma warning (default:7203)\n"
          "#pragma readwrite_buffer BUFFER8\n";
      // Final VF slices share one firmware job because their mapped writes are
      // disjoint. Only the control slice publishes architectural scalar/control
      // state through BUFFER9.  Inputs remain read-only on BUFFER1/2; the host
      // switches to this distinct private generation only after the final slice
      // group is flushed.
      if (m_structured_final_control)
        m_source += "#pragma readwrite_buffer BUFFER9\n";
      return;
    }
    if (m_output_kind != CgOutputKind::FinalStateBuffer &&
        m_output_kind != CgOutputKind::StructuredStateSnapshots)
      return;
    // PSP2 Shader Compiler User's Guide: a BUFFER parameter is read-only
    // unless this pragma gives its stores architectural side effects. Without
    // it psp2cgc legitimately reduced the first transition fixture to an
    // empty 232-byte vertex program.
    m_source += "#pragma readwrite_buffer BUFFER11\n";
    if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
      // Product submission initializes both private VU generations from the
      // same committed source before this root runs.  Recopying 1,024 qwords
      // here made an unrelated memory loop part of every generated state
      // compiler root and spent SGX bandwidth without changing ownership.
      m_source += "#pragma readwrite_buffer BUFFER12\n";
      m_source += "#pragma readwrite_buffer BUFFER13\n";
      return;
    }
    if (m_kernel.final_acc_lanes != 0)
      m_source += "#pragma readwrite_buffer BUFFER12\n";
    if (m_kernel.final_q || m_kernel.final_p || m_kernel.final_i)
      m_source += "#pragma readwrite_buffer BUFFER13\n";
  }

  bool ConfigureDirectInputLowering(std::string *error) {
    if (m_kernel.outer_iteration_count != 0u ||
        m_kernel.child_iteration_count != 0u) {
      // Only line strips currently have a nested expanded source ABI. Runtime
      // admission remains separate from source generation: a sparse ADC draw
      // does not execute every slot of the dense private journal (see TODO).
      if (!m_program->uses_loop_kernel_state_canary &&
          m_direct_contract && !m_direct_contract->gouraud &&
          m_direct_contract->primitive != GS_POINTLIST &&
          m_direct_contract->primitive != GS_LINESTRIP) {
        return Fail(error,
                    "nested flat TFX requires an expanded provoking-vertex ABI");
      }
      const u64 invocations =
          static_cast<u64>(m_kernel.outer_iteration_count) *
          m_kernel.child_iteration_count;
      if (m_output_kind != CgOutputKind::LoopKernelDirectTfx ||
          m_kernel.outer_iteration_count == 0u ||
          m_kernel.child_iteration_count == 0u ||
          m_kernel.outer_iteration_count >
              std::numeric_limits<u16>::max() ||
          m_kernel.child_iteration_count >
              std::numeric_limits<u16>::max() ||
          invocations != m_direct_contract->vertex_count ||
          invocations > (1u << 16u)) {
        return Fail(error,
                    "nested loop kernel has an invalid exact dispatch grid");
      }
      m_program->uses_nested_iteration_grid = true;
      m_program->uses_nested_batch_index_inputs = true;
      m_program->nested_outer_iterations =
          static_cast<u16>(m_kernel.outer_iteration_count);
      m_program->nested_child_iterations =
          static_cast<u16>(m_kernel.child_iteration_count);
      if ((invocations & (invocations - 1u)) != 0u) {
        u32 multiplier = 0u;
        u32 shift = 0u;
        if (!FindBoundedUnsignedDivisionMagic(
                static_cast<u32>(invocations), 1u << 16u,
                &multiplier, &shift)) {
          return Fail(error,
                      "nested loop kernel has no exact global batch-index map");
        }
      }
      if ((m_kernel.child_iteration_count &
           (m_kernel.child_iteration_count - 1u)) != 0u) {
        u32 multiplier = 0u;
        u32 shift = 0u;
        if (!FindBoundedUnsignedDivisionMagic(
                m_kernel.child_iteration_count,
                static_cast<u32>(invocations), &multiplier, &shift)) {
          return Fail(error,
                      "nested loop kernel has no exact SGX grid-index map");
        }
      }
      for (const ExpressionNode& node : m_kernel.expressions) {
        if (node.kind == ExpressionKind::OuterRepeatedAdd) {
          if (node.reg > 1u) {
            return Fail(error,
                        "outer repeated ADD has invalid operand order");
          }
          if (IsScheduledOuterRepeatedAdd(node.immediate)) {
            const u32 schedule_index =
                OuterRepeatedAddScheduleIndex(node.immediate);
            if (schedule_index >=
                    m_kernel.outer_repeated_add_schedules.size() ||
                m_kernel.outer_repeated_add_schedules[schedule_index]
                        .add_counts.size() !=
                    m_kernel.outer_iteration_count ||
                std::any_of(
                    m_kernel.outer_repeated_add_schedules[schedule_index]
                        .add_counts.begin(),
                    m_kernel.outer_repeated_add_schedules[schedule_index]
                        .add_counts.end(),
                    [](u8 count) { return count > 64u; })) {
              return Fail(error,
                          "outer repeated ADD schedule is outside its exact "
                          "grid");
            }
          } else if (node.immediate == 0u ||
                     node.immediate != m_kernel.outer_iteration_count) {
            return Fail(error,
                        "outer repeated ADD is outside its exact grid");
          }
          continue;
        }
        if (node.kind == ExpressionKind::CompactOuterInput) {
          if (node.immediate >= m_kernel.compact_outer_inputs.size() ||
              node.lane >= 4u ||
              m_kernel.compact_outer_inputs[node.immediate].sources.size() !=
                  m_kernel.outer_iteration_count) {
            return Fail(error,
                        "compact outer input is outside its exact grid");
          }
          continue;
        }
        if (node.kind != ExpressionKind::Memory)
          continue;
        if (!node.memory_address.valid ||
            node.memory_address.invocation_coefficient < 0 ||
            node.memory_address.outer_invocation_coefficient < 0) {
          return Fail(error,
                      "nested loop kernel has a non-forward input address");
        }
      }
      m_program->uses_buffered_batch_inputs = true;
      if (m_program->uses_loop_kernel_state_canary || m_direct_contract->gouraud ||
          m_direct_contract->primitive == GS_POINTLIST)
        return true;
    }
    if (m_program->uses_loop_kernel_state_canary) {
      return Fail(error, "state canary requires a complete nested invocation grid");
    }
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
    if (m_output_kind == CgOutputKind::LoopKernelDirectTfx)
      m_program->uses_flat_index_inputs = true;
    else
      m_program->uses_flat_instance_inputs = true;
    // Sony's skinning sample establishes dynamically indexed vertex uniform
    // buffers as the native way to read mapped arrays. One descriptor record
    // per VU dispatch lets a single instance range cross immutable VIF payload
    // boundaries without copying or CPU-unpacking any vertex.
    m_program->uses_buffered_batch_inputs = true;
    if (m_program->uses_flat_index_inputs) {
      constexpr u32 ExpandedIndexDomain = 1u << 16u;
      const u32 expanded_vertices_per_draw =
          static_cast<u32>(m_program->batch_primitives_per_draw) *
          m_program->flat_vertices_per_primitive;
      if (expanded_vertices_per_draw == 0u ||
          expanded_vertices_per_draw > ExpandedIndexDomain) {
        return Fail(error,
                    "flat loop kernel exceeds the GXM U16 index domain");
      }
      // Nested line strips first remove the endpoint bit. Dividing the full
      // U16 INDEX by 46/94/238 needs a wider reciprocal product; dividing its
      // 15-bit primitive index by 23/47/119 is exact with ordinary SGX u32 ALU.
      const u32 batch_divisor = UsesNestedIterationGrid()
          ? m_program->batch_primitives_per_draw : expanded_vertices_per_draw;
      const u32 batch_domain = UsesNestedIterationGrid()
          ? ExpandedIndexDomain / 2u : ExpandedIndexDomain;
      if ((batch_divisor & (batch_divisor - 1u)) != 0u) {
        u32 multiplier = 0u;
        u32 shift = 0u;
        if (!FindBoundedUnsignedDivisionMagic(
                batch_divisor, batch_domain,
                &multiplier, &shift)) {
          return Fail(error,
                      "flat loop kernel has no exact SGX batch-index map");
        }
      }
      if (m_program->flat_vertices_per_primitive == 3u) {
        u32 multiplier = 0u;
        u32 shift = 0u;
        if (!FindBoundedUnsignedDivisionMagic(
                3u, expanded_vertices_per_draw, &multiplier, &shift)) {
          return Fail(error,
                      "flat loop kernel has no exact SGX primitive-index map");
        }
      }
    }
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
        UsesExpandedFlatInputs() ? &m_flat_color_reachable : &m_reachable;
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

  bool MarkFinalStateRoots(std::string* error) {
    for (u32 reg = 1; reg < m_kernel.final_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if ((m_kernel.final_vf_lanes[reg] & (0x8u >> lane)) == 0)
          continue;
        if (!MarkReachable(m_kernel.final_vf_values[reg][lane], error))
          return false;
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if ((m_kernel.final_acc_lanes & (0x8u >> lane)) != 0 &&
          !MarkReachable(m_kernel.final_acc_values[lane], error)) {
        return false;
      }
    }
    return (!m_kernel.final_q ||
            MarkReachable(m_kernel.final_q_value, error)) &&
           (!m_kernel.final_p ||
            MarkReachable(m_kernel.final_p_value, error)) &&
           (!m_kernel.final_i ||
            MarkReachable(m_kernel.final_i_value, error));
  }

  bool MarkChildEntryRoots(std::string* error) {
    for (u32 reg = 1; reg < m_kernel.child_entry_vf_lanes.size(); reg++) {
      for (u32 lane = 0; lane < 4; lane++) {
        if ((m_kernel.child_entry_vf_lanes[reg] & (0x8u >> lane)) == 0)
          continue;
        if (!MarkReachable(m_kernel.child_entry_vf_values[reg][lane], error))
          return false;
      }
    }
    for (u32 lane = 0; lane < 4; lane++) {
      if ((m_kernel.child_entry_acc_lanes & (0x8u >> lane)) != 0 &&
          !MarkReachable(m_kernel.child_entry_acc_values[lane], error)) {
        return false;
      }
    }
    return (!m_kernel.child_entry_q ||
            MarkReachable(m_kernel.child_entry_q_value, error)) &&
           (!m_kernel.child_entry_p ||
            MarkReachable(m_kernel.child_entry_p_value, error)) &&
           (!m_kernel.child_entry_i ||
            MarkReachable(m_kernel.child_entry_i_value, error));
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
    std::map<u32, u32> compact_outer_indices;
    u32 packed_compact_memory_input = std::numeric_limits<u32>::max();
    for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
      if (!m_reachable[node_id] && !m_flat_color_reachable[node_id])
        continue;
      const ExpressionNode &node = m_kernel.expressions[node_id];
      switch (node.kind) {
      case ExpressionKind::Memory: {
        if (m_output_kind == CgOutputKind::StructuredStateSnapshots ||
            m_output_kind == CgOutputKind::StructuredExpressionScratch ||
            m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore ||
            m_output_kind == CgOutputKind::StructuredParallelDirectTfx ||
            m_output_kind == CgOutputKind::StructuredFinalState)
          break;
        const MemoryKey key = MakeMemoryKey(node.memory_address);
        if (node.memory_address.invocation_coefficient == 0 &&
            node.memory_address.outer_invocation_coefficient == 0) {
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
            key, static_cast<u32>(m_program->memory_inputs.size()));
        if (inserted) {
          m_program->memory_inputs.push_back(
              {node.memory_address, it->second});
        }
        m_memory_node_input[node_id] = it->second;
        if (UsesExpandedFlatInputs()) {
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
      case ExpressionKind::CompactOuterInput: {
        if (!UsesNestedIterationGrid() ||
            node.immediate >= m_kernel.compact_outer_inputs.size())
          break;
        const CompactOuterInputTable& table =
            m_kernel.compact_outer_inputs[node.immediate];
        if (table.sources.size() != m_kernel.outer_iteration_count)
          break;
        auto [it, inserted] = compact_outer_indices.emplace(
            node.immediate,
            static_cast<u32>(m_program->compact_outer_inputs.size()));
        if (inserted) {
          m_program->compact_outer_inputs.push_back(table);
        }
        if (packed_compact_memory_input ==
            std::numeric_limits<u32>::max()) {
          packed_compact_memory_input =
              static_cast<u32>(m_program->memory_inputs.size());
          AffineQwordAddress packed_address{0u, 0, 0, true, 0};
          m_program->memory_inputs.push_back(
              {packed_address, packed_compact_memory_input,
               CgMemoryInput::PackedCompactOuterTables});
        }
        m_memory_node_input[node_id] = packed_compact_memory_input;
        m_compact_outer_node_table[node_id] = it->second;
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
      case ExpressionKind::ArmApproximateReciprocal:
      case ExpressionKind::ArmApproximateSquareRoot:
        if (!UsesLeanNativeOutputProfile())
          m_program->uses_arm_estimate_table = true;
        break;
      case ExpressionKind::StructuredScratch:
        // Scratch producers and serial-store consumers already carry BUFFER10
        // explicitly.  Prepared final-state slices can also consume a value
        // staged by an earlier title-neutral partition, so discover that
        // dependency from the reachable IR rather than from the output kind.
        m_reads_structured_expression_scratch = true;
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

  bool UsesStructuredSoftwareF32Output() const {
    return m_output_kind == CgOutputKind::LoopKernelDirectTfx ||
           m_output_kind == CgOutputKind::StructuredStateSnapshots ||
           m_output_kind == CgOutputKind::StructuredExpressionScratch ||
           m_output_kind ==
               CgOutputKind::StructuredParallelChildMemoryStore ||
           m_output_kind == CgOutputKind::StructuredParallelDirectTfx ||
           m_output_kind == CgOutputKind::StructuredFinalState;
  }

  bool UsesStructuredSoftwareF32() const {
    return UsesStructuredSoftwareF32Output() &&
           IsGeneratedCgSoftwareF32ConfigurationSupported(
               m_kernel.configuration_bits);
  }

  bool UsesStructuredNativeF32() const {
    return UsesStructuredSoftwareF32Output() &&
           IsGeneratedCgNativeF32ConfigurationSupported(
               m_kernel.configuration_bits);
  }

  static bool IsProfiledF32Operation(ExpressionKind kind) {
    switch (kind) {
    case ExpressionKind::RoundedAdd:
    case ExpressionKind::RoundedSubtract:
    case ExpressionKind::RoundedMultiply:
    case ExpressionKind::OuterRepeatedAdd:
    case ExpressionKind::EfuSumXyzSquares:
    case ExpressionKind::ArmApproximateReciprocal:
    case ExpressionKind::ArmApproximateSquareRoot:
      return true;
    default:
      return false;
    }
  }

  bool NodeUsesStructuredExactF32(u32 node_id) const {
    if (!UsesStructuredSoftwareF32() ||
        node_id == InvalidNode || node_id >= m_kernel.expressions.size()) {
      return false;
    }
    const ExpressionKind kind = m_kernel.expressions[node_id].kind;
    if (!IsProfiledF32Operation(kind))
      return false;
    if (!UsesStructuredNativeF32())
      return true;
    if (m_output_kind != CgOutputKind::LoopKernelDirectTfx)
      return false;
    return node_id < m_exact_reachable.size() && m_exact_reachable[node_id];
  }

  bool NodeUsesStructuredNativeF32(u32 node_id) const {
    return UsesStructuredNativeF32() &&
           node_id != InvalidNode && node_id < m_kernel.expressions.size() &&
           IsProfiledF32Operation(m_kernel.expressions[node_id].kind) &&
           !NodeUsesStructuredExactF32(node_id);
  }

  bool UsesStructuredExactF32(ExpressionKind kind) const {
    for (u32 node_id = 1u; node_id < m_kernel.expressions.size(); node_id++) {
      if ((m_reachable[node_id] || m_flat_color_reachable[node_id]) &&
          m_kernel.expressions[node_id].kind == kind &&
          NodeUsesStructuredExactF32(node_id)) {
        return true;
      }
    }
    return false;
  }

  bool UsesStructuredNativeF32(ExpressionKind kind) const {
    for (u32 node_id = 1u; node_id < m_kernel.expressions.size(); node_id++) {
      if ((m_reachable[node_id] || m_flat_color_reachable[node_id]) &&
          m_kernel.expressions[node_id].kind == kind &&
          NodeUsesStructuredNativeF32(node_id)) {
        return true;
      }
    }
    return false;
  }

  bool UsesAnyStructuredExactF32() const {
    for (u32 node_id = 1u; node_id < m_kernel.expressions.size(); node_id++) {
      if ((m_reachable[node_id] || m_flat_color_reachable[node_id]) &&
          NodeUsesStructuredExactF32(node_id)) {
        return true;
      }
    }
    return false;
  }

  bool UsesAnyStructuredNativeF32() const {
    for (u32 node_id = 1u; node_id < m_kernel.expressions.size(); node_id++) {
      if ((m_reachable[node_id] || m_flat_color_reachable[node_id]) &&
          NodeUsesStructuredNativeF32(node_id)) {
        return true;
      }
    }
    return false;
  }

  bool UsesApproximateLoopKernelConversions() const {
    return (m_kernel.configuration_bits &
            UniversalConfigurationApproximateConversions) != 0u;
  }

  bool UsesLeanNativeOutputProfile() const {
    // This is an explicit product contract, not a consequence of BUFFER2
    // being absent. The canary owns exact control/address/state attestation;
    // this profile is restricted to the no-write raster output cone and needs
    // its own physical graphics and performance gate.
    return m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
           !m_emit_loop_kernel_private_output &&
           m_native_output_only_profile &&
           UsesApproximateLoopKernelConversions() &&
           UsesStructuredNativeF32();
  }

  bool UsesPlayableLoopKernelFloatToInt() const {
    // Keep the playable helper in validation roots only as a physical
    // characterization output. ABI 22 showed that ShaccCg lowers its
    // int(floor()) conversion through a pack.s16.f32 sequence which turns a
    // positive 32767.x input into 0x00008000. It therefore cannot own an
    // architectural store even when approximate conversions are enabled.
    return UsesApproximateLoopKernelConversions() &&
           UsesBranchlessLoopKernelControl();
  }

  bool IsPositiveZeroFloatNode(u32 node_id) const {
    return node_id < m_kernel.expressions.size() &&
           m_kernel.expressions[node_id].kind ==
               ExpressionKind::ConstantFloat &&
           m_kernel.expressions[node_id].immediate == 0u;
  }

  bool IsLeanIdentityOuterRepeatedAdd(const ExpressionNode& node) const {
    // OuterRepeatedAdd always carries its initial value in operand zero and
    // its repeated increment in operand one; `reg` records only whether the
    // architectural ADD carried the prior value on the left or right. The
    // no-write product already names native SGX arithmetic as an output-only
    // approximation. In that profile a canonical +0 increment contributes no
    // geometric value, so do not materialize an outer selector and a
    // multiply/add for every complete VF qword. The exact private canary does
    // not take this path: it retains every ordered add and therefore its PS2
    // signed-zero and normalization behavior.
    return UsesLeanNativeOutputProfile() &&
           node.kind == ExpressionKind::OuterRepeatedAdd &&
           IsPositiveZeroFloatNode(node.operands[1]);
  }

  u32 StructuredSoftwareF32OperationCost() const {
    if (!UsesStructuredSoftwareF32())
      return 0;
    u32 count = 0;
    for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
      if (!m_reachable[node_id] && !m_flat_color_reachable[node_id])
        continue;
      const ExpressionNode& node = m_kernel.expressions[node_id];
      switch (node.kind) {
      case ExpressionKind::RoundedAdd:
        if (!NodeUsesStructuredExactF32(node_id))
          break;
        // +VF00.x/y/z is still normalized exactly, but uses the compact
        // no-rounding helper rather than the compiler-hostile alignment path.
        if (!IsPositiveZeroFloatNode(node.operands[0]) &&
            !IsPositiveZeroFloatNode(node.operands[1]))
          count++;
        break;
      case ExpressionKind::RoundedSubtract:
      case ExpressionKind::RoundedMultiply:
        count += NodeUsesStructuredExactF32(node_id) ? 1u : 0u;
        break;
      case ExpressionKind::OuterRepeatedAdd:
        if (NodeUsesStructuredExactF32(node_id)) {
          if (IsScheduledOuterRepeatedAdd(node.immediate)) {
            const u32 schedule_index =
                OuterRepeatedAddScheduleIndex(node.immediate);
            if (schedule_index <
                m_kernel.outer_repeated_add_schedules.size()) {
              const auto& schedule =
                  m_kernel.outer_repeated_add_schedules[schedule_index];
              count += schedule.add_counts.empty()
                           ? 0u
                           : *std::max_element(schedule.add_counts.begin(),
                                               schedule.add_counts.end());
            }
          } else {
            count += m_kernel.outer_iteration_count > 0u
                         ? m_kernel.outer_iteration_count - 1u
                         : 0u;
          }
        }
        break;
      case ExpressionKind::EfuSumXyzSquares:
        count += NodeUsesStructuredExactF32(node_id) ? 5u : 0u;
        break;
      case ExpressionKind::ArmApproximateReciprocal:
        count += NodeUsesStructuredExactF32(node_id) ? 3u : 0u;
        break;
      case ExpressionKind::ArmApproximateSquareRoot:
        count += NodeUsesStructuredExactF32(node_id) ? 2u : 0u;
        break;
      default:
        break;
      }
    }
    return count;
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
    m_source += " outerCoefficients=";
    for (u32 index = 0; index < m_program->memory_inputs.size(); index++) {
      if (index != 0)
        m_source += ",";
      m_source += std::to_string(
          m_program->memory_inputs[index]
              .address.outer_invocation_coefficient);
    }
    m_source += " flatVertices=";
    m_source += std::to_string(m_program->flat_vertices_per_primitive);
    m_source += " flatStep=";
    m_source += std::to_string(m_program->flat_instance_vertex_step);
    m_source += " flatInstance=";
    m_source += m_program->uses_flat_instance_inputs ? "1" : "0";
    m_source += " flatIndex=";
    m_source += m_program->uses_flat_index_inputs ? "1" : "0";
    m_source += " batchPrimitives=";
    m_source += std::to_string(m_program->batch_primitives_per_draw);
    m_source += " bufferedBatch=";
    m_source += m_program->uses_buffered_batch_inputs ? "1" : "0";
    m_source += " nestedGrid=";
    m_source += m_program->uses_nested_iteration_grid ? "1" : "0";
    m_source += " nestedBatchIndex=";
    m_source += m_program->uses_nested_batch_index_inputs ? "1" : "0";
    m_source += " nestedOuter=";
    m_source += std::to_string(m_program->nested_outer_iterations);
    m_source += " nestedChild=";
    m_source += std::to_string(m_program->nested_child_iterations);
    m_source += " batchRecordVectors=";
    // BatchRecordVectorCount() is the physical BUFFER1 stride. ABI-54 and
    // ABI-51 already include their per-object varying tail in that value;
    // adding the tail again made the source identity claim a layout which the
    // emitted C struct and GS-side packer never used.
    m_source += std::to_string(m_program->BatchRecordVectorCount());
    if (m_program->batch_varying_live_ins.Any()) {
      m_source += " varyingConstants=";
      m_source += std::to_string(
          m_program->batch_varying_live_ins.constant_mask);
      m_source += " varyingVf=";
      m_source += std::to_string(m_program->batch_varying_live_ins.vf_mask);
      m_source += " varyingAcc=";
      m_source += m_program->batch_varying_live_ins.acc ? "1" : "0";
      m_source += " varyingScalars=";
      m_source += m_program->batch_varying_live_ins.scalars ? "1" : "0";
      m_source += " varyingVectors=";
      m_source +=
          std::to_string(m_program->BatchVaryingLiveInVectorCount());
    }
    if (m_program->uses_dynamic_batch_uniform_index)
      m_source += " dynamicBatchUniform=1";
    if (m_program->uses_sink_scheduled_outputs)
      m_source += " sinkScheduled=1";
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
      m_source += "/";
      m_source +=
          std::to_string(address.outer_invocation_coefficient);
    }
    m_source += "\n";
    m_source += "// GXM compact outer qwords=";
    for (u32 table_index = 0;
         table_index < m_program->compact_outer_inputs.size(); table_index++) {
      if (table_index != 0u)
        m_source += ";";
      const CompactOuterInputTable& table =
          m_program->compact_outer_inputs[table_index];
      for (u32 source_index = 0; source_index < table.sources.size();
           source_index++) {
        if (source_index != 0u)
          m_source += ",";
        const CompactQwordSource& source = table.sources[source_index];
        if (source.kind == CompactQwordSourceKind::InitialVf) {
          m_source += "v" + std::to_string(source.reg);
        } else {
          m_source += "m" + std::to_string(source.memory_address.base_vi) +
                      "/" +
                      std::to_string(source.memory_address.qword_offset);
        }
      }
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

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
  bool IsNormalizedFmacInput(u32 node_id) const {
    return node_id && node_id < m_kernel.expressions.size() &&
        IsGeneratedCgNormalizedF32Result(m_kernel.expressions[node_id],
                                        NodeUsesStructuredExactF32(node_id));
  }

  std::string PreparedFmacOperand(u32 node_id, std::string value) const {
    return IsNormalizedFmacInput(node_id) ? value
        : "VitaVuF32Prepare(" + value + ")";
  }

  bool PrepareNormalizedFmacHelpers(std::string* error) {
    if (!m_range_scaled_multiply_analysis || !UsesBranchlessLoopKernelControl() ||
        !UsesStructuredSoftwareF32() || UsesLeanNativeOutputProfile())
      return Fail(error, "normalized FMAC input analysis requires range-scaled exact depth");
    m_source += "#define VITASX2_GPU_VU_NORMALIZED_FMAC_INPUTS 1\n";
    // Specialize the canonical emitted arithmetic body, not a separately
    // maintained implementation. Preserve one heavy body; ordinary callers
    // retain their normalization through the forwarding wrapper. Refuse an
    // unfamiliar body rather than silently dropping a value check.
    for (const char* operation : {"Add", "Mul"}) {
      const std::string name = std::string("VitaVuF32") + operation + "Bits";
      const std::string signature = "unsigned int " + name + "(";
      const size_t begin = m_source.find(signature);
      if (begin == std::string::npos)
        continue;
      const size_t end = m_source.find("\n}\n\n", begin);
      if (end == std::string::npos ||
          m_source.find(signature, begin + signature.size()) != std::string::npos)
        return Fail(error, "normalized FMAC helper definition is ambiguous");
      std::string body = m_source.substr(begin, end + 4u - begin);
      for (const char* input : {"leftInput", "rightInput"}) {
        const std::string normalize = std::string("VitaVuF32NormalizeInputBits(") + input + ")";
        const size_t found = body.find(normalize);
        if (found == std::string::npos ||
            body.find(normalize, found + normalize.size()) != std::string::npos)
          return Fail(error, "normalized FMAC helper input is ambiguous");
        body.replace(found, normalize.size(), input);
      }
      const std::string normalized = std::string("VitaVuF32") + operation + "NormalizedBits";
      body.replace(std::string("unsigned int ").size(), name.size(), normalized);
      body += "unsigned int " + name + "(unsigned int leftInput, unsigned int rightInput)\n{\n"
          "  return " + normalized + "(VitaVuF32NormalizeInputBits(leftInput), "
          "VitaVuF32NormalizeInputBits(rightInput));\n}\n\n";
      m_source.replace(begin, end + 4u - begin, body);
      const std::string wrapper = std::string("VitaVuF32") + operation + "Normalized";
      m_source += "float " + wrapper + "(float left, float right)\n{\n"
          "  return intBitsToFloat(int(" + normalized + "(unsigned int(floatToRawIntBits(left)), "
          "unsigned int(floatToRawIntBits(right)))));\n}\n\n";
      m_source += "float4 " + wrapper + "4(float4 left, float4 right)\n{\n"
          "  return float4(" + wrapper + "(left.x,right.x)," + wrapper + "(left.y,right.y)," +
          wrapper + "(left.z,right.z)," + wrapper + "(left.w,right.w));\n}\n\n";
    }
    if (UsesStructuredExactF32(ExpressionKind::RoundedSubtract)) {
      m_source += "float VitaVuF32SubNormalized(float left, float right)\n{\n"
          "  return intBitsToFloat(int(VitaVuF32AddNormalizedBits(unsigned int(floatToRawIntBits(left)), "
          "unsigned int(floatToRawIntBits(right)) ^ 2147483648u)));\n}\n\n"
          "float4 VitaVuF32SubNormalized4(float4 left, float4 right)\n{\n"
          "  return float4(VitaVuF32SubNormalized(left.x,right.x),VitaVuF32SubNormalized(left.y,right.y),"
          "VitaVuF32SubNormalized(left.z,right.z),VitaVuF32SubNormalized(left.w,right.w));\n}\n\n";
    }
    m_source += "float VitaVuF32Prepare(float value)\n{\n"
        "  return intBitsToFloat(int(VitaVuF32NormalizeInputBits(unsigned int(floatToRawIntBits(value)))));\n}\n\n"
        "float4 VitaVuF32Prepare4(float4 value)\n{\n"
        "  return float4(VitaVuF32Prepare(value.x),VitaVuF32Prepare(value.y),"
        "VitaVuF32Prepare(value.z),VitaVuF32Prepare(value.w));\n}\n\n";
    return true;
  }

  void AppendRangeScaledF32Multiply() {
    // VUops.cpp::vuDouble/vuMUL still own input normalization, nearest-even
    // rounding and overflow/underflow handling. Scale only the host multiply
    // to [1,2)*[1,2): no native underflow/overflow can lose its significand.
    // The low 23/24 product bits decide rounding and need just one modulo-u32
    // multiply. This is an offline candidate, not a new attested SGX provider.
    m_source += R"(unsigned int VitaVuF32MulBits(unsigned int leftInput, unsigned int rightInput)
{
  const unsigned int left = VitaVuF32NormalizeInputBits(leftInput);
  const unsigned int right = VitaVuF32NormalizeInputBits(rightInput);
  const unsigned int resultSignBits = (left ^ right) & 2147483648u;
  const unsigned int leftFraction = left & 8388607u;
  const unsigned int rightFraction = right & 8388607u;
  const float scaledLeft = intBitsToFloat(int(leftFraction | 1065353216u));
  const float scaledRight = intBitsToFloat(int(rightFraction | 1065353216u));
  const unsigned int scaledProductBits = unsigned int(floatToRawIntBits(scaledLeft * scaledRight));
  const unsigned int highBit = (scaledProductBits >> 23u) - 127u;
  const unsigned int truncated = (scaledProductBits & 8388607u) | 8388608u;
  const unsigned int productLow = (leftFraction | 8388608u) * (rightFraction | 8388608u);
  const unsigned int remainder = productLow & ((8388608u << highBit) - 1u);
  const unsigned int halfway = 4194304u << highBit;
  const unsigned int increment = (unsigned int(remainder > halfway) & 1u) |
      ((unsigned int(remainder == halfway) & 1u) & (truncated & 1u));
  const unsigned int rounded = truncated + increment;
  const int exponent = int((left >> 23u) & 255u) + int((right >> 23u) & 255u) - 127 +
      int(highBit) + int(rounded >> 24u);
  unsigned int result = resultSignBits | (unsigned int(exponent) << 23u) | (rounded & 8388607u);
  result = VitaVuF32SelectBits(result, resultSignBits | 2139095039u,
      0u - (unsigned int(exponent >= 255) & 1u));
  result = VitaVuF32SelectBits(result, resultSignBits,
      0u - (unsigned int(exponent <= 0) & 1u));
  const unsigned int zeroBit = (unsigned int((left & 2147483647u) == 0u) & 1u) |
      (unsigned int((right & 2147483647u) == 0u) & 1u);
  return VitaVuF32SelectBits(result, resultSignBits, 0u - zeroBit);
}

)";
  }
#endif

  void AppendStructuredSoftwareF32Helpers() {
    const bool approximate =
        UsesStructuredExactF32(ExpressionKind::ArmApproximateReciprocal) ||
        UsesStructuredExactF32(ExpressionKind::ArmApproximateSquareRoot);
    bool need_positive_zero_add = false;
    bool need_general_add =
        UsesStructuredExactF32(ExpressionKind::EfuSumXyzSquares) ||
        UsesStructuredExactF32(ExpressionKind::OuterRepeatedAdd);
    bool positive_zero_left_only = false;
    bool positive_zero_right_only = false;
    for (u32 node_id = 1; node_id < m_kernel.expressions.size(); node_id++) {
      if ((!m_reachable[node_id] && !m_flat_color_reachable[node_id]) ||
          m_kernel.expressions[node_id].kind != ExpressionKind::RoundedAdd ||
          !NodeUsesStructuredExactF32(node_id)) {
        continue;
      }
      const ExpressionNode& node = m_kernel.expressions[node_id];
      const bool positive_zero_left =
          IsPositiveZeroFloatNode(node.operands[0]);
      const bool positive_zero_right =
          IsPositiveZeroFloatNode(node.operands[1]);
      const bool positive_zero = positive_zero_left || positive_zero_right;
      need_positive_zero_add |= positive_zero;
      need_general_add |= !positive_zero;
      positive_zero_left_only |= positive_zero_left && !positive_zero_right;
      positive_zero_right_only |= positive_zero_right && !positive_zero_left;
    }
    // Vector grouping can combine otherwise independent lanes. If their
    // hardwired zero appears on opposite sides, neither vector operand is
    // uniformly zero and the emitted group correctly uses the general helper.
    need_general_add |= positive_zero_left_only && positive_zero_right_only;
    const bool need_add = need_general_add;
    const bool need_sub =
        UsesStructuredExactF32(ExpressionKind::RoundedSubtract) || approximate;
    const bool add_sub = need_add || need_sub;
    const bool multiply =
        UsesStructuredExactF32(ExpressionKind::RoundedMultiply) ||
        UsesStructuredExactF32(ExpressionKind::EfuSumXyzSquares) ||
        approximate;
    if (UsesBranchlessLoopKernelControl()) {
      // The hardware-driven loop root is a static data-parallel kernel. Keep
      // the exact PCSX2 arithmetic dependency cone branch-free as well: every
      // exceptional case is a one-bit predicate expanded to a select mask.
      // This prevents a per-vertex control-flow job from replacing the serial
      // interpreter bottleneck we removed.
      m_source +=
          "unsigned int VitaVuF32SelectBits(unsigned int falseBits, "
          "unsigned int trueBits, unsigned int mask)\n"
          "{\n"
          "\treturn (falseBits & ~mask) | (trueBits & mask);\n"
          "}\n\n"
          "unsigned int VitaVuF32NormalizeInputBits(unsigned int bits)\n"
          "{\n"
          "\tconst unsigned int exponent = bits & 2139095040u;\n"
          "\tconst unsigned int signBits = bits & 2147483648u;\n"
          "\tconst unsigned int zeroMask = 0u - "
          "(unsigned int(exponent == 0u) & 1u);\n"
          "\tconst unsigned int specialMask = 0u - "
          "(unsigned int(exponent == 2139095040u) & 1u);\n"
          "\tconst unsigned int zeroSelected = VitaVuF32SelectBits("
          "bits, signBits, zeroMask);\n"
          "\treturn VitaVuF32SelectBits(zeroSelected, signBits | "
          "2139095039u, specialMask);\n"
          "}\n\n";
      if (add_sub) {
        m_source +=
            "unsigned int VitaVuF32ShiftRightJam(unsigned int value, "
            "unsigned int distance)\n"
            "{\n"
            "\tconst unsigned int smallBit = "
            "unsigned int(distance < 32u) & 1u;\n"
            "\tconst unsigned int smallMask = 0u - smallBit;\n"
            "\tconst unsigned int safeDistance = "
            "VitaVuF32SelectBits(31u, distance, smallMask);\n"
            "\tconst unsigned int discardedMask = "
            "(1u << safeDistance) - 1u;\n"
            "\tconst unsigned int small = (value >> safeDistance) | "
            "(unsigned int((value & discardedMask) != 0u) & 1u);\n"
            "\tconst unsigned int large = unsigned int(value != 0u) & 1u;\n"
            "\treturn VitaVuF32SelectBits(large, small, smallMask);\n"
            "}\n\n"
            "unsigned int VitaVuF32AddBits(unsigned int leftInput, "
            "unsigned int rightInput)\n"
            "{\n"
            "\tconst unsigned int originalLeft = "
            "VitaVuF32NormalizeInputBits(leftInput);\n"
            "\tconst unsigned int originalRight = "
            "VitaVuF32NormalizeInputBits(rightInput);\n"
            "\tconst unsigned int originalLeftMagnitude = "
            "originalLeft & 2147483647u;\n"
            "\tconst unsigned int originalRightMagnitude = "
            "originalRight & 2147483647u;\n"
            "\tconst unsigned int swapMask = 0u - "
            "(unsigned int(originalLeftMagnitude < "
            "originalRightMagnitude) & 1u);\n"
            "\tconst unsigned int left = VitaVuF32SelectBits("
            "originalLeft, originalRight, swapMask);\n"
            "\tconst unsigned int right = VitaVuF32SelectBits("
            "originalRight, originalLeft, swapMask);\n"
            "\tconst unsigned int leftMagnitude = left & 2147483647u;\n"
            "\tconst unsigned int rightMagnitude = right & 2147483647u;\n"
            "\tconst unsigned int resultSignBits = left & 2147483648u;\n"
            "\tconst unsigned int leftExponent = (left >> 23u) & 255u;\n"
            "\tconst unsigned int rightExponent = (right >> 23u) & 255u;\n"
            "\tconst unsigned int leftSignificand = "
            "((left & 8388607u) | 8388608u) << 3u;\n"
            "\tconst unsigned int rightSignificand = "
            "VitaVuF32ShiftRightJam(((right & 8388607u) | 8388608u) "
            "<< 3u, leftExponent - rightExponent);\n"
            "\tconst unsigned int sameSignMask = 0u - "
            "(unsigned int(((left ^ right) & 2147483648u) == 0u) & 1u);\n"
            "\tconst unsigned int exactSignificand = VitaVuF32SelectBits("
            "leftSignificand - rightSignificand, "
            "leftSignificand + rightSignificand, sameSignMask);\n"
            "\tconst float nativeLeft = intBitsToFloat(int(left));\n"
            "\tconst float nativeRight = intBitsToFloat(int(right));\n"
            "\tconst unsigned int nativeBits = unsigned int("
            "floatToRawIntBits(nativeLeft + nativeRight));\n"
            "\tconst unsigned int nativeMagnitude = "
            "nativeBits & 2147483647u;\n"
            "\tconst unsigned int nativeExponent = nativeMagnitude >> 23u;\n"
            "\tconst int exponentShift = "
            "int(leftExponent) - int(nativeExponent);\n"
            "\tconst unsigned int leftShiftMask = 0u - "
            "(unsigned int(exponentShift >= 0) & 1u);\n"
            "\tconst unsigned int leftShift = "
            "unsigned int(exponentShift) & leftShiftMask;\n"
            "\tconst unsigned int rightShift = "
            "unsigned int(-exponentShift) & ~leftShiftMask;\n"
            "\tconst unsigned int normalized = VitaVuF32SelectBits("
            "VitaVuF32ShiftRightJam(exactSignificand, rightShift), "
            "exactSignificand << leftShift, leftShiftMask);\n"
            "\tconst unsigned int roundBits = normalized & 7u;\n"
            "\tconst unsigned int increment = "
            "(unsigned int(roundBits > 4u) & 1u) | "
            "((unsigned int(roundBits == 4u) & 1u) & "
            "(nativeMagnitude & 1u));\n"
            "\tconst unsigned int correctedMagnitude = "
            "nativeMagnitude + increment;\n"
            "\tconst unsigned int correctedOverflowMask = 0u - "
            "(unsigned int(correctedMagnitude >= 2139095040u) & 1u);\n"
            "\tunsigned int result = resultSignBits | "
            "VitaVuF32SelectBits(correctedMagnitude, 2139095039u, "
            "correctedOverflowMask);\n"
            "\tconst unsigned int nativeOverflowMask = 0u - "
            "(unsigned int(nativeExponent >= 255u) & 1u);\n"
            "\tconst unsigned int nativeZeroMask = 0u - "
            "(unsigned int(nativeExponent == 0u) & 1u);\n"
            "\tconst unsigned int exactZeroMask = 0u - "
            "(unsigned int(exactSignificand == 0u) & 1u);\n"
            "\tresult = VitaVuF32SelectBits(result, resultSignBits | "
            "2139095039u, nativeOverflowMask);\n"
            "\tresult = VitaVuF32SelectBits(result, resultSignBits, "
            "nativeZeroMask);\n"
            "\tresult = VitaVuF32SelectBits(result, 0u, exactZeroMask);\n"
            "\tconst unsigned int originalRightZeroMask = 0u - "
            "(unsigned int(originalRightMagnitude == 0u) & 1u);\n"
            "\tconst unsigned int originalLeftZeroMask = 0u - "
            "(unsigned int(originalLeftMagnitude == 0u) & 1u);\n"
            "\tresult = VitaVuF32SelectBits(result, originalLeft, "
            "originalRightZeroMask);\n"
            "\tresult = VitaVuF32SelectBits(result, originalRight, "
            "originalLeftZeroMask);\n"
            "\tconst unsigned int bothZeroMask = "
            "originalLeftZeroMask & originalRightZeroMask;\n"
            "\tconst unsigned int sameZeroSignMask = 0u - "
            "(unsigned int(((originalLeft ^ originalRight) & "
            "2147483648u) == 0u) & 1u);\n"
            "\tconst unsigned int bothZeroValue = "
            "(originalLeft & 2147483648u) & sameZeroSignMask;\n"
            "\treturn VitaVuF32SelectBits(result, bothZeroValue, "
            "bothZeroMask);\n"
            "}\n\n";
      }
      if (multiply) {
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
        if (m_range_scaled_multiply_analysis)
          AppendRangeScaledF32Multiply();
        else
#endif
        m_source +=
            "unsigned int VitaVuF32MulBits(unsigned int leftInput, "
            "unsigned int rightInput)\n"
            "{\n"
            "\tconst unsigned int left = "
            "VitaVuF32NormalizeInputBits(leftInput);\n"
            "\tconst unsigned int right = "
            "VitaVuF32NormalizeInputBits(rightInput);\n"
            "\tconst unsigned int resultSignBits = "
            "(left ^ right) & 2147483648u;\n"
            "\tint exponent = int((left >> 23u) & 255u) + "
            "int((right >> 23u) & 255u) - 127;\n"
            "\tconst unsigned int leftSignificand = "
            "(left & 8388607u) | 8388608u;\n"
            "\tconst unsigned int rightSignificand = "
            "(right & 8388607u) | 8388608u;\n"
            "\tconst unsigned int leftLow = leftSignificand & 65535u;\n"
            "\tconst unsigned int leftHigh = leftSignificand >> 16u;\n"
            "\tconst unsigned int rightLow = rightSignificand & 65535u;\n"
            "\tconst unsigned int rightHigh = rightSignificand >> 16u;\n"
            "\tconst unsigned int lowProduct = leftLow * rightLow;\n"
            "\tconst unsigned int crossProduct = "
            "leftHigh * rightLow + leftLow * rightHigh;\n"
            "\tconst unsigned int productLow = "
            "lowProduct + (crossProduct << 16u);\n"
            "\tconst unsigned int carry = "
            "unsigned int(productLow < lowProduct) & 1u;\n"
            "\tconst unsigned int productHigh = "
            "leftHigh * rightHigh + (crossProduct >> 16u) + carry;\n"
            "\tconst unsigned int highBit = "
            "unsigned int((productHigh & 32768u) != 0u) & 1u;\n"
            "\tconst unsigned int highMask = 0u - highBit;\n"
            "\tconst unsigned int lowTruncated = "
            "(productHigh << 9u) | (productLow >> 23u);\n"
            "\tconst unsigned int highTruncated = "
            "(productHigh << 8u) | (productLow >> 24u);\n"
            "\tconst unsigned int truncated = VitaVuF32SelectBits("
            "lowTruncated, highTruncated, highMask);\n"
            "\tconst unsigned int remainderMask = VitaVuF32SelectBits("
            "8388607u, 16777215u, highMask);\n"
            "\tconst unsigned int halfway = VitaVuF32SelectBits("
            "4194304u, 8388608u, highMask);\n"
            "\tconst unsigned int remainder = productLow & remainderMask;\n"
            "\texponent += int(highBit);\n"
            "\tconst unsigned int increment = "
            "(unsigned int(remainder > halfway) & 1u) | "
            "((unsigned int(remainder == halfway) & 1u) & "
            "(truncated & 1u));\n"
            "\tconst unsigned int nativeLeftBits = unsigned int("
            "floatToRawIntBits(intBitsToFloat(int(left)) * "
            "intBitsToFloat(int(right))));\n"
            "\tconst unsigned int nativeMagnitude = "
            "nativeLeftBits & 2147483647u;\n"
            "\tconst unsigned int correctedMagnitude = "
            "nativeMagnitude + increment;\n"
            "\tconst unsigned int overflowBit = "
            "(unsigned int(exponent >= 255) & 1u) | "
            "(unsigned int((nativeMagnitude >> 23u) >= 255u) & 1u) | "
            "(unsigned int(correctedMagnitude >= 2139095040u) & 1u);\n"
            "\tconst unsigned int overflowMask = 0u - overflowBit;\n"
            "\tunsigned int result = resultSignBits | "
            "VitaVuF32SelectBits(correctedMagnitude, 2139095039u, "
            "overflowMask);\n"
            "\tconst unsigned int underflowRounded = truncated + increment;\n"
            "\tconst unsigned int underflowNormalBit = "
            "(unsigned int(exponent == 0) & 1u) & "
            "(unsigned int((underflowRounded & 16777216u) != 0u) & 1u);\n"
            "\tconst unsigned int underflowValue = resultSignBits | "
            "(8388608u & (0u - underflowNormalBit));\n"
            "\tconst unsigned int underflowMask = 0u - "
            "(unsigned int(exponent <= 0) & 1u);\n"
            "\tresult = VitaVuF32SelectBits(result, underflowValue, "
            "underflowMask);\n"
            "\tconst unsigned int zeroBit = "
            "(unsigned int((left & 2147483647u) == 0u) & 1u) | "
            "(unsigned int((right & 2147483647u) == 0u) & 1u);\n"
            "\treturn VitaVuF32SelectBits(result, resultSignBits, "
            "0u - zeroBit);\n"
            "}\n\n";
      }
      if (need_positive_zero_add) {
        m_source +=
            "float VitaVuF32AddPositiveZero(float value)\n"
            "{\n"
            "\tconst unsigned int bits = VitaVuF32NormalizeInputBits("
            "unsigned int(floatToRawIntBits(value)));\n"
            "\tconst unsigned int zeroMask = 0u - "
            "(unsigned int((bits & 2147483647u) == 0u) & 1u);\n"
            "\treturn intBitsToFloat(int(VitaVuF32SelectBits(bits, 0u, "
            "zeroMask)));\n"
            "}\n\n"
            "float4 VitaVuF32AddPositiveZero4(float4 value)\n"
            "{\n"
            "\treturn float4(VitaVuF32AddPositiveZero(value.x), "
            "VitaVuF32AddPositiveZero(value.y), "
            "VitaVuF32AddPositiveZero(value.z), "
            "VitaVuF32AddPositiveZero(value.w));\n"
            "}\n\n";
      }
      if (need_add) {
        m_source +=
            "float VitaVuF32Add(float left, float right)\n"
            "{\n"
            "\treturn intBitsToFloat(int(VitaVuF32AddBits("
            "unsigned int(floatToRawIntBits(left)), "
            "unsigned int(floatToRawIntBits(right)))));\n"
            "}\n\n"
            "float4 VitaVuF32Add4(float4 left, float4 right)\n"
            "{\n"
            "\treturn float4(VitaVuF32Add(left.x, right.x), "
            "VitaVuF32Add(left.y, right.y), "
            "VitaVuF32Add(left.z, right.z), "
            "VitaVuF32Add(left.w, right.w));\n"
            "}\n\n";
      }
      if (need_sub) {
        m_source +=
            "float VitaVuF32Sub(float left, float right)\n"
            "{\n"
            "\treturn intBitsToFloat(int(VitaVuF32AddBits("
            "unsigned int(floatToRawIntBits(left)), "
            "unsigned int(floatToRawIntBits(right)) ^ 2147483648u)));\n"
            "}\n\n"
            "float4 VitaVuF32Sub4(float4 left, float4 right)\n"
            "{\n"
            "\treturn float4(VitaVuF32Sub(left.x, right.x), "
            "VitaVuF32Sub(left.y, right.y), "
            "VitaVuF32Sub(left.z, right.z), "
            "VitaVuF32Sub(left.w, right.w));\n"
            "}\n\n";
      }
      if (multiply) {
        m_source +=
            "float VitaVuF32Mul(float left, float right)\n"
            "{\n"
            "\treturn intBitsToFloat(int(VitaVuF32MulBits("
            "unsigned int(floatToRawIntBits(left)), "
            "unsigned int(floatToRawIntBits(right)))));\n"
            "}\n\n"
            "float4 VitaVuF32Mul4(float4 left, float4 right)\n"
            "{\n"
            "\treturn float4(VitaVuF32Mul(left.x, right.x), "
            "VitaVuF32Mul(left.y, right.y), "
            "VitaVuF32Mul(left.z, right.z), "
            "VitaVuF32Mul(left.w, right.w));\n"
            "}\n\n";
      }
      return;
    }
    m_source +=
        "unsigned int VitaVuF32NormalizeInputBits(unsigned int bits)\n"
        "{\n"
        "\tconst unsigned int exponent = bits & 2139095040u;\n"
        "\tif (exponent == 0u) return bits & 2147483648u;\n"
        "\tif (exponent == 2139095040u) "
        "return (bits & 2147483648u) | 2139095039u;\n"
        "\treturn bits;\n"
        "}\n\n";
    if (add_sub) {
      m_source +=
          "unsigned int VitaVuF32ShiftRightJam(unsigned int value, "
          "unsigned int distance)\n"
          "{\n"
          "\tif (distance == 0u) return value;\n"
          "\tif (distance < 32u)\n"
          "\t{\n"
          "\t\tconst unsigned int mask = (1u << distance) - 1u;\n"
          "\t\treturn (value >> distance) | "
          "((value & mask) != 0u ? 1u : 0u);\n"
          "\t}\n"
          "\treturn value != 0u ? 1u : 0u;\n"
          "}\n\n"
          "unsigned int VitaVuF32AddBits(unsigned int leftInput, "
          "unsigned int rightInput)\n"
          "{\n"
          "\tunsigned int left = VitaVuF32NormalizeInputBits(leftInput);\n"
          "\tunsigned int right = VitaVuF32NormalizeInputBits(rightInput);\n"
          "\tunsigned int leftMagnitude = left & 2147483647u;\n"
          "\tunsigned int rightMagnitude = right & 2147483647u;\n"
          "\tconst unsigned int leftSign = left >> 31u;\n"
          "\tconst unsigned int rightSign = right >> 31u;\n"
          "\tif (leftMagnitude == 0u && rightMagnitude == 0u)\n"
          "\t\treturn leftSign == rightSign ? leftSign << 31u : 0u;\n"
          "\tif (leftMagnitude == 0u) return right;\n"
          "\tif (rightMagnitude == 0u) return left;\n"
          "\tif (leftMagnitude < rightMagnitude)\n"
          "\t{\n"
          "\t\tconst unsigned int swapBits = left; left = right; "
          "right = swapBits;\n"
          "\t\tconst unsigned int swapMagnitude = leftMagnitude; "
          "leftMagnitude = rightMagnitude; rightMagnitude = swapMagnitude;\n"
          "\t}\n"
          "\tconst unsigned int resultSignBits = left & 2147483648u;\n"
          "\tconst unsigned int leftExponent = (left >> 23u) & 255u;\n"
          "\tconst unsigned int rightExponent = (right >> 23u) & 255u;\n"
          "\tconst unsigned int leftSignificand = "
          "((left & 8388607u) | 8388608u) << 3u;\n"
          "\tunsigned int rightSignificand = "
          "((right & 8388607u) | 8388608u) << 3u;\n"
          "\trightSignificand = VitaVuF32ShiftRightJam(rightSignificand, "
          "leftExponent - rightExponent);\n"
          "\tunsigned int exactSignificand;\n"
          "\tif (((left ^ right) & 2147483648u) == 0u)\n"
          "\t\texactSignificand = leftSignificand + rightSignificand;\n"
          "\telse\n"
          "\t\texactSignificand = leftSignificand - rightSignificand;\n"
          "\tif (exactSignificand == 0u) return 0u;\n"
          // Series5XT's native F32 ALU supplies the correctly normalized
          // round-toward-zero candidate.  Reconstruct only the three discarded
          // bits at that candidate's exponent, then move one ULP away from zero
          // for nearest-even.  This is an architectural rounding boundary, not
          // an approximation: source normalization remains PCSX2 VUops.cpp's
          // vuDouble() contract, and the integer remainder decides the sole
          // possible correction.
          "\tconst float nativeLeft = intBitsToFloat(int(left));\n"
          "\tconst float nativeRight = intBitsToFloat(int(right));\n"
          "\tconst unsigned int nativeBits = unsigned int(floatToRawIntBits("
          "nativeLeft + nativeRight));\n"
          "\tconst unsigned int nativeMagnitude = nativeBits & 2147483647u;\n"
          "\tconst unsigned int nativeExponent = nativeMagnitude >> 23u;\n"
          "\tif (nativeExponent == 0u) return resultSignBits;\n"
          "\tif (nativeExponent >= 255u) "
          "return resultSignBits | 2139095039u;\n"
          "\tconst int exponentShift = int(leftExponent) - int(nativeExponent);\n"
          "\tconst unsigned int normalized = exponentShift >= 0 ? "
          "(exactSignificand << unsigned int(exponentShift)) : "
          "VitaVuF32ShiftRightJam(exactSignificand, "
          "unsigned int(-exponentShift));\n"
          "\tconst unsigned int roundBits = normalized & 7u;\n"
          "\tconst bool increment = roundBits > 4u || "
          "(roundBits == 4u && (nativeMagnitude & 1u) != 0u);\n"
          "\tconst unsigned int correctedMagnitude = nativeMagnitude + "
          "(increment ? 1u : 0u);\n"
          "\treturn correctedMagnitude >= 2139095040u ? "
          "(resultSignBits | 2139095039u) : "
          "(resultSignBits | correctedMagnitude);\n"
          "}\n\n";
    }
    if (multiply) {
      m_source +=
          "unsigned int VitaVuF32MulBits(unsigned int leftInput, "
          "unsigned int rightInput)\n"
          "{\n"
          "\tconst unsigned int left = "
          "VitaVuF32NormalizeInputBits(leftInput);\n"
          "\tconst unsigned int right = "
          "VitaVuF32NormalizeInputBits(rightInput);\n"
          "\tconst unsigned int resultSignBits = "
          "(left ^ right) & 2147483648u;\n"
          "\tif ((left & 2147483647u) == 0u || "
          "(right & 2147483647u) == 0u) return resultSignBits;\n"
          "\tint exponent = int((left >> 23u) & 255u) + "
          "int((right >> 23u) & 255u) - 127;\n"
          "\tconst unsigned int leftSignificand = "
          "(left & 8388607u) | 8388608u;\n"
          "\tconst unsigned int rightSignificand = "
          "(right & 8388607u) | 8388608u;\n"
          "\tconst unsigned int leftLow = leftSignificand & 65535u;\n"
          "\tconst unsigned int leftHigh = leftSignificand >> 16u;\n"
          "\tconst unsigned int rightLow = rightSignificand & 65535u;\n"
          "\tconst unsigned int rightHigh = rightSignificand >> 16u;\n"
          "\tconst unsigned int lowProduct = leftLow * rightLow;\n"
          "\tconst unsigned int crossProduct = "
          "leftHigh * rightLow + leftLow * rightHigh;\n"
          "\tconst unsigned int productLow = "
          "lowProduct + (crossProduct << 16u);\n"
          "\tconst unsigned int carry = "
          "productLow < lowProduct ? 1u : 0u;\n"
          "\tconst unsigned int productHigh = "
          "leftHigh * rightHigh + (crossProduct >> 16u) + carry;\n"
          "\tunsigned int truncated;\n"
          "\tunsigned int remainder;\n"
          "\tunsigned int halfway;\n"
          "\tif ((productHigh & 32768u) != 0u)\n"
          "\t{\n"
          "\t\ttruncated = (productHigh << 8u) | (productLow >> 24u);\n"
          "\t\tremainder = productLow & 16777215u;\n"
          "\t\thalfway = 8388608u;\n"
          "\t\texponent++;\n"
          "\t}\n"
          "\telse\n"
          "\t{\n"
          "\t\ttruncated = (productHigh << 9u) | (productLow >> 23u);\n"
          "\t\tremainder = productLow & 8388607u;\n"
          "\t\thalfway = 4194304u;\n"
          "\t}\n"
          "\tconst bool increment = remainder > halfway || "
          "(remainder == halfway && (truncated & 1u) != 0u);\n"
          "\tif (exponent <= 0)\n"
          "\t{\n"
          "\t\tconst unsigned int underflowRounded = truncated + "
          "(increment ? 1u : 0u);\n"
          "\t\treturn exponent == 0 && "
          "(underflowRounded & 16777216u) != 0u ? "
          "(resultSignBits | 8388608u) : resultSignBits;\n"
          "\t}\n"
          "\tif (exponent >= 255) return resultSignBits | 2139095039u;\n"
          "\tconst float nativeLeft = intBitsToFloat(int(left));\n"
          "\tconst float nativeRight = intBitsToFloat(int(right));\n"
          "\tconst unsigned int nativeMagnitude = "
          "unsigned int(floatToRawIntBits(nativeLeft * nativeRight)) & "
          "2147483647u;\n"
          "\tif ((nativeMagnitude >> 23u) >= 255u) "
          "return resultSignBits | 2139095039u;\n"
          "\tconst unsigned int correctedMagnitude = nativeMagnitude + "
          "(increment ? 1u : 0u);\n"
          "\treturn correctedMagnitude >= 2139095040u ? "
          "(resultSignBits | 2139095039u) : "
          "(resultSignBits | correctedMagnitude);\n"
          "}\n\n";
    }
    if (need_positive_zero_add) {
      m_source +=
          // ADD against hardwired VF00 zero is common in parent-state roots.
          // It still owns a VU numeric boundary: denormals become signed zero,
          // overflow encodings clamp, and -0 + +0 publishes +0.  Keep that
          // exact but do not make Sony's compiler inline the complete
          // alignment/rounding helper for an operation which cannot round.
          "float VitaVuF32AddPositiveZero(float value)\n"
          "{\n"
          "\tconst unsigned int bits = VitaVuF32NormalizeInputBits("
          "unsigned int(floatToRawIntBits(value)));\n"
          "\treturn intBitsToFloat(int((bits & 2147483647u) == 0u ? "
          "0u : bits));\n"
          "}\n\n"
          "float4 VitaVuF32AddPositiveZero4(float4 value)\n"
          "{\n"
          "\treturn float4(VitaVuF32AddPositiveZero(value.x), "
          "VitaVuF32AddPositiveZero(value.y), "
          "VitaVuF32AddPositiveZero(value.z), "
          "VitaVuF32AddPositiveZero(value.w));\n"
          "}\n\n";
    }
    if (need_add) {
      m_source +=
          "float VitaVuF32Add(float left, float right)\n"
          "{\n"
          "\treturn intBitsToFloat(int(VitaVuF32AddBits("
          "unsigned int(floatToRawIntBits(left)), "
          "unsigned int(floatToRawIntBits(right)))));\n"
          "}\n\n"
          "float4 VitaVuF32Add4(float4 left, float4 right)\n"
          "{\n"
          "\treturn float4(VitaVuF32Add(left.x, right.x), "
          "VitaVuF32Add(left.y, right.y), VitaVuF32Add(left.z, right.z), "
          "VitaVuF32Add(left.w, right.w));\n"
          "}\n\n";
    }
    if (need_sub) {
      m_source +=
          "float VitaVuF32Sub(float left, float right)\n"
          "{\n"
          "\treturn intBitsToFloat(int(VitaVuF32AddBits("
          "unsigned int(floatToRawIntBits(left)), "
          "unsigned int(floatToRawIntBits(right)) ^ 2147483648u)));\n"
          "}\n\n"
          "float4 VitaVuF32Sub4(float4 left, float4 right)\n"
          "{\n"
          "\treturn float4(VitaVuF32Sub(left.x, right.x), "
          "VitaVuF32Sub(left.y, right.y), VitaVuF32Sub(left.z, right.z), "
          "VitaVuF32Sub(left.w, right.w));\n"
          "}\n\n";
    }
    if (multiply) {
      m_source +=
          "float VitaVuF32Mul(float left, float right)\n"
          "{\n"
          "\treturn intBitsToFloat(int(VitaVuF32MulBits("
          "unsigned int(floatToRawIntBits(left)), "
          "unsigned int(floatToRawIntBits(right)))));\n"
          "}\n\n"
          "float4 VitaVuF32Mul4(float4 left, float4 right)\n"
          "{\n"
          "\treturn float4(VitaVuF32Mul(left.x, right.x), "
          "VitaVuF32Mul(left.y, right.y), VitaVuF32Mul(left.z, right.z), "
          "VitaVuF32Mul(left.w, right.w));\n"
          "}\n\n";
    }
  }

  void AppendStructuredNativeF32Helpers() {
    const bool efu_sum =
        UsesStructuredNativeF32(ExpressionKind::EfuSumXyzSquares);
    const bool approximate_qp =
        UsesStructuredNativeF32(ExpressionKind::ArmApproximateReciprocal) ||
        UsesStructuredNativeF32(ExpressionKind::ArmApproximateSquareRoot);
    const bool add = UsesStructuredNativeF32(ExpressionKind::RoundedAdd) ||
                     UsesStructuredNativeF32(
                         ExpressionKind::OuterRepeatedAdd) ||
                     efu_sum;
    const bool subtract =
        UsesStructuredNativeF32(ExpressionKind::RoundedSubtract) ||
        approximate_qp;
    const bool multiply =
        UsesStructuredNativeF32(ExpressionKind::RoundedMultiply) || efu_sum ||
        approximate_qp;
    if (!add && !subtract && !multiply)
      return;

    // This is the named playable-profile approximation. The SGX owns native
    // finite/FTZ arithmetic; do not scalarize every vector lane into three
    // explicit normalize passes. Offline SDK 3.570 characterization showed
    // that shape taking more than three minutes at O1, while O0 emitted a
    // 10,132-cycle per-instance root. Exact mode and the fixed numeric owner
    // retain explicit PCSX2-oracle normalization.
    //
    // ABI 21 tried to make each rounded multiply compiler-visible through a
    // dynamically loaded, host-zeroed XOR dependency. Physical ABI-21
    // psp2shaderperf and BSpline evidence showed that this enlarged the root,
    // added integer work, and increased rather than reduced oracle deltas.
    // Keep native finite FMAC explicitly profile-owned and let the final GXP
    // resource/correctness certificate decide admission.
    const auto append_binary =
        [this](const char* name, const char* operation) {
      m_source += "float ";
      m_source += name;
      m_source += "(float left, float right)\n{\n";
      m_source += "\tconst float result = left ";
      m_source += operation;
      m_source += " right;\n";
      m_source +=
          "\treturn intBitsToFloat(floatToRawIntBits(result));\n}\n\n";
      m_source += "float4 ";
      m_source += name;
      m_source += "4(float4 left, float4 right)\n{\n";
      m_source += "\tconst float4 result = left ";
      m_source += operation;
      m_source += " right;\n";
      m_source +=
          "\treturn bit_cast<float4>(bit_cast<unsigned int4>(result));\n}\n\n";
    };
    if (add)
      append_binary("VitaVuNativeF32Add", "+");
    if (subtract)
      append_binary("VitaVuNativeF32Sub", "-");
    if (multiply)
      append_binary("VitaVuNativeF32Mul", "*");
  }

  bool UsesOuterRepeatedAddSchedule(u32 schedule_index,
                                    const std::vector<bool>* reachable = nullptr) const {
    for (u32 node_id = 1u; node_id < m_kernel.expressions.size(); node_id++) {
      if (reachable ? !(*reachable)[node_id]
                    : (!m_reachable[node_id] && !m_flat_color_reachable[node_id]))
        continue;
      const ExpressionNode& node = m_kernel.expressions[node_id];
      if (node.kind == ExpressionKind::OuterRepeatedAdd &&
          IsScheduledOuterRepeatedAdd(node.immediate) &&
          OuterRepeatedAddScheduleIndex(node.immediate) == schedule_index &&
          !IsLeanIdentityOuterRepeatedAdd(node)) {
        return true;
      }
    }
    return false;
  }

  bool UsesUnscheduledOuterRepeatedAdd() const {
    for (u32 node_id = 1u; node_id < m_kernel.expressions.size(); node_id++) {
      if ((!m_reachable[node_id] && !m_flat_color_reachable[node_id]) ||
          m_kernel.expressions[node_id].kind !=
              ExpressionKind::OuterRepeatedAdd) {
        continue;
      }
      if (IsLeanIdentityOuterRepeatedAdd(m_kernel.expressions[node_id]))
        continue;
      if (!IsScheduledOuterRepeatedAdd(
              m_kernel.expressions[node_id].immediate)) {
        return true;
      }
    }
    return false;
  }

  std::string OuterRepeatedAddSchedulePredicate(
      const OuterRepeatedAddSchedule& schedule, u32 publication) const {
    std::vector<std::pair<u32, u32>> ranges;
    for (u32 outer = 0u; outer < schedule.add_counts.size();) {
      if (schedule.add_counts[outer] < publication) {
        outer++;
        continue;
      }
      const u32 first = outer;
      while (outer + 1u < schedule.add_counts.size() &&
             schedule.add_counts[outer + 1u] >= publication) {
        outer++;
      }
      ranges.emplace_back(first, outer);
      outer++;
    }
    if (ranges.empty())
      return "false";
    if (ranges.size() == 1u && ranges.front().first == 0u &&
        ranges.front().second + 1u == schedule.add_counts.size()) {
      return "true";
    }
    std::string predicate;
    for (const auto [first, last] : ranges) {
      if (!predicate.empty())
        predicate += " || ";
      if (first == last) {
        predicate += "outer == " + std::to_string(first) + "u";
      } else if (first == 0u) {
        predicate += "outer <= " + std::to_string(last) + "u";
      } else if (last + 1u == schedule.add_counts.size()) {
        predicate += "outer >= " + std::to_string(first) + "u";
      } else {
        predicate += "(outer >= " + std::to_string(first) +
                     "u && outer <= " + std::to_string(last) + "u)";
      }
    }
    return predicate;
  }

  std::string OuterRepeatedAddScheduleCountExpression(
      const OuterRepeatedAddSchedule& schedule,
      const char* outer_name) const {
    if (schedule.add_counts.empty())
      return "0.0f";

    // The product profile is allowed to approximate output-only vertex
    // arithmetic, but not the compiler-proven invocation selector. Keep the
    // outer-to-count mapping exact and replace only the repeated floating-point
    // additions with one native SGX multiply/add. The product ABI materializes
    // selector once in main so its reuse does not depend on Shacc inlining or
    // common-subexpression recovery across BSpline's many use sites.
    std::string expression =
        std::to_string(schedule.add_counts.back());
    for (u32 outer = static_cast<u32>(schedule.add_counts.size() - 1u);
         outer-- > 0u;) {
      expression = "VitaVuSelectBits(" + expression + ", " +
                   std::to_string(schedule.add_counts[outer]) +
                   ", -int(unsigned int(" + std::string(outer_name) +
                   " == " + std::to_string(outer) + "u) & 1u))";
    }
    return "float(" + expression + ")";
  }

  void AppendLeanOuterRepeatedAddScheduleCounts() {
    if (!UsesLeanNativeOutputProfile())
      return;
    for (u32 schedule_index = 0u;
         schedule_index < m_kernel.outer_repeated_add_schedules.size();
         schedule_index++) {
      for (bool flat_color : {false, true}) {
        if (!UsesOuterRepeatedAddSchedule(schedule_index,
                flat_color ? &m_flat_color_reachable : &m_reachable))
          continue;
        m_source += "\tconst float VuOuterRepeatCount";
        m_source += std::to_string(schedule_index);
        m_source += flat_color ? "Flat = " : " = ";
        m_source += OuterRepeatedAddScheduleCountExpression(
            m_kernel.outer_repeated_add_schedules[schedule_index],
            OuterIterationName(flat_color));
        m_source += ";\n";
      }
    }
  }

  void AppendOuterRepeatedAddScheduleHelpers() {
    for (u32 schedule_index = 0u;
         schedule_index < m_kernel.outer_repeated_add_schedules.size();
         schedule_index++) {
      if (!UsesOuterRepeatedAddSchedule(schedule_index))
        continue;
      // The product publishes one exact outer-to-count selection in main and
      // lets every output-only recurrence consume that scalar. Emitting a
      // helper here would let Shacc inline and rebuild the same selector at
      // every BSpline use site.
      if (UsesLeanNativeOutputProfile())
        continue;
      const OuterRepeatedAddSchedule& schedule =
          m_kernel.outer_repeated_add_schedules[schedule_index];
      const u32 maximum = schedule.add_counts.empty()
                              ? 0u
                              : *std::max_element(schedule.add_counts.begin(),
                                                  schedule.add_counts.end());
      for (u32 right_carried = 0u; right_carried < 2u; right_carried++) {
        const std::string name = std::string("VitaVuOuterScheduledAdd") +
            (right_carried != 0u ? "Right" : "Left") +
            std::to_string(schedule_index);
        std::string definition = "float " + name +
                    "(float initial, float increment, unsigned int outer)\n"
                    "{\n";
        definition += "\tfloat value = initial;\n";
        for (u32 publication = 1u; publication <= maximum; publication++) {
          const std::string next =
              "scheduledNext" + std::to_string(publication);
          definition += "\tconst float " + next + " = ";
          definition +=
              UsesStructuredExactF32(ExpressionKind::OuterRepeatedAdd)
                  ? "VitaVuF32Add"
                  : "VitaVuNativeF32Add";
          definition += right_carried != 0u ? "(increment, value);\n"
                                           : "(value, increment);\n";
          definition +=
              "\tvalue = intBitsToFloat(VitaVuSelectBits("
              "floatToRawIntBits(value), floatToRawIntBits(" +
              next + "), -int(unsigned int(" +
              OuterRepeatedAddSchedulePredicate(schedule, publication) +
              ") & 1u)));\n";
        }
        definition += "\treturn value;\n}\n\n";
        AppendProfiledF32Helper(ExpressionKind::OuterRepeatedAdd,
            name.c_str(), definition);
      }
    }
  }

  std::string ProfiledF32HelperName(u32 node_id, const char* name) const {
    return std::string(name) +
        (UsesAnyStructuredExactF32() && NodeUsesStructuredNativeF32(node_id)
             ? "NativeFmac" : "");
  }

  void AppendProfiledF32Helper(ExpressionKind kind, const char* name,
                              std::string_view definition) {
    if (UsesAnyStructuredExactF32() && UsesAnyStructuredNativeF32() &&
        !Uses(kind))
      return;
    if (!UsesAnyStructuredExactF32() || !UsesStructuredNativeF32(kind)) {
      m_source += definition;
      return;
    }
    // A mixed root cannot alias the global exact helper names to native
    // FMAC. Give this composite operation a separately named native body;
    // keep the exact body as well when another node of this kind needs it.
    std::string native_definition(definition);
    const auto replace_identifier_prefix = [&](std::string_view from,
                                               std::string_view to) {
      size_t position = 0u;
      while ((position = native_definition.find(from, position)) !=
             std::string::npos) {
        native_definition.replace(position, from.size(), to);
        position += to.size();
      }
    };
    replace_identifier_prefix(name, std::string(name) + "NativeFmac");
    replace_identifier_prefix("VitaVuF32", "VitaVuNativeF32");
    m_source += native_definition;
    if (UsesStructuredExactF32(kind))
      m_source += definition;
  }

  void AppendHelpers() {
    m_source +=
        "// Generated from PairPlan semantics; ShaccCg owns SSA, allocation, "
        "and scheduling.\n";
    if (m_program->uses_buffered_batch_inputs) {
      // PSP2 Cg scalarizes a dynamically indexed int4.w at the following
      // vector: the exact IGA resume shader loaded record dword 0x3c instead
      // of 0x3b.  Keep raw binding indices integer, but use float4 for VU
      // seeds so psp2cgc emits an aligned fetch4 and selects w correctly.
      m_source +=
          "struct VitaVuBatchRecord\n"
          "{\n"
          "\tint4 bindings[";
      m_source += std::to_string(m_program->BatchBindingVectorCount());
      m_source += "];\n";
      if (!m_program->uses_instance_indexed_batch_live_ins &&
          m_program->BatchInvariantUniformVectorCount() != 0) {
        m_source += "\tfloat4 uniforms[";
        // The C structure stride must describe exactly the host-packed BUFFER1
        // record, not the total semantic live-in count. ABI 28/32 declared the
        // latter while GSDeviceGXM advanced records by BatchRecordVectorCount();
        // object one therefore read its bindings one vector late whenever one
        // live-in varied. ABI-51 and product ABI-54 place those values in the
        // explicit per-object tail below.
        m_source +=
            std::to_string(m_program->BatchInvariantUniformVectorCount());
        m_source += "];\n";
      }
      if (!m_program->uses_instance_indexed_batch_live_ins &&
          m_program->UsesInlineBatchVaryingTail() &&
          m_program->BatchVaryingLiveInVectorCount() != 0u) {
        // Hot partials reuse BUFFER1's already-proven dynamic record base.
        // Invariants stay at record zero while only the observed varying
        // vectors occupy this per-object tail. ABI-26 keeps its historical
        // canary layout and remains a distinct correctness/cache namespace.
        // "varying" is a reserved Cg token.  Keep the generated member name
        // explicit and compiler-neutral; exact libshaccCg rejects a struct
        // field named varying even though the host source validators accept it.
        m_source += "\tfloat4 object_values[";
        m_source +=
            std::to_string(m_program->BatchVaryingLiveInVectorCount());
        m_source += "];\n";
      }
      m_source += "};\n\n";
    }
    if (UsesBranchlessLoopKernelControl()) {
      m_source +=
          "int VitaVuSelectBits(int falseBits, int trueBits, int mask)\n"
          "{\n"
          "\treturn (falseBits & ~mask) | (trueBits & mask);\n"
          "}\n\n";
    }
    if (UsesAnyStructuredNativeF32() && !UsesLeanNativeOutputProfile()) {
      AppendStructuredNativeF32Helpers();
    }
    if (UsesAnyStructuredExactF32() &&
        (Uses(ExpressionKind::RoundedAdd) ||
         Uses(ExpressionKind::OuterRepeatedAdd) ||
         Uses(ExpressionKind::RoundedSubtract) ||
         Uses(ExpressionKind::RoundedMultiply) ||
         Uses(ExpressionKind::EfuSumXyzSquares) ||
         Uses(ExpressionKind::ArmApproximateReciprocal) ||
         Uses(ExpressionKind::ArmApproximateSquareRoot))) {
      AppendStructuredSoftwareF32Helpers();
    } else if (!UsesLeanNativeOutputProfile() &&
               !UsesStructuredSoftwareF32() &&
               (Uses(ExpressionKind::RoundedMultiply) ||
                Uses(ExpressionKind::EfuSumXyzSquares) ||
                Uses(ExpressionKind::ArmApproximateReciprocal) ||
                Uses(ExpressionKind::ArmApproximateSquareRoot))) {
      // Keep the helpers shared and their names compact: generated structured
      // roots can contain hundreds of PairPlan arithmetic boundaries, and
      // ShaccCg has a materially lower reliable source-size ceiling than the
      // offline compiler.
      m_source +=
          "float VitaVuF32Mul(float left, float right)\n"
          "{\n"
          "\treturn intBitsToFloat(floatToRawIntBits(left * right));\n"
          "}\n\n"
          "float4 VitaVuF32Mul4(float4 left, float4 right)\n"
          "{\n"
          "\treturn bit_cast<float4>(bit_cast<unsigned int4>(left * right));\n"
          "}\n\n";
    }
    if (!UsesLeanNativeOutputProfile() &&
        !UsesStructuredSoftwareF32() &&
        Uses(ExpressionKind::EfuSumXyzSquares)) {
      m_source +=
          "float VitaVuF32Add(float left, float right)\n"
          "{\n"
          "\treturn intBitsToFloat(floatToRawIntBits(left + right));\n"
          "}\n\n";
    }
    if (!UsesLeanNativeOutputProfile() &&
        !UsesStructuredSoftwareF32() &&
        (Uses(ExpressionKind::ArmApproximateReciprocal) ||
         Uses(ExpressionKind::ArmApproximateSquareRoot))) {
      m_source +=
          "float VitaVuF32Sub(float left, float right)\n"
          "{\n"
          "\treturn intBitsToFloat(floatToRawIntBits(left - right));\n"
          "}\n\n";
    }
    if (!UsesLeanNativeOutputProfile() && !UsesAnyStructuredExactF32() &&
        (UsesStructuredNativeF32(ExpressionKind::EfuSumXyzSquares) ||
        UsesStructuredNativeF32(ExpressionKind::ArmApproximateReciprocal) ||
        UsesStructuredNativeF32(ExpressionKind::ArmApproximateSquareRoot))) {
      // ApproximateFmac is an explicit product permission for native SGX
      // arithmetic. Preserve the PCSX2/ARM estimate seed and each refinement
      // boundary, but keep ERSADD/ESQRT on the same native non-fused arithmetic
      // tier instead of pulling the scalar software-F32 library into every
      // point invocation. Exact mode never reaches these aliases.
      m_source +=
          "float VitaVuF32Mul(float left, float right)\n"
          "{\n\treturn VitaVuNativeF32Mul(left, right);\n}\n\n";
      if (UsesStructuredNativeF32(ExpressionKind::EfuSumXyzSquares)) {
        m_source +=
            "float VitaVuF32Add(float left, float right)\n"
            "{\n\treturn VitaVuNativeF32Add(left, right);\n}\n\n";
      }
      if (UsesStructuredNativeF32(
              ExpressionKind::ArmApproximateReciprocal) ||
          UsesStructuredNativeF32(
              ExpressionKind::ArmApproximateSquareRoot)) {
        m_source +=
            "float VitaVuF32Sub(float left, float right)\n"
            "{\n\treturn VitaVuNativeF32Sub(left, right);\n}\n\n";
      }
    }
    if (UsesUnscheduledOuterRepeatedAdd() &&
        !UsesLeanNativeOutputProfile()) {
      std::string definition =
          "float VitaVuOuterRepeatedAdd(float initial, float increment, "
          "unsigned int outer)\n"
          "{\n"
          "\tfloat value = initial;\n";
      for (u32 iteration = 1u;
           iteration < m_kernel.outer_iteration_count; iteration++) {
          definition += "\tconst float next" + std::to_string(iteration) +
                      " = ";
          definition +=
              UsesStructuredExactF32(ExpressionKind::OuterRepeatedAdd)
                  ? "VitaVuF32Add"
                  : "VitaVuNativeF32Add";
          definition += "(value, increment);\n";
          definition +=
              "\tvalue = intBitsToFloat(VitaVuSelectBits("
              "floatToRawIntBits(value), floatToRawIntBits(next" +
              std::to_string(iteration) + "), -int(unsigned int(outer >= " +
              std::to_string(iteration) + "u) & 1u)));\n";
      }
      definition += "\treturn value;\n}\n\n";
      AppendProfiledF32Helper(ExpressionKind::OuterRepeatedAdd,
          "VitaVuOuterRepeatedAdd", definition);
      definition =
          "float VitaVuOuterRepeatedAddRight(float initial, float increment, "
          "unsigned int outer)\n"
          "{\n"
          "\tfloat value = initial;\n";
      for (u32 iteration = 1u;
           iteration < m_kernel.outer_iteration_count; iteration++) {
          definition += "\tconst float rightNext" +
                      std::to_string(iteration) + " = ";
          definition +=
              UsesStructuredExactF32(ExpressionKind::OuterRepeatedAdd)
                  ? "VitaVuF32Add"
                  : "VitaVuNativeF32Add";
          definition += "(increment, value);\n";
          definition +=
              "\tvalue = intBitsToFloat(VitaVuSelectBits("
              "floatToRawIntBits(value), floatToRawIntBits(rightNext" +
              std::to_string(iteration) + "), -int(unsigned int(outer >= " +
              std::to_string(iteration) + "u) & 1u)));\n";
      }
      definition += "\treturn value;\n}\n\n";
      AppendProfiledF32Helper(ExpressionKind::OuterRepeatedAdd,
          "VitaVuOuterRepeatedAddRight", definition);
    }
    AppendOuterRepeatedAddScheduleHelpers();
    if (!UsesLeanNativeOutputProfile() &&
        (Uses(ExpressionKind::Normalize) || Uses(ExpressionKind::Divide) ||
         Uses(ExpressionKind::EfuSumXyzSquares) ||
         Uses(ExpressionKind::ArmApproximateReciprocal) ||
         Uses(ExpressionKind::ArmApproximateSquareRoot))) {
      if (UsesBranchlessLoopKernelControl()) {
        m_source +=
            "float VitaVuNormalize(float value)\n"
            "{\n"
            "\tconst int bits = floatToRawIntBits(value);\n"
            "\tconst int exponent = bits & 2139095040;\n"
            "\tconst int signBits = bits & (-2147483647 - 1);\n"
            "\tconst int zeroMask = -int(unsigned int(exponent == 0) & 1u);\n"
            "\tconst int specialMask = -int(unsigned int(exponent == 2139095040) & 1u);\n"
            "\tconst int zeroSelected = VitaVuSelectBits(bits, signBits, "
            "zeroMask);\n"
            "\tconst int specialValue = signBits | 2139095039;\n"
            "\treturn intBitsToFloat(VitaVuSelectBits(zeroSelected, "
            "specialValue, specialMask));\n"
            "}\n\n";
      } else {
        m_source +=
            "float VitaVuNormalize(float value)\n"
            "{\n"
            "\tconst int bits = floatToRawIntBits(value);\n"
            "\tconst int exponent = bits & 2139095040;\n"
            "\tif (exponent == 0)\n"
            "\t\treturn intBitsToFloat(bits & (-2147483647 - 1));\n"
            "\tif (exponent == 2139095040)\n"
            "\t\treturn intBitsToFloat((bits < 0 ? "
            "(-2147483647 - 1) : 0) | 2139095039);\n"
            "\treturn value;\n"
            "}\n\n";
      }
      if (HasVectorGroup(ExpressionKind::Normalize)) {
        m_source +=
          // Keep lane-isomorphic VU normalization as one branch-free float4
          // expression.  Besides exposing SGX vector integer arithmetic,
          // this prevents Sony's O1 optimizer from constructing sixteen
          // independent scalar control-flow diamonds for a four-vector sink.
          // Shacc 3.0 can materialize a true predicate as 0xffffffff. Masking
          // to one bit before negation makes both 1 and 0xffffffff produce the
          // same ordinary all-bits select mask.
          "float4 VitaVuNormalize4(float4 value)\n"
          "{\n"
          "\tconst int4 bits = bit_cast<int4>(value);\n"
          "\tconst int4 exponent = bits & int4(2139095040);\n"
          "\tconst int4 sign = bits & int4(-2147483647 - 1);\n"
          "\tconst int4 zeroMask = -(int4(exponent == int4(0)) & int4(1));\n"
          "\tconst int4 specialMask = -(int4(exponent == "
          "int4(2139095040)) & int4(1));\n"
          "\tconst int4 zeroValue = sign;\n"
          "\tconst int4 specialValue = sign | int4(2139095039);\n"
          "\tconst int4 zeroSelected = (bits & ~zeroMask) | "
          "(zeroValue & zeroMask);\n"
          "\tconst int4 result = (zeroSelected & ~specialMask) | "
          "(specialValue & specialMask);\n"
          "\treturn bit_cast<float4>(result);\n"
          "}\n\n";
      }
    }
    if (Uses(ExpressionKind::Divide) && !UsesLeanNativeOutputProfile()) {
      if (m_output_kind != CgOutputKind::LoopKernelDirectTfx) {
        // Historical specialized/validation roots retain their established
        // native quotient contract. They are separately attested and are not
        // permitted to satisfy the hardware-driven loop-kernel cache key.
        if (UsesBranchlessLoopKernelControl()) {
          m_source +=
              "float VitaVuDivide(float numerator, float denominator)\n"
              "{\n"
              "\tconst int signBit = (floatToRawIntBits(numerator) ^ "
              "floatToRawIntBits(denominator)) & (-2147483647 - 1);\n"
              "\tconst int zeroValue = signBit | 2139095039;\n"
              "\tconst int quotient = floatToRawIntBits(VitaVuNormalize("
              "numerator / denominator));\n"
              "\tconst int zeroMask = -int(unsigned int(denominator == 0.0f) & 1u);\n"
              "\treturn intBitsToFloat(VitaVuSelectBits(quotient, "
              "zeroValue, zeroMask));\n"
              "}\n\n";
        } else {
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
      } else {
      // PVR_PSP2's Series5 compiler expands floating DIV into FRCP followed by
      // FMUL.  Neither that compiler source nor the Series5XT architecture
      // guide promises a correctly-rounded quotient, so exact-Q cannot use Cg
      // '/'.  Emit a fixed, branch-free restoring divider over normalized
      // 24-bit significands.  Keeping every step lexical and predicated lets
      // Sony's backend retain parallel execution mode instead of introducing
      // data-dependent flow control.
      m_source +=
          "float VitaVuDivide(float numeratorValue, float denominatorValue)\n"
          "{\n"
          "\tconst unsigned int numerator = unsigned int(floatToRawIntBits("
          "VitaVuNormalize(numeratorValue)));\n"
          "\tconst unsigned int denominator = unsigned int(floatToRawIntBits("
          "VitaVuNormalize(denominatorValue)));\n"
          "\tconst unsigned int signBits = (numerator ^ denominator) & "
          "2147483648u;\n"
          "\tconst unsigned int numeratorSignificand = "
          "(numerator & 8388607u) | 8388608u;\n"
          "\tconst unsigned int denominatorSignificand = "
          "(denominator & 8388607u) | 8388608u;\n"
          "\tconst unsigned int ratioShift = unsigned int("
          "numeratorSignificand < denominatorSignificand) & 1u;\n"
          "\tint exponent = int((numerator >> 23u) & 255u) - "
          "int((denominator >> 23u) & 255u) + 127 - int(ratioShift);\n"
          "\tunsigned int remainder = (numeratorSignificand << ratioShift) - "
          "denominatorSignificand;\n"
          "\tunsigned int quotient = 8388608u;\n";
      for (s32 bit = 22; bit >= 0; bit--) {
        const std::string suffix = std::to_string(bit);
        m_source +=
            "\tremainder <<= 1u;\n"
            "\tconst unsigned int quotientBit" + suffix +
            " = unsigned int(remainder >= denominatorSignificand) & 1u;\n"
            "\tremainder -= denominatorSignificand & "
            "(0u - quotientBit" + suffix + ");\n"
            "\tquotient |= quotientBit" + suffix + " << " + suffix +
            "u;\n";
      }
      m_source +=
          "\tconst unsigned int twiceRemainder = remainder << 1u;\n"
          "\tconst unsigned int roundUp = "
          "(unsigned int(twiceRemainder > denominatorSignificand) & 1u) | "
          "((unsigned int(twiceRemainder == denominatorSignificand) & 1u) & "
          "(quotient & 1u));\n"
          "\tquotient += roundUp;\n"
          "\tconst unsigned int carry = quotient >> 24u;\n"
          "\tquotient >>= carry;\n"
          "\texponent += int(carry);\n"
          "\tconst unsigned int underflowMask = 0u - "
          "(unsigned int(exponent <= 0) & 1u);\n"
          "\tconst unsigned int overflowMask = 0u - "
          "(unsigned int(exponent >= 255) & 1u);\n"
          "\tconst unsigned int numeratorZeroMask = 0u - "
          "(unsigned int((numerator & 2147483647u) == 0u) & 1u);\n"
          "\tconst unsigned int denominatorZeroMask = 0u - "
          "(unsigned int((denominator & 2147483647u) == 0u) & 1u);\n"
          "\tunsigned int result = signBits | "
          "(unsigned int(exponent) << 23u) | (quotient & 8388607u);\n"
          "\tresult = (result & ~underflowMask) | "
          "(signBits & underflowMask);\n"
          "\tresult = (result & ~overflowMask) | "
          "((signBits | 2139095039u) & overflowMask);\n"
          "\tresult = (result & ~numeratorZeroMask) | "
          "(signBits & numeratorZeroMask);\n"
          "\tresult = (result & ~denominatorZeroMask) | "
          "((signBits | 2139095039u) & denominatorZeroMask);\n"
          "\treturn intBitsToFloat(int(result));\n"
          "}\n\n";
      }
    }
    if ((Uses(ExpressionKind::Minimum) || Uses(ExpressionKind::Maximum)) &&
        !UsesLeanNativeOutputProfile()) {
      // The PSP2 Cg compiler has no unambiguous integer min/max overload.
      // Explicit signed bit comparisons preserve the existing PS2 raw-float
      // ordering without converting the operands through host floating point.
      const char* const minimum_select = UsesBranchlessLoopKernelControl()
          ? "VitaVuSelectBits(rightBits, leftBits, "
            "-int(unsigned int(leftKey < rightKey) & 1u))"
          : "leftKey < rightKey ? leftBits : rightBits";
      const char* const maximum_select = UsesBranchlessLoopKernelControl()
          ? "VitaVuSelectBits(rightBits, leftBits, "
            "-int(unsigned int(leftKey > rightKey) & 1u))"
          : "leftKey > rightKey ? leftBits : rightBits";
      m_source +=
          "float VitaVuMinimum(float left, float right)\n"
          "{\n"
          "\tconst int leftBits = floatToRawIntBits(left);\n"
          "\tconst int rightBits = floatToRawIntBits(right);\n"
          "\tconst unsigned int leftKey = unsigned int(leftBits ^ "
          "((leftBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst unsigned int rightKey = unsigned int(rightBits ^ "
          "((rightBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst int orderedBits = ";
      m_source += minimum_select;
      m_source +=
          ";\n\treturn intBitsToFloat(orderedBits);\n"
          "}\n\n"
          "float VitaVuMaximum(float left, float right)\n"
          "{\n"
          "\tconst int leftBits = floatToRawIntBits(left);\n"
          "\tconst int rightBits = floatToRawIntBits(right);\n"
          "\tconst unsigned int leftKey = unsigned int(leftBits ^ "
          "((leftBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst unsigned int rightKey = unsigned int(rightBits ^ "
          "((rightBits >> 31) | (-2147483647 - 1)));\n"
          "\tconst int orderedBits = ";
      m_source += maximum_select;
      m_source +=
          ";\n\treturn intBitsToFloat(orderedBits);\n"
          "}\n\n";
    }
    if (Uses(ExpressionKind::EfuSumXyzSquares)) {
      if (UsesLeanNativeOutputProfile()) {
        m_source +=
            "float VitaVuNativeEfuSumXyzSquares(float xValue, float yValue, "
            "float zValue)\n"
            "{\n"
            "\tconst float3 value = float3(xValue, yValue, zValue);\n"
            "\treturn dot(value, value);\n"
            "}\n\n";
      } else {
        AppendProfiledF32Helper(ExpressionKind::EfuSumXyzSquares,
            "VitaVuEfuSumXyzSquares",
            "float VitaVuEfuSumXyzSquares(float xValue, float yValue, "
            "float zValue)\n"
            "{\n"
            "\tconst float x = VitaVuNormalize(xValue);\n"
            "\tconst float y = VitaVuNormalize(yValue);\n"
            "\tconst float z = VitaVuNormalize(zValue);\n"
            "\tconst float xx = VitaVuF32Mul(x, x);\n"
            "\tconst float yy = VitaVuF32Mul(y, y);\n"
            "\tconst float zz = VitaVuF32Mul(z, z);\n"
            "\tconst float xy = VitaVuF32Add(xx, yy);\n"
            "\treturn VitaVuF32Add(xy, zz);\n"
            "}\n\n");
      }
    }
    if ((Uses(ExpressionKind::ArmApproximateReciprocal) ||
         Uses(ExpressionKind::ArmApproximateSquareRoot)) &&
        !UsesLeanNativeOutputProfile()) {
      // PCSX2 owners: VUops.cpp::_vuERSADD()/_vuESQRT() with
      // vu1ApproximateP. BUFFER7 is
      // initialized from ARM ARM A8.6.378's RecipSqrtEstimate table by the
      // persistent GPU-VU owner.  Every arithmetic intermediate crosses raw
      // bits so ShaccCg cannot fuse the Cortex-A9 VRSQRTS refinement.
      if (UsesBranchlessLoopKernelControl()) {
        m_source +=
          "unsigned int VitaVuApproximateReciprocalEstimateIndex("
          "float value)\n"
          "{\n"
          "\tconst unsigned int bits = unsigned int(floatToRawIntBits("
          "VitaVuNormalize(value)));\n"
          "\treturn (bits >> 15u) & 255u;\n"
          "}\n\n";
        AppendProfiledF32Helper(ExpressionKind::ArmApproximateReciprocal,
          "VitaVuArmApproximateReciprocal",
          "float VitaVuArmApproximateReciprocal(float value, "
          "unsigned int estimateField)\n"
          "{\n"
          "\tconst unsigned int valueBits = unsigned int("
          "floatToRawIntBits(VitaVuNormalize(value)));\n"
          "\tconst unsigned int signBits = valueBits & 2147483648u;\n"
          "\tconst unsigned int exponent = (valueBits >> 23u) & 255u;\n"
          "\tconst int zeroMask = -int(unsigned int(exponent == 0u) & 1u);\n"
          "\tconst int extremeMask = -int(unsigned int(exponent >= 253u) & 1u);\n"
          "\tconst int nonNormalMask = zeroMask | extremeMask;\n"
          "\tconst unsigned int safeBits = unsigned int(VitaVuSelectBits("
          "int(valueBits), 1065353216, nonNormalMask));\n"
          "\tconst unsigned int safeExponent = (safeBits >> 23u) & 255u;\n"
          "\tconst unsigned int seedBits = (safeBits & 2147483648u) | "
          "((253u - safeExponent) << 23u) | (estimateField << 15u);\n"
          "\tconst float rawValue = intBitsToFloat(int(safeBits));\n"
          "\tconst float seed = intBitsToFloat(int(seedBits));\n"
          "\tconst float product = VitaVuF32Mul(rawValue, seed);\n"
          "\tconst float refinementStep = VitaVuF32Sub(2.0f, product);\n"
          "\tconst int refinedBits = floatToRawIntBits("
          "VitaVuF32Mul(seed, refinementStep));\n"
          "\tconst int extremeBits = VitaVuSelectBits(refinedBits, "
          "int(signBits), extremeMask);\n"
          "\treturn intBitsToFloat(VitaVuSelectBits(extremeBits, "
          "int(valueBits), zeroMask));\n"
          "}\n\n");
        m_source +=
          "unsigned int VitaVuApproximateSqrtEstimateIndex(float value)\n"
          "{\n"
          "\tconst unsigned int bits = unsigned int(floatToRawIntBits("
          "VitaVuNormalize(value)));\n"
          "\tconst unsigned int exponent = (bits >> 23u) & 255u;\n"
          "\tconst unsigned int odd = 128u | ((bits >> 16u) & 127u);\n"
          "\tconst unsigned int even = 256u | ((bits >> 15u) & 255u);\n"
          "\tconst int oddMask = -int(unsigned int((exponent & 1u) != 0u) & 1u);\n"
          "\tconst unsigned int scaled = unsigned int(VitaVuSelectBits("
          "int(even), int(odd), oddMask));\n"
          "\treturn 256u + scaled - 128u;\n"
          "}\n\n";
        AppendProfiledF32Helper(ExpressionKind::ArmApproximateSquareRoot,
          "VitaVuArmApproximateSqrt",
          "float VitaVuArmApproximateSqrt(float value, "
          "unsigned int estimateField)\n"
          "{\n"
          "\tconst unsigned int valueBits = unsigned int("
          "floatToRawIntBits(VitaVuNormalize(value)));\n"
          "\tconst unsigned int exponent = (valueBits >> 23u) & 255u;\n"
          "\tconst unsigned int specialBit = "
          "(unsigned int((valueBits & 2147483648u) != 0u) & 1u) | "
          "(unsigned int((valueBits & 2147483647u) == 0u) & 1u) | "
          "(unsigned int(exponent == 255u) & 1u);\n"
          "\tconst int specialMask = -int(specialBit);\n"
          "\tconst unsigned int safeBits = unsigned int(VitaVuSelectBits("
          "int(valueBits), 1065353216, specialMask));\n"
          "\tconst unsigned int safeExponent = (safeBits >> 23u) & 255u;\n"
          "\tconst unsigned int seedBits = "
          "(((380u - safeExponent) >> 1u) << 23u) | "
          "(estimateField << 15u);\n"
          "\tconst float seed = intBitsToFloat(int(seedBits));\n"
          "\tconst float rawValue = intBitsToFloat(int(safeBits));\n"
          "\tconst float seedSquare = VitaVuF32Mul(seed, seed);\n"
          "\tconst float product = VitaVuF32Mul(rawValue, seedSquare);\n"
          "\tconst float difference = VitaVuF32Sub(3.0f, product);\n"
          "\tconst float refinementStep = VitaVuF32Mul(difference, 0.5f);\n"
          "\tconst float refined = VitaVuF32Mul(seed, refinementStep);\n"
          "\tconst int resultBits = floatToRawIntBits("
          "VitaVuF32Mul(rawValue, refined));\n"
          "\treturn intBitsToFloat(VitaVuSelectBits(resultBits, "
          "int(valueBits), specialMask));\n"
          "}\n\n");
      } else {
        m_source +=
          "unsigned int VitaVuApproximateReciprocalEstimateIndex("
          "float value)\n"
          "{\n"
          "\tconst unsigned int bits = unsigned int(floatToRawIntBits("
          "VitaVuNormalize(value)));\n"
          "\treturn (bits >> 15u) & 255u;\n"
          "}\n\n";
        AppendProfiledF32Helper(ExpressionKind::ArmApproximateReciprocal,
          "VitaVuArmApproximateReciprocal",
          "float VitaVuArmApproximateReciprocal(float value, "
          "unsigned int estimateField)\n"
          "{\n"
          "\tconst unsigned int valueBits = unsigned int("
          "floatToRawIntBits(VitaVuNormalize(value)));\n"
          "\tconst unsigned int signBits = valueBits & 2147483648u;\n"
          "\tconst unsigned int exponent = (valueBits >> 23u) & 255u;\n"
          "\tif (exponent == 0u) return intBitsToFloat(int(valueBits));\n"
          "\tif (exponent == 255u || exponent >= 253u) "
          "return intBitsToFloat(int(signBits));\n"
          "\tconst unsigned int seedBits = signBits | "
          "((253u - exponent) << 23u) | (estimateField << 15u);\n"
          "\tconst float rawValue = intBitsToFloat(int(valueBits));\n"
          "\tconst float seed = intBitsToFloat(int(seedBits));\n"
          "\tconst float product = VitaVuF32Mul(rawValue, seed);\n"
          "\tconst float refinementStep = VitaVuF32Sub(2.0f, product);\n"
          "\treturn VitaVuF32Mul(seed, refinementStep);\n"
          "}\n\n");
        m_source +=
          "unsigned int VitaVuApproximateSqrtEstimateIndex(float value)\n"
          "{\n"
          "\tconst unsigned int bits = unsigned int(floatToRawIntBits("
          "VitaVuNormalize(value)));\n"
          "\tconst unsigned int exponent = (bits >> 23u) & 255u;\n"
          "\tconst unsigned int scaled = (exponent & 1u) != 0u ? "
          "(128u | ((bits >> 16u) & 127u)) : "
          "(256u | ((bits >> 15u) & 255u));\n"
          "\treturn 256u + scaled - 128u;\n"
          "}\n\n";
        AppendProfiledF32Helper(ExpressionKind::ArmApproximateSquareRoot,
          "VitaVuArmApproximateSqrt",
          "float VitaVuArmApproximateSqrt(float value, "
          "unsigned int estimateField)\n"
          "{\n"
          "\tconst unsigned int valueBits = unsigned int("
          "floatToRawIntBits(VitaVuNormalize(value)));\n"
          "\tconst unsigned int exponent = (valueBits >> 23u) & 255u;\n"
          "\tif ((valueBits & 2147483648u) != 0u || "
          "(valueBits & 2147483647u) == 0u || exponent == 255u)\n"
          "\t\treturn intBitsToFloat(int(valueBits));\n"
          "\tconst unsigned int seedBits = "
          "(((380u - exponent) >> 1u) << 23u) | "
          "(estimateField << 15u);\n"
          "\tconst float seed = intBitsToFloat(int(seedBits));\n"
          "\tconst float rawValue = intBitsToFloat(int(valueBits));\n"
          "\tconst float seedSquare = VitaVuF32Mul(seed, seed);\n"
          "\tconst float product = VitaVuF32Mul(rawValue, seedSquare);\n"
          "\tconst float difference = VitaVuF32Sub(3.0f, product);\n"
          "\tconst float refinementStep = VitaVuF32Mul(difference, 0.5f);\n"
          "\tconst float refined = VitaVuF32Mul(seed, refinementStep);\n"
          "\treturn VitaVuF32Mul(rawValue, refined);\n"
          "}\n\n");
      }
    }
    if (Uses(ExpressionKind::FloatToInt) &&
        !UsesLeanNativeOutputProfile() &&
        (!UsesApproximateLoopKernelConversions() ||
         UsesBranchlessLoopKernelControl() ||
         m_program->uses_loop_kernel_ftoi_probe_output)) {
      if (UsesBranchlessLoopKernelControl()) {
        m_source +=
            "int VitaVuFloatToInt(float value, unsigned int scaleOffset)\n"
            "{\n"
            "\tconst unsigned int bits = unsigned int(floatToRawIntBits(value));\n"
            "\tconst unsigned int exponent = (bits >> 23u) & 255u;\n"
            "\tconst int integerExponent = int(exponent) + int(scaleOffset) - 127;\n"
            "\tconst int binaryShift = integerExponent - 23;\n"
            // SceShaccCg 3.0 does not consistently preserve Cg's 0/1 integer
            // representation when several comparison results feed integer
            // multiplication. The ABI-9 physical GXP materialized each true
            // finite-range predicate as 0xffffffff, multiplied the three
            // values, and then negated the product, leaving a one-bit mask.
            // Derive every direction/range mask from arithmetic sign bits so
            // neither Shacc nor psp2cgc can choose a predicate representation.
            "\tconst int negativeShiftMask = binaryShift >> 31;\n"
            "\tconst unsigned int leftShift = "
            "unsigned int(binaryShift & ~negativeShiftMask) & 31u;\n"
            "\tconst unsigned int rightShift = "
            "unsigned int((-binaryShift) & negativeShiftMask) & 31u;\n"
            "\tconst unsigned int significand = (bits & 8388607u) | 8388608u;\n"
            "\tconst unsigned int magnitude = (significand >> rightShift) << "
            "leftShift;\n"
            "\tconst unsigned int exponentNonZeroBits = "
            "exponent | (0u - exponent);\n"
            "\tconst int exponentNonZeroMask = "
            "-int((exponentNonZeroBits >> 31u) & 1u);\n"
            "\tconst int nonNegativeMask = ~(integerExponent >> 31);\n"
            "\tconst int lessThan31Mask = (integerExponent - 31) >> 31;\n"
            "\tconst int finiteMask = exponentNonZeroMask & "
            "nonNegativeMask & lessThan31Mask;\n"
            "\tconst int finiteMagnitude = int(magnitude & "
            "unsigned int(finiteMask));\n"
            "\tconst int signMask = int(bits) >> 31;\n"
            "\tconst int converted = VitaVuSelectBits(finiteMagnitude, "
            "-finiteMagnitude, signMask);\n"
            "\tconst int saturated = VitaVuSelectBits(2147483647, "
            "(-2147483647 - 1), signMask);\n"
            "\tconst unsigned int exponent255Delta = exponent ^ 255u;\n"
            "\tconst unsigned int exponent255NonZeroBits = "
            "exponent255Delta | (0u - exponent255Delta);\n"
            "\tconst int exponent255Mask = "
            "~(-int((exponent255NonZeroBits >> 31u) & 1u));\n"
            "\tconst int atLeast31Mask = ~lessThan31Mask;\n"
            "\tconst int overflowMask = exponent255Mask | atLeast31Mask;\n"
            "\treturn VitaVuSelectBits(converted, saturated, "
            "overflowMask);\n"
            "}\n\n";
        m_source +=
            "float VitaVuFloatToMaskedFloat(float value, float scale, "
            "float modulus)\n"
            "{\n"
            "\tconst float scaled = value * scale;\n"
            "\tconst int bits = floatToRawIntBits(scaled);\n"
            "\tconst int signMask = -int(unsigned int(bits < 0) & 1u);\n"
            "\tconst int overflowMask = -int(unsigned int("
            "(bits & 2139095040) >= 1325400064) & 1u);\n"
            "\tconst float truncatedMagnitude = floor(abs(scaled));\n"
            "\tconst float remainder = "
            "frac(truncatedMagnitude / modulus) * modulus;\n"
            "\tconst int negativeRemainderMask = signMask & "
            "-int(unsigned int(remainder != 0.0f) & 1u);\n"
            "\tconst int corrected = VitaVuSelectBits("
            "floatToRawIntBits(remainder), "
            "floatToRawIntBits(modulus - remainder), "
            "negativeRemainderMask);\n"
            "\tconst int overflowValue = VitaVuSelectBits("
            "floatToRawIntBits(modulus - 1.0f), floatToRawIntBits(0.0f), "
            "signMask);\n"
            "\treturn intBitsToFloat(VitaVuSelectBits(corrected, "
            "overflowValue, overflowMask));\n"
            "}\n\n";
      } else {
        m_source +=
            "int VitaVuFloatToInt(float value, unsigned int scaleOffset)\n"
            "{\n"
            "\tconst unsigned int bits = unsigned int(floatToRawIntBits(value));\n"
            "\tconst unsigned int exponent = (bits >> 23u) & 255u;\n"
            "\tconst bool negative = (bits >> 31u) != 0u;\n"
            "\tconst int integerExponent = int(exponent) + int(scaleOffset) - 127;\n"
            "\tif (exponent == 255u || integerExponent >= 31)\n"
            "\t\treturn negative ? (-2147483647 - 1) : 2147483647;\n"
            "\tif (exponent == 0u || integerExponent < 0)\n"
            "\t\treturn 0;\n"
            "\tconst unsigned int significand = (bits & 8388607u) | 8388608u;\n"
            "\tconst unsigned int magnitude = integerExponent >= 23 ? "
            "significand << unsigned int(integerExponent - 23) : "
            "significand >> unsigned int(23 - integerExponent);\n"
            "\treturn negative ? -int(magnitude) : int(magnitude);\n"
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
    if (Uses(ExpressionKind::FloatToInt) &&
        !UsesLeanNativeOutputProfile() &&
        UsesPlayableLoopKernelFloatToInt()) {
      // Diagnostic only. The generated store path below deliberately uses the
      // raw-bit helper after physical ABI-22 disassembly and output showed that
      // ShaccCg's floor/int lowering is not full-range safe on Series5XT.
      m_source +=
          "int VitaVuPlayableFloatToInt(float value, unsigned int scaleOffset)\n"
          "{\n"
          "\tconst unsigned int bits = unsigned int(floatToRawIntBits(value));\n"
          "\tconst unsigned int exponent = (bits >> 23u) & 255u;\n"
          "\tconst int integerExponent = int(exponent) + int(scaleOffset) - 127;\n"
          "\tconst unsigned int exponentNonZeroBits = exponent | (0u - exponent);\n"
          "\tconst int exponentNonZeroMask = "
          "-int((exponentNonZeroBits >> 31u) & 1u);\n"
          "\tconst int nonNegativeMask = ~(integerExponent >> 31);\n"
          "\tconst int lessThan31Mask = (integerExponent - 31) >> 31;\n"
          "\tconst int finiteMask = exponentNonZeroMask & "
          "nonNegativeMask & lessThan31Mask;\n"
          "\tconst int safeBits = int(bits) & finiteMask;\n"
          "\tconst unsigned int scaleBits = (127u + scaleOffset) << 23u;\n"
          "\tconst float scaledMagnitude = abs(intBitsToFloat(safeBits) * "
          "intBitsToFloat(int(scaleBits)));\n"
          "\tconst int finiteMagnitude = int(floor(scaledMagnitude));\n"
          "\tconst int signMask = int(bits) >> 31;\n"
          "\tconst int converted = VitaVuSelectBits(finiteMagnitude, "
          "-finiteMagnitude, signMask);\n"
          "\tconst int saturated = VitaVuSelectBits(2147483647, "
          "(-2147483647 - 1), signMask);\n"
          "\tconst unsigned int exponent255Delta = exponent ^ 255u;\n"
          "\tconst unsigned int exponent255NonZeroBits = "
          "exponent255Delta | (0u - exponent255Delta);\n"
          "\tconst int exponent255Mask = "
          "~(-int((exponent255NonZeroBits >> 31u) & 1u));\n"
          "\tconst int overflowMask = exponent255Mask | ~lessThan31Mask;\n"
          "\treturn VitaVuSelectBits(converted, saturated, overflowMask);\n"
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

  bool CompactOuterTableUsedBy(u32 table_index,
                               const std::vector<bool>& reachable) const {
    for (const auto& [node_id, node_table] : m_compact_outer_node_table) {
      if (node_table == table_index && reachable[node_id])
        return true;
    }
    return false;
  }

  // ConfigureDirectInputLowering proves each division over this entire input
  // domain. Keep INDEX arithmetic unsigned and branch-free on SGX.
  void AppendBoundedIndexSplit(const char* input, u32 divisor, u32 domain,
                               const char* quotient, const char* remainder) {
    if ((divisor & (divisor - 1u)) == 0u) {
      u32 shift = 0u;
      while ((1u << shift) != divisor)
        shift++;
      m_source += "\tconst unsigned int " + std::string(quotient) + " = " +
                  input + " >> " + std::to_string(shift) + "u;\n";
      m_source += "\tconst unsigned int " + std::string(remainder) + " = " +
                  input + " & " + std::to_string(divisor - 1u) + "u;\n";
      return;
    }
    u32 multiplier = 0u;
    u32 shift = 0u;
    FindBoundedUnsignedDivisionMagic(divisor, domain, &multiplier, &shift);
    m_source += "\tconst unsigned int " + std::string(quotient) + " = (" +
                input + " * " + std::to_string(multiplier) + "u) >> " +
                std::to_string(shift) + "u;\n";
    m_source += "\tconst unsigned int " + std::string(remainder) + " = " +
                input + " - " + quotient + " * " +
                std::to_string(divisor) + "u;\n";
  }

  void AppendEntrySignature() {
    m_source += "void main(\n";
    u32 attribute_semantic = 0;
    if (UsesNestedIterationGrid()) {
      AppendParameter(
          "uniform int4 VuRawQwords[" +
          std::to_string(GeneratedCgProgram::DeclaredBufferVectors) +
          "] : BUFFER[0]");
      AppendParameter(
          "uniform VitaVuBatchRecord VuBatchData[1] : BUFFER[1]");
      if (m_program->batch_varying_live_ins.Any() &&
          !m_program->UsesInlineBatchVaryingTail()) {
        AppendParameter(
            "uniform float4 VuBatchVarying[1] : BUFFER[4]");
      }
      if (m_program->uses_instance_indexed_batch_live_ins) {
        AppendParameter(
            "__regformat unsigned int4 VuBatchIdentity : TEXCOORD" +
            std::to_string(attribute_semantic++));
        for (const CgConstantInput& input : m_program->constant_inputs) {
          AppendParameter("__regformat float4 VuConstant" +
                          std::to_string(input.uniform_index) + " : TEXCOORD" +
                          std::to_string(attribute_semantic++));
        }
        for (u32 reg = 1u; reg < 32u; reg++) {
          if ((m_program->vf_uniform_mask & (1u << reg)) == 0u)
            continue;
          AppendParameter("__regformat float4 VF" +
                          (reg < 10u ? std::string("0") : std::string()) +
                          std::to_string(reg) + " : TEXCOORD" +
                          std::to_string(attribute_semantic++));
        }
        if (m_program->uses_acc_uniform) {
          AppendParameter("__regformat float4 ACC : TEXCOORD" +
                          std::to_string(attribute_semantic++));
        }
        if (m_program->uses_q_uniform || m_program->uses_p_uniform ||
            m_program->uses_i_uniform || m_program->uses_gif_q_uniform) {
          AppendParameter(
              "__regformat float4 VuBatchScalars : TEXCOORD" +
              std::to_string(attribute_semantic++));
        }
      }
      AppendParameter(m_program->uses_flat_index_inputs
                          ? "unsigned int VuExpandedVertex : INDEX"
                          : "unsigned int VuInvocation : INDEX");
    } else if (UsesExpandedFlatInputs()) {
      if (m_program->uses_buffered_batch_inputs) {
        AppendParameter(
            "uniform int4 VuRawQwords[" +
            std::to_string(GeneratedCgProgram::DeclaredBufferVectors) +
            "] : BUFFER[0]");
        AppendParameter(
            "uniform VitaVuBatchRecord VuBatchData[1] : BUFFER[1]");
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
      // vertex inputs allocated after ordinary parameters. The hardware-loop
      // ABI deliberately exposes only INDEX: Sony's compiler otherwise marks
      // the complete root per-instance and serializes the hot path.
      if (m_program->uses_flat_index_inputs) {
        AppendParameter("unsigned int VuExpandedVertex : INDEX");
      } else {
        AppendParameter("unsigned int VuLane : INDEX");
        AppendParameter("unsigned int VuPrimitive : INSTANCE");
      }
    } else {
      for (const CgMemoryInput &input : m_program->memory_inputs) {
        AppendParameter("__regformat int4 VuMemory" +
                        std::to_string(input.attribute_index) + " : TEXCOORD" +
                        std::to_string(attribute_semantic++));
      }
      if (m_output_kind == CgOutputKind::LoopKernelDirectTfx)
        AppendParameter("unsigned int VuInvocation : INDEX");
    }
    if (m_program->uses_arm_estimate_table) {
      AppendParameter(
          "uniform unsigned int VuArmEstimateWords[640] : BUFFER[7]");
    }
    if (!m_program->uses_buffered_batch_inputs &&
        m_output_kind != CgOutputKind::StructuredStateSnapshots &&
        !IsStructuredSnapshotConsumer()) {
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
      if (m_direct_contract && m_program->uses_gif_q_uniform)
        AppendParameter("uniform float GifQ");
    }
    if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
      AppendParameter("uniform float4 VuPrivateVf[33] : BUFFER1");
      AppendParameter("uniform unsigned int VuPrivateState[112] : BUFFER2");
      AppendParameter("uniform int4 VuRawQwords[1024] : BUFFER3");
    }
    if (IsStructuredSnapshotConsumer()) {
      AppendParameter("uniform int4 VuRawQwords[1024] : BUFFER3");
      if (m_output_kind == CgOutputKind::StructuredExpressionScratch ||
          m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore ||
          m_reads_structured_expression_scratch) {
        AppendParameter("uniform int VuScratchWords[1] : BUFFER10");
      }
      if (m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore)
        AppendParameter("uniform int VuOutputWords[4096] : BUFFER8");
      if (m_output_kind == CgOutputKind::StructuredFinalState) {
        AppendParameter("uniform float4 VuPrivateVfOutput[33] : BUFFER8");
        AppendParameter(
            "uniform unsigned int VuPrivateStateOutput[112] : BUFFER9");
      }
      AppendParameter(
          "uniform float4 VuOuterSnapshots[2304] : BUFFER11");
      AppendParameter("uniform unsigned int VuOuterState[16] : BUFFER12");
      AppendParameter(
          "uniform unsigned int4 VuOuterViSnapshots[256] : BUFFER13");
      if (IsStructuredParallelChildOutput()) {
        AppendParameter("unsigned int VuInvocation : INDEX");
      } else {
        AppendParameter("uniform float4 VuPrivateVf[33] : BUFFER1");
        AppendParameter("uniform unsigned int VuPrivateState[112] : BUFFER2");
      }
    }
    if (IsDirectTfxOutput()) {
      if (m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
          m_emit_loop_kernel_private_output) {
        AppendParameter(
            "uniform int VuPrivateStoreWords[1] : BUFFER[2]");
        if (m_program->uses_loop_kernel_ftoi_probe_output) {
          AppendParameter(
              "uniform int4 VuFtoiProbeQwords[1] : BUFFER[3]");
        }
      }
      if (!m_program->uses_loop_kernel_state_canary) {
        AppendParameter(m_program->uses_tfx_point_size
                            ? "uniform float4 VertexScaleOffset[3]"
                            : "uniform float4 VertexScaleOffset[2]");
        AppendParameter("uniform float MaxDepth");
      }
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
    } else if (m_output_kind == CgOutputKind::StoreVaryings) {
      AppendParameter("out float4 VuPosition : POSITION");
      for (u32 i = 0; i < m_kernel.stores.size(); i++) {
        AppendParameter("out float4 VuStore" + std::to_string(i) +
                        " : TEXCOORD" + std::to_string(i));
      }
    } else if (m_output_kind == CgOutputKind::StructuredExpressionScratch ||
               m_output_kind == CgOutputKind::StructuredParallelChildMemoryStore ||
               m_output_kind == CgOutputKind::StructuredFinalState) {
      AppendParameter("out float4 VuPosition : POSITION");
      AppendParameter("out float4 VuResult : TEXCOORD0");
    } else if (m_output_kind == CgOutputKind::FinalStateBuffer) {
      // Sony's skinning_reuse sample and the fixed universal root establish
      // writable uniform buffers as the GXM vertex-stage export mechanism.
      // Only boundary-live lanes are meaningful in this private scratch ABI.
      AppendParameter("uniform float4 VuFinalVf[33] : BUFFER11");
      if (m_kernel.final_acc_lanes != 0)
        AppendParameter("uniform float4 VuFinalAcc[1] : BUFFER12");
      if (m_kernel.final_q || m_kernel.final_p || m_kernel.final_i)
        AppendParameter("uniform float4 VuFinalScalars[1] : BUFFER13");
      AppendParameter("out float4 VuPosition : POSITION");
      AppendParameter("out float4 VuResult : TEXCOORD0");
    } else {
      AppendParameter(
          "uniform float4 VuOuterSnapshots[2304] : BUFFER11");
      AppendParameter("uniform unsigned int VuOuterState[16] : BUFFER12");
      AppendParameter(
          "uniform unsigned int4 VuOuterViSnapshots[256] : BUFFER13");
      AppendParameter("out float4 VuPosition : POSITION");
      AppendParameter("out float4 VuResult : TEXCOORD0");
    }
    if (!IsDirectTfxOutput())
      AppendParameter("out float VuPointSize : PSIZE");
    m_source += ")\n{\n";
    if (!IsDirectTfxOutput())
      m_source += "\tVuPointSize = 1.0f;\n";
    if (UsesNestedIterationGrid()) {
      const u32 child_iterations = m_program->nested_child_iterations;
      const u32 invocations_per_draw =
          static_cast<u32>(m_program->nested_outer_iterations) *
          child_iterations;
      if (m_program->uses_flat_index_inputs) {
        // GSState::VertexKick selects the last line endpoint's RGBA for both
        // vertices. Geometry and color can straddle an outer-loop boundary;
        // they must not share a derived outer or child coordinate.
        m_source +=
            "\tconst unsigned int VuGlobalPrimitive = VuExpandedVertex >> 1u;\n";
        AppendBoundedIndexSplit("VuGlobalPrimitive",
                                m_program->batch_primitives_per_draw, 1u << 15u,
                                "VuBatchDraw", "VuLocalPrimitive");
        m_source +=
            "\tconst unsigned int VuLocalInvocation = VuLocalPrimitive + "
            "(VuExpandedVertex & 1u);\n"
            "\tconst unsigned int VuFlatLocalInvocation = "
            "VuLocalPrimitive + 1u;\n";
      } else if (m_program->uses_instance_indexed_batch_live_ins) {
        m_source +=
            "\tconst unsigned int VuBatchDraw = VuBatchIdentity.x;\n"
            "\tconst unsigned int VuLocalInvocation = VuInvocation;\n"
            "\tconst unsigned int VuGlobalInvocation = VuBatchDraw * " +
            std::to_string(invocations_per_draw) + "u + VuInvocation;\n";
      } else if ((invocations_per_draw & (invocations_per_draw - 1u)) == 0u) {
        u32 shift = 0u;
        while ((1u << shift) != invocations_per_draw)
          shift++;
        m_source +=
            "\tconst unsigned int VuBatchDraw = VuInvocation >> " +
            std::to_string(shift) + "u;\n";
        m_source +=
            "\tconst unsigned int VuLocalInvocation = VuInvocation & " +
            std::to_string(invocations_per_draw - 1u) + "u;\n";
      } else {
        u32 multiplier = 0u;
        u32 shift = 0u;
        FindBoundedUnsignedDivisionMagic(invocations_per_draw, 1u << 16u,
                                        &multiplier, &shift);
        m_source +=
            "\tconst unsigned int VuBatchDraw = (VuInvocation * " +
            std::to_string(multiplier) + "u) >> " +
            std::to_string(shift) + "u;\n";
        m_source +=
            "\tconst unsigned int VuLocalInvocation = VuInvocation - "
            "VuBatchDraw * " +
            std::to_string(invocations_per_draw) + "u;\n";
      }
      if (m_program->nested_outer_iterations == 1u) {
        // The exact dispatch domain contains only row zero. Avoid spending
        // SGX vertex ALU on a quotient which is architecturally constant and
        // keep the child index in its native INDEX form.
        m_source +=
            "\tconst unsigned int VuOuterIteration = 0u;\n";
        m_source +=
            "\tconst unsigned int VuChildIteration = VuLocalInvocation;\n";
      } else if ((child_iterations & (child_iterations - 1u)) == 0u) {
        u32 shift = 0u;
        while ((1u << shift) != child_iterations)
          shift++;
        m_source += "\tconst unsigned int VuOuterIteration = "
                    "VuLocalInvocation >> " +
                    std::to_string(shift) + "u;\n";
        m_source += "\tconst unsigned int VuChildIteration = "
                    "VuLocalInvocation & " +
                    std::to_string(child_iterations - 1u) + "u;\n";
      } else {
        u32 multiplier = 0u;
        u32 shift = 0u;
        FindBoundedUnsignedDivisionMagic(
            child_iterations,
            static_cast<u32>(m_program->nested_outer_iterations) *
                child_iterations,
            &multiplier, &shift);
        m_source += "\tconst unsigned int VuOuterIteration = "
                    "(VuLocalInvocation * " +
                    std::to_string(multiplier) + "u) >> " +
                    std::to_string(shift) + "u;\n";
        m_source += "\tconst unsigned int VuChildIteration = "
                    "VuLocalInvocation - VuOuterIteration * " +
            std::to_string(child_iterations) + "u;\n";
      }
      if (m_program->uses_flat_index_inputs) {
        if (m_program->nested_outer_iterations == 1u) {
          m_source +=
              "\tconst unsigned int VuFlatOuterIteration = 0u;\n"
              "\tconst unsigned int VuFlatChildIteration = "
              "VuFlatLocalInvocation;\n";
        } else {
          AppendBoundedIndexSplit("VuFlatLocalInvocation", child_iterations,
                                  invocations_per_draw,
                                  "VuFlatOuterIteration", "VuFlatChildIteration");
        }
      }
      static constexpr std::array<const char*, 4> components = {
          "x", "y", "z", "w"};
      // BUFFER1 always carries varying raw bindings. Small descriptors carry
      // their live-ins through one INSTANCE_16BIT stream, while larger roots
      // retain host-proven bit-identical BUFFER1 live-ins at record zero.
      u32 uniform_vector = 0u;
      u32 varying_vector = 0u;
      const u32 varying_vector_count =
          m_program->BatchVaryingLiveInVectorCount();
      m_lazy_varying_constant_slots.assign(
          m_program->constant_inputs.size(),
          std::numeric_limits<u32>::max());
      m_lazy_varying_constant_declared.assign(
          m_program->constant_inputs.size(), false);
      if (!m_program->uses_instance_indexed_batch_live_ins) {
        for (u32 input_index = 0u;
             input_index < m_program->constant_inputs.size(); input_index++) {
          const CgConstantInput& input =
              m_program->constant_inputs[input_index];
          const bool varying =
              input_index < 64u &&
              (m_program->batch_varying_live_ins.constant_mask &
               (1ull << input_index)) != 0u;
          if (varying && UsesLazySinkScheduledVaryingConstants()) {
            m_lazy_varying_constant_slots[input_index] = varying_vector++;
            continue;
          }
          m_source += "\tconst float4 VuConstant" +
                      std::to_string(input.uniform_index) + " = ";
          if (varying) {
            if (m_program->UsesInlineBatchVaryingTail()) {
              m_source += "VuBatchData[VuBatchDraw].object_values[" +
                          std::to_string(varying_vector++) + "u];\n";
            } else {
              m_source += "VuBatchVarying[VuBatchDraw * " +
                          std::to_string(varying_vector_count) + "u + " +
                          std::to_string(varying_vector++) + "u];\n";
            }
          } else {
            m_source += "VuBatchData[0].uniforms[" +
                        std::to_string(uniform_vector++) + "];\n";
          }
        }
        for (u32 reg = 1u; reg < 32u; reg++) {
          if ((m_program->vf_uniform_mask & (1u << reg)) == 0u)
            continue;
          m_source += "\tconst float4 VF";
          if (reg < 10u)
            m_source += "0";
          m_source += std::to_string(reg) + " = ";
          if ((m_program->batch_varying_live_ins.vf_mask &
               (1u << reg)) != 0u) {
            if (m_program->UsesInlineBatchVaryingTail()) {
              m_source += "VuBatchData[VuBatchDraw].object_values[" +
                          std::to_string(varying_vector++) + "u];\n";
            } else {
              m_source += "VuBatchVarying[VuBatchDraw * " +
                          std::to_string(varying_vector_count) + "u + " +
                          std::to_string(varying_vector++) + "u];\n";
            }
          } else {
            m_source += "VuBatchData[0].uniforms[" +
                        std::to_string(uniform_vector++) + "];\n";
          }
        }
        if (m_program->uses_acc_uniform) {
          m_source += "\tconst float4 ACC = ";
          if (m_program->batch_varying_live_ins.acc) {
            if (m_program->UsesInlineBatchVaryingTail()) {
              m_source += "VuBatchData[VuBatchDraw].object_values[" +
                          std::to_string(varying_vector++) + "u];\n";
            } else {
              m_source += "VuBatchVarying[VuBatchDraw * " +
                          std::to_string(varying_vector_count) + "u + " +
                          std::to_string(varying_vector++) + "u];\n";
            }
          } else {
            m_source += "VuBatchData[0].uniforms[" +
                        std::to_string(uniform_vector++) + "];\n";
          }
        }
      }
      if (m_program->uses_q_uniform || m_program->uses_p_uniform ||
          m_program->uses_i_uniform || m_program->uses_gif_q_uniform) {
        if (!m_program->uses_instance_indexed_batch_live_ins) {
          m_source += "\tconst float4 VuBatchScalars = ";
          if (m_program->batch_varying_live_ins.scalars) {
            if (m_program->UsesInlineBatchVaryingTail()) {
              m_source += "VuBatchData[VuBatchDraw].object_values[" +
                          std::to_string(varying_vector++) + "u];\n";
            } else {
              m_source += "VuBatchVarying[VuBatchDraw * " +
                          std::to_string(varying_vector_count) + "u + " +
                          std::to_string(varying_vector++) + "u];\n";
            }
          } else {
            m_source += "VuBatchData[0].uniforms[" +
                        std::to_string(uniform_vector++) + "];\n";
          }
        }
        if (m_program->uses_q_uniform)
          m_source += "\tconst float Q = VuBatchScalars.x;\n";
        if (m_program->uses_p_uniform)
          m_source += "\tconst float P = VuBatchScalars.y;\n";
        if (m_program->uses_i_uniform)
          m_source += "\tconst float I = VuBatchScalars.z;\n";
        if (m_program->uses_gif_q_uniform)
          m_source += "\tconst float GifQ = VuBatchScalars.w;\n";
      }
      if (!m_program->uses_instance_indexed_batch_live_ins &&
          (uniform_vector !=
               m_program->BatchInvariantUniformVectorCount() ||
           varying_vector != varying_vector_count)) {
        // Metadata/source divergence is an internal compiler bug.  Emit a
        // forbidden dynamic-flow token so the registry rejects the source
        // before ShaccCg rather than accepting a misbound executable.
        m_source += "\tif (false) { }\n";
      }
      for (const CgMemoryInput& input : m_program->memory_inputs) {
        const u32 binding_vector = input.attribute_index / 4u;
        const u32 binding_component = input.attribute_index & 3u;
        const std::string binding =
            "VuBatchData[VuBatchDraw].bindings[" +
            std::to_string(binding_vector) + "]." +
            components[binding_component];
        if (input.compact_outer_table ==
            CgMemoryInput::PackedCompactOuterTables) {
          u32 table_qword_offset = 0u;
          for (u32 table_index = 0u;
               table_index < m_program->compact_outer_inputs.size();
               table_index++) {
            for (bool flat_color : {false, true}) {
              if (flat_color && !UsesExpandedFlatInputs())
                continue;
              if (!CompactOuterTableUsedBy(table_index,
                      flat_color ? m_flat_color_reachable : m_reachable))
                continue;
              m_source += "\tconst int4 VuCompactOuter" +
                          std::to_string(table_index) +
                          (flat_color ? "Flat" : "") +
                          " = VuRawQwords[(" + binding + " & 32767) + " +
                          std::to_string(table_qword_offset) + " + int(" +
                          OuterIterationName(flat_color) + ")];\n";
            }
            table_qword_offset += static_cast<u32>(
                m_program->compact_outer_inputs[table_index].sources.size());
          }
          continue;
        }
        for (bool flat_color : {false, true}) {
          if (flat_color && !UsesExpandedFlatInputs())
            continue;
          if (!MemoryInputUsedBy(input.attribute_index,
                  flat_color ? m_flat_color_reachable : m_reachable))
            continue;
          const std::string suffix = std::to_string(input.attribute_index) +
              (flat_color ? "Vertex1" : "");
          m_source += "\tconst int VuInputOffset" + suffix + " = 0";
          if (input.address.outer_invocation_coefficient != 0) {
            m_source += " + int(" + std::string(OuterIterationName(flat_color)) +
                        " * " + std::to_string(
                            input.address.outer_invocation_coefficient) + "u)";
          }
          if (input.address.invocation_coefficient != 0) {
            m_source += " + int(" + std::string(ChildIterationName(flat_color)) +
                        " * " +
                        std::to_string(input.address.invocation_coefficient) +
                        "u)";
          }
          m_source += ";\n";
          m_source += "\tconst int4 VuMemory" + suffix +
                      " = VuRawQwords[(" + binding + " & 32767) + "
                      "VuInputOffset" + suffix + "];\n";
        }
      }
    } else if (UsesExpandedFlatInputs()) {
      const u32 primitives_per_draw =
          m_program->batch_primitives_per_draw;
      if (m_program->uses_flat_index_inputs) {
        const u32 vertices_per_primitive =
            m_program->flat_vertices_per_primitive;
        const u32 expanded_vertices_per_draw =
            primitives_per_draw * vertices_per_primitive;
        if ((expanded_vertices_per_draw &
             (expanded_vertices_per_draw - 1u)) == 0u) {
          u32 shift = 0u;
          while ((1u << shift) != expanded_vertices_per_draw)
            shift++;
          m_source +=
              "\tconst unsigned int VuBatchDraw = VuExpandedVertex >> ";
          m_source += std::to_string(shift);
          m_source +=
              "u;\n\tconst unsigned int VuLocalExpandedVertex = "
              "VuExpandedVertex & ";
          m_source += std::to_string(expanded_vertices_per_draw - 1u);
          m_source += "u;\n";
        } else {
          u32 multiplier = 0u;
          u32 shift = 0u;
          FindBoundedUnsignedDivisionMagic(
              expanded_vertices_per_draw, 1u << 16u,
              &multiplier, &shift);
          m_source +=
              "\tconst unsigned int VuBatchDraw = "
              "(VuExpandedVertex * ";
          m_source += std::to_string(multiplier);
          m_source += "u) >> ";
          m_source += std::to_string(shift);
          m_source +=
              "u;\n\tconst unsigned int VuLocalExpandedVertex = "
              "VuExpandedVertex - VuBatchDraw * ";
          m_source += std::to_string(expanded_vertices_per_draw);
          m_source += "u;\n";
        }
        if (vertices_per_primitive == 2u) {
          m_source +=
              "\tconst unsigned int VuLocalPrimitive = "
              "VuLocalExpandedVertex >> 1u;\n"
              "\tconst int VuInputLane = "
              "int(VuLocalExpandedVertex & 1u);\n";
        } else {
          u32 multiplier = 0u;
          u32 shift = 0u;
          FindBoundedUnsignedDivisionMagic(
              3u, expanded_vertices_per_draw, &multiplier, &shift);
          m_source +=
              "\tconst unsigned int VuLocalPrimitive = "
              "(VuLocalExpandedVertex * ";
          m_source += std::to_string(multiplier);
          m_source += "u) >> ";
          m_source += std::to_string(shift);
          m_source +=
              "u;\n"
              "\tconst int VuInputLane = int(VuLocalExpandedVertex - "
              "VuLocalPrimitive * 3u);\n";
        }
      } else {
        m_source += "\tconst int VuInputLane = int(VuLane);\n";
        if (m_program->uses_buffered_batch_inputs) {
          if ((primitives_per_draw & (primitives_per_draw - 1u)) == 0u) {
            u32 shift = 0u;
            while ((1u << shift) != primitives_per_draw)
              shift++;
            m_source +=
                "\tconst unsigned int VuBatchDraw = VuPrimitive >> ";
            m_source += std::to_string(shift);
            m_source +=
                "u;\n\tconst unsigned int VuLocalPrimitive = "
                "VuPrimitive & ";
            m_source += std::to_string(primitives_per_draw - 1u);
            m_source += "u;\n";
          } else {
            m_source +=
                "\tconst unsigned int VuBatchDraw = VuPrimitive / ";
            m_source += std::to_string(primitives_per_draw);
            m_source +=
                "u;\n\tconst unsigned int VuLocalPrimitive = "
                "VuPrimitive - VuBatchDraw * ";
            m_source += std::to_string(primitives_per_draw);
            m_source += "u;\n";
          }
        } else {
          m_source +=
              "\tconst unsigned int VuLocalPrimitive = VuPrimitive;\n";
        }
      }
      if (m_program->flat_strip_winding) {
        if (m_program->uses_flat_index_inputs) {
          m_source +=
              "\tconst int VuSwapStripLane = "
              "int(VuLocalPrimitive & 1u) * "
              "int(unsigned int(VuInputLane < 2) & 1u);\n"
              "\tconst int VuSelectedLane = VuInputLane + "
              "VuSwapStripLane * (1 - 2 * VuInputLane);\n";
        } else {
          m_source +=
              "\tconst bool VuSwapStripLane = "
              "((VuLocalPrimitive & 1u) != 0u) && VuInputLane < 2;\n"
              "\tconst int VuSelectedLane = VuSwapStripLane ? "
              "(1 - VuInputLane) : VuInputLane;\n";
        }
      } else {
        m_source += "\tconst int VuSelectedLane = VuInputLane;\n";
      }
      if (m_program->uses_buffered_batch_inputs) {
        static constexpr std::array<const char *, 4> components = {
            "x", "y", "z", "w"};
        u32 uniform_vector = 0;
        for (const CgConstantInput &input : m_program->constant_inputs) {
          m_source += "\tconst float4 VuConstant";
          m_source += std::to_string(input.uniform_index);
          m_source += " = VuBatchData[VuBatchDraw].uniforms[";
          m_source += std::to_string(uniform_vector++);
          m_source += "];\n";
        }
        for (u32 reg = 1; reg < 32; reg++) {
          if ((m_program->vf_uniform_mask & (1u << reg)) == 0)
            continue;
          m_source += "\tconst float4 VF";
          if (reg < 10)
            m_source += "0";
          m_source += std::to_string(reg);
          m_source += " = VuBatchData[VuBatchDraw].uniforms[";
          m_source += std::to_string(uniform_vector++);
          m_source += "];\n";
        }
        if (m_program->uses_acc_uniform) {
          m_source += "\tconst float4 ACC = "
                      "VuBatchData[VuBatchDraw].uniforms[";
          m_source += std::to_string(uniform_vector++);
          m_source += "];\n";
        }
        if (m_program->uses_q_uniform || m_program->uses_p_uniform ||
            m_program->uses_i_uniform || m_program->uses_gif_q_uniform) {
          m_source += "\tconst float4 VuBatchScalars = "
                      "VuBatchData[VuBatchDraw].uniforms[";
          m_source += std::to_string(uniform_vector++);
          m_source += "];\n";
          if (m_program->uses_q_uniform)
            m_source += "\tconst float Q = VuBatchScalars.x;\n";
          if (m_program->uses_p_uniform)
            m_source += "\tconst float P = VuBatchScalars.y;\n";
          if (m_program->uses_i_uniform)
            m_source += "\tconst float I = VuBatchScalars.z;\n";
          if (m_program->uses_gif_q_uniform)
            m_source += "\tconst float GifQ = VuBatchScalars.w;\n";
        }
        for (const CgMemoryInput &input : m_program->memory_inputs) {
          const u32 binding_vector = input.attribute_index / 4u;
          const u32 binding_component = input.attribute_index & 3u;
          const auto append_load =
              [this, binding_vector, binding_component, &input](
                  const std::string &suffix, const std::string &vertex) {
            const std::string name =
                std::to_string(input.attribute_index) + suffix;
            m_source += "\tconst int VuInputOffset" + name +
                        " = (VuLocalPrimitive * ";
            m_source +=
                std::to_string(m_program->flat_instance_vertex_step);
            m_source += " + ";
            m_source += vertex;
            m_source += ") * ";
            m_source += std::to_string(
                input.address.invocation_coefficient);
            m_source += ";\n";
            m_source += "\tconst int4 VuMemory" + name +
                        " = VuRawQwords[(VuBatchData[VuBatchDraw].bindings[";
            m_source += std::to_string(binding_vector);
            m_source += "].";
            m_source += components[binding_component];
            m_source += " & 32767) + VuInputOffset" + name + "];\n";
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

  std::string VfName(u32 reg, bool parent_state) const {
    if (IsStructuredSnapshotConsumer()) {
      return "VuChildVF" +
             (reg < 10 ? std::string("0") : std::string()) +
             std::to_string(reg);
    }
    if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
      return "VuParentVF" +
             (reg < 10 ? std::string("0") : std::string()) +
             std::to_string(reg);
    }
    return (parent_state ? "VuParentVF" : "VF") +
           (reg < 10 ? std::string("0") : std::string()) +
           std::to_string(reg);
  }

  std::string StateName(const char* ordinary, const char* parent) const {
    if (IsStructuredSnapshotConsumer())
      return std::string("VuChild") + ordinary;
    return m_output_kind == CgOutputKind::StructuredStateSnapshots
               ? parent
               : ordinary;
  }

  std::string StructuredQwordAddress(
      const AffineQwordAddress& address) const {
    std::string value = address.base_vi == 0
                            ? std::string("0u")
                            : ChildViName(address.base_vi);
    if (address.invocation_coefficient != 0) {
      value += " + VuChildIteration * " +
               std::to_string(static_cast<u16>(
                   address.invocation_coefficient)) +
               "u";
    }
    if (address.qword_offset != 0) {
      value += " + " +
               std::to_string(static_cast<u16>(address.qword_offset)) +
               "u";
    }
    return "(" + value + ") & 1023u";
  }

  // The structured state root executes the acyclic parent prefix before a
  // child iteration exists.  Its PairPlan memory expressions therefore use
  // the parent-entry VI base directly.  Keep those reads on the mapped
  // transactional VU-memory generation instead of reconstructing constant
  // qwords on ARM as ordinary direct-output roots do.
  std::string StructuredParentQwordAddress(
      const AffineQwordAddress& address) const {
    std::string value = address.base_vi == 0
                            ? std::string("0u")
                            : ParentViName(address.base_vi);
    if (address.qword_offset != 0) {
      value += " + " +
               std::to_string(static_cast<u16>(address.qword_offset)) +
               "u";
    }
    return "(" + value + ") & 1023u";
  }

  std::string StructuredFinalViValue(
      const AffineQwordAddress& value) const {
    std::string result = value.base_vi == 0
                             ? std::string("0u")
                             : ChildViName(value.base_vi);
    if (value.invocation_coefficient != 0) {
      result += " + VuChildIteration * " +
                std::to_string(static_cast<u16>(
                    value.invocation_coefficient)) + "u";
    }
    if (value.qword_offset != 0) {
      result += " + " +
                std::to_string(static_cast<u16>(value.qword_offset)) + "u";
    }
    return "(" + result + ") & 65535u";
  }

  std::string ParentViName(u32 reg) const {
    if (reg == 0)
      return "0u";
    if (reg == m_enclosing_loop->counter_reg)
      return "VuOuterCounter";
    if (reg == m_enclosing_loop->counter_limit_reg)
      return "VuOuterLimit";
    return "VuParentVI" +
           (reg < 10 ? std::string("0") : std::string()) +
           std::to_string(reg);
  }

  std::string ChildViName(u32 reg) const {
    return "VuChildVI" +
           (reg < 10 ? std::string("0") : std::string()) +
           std::to_string(reg);
  }

  std::string ViFormula(const AffineViValue& value) const {
    const u32 offset = static_cast<u16>(value.offset);
    if (value.base_vi == 0)
      return std::to_string(offset) + "u";
    if (offset == 0)
      return ParentViName(value.base_vi);
    return "(" + ParentViName(value.base_vi) + " + " +
           std::to_string(offset) + "u) & 65535u";
  }

  void AppendStructuredStorePrelude() {
    if (!IsStructuredSnapshotConsumer())
      return;
    if (IsStructuredParallelChildOutput()) {
      const bool power_of_two =
          (m_maximum_structured_child_iterations &
           (m_maximum_structured_child_iterations - 1u)) == 0u;
      if (power_of_two) {
        u32 child_shift = 0u;
        for (u32 value = m_maximum_structured_child_iterations;
             value > 1u; value >>= 1u) {
          child_shift++;
        }
        m_source +=
            "\tconst unsigned int VuChildIteration = VuInvocation & ";
        m_source += std::to_string(
            m_maximum_structured_child_iterations - 1u);
        m_source +=
            "u;\n\tconst unsigned int VuOuterIteration = VuInvocation >> ";
        m_source += std::to_string(child_shift);
        m_source += "u;\n";
      } else {
        u32 multiplier = 0u;
        u32 shift = 0u;
        const u32 invocation_count =
            m_output_kind == CgOutputKind::StructuredParallelDirectTfx
                ? m_direct_contract->vertex_count
                : m_maximum_structured_iterations *
                      m_maximum_structured_child_iterations;
        FindBoundedUnsignedDivisionMagic(
            m_maximum_structured_child_iterations, invocation_count,
            &multiplier, &shift);
        m_source +=
            "\tconst unsigned int VuOuterIteration = "
            "(VuInvocation * ";
        m_source += std::to_string(multiplier);
        m_source += "u) >> ";
        m_source += std::to_string(shift);
        m_source +=
            "u;\n\tconst unsigned int VuChildIteration = VuInvocation - "
            "VuOuterIteration * ";
        m_source += std::to_string(m_maximum_structured_child_iterations);
        m_source += "u;\n";
      }
      m_source += "\tconst unsigned int VuOuterCount = VuOuterState[0];\n";
    } else {
      m_source +=
          "\tconst unsigned int VuOuterCount = VuOuterState[0];\n"
          "\tconst unsigned int VuOuterIteration = VuOuterCount != 0u ? "
          "VuOuterCount - 1u : 0u;\n";
    }
    m_source +=
        "\tconst unsigned int VuSafeOuter = VuOuterIteration < VuOuterCount "
        "&& VuOuterIteration < ";
    m_source += std::to_string(m_maximum_structured_iterations);
    m_source += "u ? VuOuterIteration : 0u;\n";
    for (u32 group = 0; group < 4; group++) {
      const u16 group_mask = static_cast<u16>(0x0fu << (group * 4u));
      if ((m_enclosing_boundary->demanded_vi_mask & group_mask) == 0)
        continue;
      m_source += "\tconst unsigned int4 VuChildVIGroup" +
                  std::to_string(group) +
                  " = VuOuterViSnapshots[VuSafeOuter * 4u + " +
                  std::to_string(group) + "u];\n";
      for (u32 lane = 0; lane < 4; lane++) {
        const u32 reg = group * 4u + lane;
        if ((m_enclosing_boundary->demanded_vi_mask & (1u << reg)) == 0)
          continue;
        m_source += "\tconst unsigned int " + ChildViName(reg) +
                    " = VuChildVIGroup" + std::to_string(group) + "." +
                    LaneName(lane) + ";\n";
      }
    }
    if (m_output_kind == CgOutputKind::StructuredFinalState) {
      // The state root computed this value while producing the exact child-
      // entry snapshots. The fixed preflight independently reconstructs it
      // from BUFFER13 and rejects a mismatch before this final root runs.
      m_source +=
          "\tconst unsigned int VuChildTripCount = VuOuterState[10];\n";
    } else {
      const std::string child_counter =
          ChildViName(m_child_loop->counter_reg);
      const std::string child_limit =
          m_child_loop->counter_limit_reg == 0
              ? std::string("0u")
              : ChildViName(m_child_loop->counter_limit_reg);
      m_source += "\tconst unsigned int VuChildTripCount = (";
      if (m_child_loop->counter_step > 0)
        m_source += child_limit + " - " + child_counter;
      else
        m_source += child_counter + " - " + child_limit;
      m_source += ") & 65535u;\n";
    }
    if (IsStructuredParallelChildOutput()) {
      m_source +=
          "\tconst bool VuStoreActive = VuOuterState[3] == 0u && "
          "VuOuterState[2] == 1u && VuOuterCount != 0u && VuOuterCount <= ";
      m_source += std::to_string(m_maximum_structured_iterations);
      m_source +=
          "u && VuOuterIteration < VuOuterCount && VuChildTripCount != 0u "
          "&& VuChildIteration < VuChildTripCount "
          "&& VuChildTripCount <= ";
      m_source += std::to_string(m_maximum_structured_child_iterations);
      m_source += "u;\n";
    } else {
      m_source +=
          "\tconst unsigned int VuChildIteration = VuChildTripCount != 0u ? "
          "VuChildTripCount - 1u : 0u;\n"
          "\tconst bool VuFinalActive = VuOuterState[12] == 1u;\n";
    }

    for (u32 reg = 1; reg < 32; reg++) {
      if ((m_program->vf_uniform_mask & (1u << reg)) == 0)
        continue;
      m_source += "\tconst float4 VuChildVF";
      if (reg < 10)
        m_source += "0";
      m_source += std::to_string(reg) +
                  " = VuOuterSnapshots[VuSafeOuter * 36u + " +
                  std::to_string(reg) + "u];\n";
    }
    if (m_program->uses_acc_uniform) {
      m_source +=
          "\tconst float4 VuChildACC = "
          "VuOuterSnapshots[VuSafeOuter * 36u];\n";
    }
    if (m_program->uses_q_uniform || m_program->uses_p_uniform ||
        m_program->uses_i_uniform) {
      m_source +=
          "\tconst float4 VuChildScalars = "
          "VuOuterSnapshots[VuSafeOuter * 36u + 32u];\n";
      if (m_program->uses_q_uniform)
        m_source += "\tconst float VuChildQ = VuChildScalars.x;\n";
      if (m_program->uses_p_uniform)
        m_source += "\tconst float VuChildP = VuChildScalars.y;\n";
      if (m_program->uses_i_uniform)
        m_source += "\tconst float VuChildI = VuChildScalars.z;\n";
    }
  }

  void AppendStructuredInactiveGuard() {
    if (IsStructuredParallelChildOutput()) {
      m_source +=
          "\t#pragma branch (flatten: never)\n"
          "\tif (!VuStoreActive)\n"
          "\t{\n";
      if (m_output_kind == CgOutputKind::StructuredParallelDirectTfx) {
        m_source +=
            "\t\tvPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n";
        if (m_program->uses_tfx_point_size)
          m_source += "\t\tvPointSize = 1.0f;\n";
        if (!m_program->uses_tfx_uv_no_fog_interface)
          m_source += "\t\tvTexFloat = float4(0.0f);\n";
        m_source += m_program->uses_tfx_uv_no_fog_interface
            ? "\t\tvTexInt = float2(0.0f);\n"
            : "\t\tvTexInt = float4(0.0f);\n";
        m_source += "\t\tvColor = float4(0.0f);\n";
      } else {
        m_source +=
            "\t\tVuPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n"
            "\t\tVuResult = float4(float(VuOuterIteration), "
            "float(VuChildIteration), 0.0f, 0.0f);\n";
      }
      m_source +=
          "\t\treturn;\n"
          "\t}\n";
      return;
    }
    if (m_output_kind != CgOutputKind::StructuredFinalState)
      return;
    m_source +=
        "\t#pragma branch (flatten: never)\n"
        "\tif (!VuFinalActive)\n"
        "\t{\n"
        "\t\tVuPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n"
        "\t\tVuResult = float4(0.0f, float(VuOuterIteration), "
        "float(VuChildIteration), 0.0f);\n";
    m_source +=
        "\t\treturn;\n"
        "\t}\n";
  }

  void AppendStructuredStatePrelude() {
    if (m_output_kind != CgOutputKind::StructuredStateSnapshots)
      return;
    m_source += "\tunsigned int VuOuterCounter = VuPrivateState[" +
                std::to_string(8u + m_enclosing_loop->counter_reg) +
                "] & 65535u;\n";
    if (m_enclosing_loop->counter_limit_reg == 0) {
      m_source += "\tconst unsigned int VuOuterLimit = 0u;\n";
    } else {
      m_source += "\tconst unsigned int VuOuterLimit = VuPrivateState[" +
                  std::to_string(8u +
                                 m_enclosing_loop->counter_limit_reg) +
                  "] & 65535u;\n";
    }
    for (u32 reg = 1; reg < 16; reg++) {
      if ((m_program->vi_uniform_mask & (1u << reg)) == 0)
        continue;
      m_source += "\tunsigned int VuParentVI";
      if (reg < 10)
        m_source += "0";
      m_source += std::to_string(reg) + " = VuPrivateState[" +
                  std::to_string(8u + reg) + "] & 65535u;\n";
    }
    for (u32 reg = 1; reg < 32; reg++) {
      if ((m_program->vf_uniform_mask & (1u << reg)) == 0)
        continue;
      m_source += "\tfloat4 " + VfName(reg, true) + " = VuPrivateVf[" +
                  std::to_string(reg) + "];\n";
    }
    if (m_program->uses_acc_uniform)
      m_source += "\tfloat4 VuParentACC = VuPrivateVf[32];\n";
    if (m_program->uses_q_uniform)
      m_source += "\tfloat VuParentQ = intBitsToFloat(int(VuPrivateState[25]));\n";
    if (m_program->uses_p_uniform)
      m_source += "\tfloat VuParentP = intBitsToFloat(int(VuPrivateState[26]));\n";
    if (m_program->uses_i_uniform)
      m_source += "\tfloat VuParentI = intBitsToFloat(int(VuPrivateState[24]));\n";
    m_source +=
        "\tconst bool VuTerminalPassthrough = VuPrivateState[59] == 2u && "
        "VuPrivateState[2] == 1u;\n"
        "\tconst bool VuStructuredActive = !VuTerminalPassthrough;\n"
        "\tunsigned int VuOuterExecuted = 0u;\n"
        "\tunsigned int VuStructuredPairs = 0u;\n"
        "\tunsigned int VuLastChildTripCount = 0u;\n"
        "\tbool VuOuterCompleted = false;\n"
        "\tunsigned int VuStructuredFailure = !VuStructuredActive ? 0u : "
        "(VuPrivateState[0] != ";
    m_source += std::to_string(m_program->structured_entry_pc);
    m_source +=
        "u ? " +
        std::to_string(static_cast<u32>(
            StructuredGeneratedRuntimeFailure::EntryPc)) +
        "u : (VuPrivateState[59] != 1u || VuPrivateState[2] != 13u || "
        "VuPrivateState[61] == 0u ? " +
        std::to_string(static_cast<u32>(
            StructuredGeneratedRuntimeFailure::ContinuationState)) +
        "u : 0u));\n"
        "\t#pragma loop (unroll: never)\n"
        "\tfor (unsigned int VuOuterIteration = 0u; VuOuterIteration < ";
    m_source += std::to_string(m_maximum_structured_iterations);
    m_source +=
        "u && VuStructuredActive && VuStructuredFailure == 0u; "
        "VuOuterIteration++)\n\t{\n";
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
    // Runtime ShaccCg safety is governed by the exact source handed to the
    // compiler. Nested direct roots can carry hundreds of SSA values, and the
    // descriptive diagnostic names alone pushed an otherwise spill-free
    // BSpline root past the smallest observed in-process compiler fault size.
    // Keep readable names for validation/module roots and use deterministic
    // compact temporaries only for this direct hot tier.
    if (m_output_kind == CgOutputKind::StructuredParallelDirectTfx)
      return (flat_color ? "_c" : "_a") + std::to_string(node_id);
    return flat_color ? "VuFlatValue" + std::to_string(node_id)
                      : "VuValue" + std::to_string(node_id);
  }

  std::string VectorValue(u32 representative, bool flat_color) const {
    if (m_output_kind == CgOutputKind::StructuredParallelDirectTfx)
      return (flat_color ? "_d" : "_b") +
             std::to_string(representative);
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
          VfName(node.reg,
                 m_output_kind == CgOutputKind::StructuredStateSnapshots) +
          "." + LaneName(node.lane);
      return ReadFloatUniform(name, node.domain);
    }
    case ExpressionKind::InitialAcc:
    case ExpressionKind::InvariantAcc:
      return ReadFloatUniform(
          StateName("ACC", "VuParentACC") +
              "." + std::string(LaneName(node.lane)),
                              node.domain);
    case ExpressionKind::InitialQ:
    case ExpressionKind::InvariantQ:
      return ReadFloatUniform(StateName("Q", "VuParentQ"), node.domain);
    case ExpressionKind::InitialP:
    case ExpressionKind::InvariantP:
      return ReadFloatUniform(StateName("P", "VuParentP"), node.domain);
    case ExpressionKind::InitialI:
    case ExpressionKind::InvariantI:
      return ReadFloatUniform(StateName("I", "VuParentI"), node.domain);
    case ExpressionKind::StructuredScratch: {
      const std::string raw =
          "VuScratchWords[((VuOuterIteration * " +
          std::to_string(m_maximum_structured_child_iterations) +
          "u + VuChildIteration) * " +
          std::to_string(StructuredGeneratedScratchSlots) + "u) + " +
          std::to_string(node.immediate) + "u]";
      if (node.domain == ScalarDomain::Float)
        return FloatFromBits(raw);
      if (node.domain == ScalarDomain::UnsignedInt)
        return "unsigned int(" + raw + ")";
      return raw;
    }
    case ExpressionKind::CompactOuterInput: {
      const auto table = m_compact_outer_node_table.find(node_id);
      if (table == m_compact_outer_node_table.end()) {
        Fail(error, "compact outer input has no raw-qword binding");
        return {};
      }
      const std::string raw =
          "VuCompactOuter" + std::to_string(table->second) +
          (flat_color ? "Flat" : "") + "." +
          LaneName(node.lane);
      if (node.domain == ScalarDomain::Float)
        return FloatFromBits(raw);
      if (node.domain == ScalarDomain::UnsignedInt)
        return "unsigned int(" + raw + ")";
      return raw;
    }
    case ExpressionKind::Memory: {
      if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
        const std::string raw =
            "VuRawQwords[" +
            StructuredParentQwordAddress(node.memory_address) + "]." +
            LaneName(node.lane);
        if (node.domain == ScalarDomain::Float)
          return FloatFromBits(raw);
        if (node.domain == ScalarDomain::UnsignedInt)
          return "unsigned int(" + raw + ")";
        return raw;
      }
      if (IsStructuredSnapshotConsumer()) {
        const std::string raw =
            "VuRawQwords[" + StructuredQwordAddress(node.memory_address) +
            "]." + LaneName(node.lane);
        if (node.domain == ScalarDomain::Float)
          return FloatFromBits(raw);
        if (node.domain == ScalarDomain::UnsignedInt)
          return "unsigned int(" + raw + ")";
        return raw;
      }
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
    case ExpressionKind::RoundedAdd:
      if (UsesLeanNativeOutputProfile())
        return "(" + operand(0) + " + " + operand(1) + ")";
      if (NodeUsesStructuredNativeF32(node_id))
        return "VitaVuNativeF32Add(" + operand(0) + ", " + operand(1) + ")";
      if (NodeUsesStructuredExactF32(node_id) &&
          IsPositiveZeroFloatNode(node.operands[0]))
        return "VitaVuF32AddPositiveZero(" + operand(1) + ")";
      if (NodeUsesStructuredExactF32(node_id) &&
          IsPositiveZeroFloatNode(node.operands[1]))
        return "VitaVuF32AddPositiveZero(" + operand(0) + ")";
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
      if (m_normalized_fmac_inputs_analysis && NodeUsesStructuredExactF32(node_id))
        return "VitaVuF32AddNormalized(" + PreparedFmacOperand(node.operands[0], operand(0)) +
            ", " + PreparedFmacOperand(node.operands[1], operand(1)) + ")";
#endif
      if (NodeUsesStructuredExactF32(node_id))
        return "VitaVuF32Add(" + operand(0) + ", " + operand(1) + ")";
      // ShaccCg/psp2cgc O0 ICEs when this conversion is hidden behind a Cg
      // helper.  Keep the operation inline and bounded to real VU arithmetic
      // nodes; ordinary symbolic Add remains compact above.
      return "intBitsToFloat(floatToRawIntBits(" + operand(0) + " + " +
             operand(1) + "))";
    case ExpressionKind::RoundedSubtract:
      if (UsesLeanNativeOutputProfile())
        return "(" + operand(0) + " - " + operand(1) + ")";
      if (NodeUsesStructuredNativeF32(node_id))
        return "VitaVuNativeF32Sub(" + operand(0) + ", " + operand(1) + ")";
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
      if (m_normalized_fmac_inputs_analysis && NodeUsesStructuredExactF32(node_id))
        return "VitaVuF32SubNormalized(" + PreparedFmacOperand(node.operands[0], operand(0)) +
            ", " + PreparedFmacOperand(node.operands[1], operand(1)) + ")";
#endif
      if (NodeUsesStructuredExactF32(node_id))
        return "VitaVuF32Sub(" + operand(0) + ", " + operand(1) + ")";
      return "intBitsToFloat(floatToRawIntBits(" + operand(0) + " - " +
             operand(1) + "))";
    case ExpressionKind::RoundedMultiply:
      if (UsesLeanNativeOutputProfile())
        return "(" + operand(0) + " * " + operand(1) + ")";
      if (NodeUsesStructuredNativeF32(node_id))
        return "VitaVuNativeF32Mul(" + operand(0) + ", " + operand(1) + ")";
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
      if (m_normalized_fmac_inputs_analysis && NodeUsesStructuredExactF32(node_id))
        return "VitaVuF32MulNormalized(" + PreparedFmacOperand(node.operands[0], operand(0)) +
            ", " + PreparedFmacOperand(node.operands[1], operand(1)) + ")";
#endif
      // This is a semantic operation boundary in the shared PairPlan IR, not
      // a blanket optimizer barrier.  The raw-bit round trip makes the product
      // observable so Sony's compiler cannot fold it into a consuming MAD.
      return "VitaVuF32Mul(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Divide:
      if (UsesLeanNativeOutputProfile())
        return "(" + operand(0) + " / " + operand(1) + ")";
      return "VitaVuDivide(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Minimum:
      if (UsesLeanNativeOutputProfile())
        return "min(" + operand(0) + ", " + operand(1) + ")";
      return "VitaVuMinimum(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Maximum:
      if (UsesLeanNativeOutputProfile())
        return "max(" + operand(0) + ", " + operand(1) + ")";
      return "VitaVuMaximum(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Absolute:
      return "abs(" + operand(0) + ")";
    case ExpressionKind::Negate:
      return "(-" + operand(0) + ")";
    case ExpressionKind::Reciprocal:
      return "(1.0f / " + operand(0) + ")";
    case ExpressionKind::SquareRoot:
      return "sqrt(" + operand(0) + ")";
    case ExpressionKind::EfuSumXyzSquares:
      if (UsesLeanNativeOutputProfile())
        return "VitaVuNativeEfuSumXyzSquares(" + operand(0) + ", " +
               operand(1) + ", " + operand(2) + ")";
      return ProfiledF32HelperName(node_id, "VitaVuEfuSumXyzSquares") + "(" + operand(0) + ", " + operand(1) +
             ", " + operand(2) + ")";
    case ExpressionKind::ArmApproximateReciprocal:
      if (UsesLeanNativeOutputProfile())
        return "(1.0f / " + operand(0) + ")";
      return ProfiledF32HelperName(node_id, "VitaVuArmApproximateReciprocal") + "(" + operand(0) +
             ", VuArmEstimateWords["
             "VitaVuApproximateReciprocalEstimateIndex(" + operand(0) +
             ")])";
    case ExpressionKind::ArmApproximateSquareRoot:
      if (UsesLeanNativeOutputProfile())
        return "sqrt(" + operand(0) + ")";
      return ProfiledF32HelperName(node_id, "VitaVuArmApproximateSqrt") + "(" + operand(0) +
             ", VuArmEstimateWords[VitaVuApproximateSqrtEstimateIndex(" +
             operand(0) + ")])";
    case ExpressionKind::ReciprocalSquareRoot:
      return "rsqrt(" + operand(0) + ")";
    case ExpressionKind::FloatToInt:
      if (UsesLeanNativeOutputProfile())
        return "int(" + operand(0) + " * " +
               ScaleLiteral(node.immediate) + ")";
      // Runtime-generated closed-form roots must avoid ShaccCg's observed
      // pack.s16.f32 floor/int lowering. The raw-bit helper has no compiler
      // conversion-range premise and matches the PCSX2-derived FTOI contract.
      if (UsesBranchlessLoopKernelControl())
        return "VitaVuFloatToInt(" + operand(0) + ", " +
               std::to_string(node.immediate) + "u)";
      if (UsesPlayableLoopKernelFloatToInt())
        return "VitaVuPlayableFloatToInt(" + operand(0) + ", " +
               std::to_string(node.immediate) + "u)";
      if (UsesApproximateLoopKernelConversions())
        return "int(" + operand(0) + " * " +
               ScaleLiteral(node.immediate) + ")";
      return "VitaVuFloatToInt(" + operand(0) + ", " +
             std::to_string(node.immediate) + "u)";
    case ExpressionKind::IntToFloat:
      return "(float(" + operand(0) + ") * " +
             ReciprocalScaleLiteral(node.immediate) + ")";
    case ExpressionKind::Normalize:
      if (UsesLeanNativeOutputProfile())
        return operand(0);
      return "VitaVuNormalize(" + operand(0) + ")";
    case ExpressionKind::OuterRepeatedAdd:
      if (!UsesNestedIterationGrid() || node.reg > 1u) {
        Fail(error,
             "outer repeated ADD is outside its exact invocation grid");
        return {};
      }
      if (IsScheduledOuterRepeatedAdd(node.immediate)) {
        const u32 schedule_index =
            OuterRepeatedAddScheduleIndex(node.immediate);
        if (schedule_index >=
                m_kernel.outer_repeated_add_schedules.size() ||
            m_kernel.outer_repeated_add_schedules[schedule_index]
                    .add_counts.size() != m_kernel.outer_iteration_count) {
          Fail(error,
               "outer repeated ADD schedule is outside its exact invocation "
               "grid");
          return {};
        }
        if (UsesLeanNativeOutputProfile()) {
          if (IsLeanIdentityOuterRepeatedAdd(node))
            return operand(0);
          const std::string repeat =
              "VuOuterRepeatCount" + std::to_string(schedule_index) +
              (flat_color ? "Flat" : "");
          return node.reg == 0u
              ? "(" + operand(0) + " + " + operand(1) + " * " + repeat + ")"
              : "(" + operand(1) + " * " + repeat + " + " + operand(0) + ")";
        }
        const std::string helper = std::string(node.reg == 0u
                               ? "VitaVuOuterScheduledAddLeft"
                               : "VitaVuOuterScheduledAddRight") +
               std::to_string(schedule_index);
        return ProfiledF32HelperName(node_id, helper.c_str()) + "(" + operand(0) + ", " +
               operand(1) + ", " + OuterIterationName(flat_color) + ")";
      }
      if (node.immediate == 0u ||
          node.immediate != m_kernel.outer_iteration_count) {
        Fail(error,
             "outer repeated ADD is outside its exact invocation grid");
        return {};
      }
      if (UsesLeanNativeOutputProfile()) {
        if (IsLeanIdentityOuterRepeatedAdd(node))
          return operand(0);
        const std::string repeat =
            "float(" + std::string(OuterIterationName(flat_color)) + ")";
        return node.reg == 0u
            ? "(" + operand(0) + " + " + operand(1) + " * " + repeat + ")"
            : "(" + operand(1) + " * " + repeat + " + " + operand(0) + ")";
      }
      return ProfiledF32HelperName(node_id, node.reg == 0u ? "VitaVuOuterRepeatedAdd"
                                        : "VitaVuOuterRepeatedAddRight") +
             "(" + operand(0) + ", " + operand(1) + ", " +
             OuterIterationName(flat_color) + ")";
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
    case ExpressionKind::CompactOuterInput:
    case ExpressionKind::Memory:
      return true;
    default:
      return false;
    }
  }

  bool IsVectorOperation(const ExpressionNode &node) const {
    // Physical ShaccCg 3.0 execution disproved both integer-vector FTOI
    // lowerings used by ABI 13 and ABI 14. ABI 13 performed the conversion
    // with packed integer arithmetic; ABI 14 called the scalar helper four
    // times inside one int4 constructor. Both forms compiled without spills,
    // but lanes other than the independently emitted scalar probe could
    // publish another lane's value or an incorrect saturation result. Keep
    // every architectural conversion as its own scalar SSA publication.
    if (node.kind == ExpressionKind::FloatToInt)
      return false;
    if (node.domain != ScalarDomain::Float)
      return false;
    switch (node.kind) {
    case ExpressionKind::Add:
    case ExpressionKind::Subtract:
    case ExpressionKind::Multiply:
    case ExpressionKind::RoundedAdd:
    case ExpressionKind::RoundedSubtract:
    case ExpressionKind::RoundedMultiply:
    case ExpressionKind::Absolute:
    case ExpressionKind::Negate:
    case ExpressionKind::Normalize:
      return true;
    case ExpressionKind::OuterRepeatedAdd:
      // The no-write product replaces an output-only rounded
      // recurrence with one native multiply/add.  BSpline carries most of
      // those recurrences as complete VF qwords (48 scalar lanes in the
      // observed 5x24 root), so retaining them as scalar SSA needlessly
      // repeats the same increment and outer-count operation four times.
      // The private exact canary must keep its ordered scalar publications.
      return UsesLeanNativeOutputProfile();
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

  bool HasVectorGroup(ExpressionKind kind) const {
    for (const auto& groups : m_vector_groups) {
      for (const auto& [representative, group] : groups) {
        (void)group;
        if (representative < m_kernel.expressions.size() &&
            m_kernel.expressions[representative].kind == kind) {
          return true;
        }
      }
    }
    return false;
  }

  bool UsesCompilerSafeScalarVectorOperations() const {
    // libshaccCg 3.0 has a reproducible backend failure on large native
    // straight-line roots which combine a wide batch-uniform live-in set with
    // a lane-isomorphic float4 DAG.  The same PairPlan root compiles when the
    // arithmetic DAG is represented as independent scalar SSA lanes.  Keep
    // vector leaves for compact BUFFER loads, but stop growing vector
    // operation groups once the immutable live-in shape crosses this bounded
    // compiler class.  This is a source-shape decision only: program identity,
    // title, PC and cache hash never select semantic support.
    constexpr u32 MaximumVectorizedLeanBatchUniforms = 16u;
    return UsesLeanNativeOutputProfile() &&
           m_program->BatchUniformVectorCount() >
               MaximumVectorizedLeanBatchUniforms;
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

    if (UsesCompilerSafeScalarVectorOperations())
      return;

    // Grow lane-isomorphic float4 trees from the vector leaves. Each
    // operation may consume either a corresponding-lane vector or one scalar
    // broadcast. Trying both forms is important for VU lighting expressions:
    // VF08..VF11 vary across color lanes while each light coefficient is the
    // same scalar in all four lanes. This is local expression formation only;
    // ShaccCg still owns SSA, allocation, and scheduling for the whole shader.
    using OperationKey =
        std::tuple<ExpressionKind, ScalarDomain, u32, u8, u8,
                   std::array<u32, 3>, std::array<u8, 3>>;
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
            const u8 arithmetic_profile =
                NodeUsesStructuredExactF32(node_id)
                    ? 1u
                    : (NodeUsesStructuredNativeF32(node_id) ? 2u : 0u);
            const OperationKey key{node.kind, node.domain, node.immediate,
                                   node.reg, arithmetic_profile, operand_ids,
                                   operand_is_vector};
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
    bool vector = true;
    u32 vector_representative = InvalidNode;
    for (u32 lane = 0; lane < group.nodes.size(); lane++) {
      const u32 node_operand =
          m_kernel.expressions[group.nodes[lane]].operands[operand];
      scalar &= node_operand == first;
      if (!vector)
        continue;
      if (node_operand == InvalidNode) {
        vector = false;
        continue;
      }
      const VectorMembership membership =
          m_vector_memberships[set][node_operand];
      if (membership.representative == InvalidNode ||
          membership.lane != lane) {
        vector = false;
        continue;
      }
      if (vector_representative == InvalidNode)
        vector_representative = membership.representative;
      else if (vector_representative != membership.representative) {
        vector = false;
      }
    }
    if (vector && vector_representative != InvalidNode)
      return VectorValue(vector_representative, flat_color);
    if (scalar)
      return Value(first, flat_color);
    if (m_kernel.expressions[group.nodes[0]].kind ==
        ExpressionKind::FloatToInt) {
      std::string packed = "float4(";
      for (u32 lane = 0; lane < group.nodes.size(); lane++) {
        if (lane != 0u)
          packed += ", ";
        packed += Value(
            m_kernel.expressions[group.nodes[lane]].operands[operand],
            flat_color);
      }
      packed += ")";
      return packed;
    }
    Fail(error, "parallel Cg vector group has mismatched operands");
    return {};
  }

  std::string VectorExpression(const VectorGroup &group, bool flat_color,
                               std::string *error) const {
    const ExpressionNode &node = m_kernel.expressions[group.nodes[0]];
    const auto operand = [this, &group, flat_color, error](u32 index) {
      return VectorOperand(group, index, flat_color, error);
    };
    const auto positive_zero_operand = [this, &group](u32 index) {
      for (const u32 node_id : group.nodes) {
        if (!IsPositiveZeroFloatNode(
                m_kernel.expressions[node_id].operands[index]))
          return false;
      }
      return true;
    };
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
    const auto prepared_operand = [this, &group, &operand](u32 index) {
      const std::string value = operand(index);
      for (u32 id : group.nodes)
        if (!IsNormalizedFmacInput(m_kernel.expressions[id].operands[index]))
          return "VitaVuF32Prepare4(" + value + ")";
      return value;
    };
#endif
    switch (node.kind) {
    case ExpressionKind::InitialVf:
    case ExpressionKind::InvariantVf:
      return VfName(
          node.reg,
          m_output_kind == CgOutputKind::StructuredStateSnapshots ||
              IsStructuredSnapshotConsumer());
    case ExpressionKind::InitialAcc:
    case ExpressionKind::InvariantAcc:
      return StateName("ACC", "VuParentACC");
    case ExpressionKind::CompactOuterInput: {
      const auto table = m_compact_outer_node_table.find(group.nodes[0]);
      if (table == m_compact_outer_node_table.end()) {
        Fail(error, "compact outer vector has no raw-qword binding");
        return {};
      }
      return FloatFromBits("VuCompactOuter" +
                           std::to_string(table->second) +
                           (flat_color ? "Flat" : ""));
    }
    case ExpressionKind::Memory: {
      if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
        return FloatFromBits(
            "VuRawQwords[" +
            StructuredParentQwordAddress(node.memory_address) + "]");
      }
      if (IsStructuredSnapshotConsumer()) {
        return FloatFromBits(
            "VuRawQwords[" +
            StructuredQwordAddress(node.memory_address) + "]");
      }
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
    case ExpressionKind::RoundedAdd:
      if (UsesLeanNativeOutputProfile())
        return "(" + operand(0) + " + " + operand(1) + ")";
      if (NodeUsesStructuredNativeF32(group.nodes[0]))
        return "VitaVuNativeF32Add4(" + operand(0) + ", " + operand(1) + ")";
      if (NodeUsesStructuredExactF32(group.nodes[0]) &&
          positive_zero_operand(0u))
        return "VitaVuF32AddPositiveZero4(" + operand(1) + ")";
      if (NodeUsesStructuredExactF32(group.nodes[0]) &&
          positive_zero_operand(1u))
        return "VitaVuF32AddPositiveZero4(" + operand(0) + ")";
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
      if (m_normalized_fmac_inputs_analysis && NodeUsesStructuredExactF32(group.nodes[0]))
        return "VitaVuF32AddNormalized4(" + prepared_operand(0) + ", " + prepared_operand(1) + ")";
#endif
      if (NodeUsesStructuredExactF32(group.nodes[0]))
        return "VitaVuF32Add4(" + operand(0) + ", " + operand(1) + ")";
      return "bit_cast<float4>(bit_cast<unsigned int4>(" + operand(0) +
             " + " + operand(1) + "))";
    case ExpressionKind::RoundedSubtract:
      if (UsesLeanNativeOutputProfile())
        return "(" + operand(0) + " - " + operand(1) + ")";
      if (NodeUsesStructuredNativeF32(group.nodes[0]))
        return "VitaVuNativeF32Sub4(" + operand(0) + ", " + operand(1) + ")";
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
      if (m_normalized_fmac_inputs_analysis && NodeUsesStructuredExactF32(group.nodes[0]))
        return "VitaVuF32SubNormalized4(" + prepared_operand(0) + ", " + prepared_operand(1) + ")";
#endif
      if (NodeUsesStructuredExactF32(group.nodes[0]))
        return "VitaVuF32Sub4(" + operand(0) + ", " + operand(1) + ")";
      return "bit_cast<float4>(bit_cast<unsigned int4>(" + operand(0) +
             " - " + operand(1) + "))";
    case ExpressionKind::RoundedMultiply:
      if (UsesLeanNativeOutputProfile())
        return "(" + operand(0) + " * " + operand(1) + ")";
      if (NodeUsesStructuredNativeF32(group.nodes[0]))
        return "VitaVuNativeF32Mul4(" + operand(0) + ", " + operand(1) + ")";
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
      if (m_normalized_fmac_inputs_analysis && NodeUsesStructuredExactF32(group.nodes[0]))
        return "VitaVuF32MulNormalized4(" + prepared_operand(0) + ", " + prepared_operand(1) + ")";
#endif
      return "VitaVuF32Mul4(" + operand(0) + ", " + operand(1) + ")";
    case ExpressionKind::Absolute:
      return "abs(" + operand(0) + ")";
    case ExpressionKind::Negate:
      return "(-" + operand(0) + ")";
    case ExpressionKind::Normalize:
      if (UsesLeanNativeOutputProfile())
        return operand(0);
      return "VitaVuNormalize4(" + operand(0) + ")";
    case ExpressionKind::OuterRepeatedAdd: {
      if (!UsesLeanNativeOutputProfile() || !UsesNestedIterationGrid() ||
          node.reg > 1u) {
        break;
      }
      std::string repeat;
      if (IsScheduledOuterRepeatedAdd(node.immediate)) {
        const u32 schedule_index =
            OuterRepeatedAddScheduleIndex(node.immediate);
        if (schedule_index >=
                m_kernel.outer_repeated_add_schedules.size() ||
            m_kernel.outer_repeated_add_schedules[schedule_index]
                    .add_counts.size() != m_kernel.outer_iteration_count) {
          Fail(error,
               "vector outer repeated ADD schedule is outside its exact "
               "invocation grid");
          return {};
        }
        repeat = "VuOuterRepeatCount" + std::to_string(schedule_index) +
                 (flat_color ? "Flat" : "");
      } else {
        if (node.immediate == 0u ||
            node.immediate != m_kernel.outer_iteration_count) {
          Fail(error,
               "vector outer repeated ADD is outside its exact invocation "
               "grid");
          return {};
        }
        repeat = "float(" + std::string(OuterIterationName(flat_color)) + ")";
      }
      if (IsLeanIdentityOuterRepeatedAdd(node))
        return operand(0);
      return node.reg == 0u
          ? "(" + operand(0) + " + " + operand(1) + " * " + repeat + ")"
          : "(" + operand(1) + " * " + repeat + " + " + operand(0) + ")";
    }
    case ExpressionKind::FloatToInt:
      break;
    default:
      break;
    }
    Fail(error, "parallel Cg vector expression kind is not lowerable");
    return {};
  }

  u32 LoopKernelSinkAssignmentCount() const {
    if (!m_program->uses_sink_scheduled_outputs || !m_direct_contract)
      return 0u;
    u32 count = 0u;
    if (!m_program->uses_loop_kernel_state_canary) {
      count = 2u + 1u + 4u;
      if (m_direct_contract->textured)
        count += m_direct_contract->fixed_texture_coordinates ? 2u : 3u;
      count += m_direct_contract->fog_enabled ? 1u : 0u;
    }
    if (m_emit_loop_kernel_private_output) {
      for (const LoopStore& store : m_kernel.stores) {
        for (u32 lane = 0u; lane < 4u; lane++)
          count += (store.write_mask & (0x8u >> lane)) != 0u ? 1u : 0u;
      }
    }
    return count;
  }

  void AppendLoopKernelSinkDeclarations() {
    if (m_program->uses_loop_kernel_state_canary)
      return;
    m_source +=
        "\tfloat VuSinkPositionX;\n"
        "\tfloat VuSinkPositionY;\n"
        "\tfloat VuSinkDepth;\n"
        "\tfloat VuSinkColor0;\n"
        "\tfloat VuSinkColor1;\n"
        "\tfloat VuSinkColor2;\n"
        "\tfloat VuSinkColor3;\n";
    if (m_direct_contract->textured) {
      if (m_direct_contract->fixed_texture_coordinates) {
        m_source +=
            "\tfloat VuSinkUvX;\n"
            "\tfloat VuSinkUvY;\n";
      } else {
        m_source +=
            "\tfloat VuSinkStX;\n"
            "\tfloat VuSinkStY;\n"
            "\tfloat VuSinkQ;\n";
      }
    }
    if (m_direct_contract->fog_enabled)
      m_source += "\tfloat VuSinkFog;\n";
  }

  void AppendLoopKernelSinkAssignments(u32 node_id, bool flat_color) {
    if (!m_program->uses_sink_scheduled_outputs || !m_direct_contract)
      return;
    const auto packed = [this, node_id, flat_color](
                            const PackedIntegerExpression& field,
                            const char* destination, bool sink_flat_color) {
      if (flat_color != sink_flat_color || field.expression != node_id)
        return;
      m_source += "\t";
      m_source += destination;
      m_source += " = ";
      m_source += PackedValue(field, flat_color);
      m_source += ";\n";
      m_sink_scheduled_assignment_count++;
    };
    const auto scalar = [this, node_id, flat_color](
                            u32 expression, const char* destination) {
      if (flat_color || expression != node_id)
        return;
      m_source += "\t";
      m_source += destination;
      m_source += " = ";
      m_source += FloatValue(expression);
      m_source += ";\n";
      m_sink_scheduled_assignment_count++;
    };

    if (!flat_color && !m_program->uses_loop_kernel_state_canary) {
      packed(m_direct_contract->position[0], "VuSinkPositionX", false);
      packed(m_direct_contract->position[1], "VuSinkPositionY", false);
      packed(m_direct_contract->depth, "VuSinkDepth", false);
      if (m_direct_contract->textured) {
        if (m_direct_contract->fixed_texture_coordinates) {
          packed(m_direct_contract->uv[0], "VuSinkUvX", false);
          packed(m_direct_contract->uv[1], "VuSinkUvY", false);
        } else {
          scalar(m_direct_contract->st[0], "VuSinkStX");
          scalar(m_direct_contract->st[1], "VuSinkStY");
          scalar(m_direct_contract->q, "VuSinkQ");
        }
      }
      if (m_direct_contract->fog_enabled)
        packed(m_direct_contract->fog, "VuSinkFog", false);
    }
    if (!flat_color) {
      const char* const journal_invocation =
          m_program->uses_flat_index_inputs ? "VuExpandedVertex" :
          (m_program->uses_instance_indexed_batch_live_ins
               ? "VuGlobalInvocation"
               : "VuInvocation");
      for (u32 store_index = 0u;
           m_emit_loop_kernel_private_output &&
               store_index < m_kernel.stores.size();
           store_index++) {
        const LoopStore& store = m_kernel.stores[store_index];
        for (u32 lane = 0u; lane < 4u; lane++) {
          if ((store.write_mask & (0x8u >> lane)) == 0u ||
              store.values[lane] != node_id) {
            continue;
          }
          const ExpressionNode& node = m_kernel.expressions[node_id];
          m_source += "\tVuPrivateStoreWords[(";
          m_source += journal_invocation;
          m_source += " * " + std::to_string(m_kernel.stores.size()) +
                      "u + " + std::to_string(store_index) +
                      "u) * 4u + " + std::to_string(lane) + "u] = ";
          m_source += node.domain == ScalarDomain::Float
              ? RawFromFloat(Value(node_id))
              : Value(node_id);
          m_source += ";\n";
          m_sink_scheduled_assignment_count++;
        }
      }
    }

    if (m_program->uses_loop_kernel_state_canary)
      return;
    const bool color_is_flat = UsesExpandedFlatInputs();
    packed(m_direct_contract->color[0], "VuSinkColor0", color_is_flat);
    packed(m_direct_contract->color[1], "VuSinkColor1", color_is_flat);
    packed(m_direct_contract->color[2], "VuSinkColor2", color_is_flat);
    packed(m_direct_contract->color[3], "VuSinkColor3", color_is_flat);
  }

  bool AppendLazyVaryingConstantDeclaration(u32 node_id,
                                            std::string* error) {
    const auto constant = m_constant_node_input.find(node_id);
    if (constant == m_constant_node_input.end() ||
        m_lazy_varying_constant_slots.empty()) {
      return true;
    }
    u32 input_index = std::numeric_limits<u32>::max();
    for (u32 index = 0u; index < m_program->constant_inputs.size(); index++) {
      if (m_program->constant_inputs[index].uniform_index ==
          constant->second) {
        input_index = index;
        break;
      }
    }
    if (input_index == std::numeric_limits<u32>::max() ||
        input_index >= m_lazy_varying_constant_slots.size()) {
      return Fail(error,
                  "lazy varying constant has no semantic input record");
    }
    const u32 varying_slot = m_lazy_varying_constant_slots[input_index];
    if (varying_slot == std::numeric_limits<u32>::max() ||
        m_lazy_varying_constant_declared[input_index]) {
      return true;
    }
    m_source += "\tconst float4 VuConstant" +
                std::to_string(constant->second) +
                " = VuBatchData[VuBatchDraw].object_values[" +
                std::to_string(varying_slot) + "u];\n";
    m_lazy_varying_constant_declared[input_index] = true;
    return true;
  }

  bool ValidateLazyVaryingConstantDeclarations(std::string* error) const {
    for (u32 index = 0u; index < m_lazy_varying_constant_slots.size();
         index++) {
      if (m_lazy_varying_constant_slots[index] !=
              std::numeric_limits<u32>::max() &&
          !m_lazy_varying_constant_declared[index]) {
        return Fail(error,
                    "lazy varying constant has no reachable first use");
      }
    }
    return true;
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

    for (u32 node_id : group.nodes) {
      if (!AppendLazyVaryingConstantDeclaration(node_id, error))
        return false;
    }

    const std::string expression =
        VectorExpression(group, flat_color, error);
    if (error && !error->empty())
      return false;
    m_source += "\t";
    m_source += VectorType(m_kernel.expressions[group.nodes[0]].domain);
    m_source += " ";
    m_source += VectorValue(representative, flat_color);
    m_source += " = ";
    m_source += expression;
    m_source += ";\n";
    for (u32 node_id : group.nodes)
      (*emitted)[node_id] = true;
    m_program->emitted_expression_count +=
        static_cast<u32>(group.nodes.size());
    for (u32 node_id : group.nodes)
      AppendLoopKernelSinkAssignments(node_id, flat_color);
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

    if (!AppendLazyVaryingConstantDeclaration(node_id, error))
      return false;

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
    AppendLoopKernelSinkAssignments(node_id, flat_color);
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
    if (!UsesApproximateLoopKernelConversions() && field.right_shift == 0 &&
        field.mask != 0xffffffffu &&
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
    if (m_program->uses_loop_kernel_state_canary) {
      // This root attests stores, not raster output. In particular, no ADC
      // marker may remove an invocation or its journal slot. The GXM private
      // owner must submit all grid indices and disable fragment/depth writes.
      m_source +=
          "\tvPosition = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
          "\tvPointSize = 1.0f;\n"
          "\tvTexFloat = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
          "\tvTexInt = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
          "\tvColor = float4(0.0f, 0.0f, 0.0f, 0.0f);\n";
      return;
    }
    const auto &contract = *m_direct_contract;
    const bool sink_scheduled = m_program->uses_sink_scheduled_outputs;
    const bool uv_no_fog_interface =
        m_program->uses_tfx_uv_no_fog_interface;
    const std::string st_x =
        contract.textured && !contract.fixed_texture_coordinates
            ? (sink_scheduled ? "VuSinkStX" : FloatValue(contract.st[0]))
            : "0.0f";
    const std::string st_y =
        contract.textured && !contract.fixed_texture_coordinates
            ? (sink_scheduled ? "VuSinkStY" : FloatValue(contract.st[1]))
            : "0.0f";
    const std::string q =
        contract.textured
            ? (contract.fixed_texture_coordinates ? "GifQ"
                  : (sink_scheduled ? "VuSinkQ" : FloatValue(contract.q)))
            : "1.0f";
    const std::string uv_x =
        contract.textured && contract.fixed_texture_coordinates
            ? (sink_scheduled ? "VuSinkUvX" : PackedValue(contract.uv[0]))
            : "0.0f";
    const std::string uv_y =
        contract.textured && contract.fixed_texture_coordinates
            ? (sink_scheduled ? "VuSinkUvY" : PackedValue(contract.uv[1]))
            : "0.0f";
    const std::string fog =
        contract.fog_enabled ? "(" +
            (sink_scheduled ? std::string("VuSinkFog") :
                              PackedValue(contract.fog)) +
            " / 255.0f)"
                             : "0.0f";
    const bool flat_color = UsesExpandedFlatInputs();

    m_source +=
        "\tconst float2 vertexScale = VertexScaleOffset[0].xy;\n"
        "\tconst float2 vertexOffset = VertexScaleOffset[0].zw;\n"
        "\tconst float2 VuXY = float2(" +
        (sink_scheduled ? std::string("VuSinkPositionX") :
                          PackedValue(contract.position[0])) +
        ", " +
        (sink_scheduled ? std::string("VuSinkPositionY") :
                          PackedValue(contract.position[1])) + ");\n"
        "\tconst float VuZ = min(" +
        (sink_scheduled ? std::string("VuSinkDepth") :
                          PackedValue(contract.depth)) +
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
        (sink_scheduled ? std::string("VuSinkColor0") :
                          PackedValue(contract.color[0], flat_color)) + ", " +
        (sink_scheduled ? std::string("VuSinkColor1") :
                          PackedValue(contract.color[1], flat_color)) + ", " +
        (sink_scheduled ? std::string("VuSinkColor2") :
                          PackedValue(contract.color[2], flat_color)) + ", " +
        (sink_scheduled ? std::string("VuSinkColor3") :
                          PackedValue(contract.color[3], flat_color)) + ");\n";
  }

  void AppendOutputs() {
    if (IsDirectTfxOutput()) {
      AppendDirectTfxOutputs();
      if (m_output_kind == CgOutputKind::LoopKernelDirectTfx &&
          m_emit_loop_kernel_private_output) {
        if (!m_program->uses_sink_scheduled_outputs) {
          AppendLoopKernelPrivateStoreWrites();
          AppendLoopKernelFtoiProbeWrite();
        }
      }
      return;
    }
    if (m_output_kind == CgOutputKind::FinalStateBuffer) {
      AppendFinalStateOutputs();
      return;
    }
    if (m_output_kind == CgOutputKind::StructuredStateSnapshots) {
      AppendStructuredStateOutputs();
      return;
    }
    if (m_output_kind == CgOutputKind::StructuredFinalState) {
      AppendStructuredFinalStateOutputs();
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

  void AppendLoopKernelPrivateStoreWrites() {
    // Flat-shaded primitives duplicate source VU vertices in one dense INDEX
    // domain.  Journal the actual shader invocation there: using the original
    // source vertex would make several SGX instances race on one BUFFER2
    // qword.  Native and nested-grid roots retain one INDEX per VU iteration.
    const char* const journal_invocation =
        m_program->uses_flat_index_inputs
            ? "VuExpandedVertex"
            : (m_program->uses_instance_indexed_batch_live_ins
                   ? "VuGlobalInvocation"
                   : "VuInvocation");
    for (u32 store_index = 0; store_index < m_kernel.stores.size();
         store_index++) {
      const LoopStore& store = m_kernel.stores[store_index];
      for (u32 lane = 0; lane < 4; lane++) {
        if ((store.write_mask & (0x8u >> lane)) == 0u)
          continue;
        const u32 value = store.values[lane];
        const ExpressionNode& node = m_kernel.expressions[value];
        m_source += "\tVuPrivateStoreWords[(";
        m_source += journal_invocation;
        m_source += " * " + std::to_string(m_kernel.stores.size()) +
                    "u + " + std::to_string(store_index) +
                    "u) * 4u + " + std::to_string(lane) + "u] = ";
        m_source += node.domain == ScalarDomain::Float ?
            RawFromFloat(Value(value)) : Value(value);
        m_source += ";\n";
      }
    }
  }

  void AppendLoopKernelFtoiProbeWrite() {
    if (!m_program->uses_loop_kernel_ftoi_probe_output)
      return;
    const ExpressionNode& ftoi = m_kernel.expressions[m_ftoi_probe_node];
    const ExpressionNode& multiply =
        m_kernel.expressions[ftoi.operands[0]];
    const char* const journal_invocation =
        m_program->uses_flat_index_inputs
            ? "VuExpandedVertex"
            : (m_program->uses_instance_indexed_batch_live_ins
                   ? "VuGlobalInvocation"
                   : "VuInvocation");
    const std::string left = Value(multiply.operands[0]);
    const std::string right = Value(multiply.operands[1]);
    const std::string product = Value(ftoi.operands[0]);
    m_source += "\tconst unsigned int VuFtoiProbeIndex = ";
    m_source += journal_invocation;
    m_source += " * 2u;\n\tVuFtoiProbeQwords[VuFtoiProbeIndex] = int4(" +
                RawFromFloat(left) + ", " + RawFromFloat(right) + ", " +
                RawFromFloat(product) + ", VitaVuFloatToInt(" + product +
                ", " + std::to_string(ftoi.immediate) + "u));\n";
    m_source +=
        "\tVuFtoiProbeQwords[VuFtoiProbeIndex + 1u] = int4("
        "VitaVuPlayableFloatToInt(" + product + ", " +
        std::to_string(ftoi.immediate) + "u), 0, 0, 0);\n";
  }

  void AppendStructuredStoreWrites() {
    for (u32 store_index = 0; store_index < m_kernel.stores.size();
         store_index++) {
      const LoopStore& store = m_kernel.stores[store_index];
      const std::string suffix = std::to_string(store_index);
      m_source += "\tconst unsigned int VuStoreAddress" + suffix + " = " +
                  StructuredQwordAddress(store.address) + ";\n";
      for (u32 lane = 0; lane < 4; lane++) {
        if ((store.write_mask & (0x8u >> lane)) != 0) {
          m_source += "\tVuOutputWords[VuStoreAddress" + suffix +
                      " * 4u + " + std::to_string(lane) + "u] = " +
                      RawFromFloat(OutputValue(store.values[lane])) + ";\n";
        }
      }
    }
  }

  void AppendStructuredScratchWrites() {
    for (const StructuredScratchOutput& output :
         m_kernel.structured_scratch_outputs) {
      const ExpressionNode& node = m_kernel.expressions[output.expression];
      std::string value = Value(output.expression);
      if (node.domain == ScalarDomain::Float)
        value = RawFromFloat(std::move(value));
      m_source +=
          "\tVuScratchWords[((VuOuterIteration * " +
          std::to_string(m_maximum_structured_child_iterations) +
          "u + VuChildIteration) * " +
          std::to_string(StructuredGeneratedScratchSlots) + "u) + " +
          std::to_string(output.slot) + "u] = " + value + ";\n";
    }
  }

  void AppendStructuredFinalStateOutputs() {
    m_source +=
        "\tVuPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n"
        "\tVuResult = float4(1.0f, "
        "float(VuOuterIteration), float(VuChildIteration), 0.0f);\n";
    for (u32 reg = 1; reg < m_kernel.final_vf_lanes.size(); reg++) {
      if (m_kernel.final_vf_lanes[reg] == 0)
        continue;
      m_source += "\tVuPrivateVfOutput[" + std::to_string(reg) +
                  "] = float4(";
      for (u32 lane = 0; lane < 4; lane++) {
        if (lane != 0)
          m_source += ", ";
        m_source += OutputValue(m_kernel.final_vf_values[reg][lane]);
      }
      m_source += ");\n";
    }
    if (m_kernel.final_acc_lanes != 0) {
      m_source += "\tVuPrivateVfOutput[32] = float4(";
      for (u32 lane = 0; lane < 4; lane++) {
        if (lane != 0)
          m_source += ", ";
        m_source += OutputValue(m_kernel.final_acc_values[lane]);
      }
      m_source += ");\n";
    }
    if (m_kernel.final_i)
      m_source += "\tVuPrivateStateOutput[24] = unsigned int(" +
                  RawFromFloat(OutputValue(m_kernel.final_i_value)) + ");\n";
    if (m_kernel.final_q)
      m_source += "\tVuPrivateStateOutput[25] = unsigned int(" +
                  RawFromFloat(OutputValue(m_kernel.final_q_value)) + ");\n";
    if (m_kernel.final_p)
      m_source += "\tVuPrivateStateOutput[26] = unsigned int(" +
                  RawFromFloat(OutputValue(m_kernel.final_p_value)) + ");\n";
    if (m_structured_final_control) {
      for (u32 reg = 1; reg < 16; reg++) {
        if ((m_kernel.vi.written_mask & (1u << reg)) == 0)
          continue;
        m_source += "\tVuPrivateStateOutput[" + std::to_string(8u + reg) +
                    "] = " + StructuredFinalViValue(
                        m_kernel.vi.AddressesForRegister(reg).back()) + ";\n";
      }
    // The state root summarized every completed *intermediate* parent latch,
    // but the exact final tail resumes before the final latch. Restore the
    // enclosing counter to that pre-latch generation and publish the real
    // byte PC. The continuation then executes the final counter update,
    // branch delay slot, suffix, E delay pair, and output in architectural
    // order rather than duplicating those effects in this optimization root.
    const u32 counter_delta = static_cast<u16>(m_enclosing_loop->counter_step);
    m_source += "\tVuPrivateStateOutput[" +
                std::to_string(8u + m_enclosing_loop->counter_reg) +
                "] = (VuPrivateState[" +
                std::to_string(8u + m_enclosing_loop->counter_reg) +
                "] + (VuOuterCount - 1u) * " +
                std::to_string(counter_delta) + "u) & 65535u;\n";
    m_source += "\tVuPrivateStateOutput[0] = " +
                std::to_string(m_structured_tail->final_resume_pc) + "u;\n";
    m_source +=
        "\tVuPrivateStateOutput[27] = VuOuterState[8] + VuOuterState[4];\n"
        "\tVuPrivateStateOutput[46] = VuOuterState[9] + VuOuterState[4];\n"
        "\tVuPrivateStateOutput[61] = VuOuterState[6] - VuOuterState[4];\n"
        "\tVuPrivateStateOutput[2] = 0u;\n"
        "\tVuPrivateStateOutput[37] = 0u;\n"
        "\tVuPrivateStateOutput[38] = 0u;\n"
        "\tVuPrivateStateOutput[59] = 1u;\n"
        "\tVuPrivateStateOutput[64] = 0u;\n"
        "\tVuPrivateStateOutput[67] = 0u;\n"
        "\tVuPrivateStateOutput[80] = 0u;\n"
        "\tVuPrivateStateOutput[92] = 0u;\n"
        "\tVuPrivateStateOutput[95] = 0u;\n";
    }
  }

  void AppendFinalStateOutputs() {
    m_source +=
        "\tVuPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n"
        "\tVuResult = float4(0.0f);\n";
    for (u32 reg = 1; reg < m_kernel.final_vf_lanes.size(); reg++) {
      if (m_kernel.final_vf_lanes[reg] == 0)
        continue;
      m_source += "\tVuFinalVf[" + std::to_string(reg) + "] = float4(";
      for (u32 lane = 0; lane < 4; lane++) {
        if (lane != 0)
          m_source += ", ";
        if ((m_kernel.final_vf_lanes[reg] & (0x8u >> lane)) != 0)
          m_source += OutputValue(m_kernel.final_vf_values[reg][lane]);
        else
          m_source += "0.0f";
      }
      m_source += ");\n";
    }
    if (m_kernel.final_acc_lanes != 0) {
      m_source += "\tVuFinalAcc[0] = float4(";
      for (u32 lane = 0; lane < 4; lane++) {
        if (lane != 0)
          m_source += ", ";
        if ((m_kernel.final_acc_lanes & (0x8u >> lane)) != 0)
          m_source += OutputValue(m_kernel.final_acc_values[lane]);
        else
          m_source += "0.0f";
      }
      m_source += ");\n";
    }
    if (m_kernel.final_q || m_kernel.final_p || m_kernel.final_i) {
      m_source += "\tVuFinalScalars[0] = float4(";
      m_source += m_kernel.final_q ? OutputValue(m_kernel.final_q_value)
                                   : "0.0f";
      m_source += ", ";
      m_source += m_kernel.final_p ? OutputValue(m_kernel.final_p_value)
                                   : "0.0f";
      m_source += ", ";
      m_source += m_kernel.final_i ? OutputValue(m_kernel.final_i_value)
                                   : "0.0f";
      m_source += ", 0.0f);\n";
    }
  }

  void AppendStructuredStateOutputs() {
    for (u32 reg = 1; reg < 16; reg++) {
      if ((m_kernel.child_entry_vi_mask & (1u << reg)) == 0)
        continue;
      m_source += "\t\tconst unsigned int " + ChildViName(reg) + " = " +
                  ViFormula(m_kernel.child_entry_vi_values[reg]) + ";\n";
    }
    const std::string child_counter =
        ChildViName(m_child_loop->counter_reg);
    const std::string child_limit =
        m_child_loop->counter_limit_reg == 0
            ? std::string("0u")
            : ChildViName(m_child_loop->counter_limit_reg);
    m_source += "\t\tconst unsigned int VuChildTripCount = (";
    if (m_child_loop->counter_step > 0)
      m_source += child_limit + " - " + child_counter;
    else
      m_source += child_counter + " - " + child_limit;
    m_source +=
        ") & 65535u;\n"
        "\t\tif (VuChildTripCount == 0u || VuChildTripCount > ";
    m_source += std::to_string(m_maximum_structured_child_iterations);
    m_source +=
        "u)\n"
        "\t\t{\n"
        "\t\t\tif (VuStructuredFailure == 0u) VuStructuredFailure = " +
        std::to_string(static_cast<u32>(
            StructuredGeneratedRuntimeFailure::ChildTripCount)) +
        "u;\n"
        "\t\t\tbreak;\n"
        "\t\t}\n"
        "\t\tVuLastChildTripCount = VuChildTripCount;\n";
    m_source += "\t\tVuStructuredPairs += " +
                std::to_string(m_program->structured_parent_prefix_pairs) +
                "u + VuChildTripCount * " +
                std::to_string(m_program->structured_child_pairs) + "u;\n";
    if (m_structured_state_control) {
      for (u32 group = 0; group < 4; group++) {
        const u16 group_mask = static_cast<u16>(0x0fu << (group * 4u));
        if ((m_enclosing_boundary->demanded_vi_mask & group_mask) == 0)
          continue;
        m_source +=
            "\t\tVuOuterViSnapshots[VuOuterIteration * 4u + " +
            std::to_string(group) + "u] = unsigned int4(";
        for (u32 lane = 0; lane < 4; lane++) {
          if (lane != 0)
            m_source += ", ";
          const u32 reg = group * 4u + lane;
          m_source +=
              (m_enclosing_boundary->demanded_vi_mask & (1u << reg)) != 0
                  ? ChildViName(reg)
                  : std::string("0u");
        }
        m_source += ");\n";
      }
    }

    for (u32 reg = 1; reg < m_kernel.child_entry_vf_lanes.size(); reg++) {
      const u8 lanes = m_kernel.child_entry_vf_lanes[reg];
      if (lanes == 0)
        continue;
      const std::string entry = "VuChildVF" + std::to_string(reg);
      m_source += "\t\tfloat4 " + entry + " = float4(0.0f);\n";
      for (u32 lane = 0; lane < 4; lane++) {
        if ((lanes & (0x8u >> lane)) == 0)
          continue;
        m_source += "\t\t" + entry + "." + LaneName(lane) + " = " +
                    OutputValue(m_kernel.child_entry_vf_values[reg][lane]) +
                    ";\n";
      }
      m_source += "\t\tVuOuterSnapshots[VuOuterIteration * 36u + " +
                  std::to_string(reg) + "u] = " + entry + ";\n";
    }
    if (m_kernel.child_entry_acc_lanes != 0) {
      m_source += "\t\tfloat4 VuChildACC = float4(0.0f);\n";
      for (u32 lane = 0; lane < 4; lane++) {
        if ((m_kernel.child_entry_acc_lanes & (0x8u >> lane)) != 0) {
          m_source += "\t\tVuChildACC." + std::string(LaneName(lane)) +
                      " = " +
                      OutputValue(m_kernel.child_entry_acc_values[lane]) +
                      ";\n";
        }
      }
      m_source +=
          "\t\tVuOuterSnapshots[VuOuterIteration * 36u] = VuChildACC;\n";
    }
    if (m_kernel.child_entry_q || m_kernel.child_entry_p ||
        m_kernel.child_entry_i) {
      m_source += "\t\tVuOuterSnapshots[VuOuterIteration * 36u + 32u] = "
                  "float4(";
      m_source += m_kernel.child_entry_q
                      ? OutputValue(m_kernel.child_entry_q_value)
                      : "0.0f";
      m_source += ", ";
      m_source += m_kernel.child_entry_p
                      ? OutputValue(m_kernel.child_entry_p_value)
                      : "0.0f";
      m_source += ", ";
      m_source += m_kernel.child_entry_i
                      ? OutputValue(m_kernel.child_entry_i_value)
                      : "0.0f";
      m_source += ", 0.0f);\n";
    }

    for (u32 reg = 1; reg < m_kernel.final_vf_lanes.size(); reg++) {
      const u8 lanes = m_kernel.final_vf_lanes[reg];
      if (lanes == 0)
        continue;
      const std::string next = "VuNextVF" + std::to_string(reg);
      m_source += "\t\tfloat4 " + next + " = " + VfName(reg, true) + ";\n";
      for (u32 lane = 0; lane < 4; lane++) {
        if ((lanes & (0x8u >> lane)) == 0)
          continue;
        m_source += "\t\t" + next + "." + LaneName(lane) + " = " +
                    OutputValue(m_kernel.final_vf_values[reg][lane]) +
                    ";\n";
      }
    }
    if (m_kernel.final_acc_lanes != 0) {
      m_source += "\t\tfloat4 VuNextACC = VuParentACC;\n";
      for (u32 lane = 0; lane < 4; lane++) {
        if ((m_kernel.final_acc_lanes & (0x8u >> lane)) != 0) {
          m_source += "\t\tVuNextACC." + std::string(LaneName(lane)) +
                      " = " +
                      OutputValue(m_kernel.final_acc_values[lane]) + ";\n";
        }
      }
    }

    // Every next-state expression was evaluated from the old parent snapshot
    // above; publish to mutable locals only after all simultaneous roots exist.
    for (u32 reg = 1; reg < m_kernel.final_vf_lanes.size(); reg++) {
      if (m_kernel.final_vf_lanes[reg] != 0) {
        m_source += "\t\t" + VfName(reg, true) + " = VuNextVF" +
                    std::to_string(reg) + ";\n";
      }
    }
    if (m_kernel.final_acc_lanes != 0)
      m_source += "\t\tVuParentACC = VuNextACC;\n";
    if (m_kernel.final_q) {
      m_source += "\t\tVuParentQ = " +
                  OutputValue(m_kernel.final_q_value) + ";\n";
    }
    if (m_kernel.final_p) {
      m_source += "\t\tVuParentP = " +
                  OutputValue(m_kernel.final_p_value) + ";\n";
    }
    if (m_kernel.final_i) {
      m_source += "\t\tVuParentI = " +
                  OutputValue(m_kernel.final_i_value) + ";\n";
    }

    for (u32 reg = 1; reg < 16; reg++) {
      if ((m_enclosing_boundary->parent_live_vi_mask & (1u << reg)) == 0 ||
          reg == m_enclosing_loop->counter_reg ||
          reg == m_enclosing_loop->counter_limit_reg) {
        continue;
      }
      const u32 step = static_cast<u16>(m_kernel.vi.step[reg]);
      m_source += "\t\t" + ParentViName(reg) + " = (" +
                  ChildViName(reg);
      if (step != 0) {
        m_source += " + VuChildTripCount * " + std::to_string(step) + "u";
      }
      m_source += ") & 65535u;\n";
    }

    const u32 counter_delta =
        static_cast<u32>(static_cast<u16>(m_enclosing_loop->counter_step));
    m_source += "\t\tVuOuterCounter = (VuOuterCounter + " +
                std::to_string(counter_delta) + "u) & 65535u;\n";
    const LowerKind branch_kind =
        static_cast<LowerKind>(m_enclosing_loop->branch_kind);
    std::string taken_expression;
    switch (branch_kind) {
    case LowerKind::IBEQ:
      taken_expression = "VuOuterCounter == VuOuterLimit";
      break;
    case LowerKind::IBNE:
      taken_expression = "VuOuterCounter != VuOuterLimit";
      break;
    case LowerKind::IBLTZ:
      taken_expression = "int(short(VuOuterCounter)) < 0";
      break;
    case LowerKind::IBGTZ:
      taken_expression = "int(short(VuOuterCounter)) > 0";
      break;
    case LowerKind::IBLEZ:
      taken_expression = "int(short(VuOuterCounter)) <= 0";
      break;
    case LowerKind::IBGEZ:
      taken_expression = "int(short(VuOuterCounter)) >= 0";
      break;
    default:
      taken_expression = "false";
      break;
    }
    m_source += "\t\tconst bool VuOuterBranchTaken = " +
                taken_expression + ";\n";
    m_source += "\t\tconst bool VuOuterRepeat = ";
    if (!m_enclosing_loop->branch_taken_repeats)
      m_source += "!";
    m_source +=
        "VuOuterBranchTaken;\n"
        "\t\tVuOuterExecuted = VuOuterIteration + 1u;\n"
        "\t\tif (!VuOuterRepeat)\n"
        "\t\t{\n"
        "\t\t\tVuOuterCompleted = true;\n"
        "\t\t\tbreak;\n"
        "\t\t}\n";
    m_source += "\t\tVuStructuredPairs += " +
                std::to_string(m_program->structured_suffix_pairs) + "u;\n";
    m_source +=
        "\t}\n"
        "\tif (VuStructuredActive && VuStructuredFailure == 0u && "
        "!VuOuterCompleted)\n"
        "\t{\n"
        "\t\tVuStructuredFailure = " +
        std::to_string(static_cast<u32>(
            StructuredGeneratedRuntimeFailure::OuterIterationBound)) +
        "u;\n"
        "\t}\n"
        "\tif (VuStructuredActive && (VuStructuredPairs > " +
        std::to_string(m_program->structured_pair_upper_bound) +
        "u || VuStructuredPairs >= VuPrivateState[61]))\n"
        "\t{\n"
        "\t\tif (VuStructuredFailure == 0u) VuStructuredFailure = " +
        std::to_string(static_cast<u32>(
            StructuredGeneratedRuntimeFailure::PairBudget)) +
        "u;\n"
        "\t}\n";
    if (m_structured_state_control) {
      m_source +=
        "\tVuOuterState[0] = VuOuterExecuted;\n"
        "\tVuOuterState[1] = VuOuterCounter;\n"
        "\tVuOuterState[2] = VuOuterCompleted ? 1u : 0u;\n"
        "\tVuOuterState[3] = VuStructuredFailure;\n"
        "\tVuOuterState[4] = VuStructuredPairs;\n"
        "\tVuOuterState[5] = VuStructuredFailure;\n"
        "\tVuOuterState[6] = VuPrivateState[61];\n"
        "\tVuOuterState[7] = VuPrivateState[59];\n"
        "\tVuOuterState[8] = VuPrivateState[27];\n"
        "\tVuOuterState[9] = VuPrivateState[46];\n"
        "\tVuOuterState[10] = VuLastChildTripCount;\n"
        "\tVuOuterState[11] = 0u;\n"
        "\tVuOuterState[15] = VuStructuredActive ? 1u : 0u;\n";
    }
    m_source +=
        "\tVuPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n"
        "\tVuResult = float4(float(VuOuterExecuted), "
        "float(VuOuterCounter), VuOuterCompleted ? 1.0f : 0.0f, 0.0f);\n";
  }

  const ParallelLoopKernel &m_kernel;
  const DirectTfxContract *m_direct_contract;
  GeneratedCgProgram *m_program;
  CgOutputKind m_output_kind;
  const NaturalLoop* m_child_loop;
  const NaturalLoop* m_enclosing_loop;
  const EnclosingLoopEntryIndependence* m_enclosing_boundary;
  const StructuredLoopTailProof* m_structured_tail;
  u32 m_maximum_structured_iterations;
  u32 m_maximum_structured_child_iterations;
  u32 m_structured_parent_prefix_pairs;
  u32 m_structured_entry_pc;
  bool m_structured_final_control;
  bool m_structured_state_control;
  const GeneratedBatchVaryingLiveIns* m_batch_varying_live_ins;
  const GeneratedCgProgram* m_batch_varying_input_layout = nullptr;
  GeneratedBatchVaryingLiveIns m_projected_batch_varying_live_ins{};
  bool m_emit_loop_kernel_private_output;
  bool m_native_output_only_profile = false;
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
  bool m_exact_depth_analysis = false;
  std::span<const u8> m_exact_fmac_analysis_nodes;
  bool m_range_scaled_multiply_analysis = false;
  bool m_normalized_fmac_inputs_analysis = false;
#endif
  std::vector<bool> m_reachable;
  std::vector<bool> m_flat_color_reachable;
  // Optional exact-arithmetic dependency closure. Ordinary native-profile
  // sources leave it empty; the offline depth-precision experiment marks the
  // proven depth cone. Private comparison policy still owns runtime admission.
  std::vector<bool> m_exact_reachable;
  std::array<std::vector<VectorMembership>, 2> m_vector_memberships;
  std::array<std::map<u32, VectorGroup>, 2> m_vector_groups;
  std::map<u32, u32> m_memory_node_input;
  std::map<u32, u32> m_constant_node_input;
  std::map<u32, u32> m_compact_outer_node_table;
  std::vector<u32> m_lazy_varying_constant_slots;
  std::vector<bool> m_lazy_varying_constant_declared;
  std::string m_source;
  bool m_first_parameter = true;
  bool m_reads_structured_expression_scratch = false;
  u32 m_ftoi_probe_node = InvalidNode;
  u32 m_sink_scheduled_assignment_count = 0u;
};

} // namespace

bool GeneratedCgProgram::HasValidDirectTfxInputMode() const {
  const bool expanded_flat = uses_flat_instance_inputs || uses_flat_index_inputs;
  if (uses_instance_indexed_batch_live_ins ||
      (uses_flat_instance_inputs && uses_flat_index_inputs))
    return false;
  if (uses_nested_iteration_grid) {
    if (!uses_buffered_batch_inputs || nested_outer_iterations == 0u ||
        nested_child_iterations == 0u)
      return false;
    if (expanded_flat) {
      // This is ConfigureDirectInputLowering's nested flat-line INDEX domain,
      // not instance attributes or a dense writable private-state grid.
      // GSDeviceGXM's draw-time owner still proves the exact ADC indices and
      // last-vertex color contract against PCSX2 GSState::VertexKick.
      const u32 invocations = static_cast<u32>(nested_outer_iterations) *
          nested_child_iterations;
      return execution_kind == GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx &&
          (loop_kernel_source_abi == GeneratedLoopKernelNestedFlatProductCgAbiVersion ||
           loop_kernel_source_abi == GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion) &&
          uses_flat_index_inputs && !uses_flat_instance_inputs &&
          uses_nested_batch_index_inputs && uses_tfx_uniforms &&
          !uses_loop_kernel_private_store_output && !uses_loop_kernel_state_canary &&
          flat_vertices_per_primitive == 2u && flat_instance_vertex_step == 1u &&
          DirectTfxFlatLineIndexDomain(invocations) != 0u &&
          batch_primitives_per_draw == invocations - 1u && !flat_strip_winding;
    }
    return flat_vertices_per_primitive == 0u && flat_instance_vertex_step == 0u &&
        batch_primitives_per_draw == 0u && !flat_strip_winding;
  }
  if (expanded_flat) {
    return (flat_vertices_per_primitive == 2u || flat_vertices_per_primitive == 3u) &&
        flat_instance_vertex_step != 0u &&
        (!uses_flat_index_inputs || uses_buffered_batch_inputs) &&
        (!uses_buffered_batch_inputs || batch_primitives_per_draw != 0u) &&
        (!flat_strip_winding || flat_vertices_per_primitive == 3u);
  }
  return flat_vertices_per_primitive == 0u && flat_instance_vertex_step == 0u &&
      batch_primitives_per_draw == 0u && !uses_buffered_batch_inputs && !flat_strip_winding;
}

bool GeneratedCgProgram::GetDirectTfxVertexAttributeCount(u32* attribute_count) const {
  if (!attribute_count)
    return false;
  *attribute_count = 0u;
  const bool expanded_flat = uses_flat_instance_inputs || uses_flat_index_inputs;
  if (memory_inputs.size() > MaximumMemoryInputs ||
      (expanded_flat && flat_vertices_per_primitive != 2u && flat_vertices_per_primitive != 3u))
    return false;
  const u8 full_mask = expanded_flat ?
      static_cast<u8>((1u << flat_vertices_per_primitive) - 1u) : 0u;
  const u8 provoking_mask = expanded_flat ?
      static_cast<u8>(1u << (flat_vertices_per_primitive - 1u)) : 0u;
  u32 count = 0u;
  for (const auto& input : memory_inputs) {
    if (input.compact_outer_table == CgMemoryInput::PackedCompactOuterTables) {
      // CollectResources packs these tables behind one BUFFER0 base binding.
      // They declare no VertexN attributes even in a flat-line product. Keep
      // the same marker/extent contract as GSDeviceGXM's draw-time owner.
      if (!uses_buffered_batch_inputs || !uses_nested_iteration_grid ||
          compact_outer_inputs.empty() || nested_outer_iterations == 0u ||
          input.flat_attribute_vertex_mask != 0u || !input.address.valid ||
          input.address.base_vi != 0u || input.address.qword_offset != 0 ||
          input.address.invocation_coefficient != 0 ||
          input.address.outer_invocation_coefficient != 0)
        return false;
      for (const auto& table : compact_outer_inputs) {
        if (table.sources.size() != nested_outer_iterations)
          return false;
      }
      continue;
    }
    if (input.compact_outer_table != CgMemoryInput::OrdinaryVuMemory)
      return false;
    if (!expanded_flat) {
      if (input.flat_attribute_vertex_mask != 0u)
        return false;
      ++count;
    } else if (input.flat_attribute_vertex_mask == full_mask) {
      count += flat_vertices_per_primitive;
    } else if (input.flat_attribute_vertex_mask == provoking_mask) {
      ++count;
    } else {
      return false;
    }
  }
  *attribute_count = count;
  return true;
}

bool GeneratedBatchInputIdentityMap::Configure(
    const GeneratedCgProgram& source, const GeneratedCgProgram& destination,
    std::string* error) {
  const auto valid_layout = [](const GeneratedCgProgram& layout) {
    if (layout.constant_inputs.size() > MaximumConstantInputs ||
        (layout.vf_uniform_mask & 1u) != 0u)
      return false;
    for (u32 i = 0u; i < layout.constant_inputs.size(); i++) {
      const CgConstantInput& input = layout.constant_inputs[i];
      if (input.uniform_index != i || !input.address.valid ||
          input.address.base_vi >= AffineViBaseCount ||
          input.address.invocation_coefficient != 0 ||
          input.address.outer_invocation_coefficient != 0)
        return false;
      for (u32 j = 0u; j < i; j++) {
        if (input.address == layout.constant_inputs[j].address)
          return false;
      }
    }
    return true;
  };
  if (!valid_layout(source) || !valid_layout(destination)) {
    if (error)
      *error = "batch input identity map has a malformed layout";
    return false;
  }

  GeneratedBatchInputIdentityMap mapped;
  mapped.m_constant_destinations.fill(0xffu);
  mapped.m_source_constant_mask = source.constant_inputs.size() == 32u
      ? std::numeric_limits<u32>::max()
      : ((1u << source.constant_inputs.size()) - 1u);
  for (u32 i = 0u; i < source.constant_inputs.size(); i++) {
    for (u32 j = 0u; j < destination.constant_inputs.size(); j++) {
      if (source.constant_inputs[i].address == destination.constant_inputs[j].address) {
        mapped.m_constant_destinations[i] = static_cast<u8>(j);
        break;
      }
    }
  }
  const auto scalar_mask = [](const GeneratedCgProgram& layout) -> u32 {
    return static_cast<u32>(layout.uses_q_uniform) |
           (static_cast<u32>(layout.uses_p_uniform) << 1u) |
           (static_cast<u32>(layout.uses_i_uniform) << 2u) |
           (static_cast<u32>(layout.uses_gif_q_uniform) << 3u);
  };
  mapped.m_source_vf_mask = source.vf_uniform_mask;
  mapped.m_shared_vf_mask = source.vf_uniform_mask & destination.vf_uniform_mask;
  mapped.m_source_acc = source.uses_acc_uniform;
  mapped.m_shared_acc = source.uses_acc_uniform && destination.uses_acc_uniform;
  mapped.m_source_scalars = scalar_mask(source) != 0u;
  mapped.m_shared_scalars = (scalar_mask(source) & scalar_mask(destination)) != 0u;
  mapped.m_valid = true;
  *this = mapped;
  if (error)
    error->clear();
  return true;
}

bool GeneratedBatchInputIdentityMap::Project(
    const GeneratedBatchVaryingLiveIns& source,
    GeneratedBatchVaryingLiveIns* destination, std::string* error) const {
  if (!destination || !m_valid ||
      (source.constant_mask & ~static_cast<u64>(m_source_constant_mask)) != 0u ||
      (source.vf_mask & ~m_source_vf_mask) != 0u ||
      (source.acc && !m_source_acc) || (source.scalars && !m_source_scalars)) {
    if (error)
      *error = "batch variance exceeds its source input identity layout";
    return false;
  }
  GeneratedBatchVaryingLiveIns mapped;
  u32 constants = static_cast<u32>(source.constant_mask);
  u32 mapped_constants = 0u;
  while (constants != 0u) {
    const u32 index = std::countr_zero(constants);
    const u32 slot = m_constant_destinations[index];
    if (slot != 0xffu)
      mapped_constants |= 1u << slot;
    constants &= constants - 1u;
  }
  mapped.constant_mask = mapped_constants;
  mapped.vf_mask = source.vf_mask & m_shared_vf_mask;
  mapped.acc = source.acc && m_shared_acc;
  mapped.scalars = source.scalars && m_shared_scalars;
  *destination = mapped;
  if (error)
    error->clear();
  return true;
}

bool IsGeneratedCgSoftwareF32ConfigurationSupported(u32 configuration_bits) {
  return (configuration_bits & ~UniversalConfigurationKnownMask) == 0 &&
         (configuration_bits & GeneratedCgSoftwareF32NumericMask) ==
             GeneratedCgSoftwareF32NumericProfile;
}

bool IsGeneratedCgNativeF32ConfigurationSupported(u32 configuration_bits) {
  return IsGeneratedCgSoftwareF32ConfigurationSupported(configuration_bits) &&
         (configuration_bits & UniversalConfigurationApproximateFmac) != 0u;
}

bool IsGeneratedCgExactDivideConfigurationSupported(u32 configuration_bits) {
  return IsGeneratedCgSoftwareF32ConfigurationSupported(configuration_bits) &&
         (configuration_bits & UniversalConfigurationApproximateQ) == 0u;
}

bool EvaluateGeneratedCgSoftwareF32Reference(
    GeneratedCgF32BinaryOperation operation, u32 left_bits, u32 right_bits,
    u32 configuration_bits, u32* result_bits) {
  if (!result_bits ||
      !IsGeneratedCgSoftwareF32ConfigurationSupported(configuration_bits)) {
    return false;
  }
  switch (operation) {
  case GeneratedCgF32BinaryOperation::Add:
    *result_bits = AddGeneratedCgSoftwareF32(left_bits, right_bits, false);
    return true;
  case GeneratedCgF32BinaryOperation::Subtract:
    *result_bits = AddGeneratedCgSoftwareF32(left_bits, right_bits, true);
    return true;
  case GeneratedCgF32BinaryOperation::Multiply:
    *result_bits = MultiplyGeneratedCgSoftwareF32(left_bits, right_bits);
    return true;
  }
  return false;
}

bool EvaluateGeneratedCgNativeF32Reference(
    GeneratedCgF32BinaryOperation operation, u32 left_bits, u32 right_bits,
    u32 configuration_bits, u32* result_bits) {
  if (!result_bits ||
      !IsGeneratedCgNativeF32ConfigurationSupported(configuration_bits)) {
    return false;
  }
  switch (operation) {
  case GeneratedCgF32BinaryOperation::Add:
    *result_bits = AddGeneratedCgNativeF32(left_bits, right_bits, false);
    return true;
  case GeneratedCgF32BinaryOperation::Subtract:
    *result_bits = AddGeneratedCgNativeF32(left_bits, right_bits, true);
    return true;
  case GeneratedCgF32BinaryOperation::Multiply:
    *result_bits = MultiplyGeneratedCgNativeF32(left_bits, right_bits);
    return true;
  }
  return false;
}

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
bool IsGeneratedCgNormalizedF32Result(const ExpressionNode& node, bool exact_fmac) {
  if (node.domain != ScalarDomain::Float)
    return false;
  switch (node.kind) {
  case ExpressionKind::ConstantFloat: {
    const u32 exponent = node.immediate & 0x7f800000u;
    return exponent != 0x7f800000u &&
        (exponent != 0u || (node.immediate & 0x7fffffffu) == 0u);
  }
  case ExpressionKind::Normalize:
    return true;
  case ExpressionKind::RoundedAdd:
  case ExpressionKind::RoundedSubtract:
  case ExpressionKind::RoundedMultiply:
    return exact_fmac;
  default:
    return false;
  }
}

bool EvaluateGeneratedCgRangeScaledMultiplyReference(
    u32 left_bits, u32 right_bits, u32 configuration_bits, u32* result_bits) {
  if (!result_bits || !IsGeneratedCgSoftwareF32ConfigurationSupported(configuration_bits))
    return false;
  const u32 left = NormalizeGeneratedCgSoftwareF32Input(left_bits);
  const u32 right = NormalizeGeneratedCgSoftwareF32Input(right_bits);
  const u32 sign = (left ^ right) & 0x80000000u;
  const u32 left_fraction = left & 0x007fffffu, right_fraction = right & 0x007fffffu;
  // The existing native model supplies only the [1,4) chopped candidate.
  // Native A9 validation compares the result to the x86 PCSX2-profile corpus;
  // this mirror does not establish actual SGX instruction behavior.
  const u32 scaled_product = MultiplyGeneratedCgNativeF32(
      left_fraction | 0x3f800000u, right_fraction | 0x3f800000u);
  const u32 high_bit = (scaled_product >> 23u) - 127u;
  const u32 truncated = (scaled_product & 0x007fffffu) | 0x00800000u;
  const u32 product_low = (left_fraction | 0x00800000u) * (right_fraction | 0x00800000u);
  const u32 remainder = product_low & ((0x00800000u << high_bit) - 1u);
  const u32 halfway = 0x00400000u << high_bit;
  const u32 increment = (remainder > halfway) | ((remainder == halfway) & (truncated & 1u));
  const u32 rounded = truncated + increment;
  const s32 exponent = static_cast<s32>((left >> 23u) & 255u) +
      static_cast<s32>((right >> 23u) & 255u) - 127 +
      static_cast<s32>(high_bit) + static_cast<s32>(rounded >> 24u);
  u32 result = sign | (static_cast<u32>(exponent) << 23u) | (rounded & 0x007fffffu);
  if (exponent >= 255) result = sign | 0x7f7fffffu;
  if (exponent <= 0 || !(left & 0x7fffffffu) || !(right & 0x7fffffffu)) result = sign;
  *result_bits = result;
  return true;
}
#endif

bool EvaluateGeneratedCgExactDivideReference(
    u32 numerator_bits, u32 denominator_bits, u32 configuration_bits,
    u32* result_bits) {
  if (!result_bits ||
      !IsGeneratedCgExactDivideConfigurationSupported(configuration_bits)) {
    return false;
  }
  *result_bits =
      DivideGeneratedCgExactF32(numerator_bits, denominator_bits);
  return true;
}

bool EvaluateGeneratedCgExactFloatToIntReference(
    u32 input_bits, u8 scale_offset, u32 configuration_bits,
    u32* result_bits) {
  if (!result_bits ||
      !IsGeneratedCgSoftwareF32ConfigurationSupported(configuration_bits) ||
      (scale_offset != 0u && scale_offset != 4u && scale_offset != 12u &&
       scale_offset != 15u)) {
    return false;
  }

  // This mirrors the emitted Cg rather than relying on the host's float-to-int
  // instruction. The supported DAZ/FZ profile maps exponent-zero inputs to
  // zero; multiplying a normal binary32 value by these four powers of two is
  // exactly an exponent adjustment until the PCSX2 saturation boundary.
  const u32 exponent_field = (input_bits >> 23u) & 0xffu;
  const s32 integer_exponent = static_cast<s32>(exponent_field) +
                               static_cast<s32>(scale_offset) - 127;
  if (exponent_field == 0xffu || integer_exponent >= 31) {
    *result_bits = (input_bits & 0x80000000u) != 0u ? 0x80000000u :
                                                        0x7fffffffu;
    return true;
  }
  if (exponent_field == 0u || integer_exponent < 0) {
    *result_bits = 0u;
    return true;
  }
  const u32 significand = (input_bits & 0x007fffffu) | 0x00800000u;
  const u32 exponent = static_cast<u32>(integer_exponent);
  const u32 magnitude = exponent >= 23u
      ? significand << (exponent - 23u)
      : significand >> (23u - exponent);
  *result_bits = (input_bits & 0x80000000u) != 0u ? 0u - magnitude :
                                                       magnitude;
  return true;
}

bool EvaluateGeneratedCgPlayableFloatToIntReference(
    u32 input_bits, u8 scale_offset, u32 configuration_bits,
    u32* result_bits) {
  if (!result_bits ||
      !IsGeneratedCgSoftwareF32ConfigurationSupported(configuration_bits) ||
      (configuration_bits & UniversalConfigurationApproximateConversions) ==
          0u ||
      (scale_offset != 0u && scale_offset != 4u && scale_offset != 12u &&
       scale_offset != 15u)) {
    return false;
  }

  // Mirror VitaVuPlayableFloatToInt rather than substituting the raw-bit
  // helper. The only native conversion receives a finite, non-negative,
  // integral binary32 value below 2^31, where Series5 and C++ truncation have
  // the same result. Multiplication by the supported power-of-two scale is
  // exact for the finite range selected below.
  const u32 exponent = (input_bits >> 23u) & 0xffu;
  const s32 integer_exponent = static_cast<s32>(exponent) +
                               static_cast<s32>(scale_offset) - 127;
  const bool finite = exponent != 0u && integer_exponent >= 0 &&
                      integer_exponent < 31;
  const u32 safe_bits = finite ? input_bits : 0u;
  float safe_value = 0.0f;
  std::memcpy(&safe_value, &safe_bits, sizeof(safe_value));
  const u32 scale_bits = (127u + scale_offset) << 23u;
  float scale = 0.0f;
  std::memcpy(&scale, &scale_bits, sizeof(scale));
  const s32 finite_magnitude =
      static_cast<s32>(std::floor(std::fabs(safe_value * scale)));
  const u32 sign_mask = static_cast<u32>(static_cast<s32>(input_bits) >> 31);
  const u32 converted = (static_cast<u32>(finite_magnitude) & ~sign_mask) |
                        ((0u - static_cast<u32>(finite_magnitude)) & sign_mask);
  if (exponent == 0xffu || integer_exponent >= 31) {
    *result_bits = sign_mask != 0u ? 0x80000000u : 0x7fffffffu;
  } else {
    *result_bits = converted;
  }
  return true;
}

bool ApplyStructuredFixedQpNumericReference(
    const StructuredFixedQpNumericDescriptor& descriptor,
    u32 active_invocations, u32* scratch_words,
    std::size_t scratch_word_count) {
  if (!descriptor.IsValid() || !scratch_words || active_invocations == 0u ||
      active_invocations > StructuredGeneratedMaximumScratchInvocations ||
      scratch_word_count <
          static_cast<std::size_t>(active_invocations) *
              StructuredGeneratedScratchSlots) {
    return false;
  }

  for (u32 invocation = 0; invocation < active_invocations; invocation++) {
    u32* const scratch =
        scratch_words + invocation * StructuredGeneratedScratchSlots;
    scratch[descriptor.output_slot] = ApplyStructuredFixedQpNumericBits(
        descriptor.operation, scratch[descriptor.input_slot]);
  }
  return true;
}

bool ApplyStructuredFixedFmacNumericReference(
    const StructuredFixedFmacNumericDescriptor& descriptor,
    u32 active_invocations, u32* scratch_words,
    std::size_t scratch_word_count) {
  if (!descriptor.IsValid() || !scratch_words || active_invocations == 0u ||
      active_invocations > StructuredGeneratedMaximumScratchInvocations ||
      scratch_word_count <
          static_cast<std::size_t>(active_invocations) *
              StructuredGeneratedScratchSlots) {
    return false;
  }

  const auto read_operand = [](const StructuredFixedFmacOperand& operand,
                               const u32* scratch) {
    return operand.kind == StructuredFixedFmacOperandKind::Immediate
               ? operand.value
               : scratch[operand.value];
  };
  const bool native_fmac =
      (descriptor.configuration_bits & UniversalConfigurationApproximateFmac) !=
      0u;
  for (u32 invocation = 0; invocation < active_invocations; invocation++) {
    u32* const scratch =
        scratch_words + invocation * StructuredGeneratedScratchSlots;
    for (u32 index = 0; index < descriptor.operation_count; index++) {
      const StructuredFixedFmacNumericRecord& record =
          descriptor.operations[index];
      const u32 left = read_operand(record.left, scratch);
      const u32 right = read_operand(record.right, scratch);
      GeneratedCgF32BinaryOperation operation =
          GeneratedCgF32BinaryOperation::Add;
      if (record.operation ==
          StructuredFixedFmacNumericOperation::Subtract) {
        operation = GeneratedCgF32BinaryOperation::Subtract;
      } else if (record.operation ==
                 StructuredFixedFmacNumericOperation::Multiply) {
        operation = GeneratedCgF32BinaryOperation::Multiply;
      }
      u32 result = 0u;
      const bool evaluated = native_fmac
          ? EvaluateGeneratedCgNativeF32Reference(
                operation, left, right, descriptor.configuration_bits, &result)
          : EvaluateGeneratedCgSoftwareF32Reference(
                operation, left, right, descriptor.configuration_bits, &result);
      if (!evaluated) {
        return false;
      }
      scratch[record.output_slot] = result;
    }
  }
  return true;
}

bool EvaluateGeneratedCgArmApproximateReference(
    StructuredFixedQpNumericOperation operation, u32 input_bits,
    u32* result_bits) {
  if (!result_bits ||
      (operation !=
           StructuredFixedQpNumericOperation::ArmApproximateReciprocal &&
       operation !=
           StructuredFixedQpNumericOperation::ArmApproximateSquareRoot)) {
    return false;
  }
  *result_bits = ApplyStructuredFixedQpNumericBits(operation, input_bits);
  return true;
}

bool GenerateParallelStoreValidationCg(const ParallelLoopKernel &kernel,
                                       GeneratedCgProgram *program,
                                       std::string *error) {
  if (!program)
    return Fail(error, "null generated Cg output");
  *program = {};
  CgEmitter emitter(kernel, nullptr, program, CgOutputKind::StoreVaryings);
  return emitter.Generate(error);
}

bool GenerateParallelFinalStateCg(const ParallelLoopKernel& kernel,
                                  GeneratedCgProgram* program,
                                  std::string* error) {
  if (!program)
    return Fail(error, "null generated final-state Cg output");
  *program = {};
  CgEmitter emitter(kernel, nullptr, program,
                    CgOutputKind::FinalStateBuffer);
  return emitter.Generate(error);
}

bool GenerateStructuredLoopStateCg(
    const ParallelLoopKernel& transition_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 parent_entry_pc,
    u32 parent_prefix_pair_count, u32 maximum_iterations,
    u32 maximum_child_iterations,
    GeneratedCgProgram* program, std::string* error) {
  if (!program)
    return Fail(error, "null generated structured-state Cg output");
  *program = {};
  CgEmitter emitter(transition_kernel, nullptr, program,
                    CgOutputKind::StructuredStateSnapshots,
                    &child_loop, &enclosing_loop, &boundary, &tail,
                    maximum_iterations, maximum_child_iterations,
                    parent_prefix_pair_count,
                    parent_entry_pc);
  return emitter.Generate(error);
}

namespace {

bool BuildStructuredLoopStateSliceKernelImpl(
    const ParallelLoopKernel& transition_kernel,
    const std::array<u8, 32>& child_vf_lanes, u8 child_acc_lanes,
    bool child_q, bool child_p, bool child_i,
    ParallelLoopKernel* slice, std::string* error) {
  if (!slice || (child_vf_lanes[0] != 0) ||
      (child_acc_lanes & ~transition_kernel.child_entry_acc_lanes) != 0 ||
      (child_q && !transition_kernel.child_entry_q) ||
      (child_p && !transition_kernel.child_entry_p) ||
      (child_i && !transition_kernel.child_entry_i)) {
    return Fail(error, "structured state slice has an invalid destination");
  }
  for (u32 reg = 1; reg < child_vf_lanes.size(); reg++) {
    if ((child_vf_lanes[reg] &
         ~transition_kernel.child_entry_vf_lanes[reg]) != 0) {
      return Fail(error,
                  "structured state slice includes an absent VF lane");
    }
  }

  ParallelLoopKernel prepared = transition_kernel;
  prepared.child_entry_vf_lanes = child_vf_lanes;
  prepared.child_entry_acc_lanes = child_acc_lanes;
  prepared.child_entry_q = child_q;
  prepared.child_entry_p = child_p;
  prepared.child_entry_i = child_i;
  prepared.final_vf_lanes.fill(0);
  prepared.final_acc_lanes = 0;
  prepared.final_q = false;
  prepared.final_p = false;
  prepared.final_i = false;

  std::vector<bool> reachable(transition_kernel.expressions.size(), false);
  std::vector<u32> work;
  work.reserve(transition_kernel.expressions.size());
  const auto mark_reachable = [&](u32 root) {
    if (root == InvalidNode || root >= transition_kernel.expressions.size())
      return false;
    work.clear();
    work.push_back(root);
    while (!work.empty()) {
      const u32 node_id = work.back();
      work.pop_back();
      if (node_id == InvalidNode ||
          node_id >= transition_kernel.expressions.size()) {
        return false;
      }
      if (reachable[node_id])
        continue;
      reachable[node_id] = true;
      for (const u32 operand :
           transition_kernel.expressions[node_id].operands) {
        if (operand != InvalidNode)
          work.push_back(operand);
      }
    }
    return true;
  };

  for (u32 reg = 1; reg < child_vf_lanes.size(); reg++) {
    for (u32 lane = 0; lane < 4; lane++) {
      if ((child_vf_lanes[reg] & (0x8u >> lane)) != 0 &&
          !mark_reachable(
              transition_kernel.child_entry_vf_values[reg][lane])) {
        return Fail(error,
                    "structured state slice has an invalid child VF root");
      }
    }
  }
  for (u32 lane = 0; lane < 4; lane++) {
    if ((child_acc_lanes & (0x8u >> lane)) != 0 &&
        !mark_reachable(transition_kernel.child_entry_acc_values[lane])) {
      return Fail(error,
                  "structured state slice has an invalid child ACC root");
    }
  }
  if ((child_q &&
       !mark_reachable(transition_kernel.child_entry_q_value)) ||
      (child_p &&
       !mark_reachable(transition_kernel.child_entry_p_value)) ||
      (child_i &&
       !mark_reachable(transition_kernel.child_entry_i_value))) {
    return Fail(error,
                "structured state slice has an invalid child scalar root");
  }

  // A selected child destination can consume a parent value which is itself
  // updated at the end of every enclosing iteration. Retain that producer and
  // repeat to a fixed point: this is the exact recurrence closure needed by
  // the independently compiled slice, not a workload-shape classification.
  bool added_recurrence = true;
  while (added_recurrence) {
    added_recurrence = false;
    for (u32 node_id = 1; node_id < transition_kernel.expressions.size();
         node_id++) {
      if (!reachable[node_id])
        continue;
      const ExpressionNode& node = transition_kernel.expressions[node_id];
      switch (node.kind) {
      case ExpressionKind::InitialVf:
      case ExpressionKind::InvariantVf: {
        if (node.reg == 0 || node.reg >= prepared.final_vf_lanes.size() ||
            node.lane >= 4)
          break;
        const u8 lane = static_cast<u8>(0x8u >> node.lane);
        if ((transition_kernel.final_vf_lanes[node.reg] & lane) == 0 ||
            (prepared.final_vf_lanes[node.reg] & lane) != 0)
          break;
        prepared.final_vf_lanes[node.reg] |= lane;
        if (!mark_reachable(
                transition_kernel.final_vf_values[node.reg][node.lane])) {
          return Fail(error,
                      "structured state slice has an invalid VF recurrence");
        }
        added_recurrence = true;
        break;
      }
      case ExpressionKind::InitialAcc:
      case ExpressionKind::InvariantAcc: {
        if (node.lane >= 4)
          break;
        const u8 lane = static_cast<u8>(0x8u >> node.lane);
        if ((transition_kernel.final_acc_lanes & lane) == 0 ||
            (prepared.final_acc_lanes & lane) != 0)
          break;
        prepared.final_acc_lanes |= lane;
        if (!mark_reachable(
                transition_kernel.final_acc_values[node.lane])) {
          return Fail(error,
                      "structured state slice has an invalid ACC recurrence");
        }
        added_recurrence = true;
        break;
      }
      case ExpressionKind::InitialQ:
      case ExpressionKind::InvariantQ:
        if (transition_kernel.final_q && !prepared.final_q) {
          prepared.final_q = true;
          if (!mark_reachable(transition_kernel.final_q_value))
            return Fail(error,
                        "structured state slice has an invalid Q recurrence");
          added_recurrence = true;
        }
        break;
      case ExpressionKind::InitialP:
      case ExpressionKind::InvariantP:
        if (transition_kernel.final_p && !prepared.final_p) {
          prepared.final_p = true;
          if (!mark_reachable(transition_kernel.final_p_value))
            return Fail(error,
                        "structured state slice has an invalid P recurrence");
          added_recurrence = true;
        }
        break;
      case ExpressionKind::InitialI:
      case ExpressionKind::InvariantI:
        if (transition_kernel.final_i && !prepared.final_i) {
          prepared.final_i = true;
          if (!mark_reachable(transition_kernel.final_i_value))
            return Fail(error,
                        "structured state slice has an invalid I recurrence");
          added_recurrence = true;
        }
        break;
      default:
        break;
      }
    }
  }

  *slice = std::move(prepared);
  if (error)
    error->clear();
  return true;
}

} // namespace

bool BuildStructuredLoopStateSliceKernel(
    const ParallelLoopKernel& transition_kernel,
    const std::array<u8, 32>& child_vf_lanes, u8 child_acc_lanes,
    bool child_q, bool child_p, bool child_i,
    ParallelLoopKernel* slice, std::string* error) {
  return BuildStructuredLoopStateSliceKernelImpl(
      transition_kernel, child_vf_lanes, child_acc_lanes, child_q, child_p,
      child_i, slice, error);
}

bool GenerateStructuredLoopStateSliceCg(
    const ParallelLoopKernel& transition_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 parent_entry_pc,
    u32 parent_prefix_pair_count, u32 maximum_iterations,
    u32 maximum_child_iterations,
    const std::array<u8, 32>& child_vf_lanes, u8 child_acc_lanes,
    bool child_q, bool child_p, bool child_i,
    GeneratedCgProgram* program, std::string* error,
    bool publish_shared_control) {
  if (!program)
    return Fail(error, "null generated structured-state slice output");
  ParallelLoopKernel slice;
  if (!BuildStructuredLoopStateSliceKernelImpl(
          transition_kernel, child_vf_lanes, child_acc_lanes, child_q,
          child_p, child_i, &slice, error)) {
    return false;
  }
  *program = {};
  CgEmitter emitter(slice, nullptr, program,
                    CgOutputKind::StructuredStateSnapshots,
                    &child_loop, &enclosing_loop, &boundary, &tail,
                    maximum_iterations, maximum_child_iterations,
                    parent_prefix_pair_count, parent_entry_pc, true,
                    publish_shared_control);
  return emitter.Generate(error);
}

bool GenerateStructuredLoopMemoryPreflightCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, bool enable_runtime_checks,
    GeneratedCgProgram* program, std::string* error) {
  if (!program)
    return Fail(error, "null generated structured-memory preflight output");
  *program = {};
  if (boundary.child_loop != child_kernel.loop_index ||
      maximum_outer_iterations == 0 ||
      maximum_outer_iterations > MaximumStructuredOuterIterations ||
      maximum_child_iterations == 0 ||
      maximum_child_iterations > MaximumStructuredChildIterations ||
      (maximum_child_iterations & (maximum_child_iterations - 1u)) != 0 ||
      child_loop.counter_reg == 0 ||
      (boundary.demanded_vi_mask & (1u << child_loop.counter_reg)) == 0 ||
      (child_loop.counter_limit_reg != 0 &&
       (boundary.demanded_vi_mask &
        (1u << child_loop.counter_limit_reg)) == 0) ||
      static_cast<LowerKind>(child_loop.branch_kind) != LowerKind::IBNE ||
      !child_loop.branch_taken_repeats ||
      (child_loop.counter_step != 1 && child_loop.counter_step != -1)) {
    return Fail(error,
                "structured-memory preflight has an invalid bounded child "
                "contract");
  }
  if (enable_runtime_checks && child_kernel.stores.empty()) {
    return Fail(error,
                "structured-memory preflight has no private stores to prove");
  }

  std::map<MemoryKey, u8> load_lanes;
  const auto validate_address = [&boundary](const AffineQwordAddress& address) {
    return address.valid &&
           (address.base_vi == 0 ||
            (boundary.demanded_vi_mask & (1u << address.base_vi)) != 0);
  };
  for (const ExpressionNode& node : child_kernel.expressions) {
    if (node.kind != ExpressionKind::Memory)
      continue;
    if (!validate_address(node.memory_address))
      return Fail(error,
                  "structured-memory preflight reads an absent VI base");
    load_lanes[MakeMemoryKey(node.memory_address)] |=
        static_cast<u8>(0x8u >> node.lane);
  }
  for (const LoopStore& store : child_kernel.stores) {
    if (!validate_address(store.address) || store.write_mask == 0 ||
        (store.write_mask & ~0x0fu) != 0) {
      return Fail(error,
                  "structured-memory preflight writes through invalid "
                  "metadata");
    }
  }

  program->execution_kind =
      GeneratedCgExecutionKind::StructuredMemoryPreflight;
  // The source below is one fixed-shape interpreter. Only this bounded data
  // block varies with PairPlan analysis, so an unfamiliar microprogram cannot
  // make runtime ShaccCg parse a larger preflight root.
  program->uses_structured_memory_preflight = true;
  StructuredMemoryPreflightData& metadata =
      program->structured_memory_preflight;
  metadata.header0[1] = enable_runtime_checks ? 1u : 0u;
  metadata.header1[1] = child_loop.counter_reg;
  metadata.header1[2] = child_loop.counter_limit_reg;
  metadata.header1[3] = static_cast<u32>(child_loop.counter_step);
  metadata.header2[0] = maximum_outer_iterations;
  metadata.header2[1] = maximum_child_iterations;
  if (enable_runtime_checks) {
    const size_t access_count =
        load_lanes.size() + child_kernel.stores.size();
    if (access_count > StructuredMemoryPreflightMaximumAccesses) {
      return Fail(error,
                  "structured-memory preflight descriptor capacity exceeded");
    }
    metadata.header0[2] = static_cast<u32>(access_count);
    metadata.header0[3] = static_cast<u32>(load_lanes.size());
    metadata.header1[0] = static_cast<u32>(child_kernel.stores.size());
    u32 access_index = 0;
    for (const auto& [key, lanes] : load_lanes) {
      AffineQwordAddress address;
      std::tie(address.base_vi, address.invocation_coefficient,
               address.qword_offset, address.valid,
               address.outer_invocation_coefficient) = key;
      StructuredMemoryPreflightAccess& access =
          metadata.accesses[access_index++];
      access.base_vi = address.base_vi;
      access.invocation_coefficient =
          static_cast<u32>(address.invocation_coefficient);
      access.qword_offset = static_cast<u32>(address.qword_offset);
      access.control = lanes;
    }
    for (const LoopStore& store : child_kernel.stores) {
      StructuredMemoryPreflightAccess& access =
          metadata.accesses[access_index++];
      access.base_vi = store.address.base_vi;
      access.invocation_coefficient =
          static_cast<u32>(store.address.invocation_coefficient);
      access.qword_offset = static_cast<u32>(store.address.qword_offset);
      access.control = store.write_mask | StructuredMemoryPreflightStoreBit;
    }
  }

  program->source = StructuredMemoryPreflightCgSource;
  std::string& source = program->source;
  if (source.size() > MaximumGeneratedSourceBytes)
    return Fail(error,
                "generated structured-memory preflight exceeds source bound");
  program->generated_source_bytes = static_cast<u32>(source.size());
  if (error)
    error->clear();
  return true;
}

bool GenerateStructuredLoopParallelChildMemoryStoreCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, GeneratedCgProgram* program,
    std::string* error) {
  if (!program)
    return Fail(error, "null generated structured-store Cg output");
  *program = {};
  CgEmitter emitter(child_kernel, nullptr, program,
                    CgOutputKind::StructuredParallelChildMemoryStore,
                    &child_loop, nullptr, &boundary, nullptr,
                    maximum_outer_iterations, maximum_child_iterations);
  return emitter.Generate(error);
}

bool GenerateStructuredLoopExpressionScratchCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, GeneratedCgProgram* program,
    std::string* error) {
  if (!program)
    return Fail(error, "null generated structured-expression Cg output");
  *program = {};
  CgEmitter emitter(child_kernel, nullptr, program,
                    CgOutputKind::StructuredExpressionScratch,
                    &child_loop, nullptr, &boundary, nullptr,
                    maximum_outer_iterations, maximum_child_iterations);
  return emitter.Generate(error);
}

bool GenerateStructuredLoopFinalStateCg(
    const ParallelLoopKernel& complete_child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, GeneratedCgProgram* program,
    std::string* error) {
  if (!program)
    return Fail(error, "null generated structured final-state output");
  for (u32 reg = 1; reg < 32; reg++) {
    if (complete_child_kernel.final_vf_lanes[reg] != 0x0f)
      return Fail(error, "structured final-state source is not canonical VF state");
  }
  if (complete_child_kernel.final_acc_lanes != 0x0f ||
      !complete_child_kernel.final_q || !complete_child_kernel.final_p ||
      !complete_child_kernel.final_i) {
    return Fail(error,
                "structured final-state source is not canonical scalar state");
  }
  *program = {};
  CgEmitter emitter(complete_child_kernel, nullptr, program,
                    CgOutputKind::StructuredFinalState,
                    &child_loop, &enclosing_loop, &boundary, &tail,
                    maximum_outer_iterations, maximum_child_iterations);
  return emitter.Generate(error);
}

bool GenerateStructuredLoopFinalStateSliceCg(
    const ParallelLoopKernel& complete_child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, u32 vf_register_mask,
    bool publish_control_state, GeneratedCgProgram* program,
    std::string* error) {
  if (!program)
    return Fail(error, "null generated structured final-state slice");
  if ((vf_register_mask & 1u) != 0)
    return Fail(error, "structured final-state slice includes hardwired VF00");
  for (u32 reg = 1; reg < 32; reg++) {
    if (complete_child_kernel.final_vf_lanes[reg] != 0x0f)
      return Fail(error, "structured final-state source is not canonical VF state");
  }
  if (complete_child_kernel.final_acc_lanes != 0x0f ||
      !complete_child_kernel.final_q || !complete_child_kernel.final_p ||
      !complete_child_kernel.final_i) {
    return Fail(error,
                "structured final-state source is not canonical scalar state");
  }

  ParallelLoopKernel slice = complete_child_kernel;
  for (u32 reg = 1; reg < 32; reg++) {
    if ((vf_register_mask & (1u << reg)) == 0)
      slice.final_vf_lanes[reg] = 0;
  }
  if (!publish_control_state) {
    slice.final_acc_lanes = 0;
    slice.final_q = false;
    slice.final_p = false;
    slice.final_i = false;
    slice.vi.written_mask = 0;
  }

  return GenerateStructuredLoopPreparedFinalStateCg(
      slice, boundary, child_loop, enclosing_loop, tail,
      maximum_outer_iterations, maximum_child_iterations,
      publish_control_state, program, error);
}

bool GenerateStructuredLoopPreparedFinalStateCg(
    const ParallelLoopKernel& prepared_slice,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, bool publish_control_state,
    GeneratedCgProgram* program, std::string* error) {
  if (!program)
    return Fail(error, "null prepared structured final-state output");
  bool has_output = publish_control_state;
  for (u32 reg = 1; reg < 32; reg++) {
    if (prepared_slice.final_vf_lanes[reg] != 0u &&
        prepared_slice.final_vf_lanes[reg] != 0x0fu) {
      return Fail(error, "prepared structured final-state has partial VF lanes");
    }
    has_output |= prepared_slice.final_vf_lanes[reg] != 0u;
  }
  if (!has_output)
    return Fail(error, "prepared structured final-state has no output");

  *program = {};
  CgEmitter emitter(prepared_slice, nullptr, program,
                    CgOutputKind::StructuredFinalState,
                    &child_loop, &enclosing_loop, &boundary, &tail,
                    maximum_outer_iterations, maximum_child_iterations,
                    0, 0, publish_control_state);
  return emitter.Generate(error);
}

bool GenerateStructuredLoopStoreCommitCg(
    u32 maximum_outer_iterations, u32 maximum_child_iterations,
    u32 store_count, GeneratedCgProgram* program, std::string* error) {
  if (!program)
    return Fail(error, "null generated structured-store commit output");
  *program = {};
  if (maximum_outer_iterations == 0 ||
      maximum_outer_iterations > MaximumStructuredOuterIterations ||
      maximum_child_iterations == 0 ||
      maximum_child_iterations > MaximumStructuredChildIterations ||
      (maximum_child_iterations & (maximum_child_iterations - 1u)) != 0 ||
      store_count == 0 || store_count > MaximumValidationStores) {
    return Fail(error, "structured-store commit has an invalid bounded contract");
  }

  const u64 maximum_entries = static_cast<u64>(maximum_outer_iterations) *
                              maximum_child_iterations * store_count;
  if (maximum_entries > std::numeric_limits<u32>::max())
    return Fail(error, "structured-store commit journal is too large");

  program->execution_kind = GeneratedCgExecutionKind::StructuredStoreCommit;
  program->uses_structured_store_commit = true;
  program->maximum_structured_iterations = maximum_outer_iterations;
  program->maximum_structured_child_iterations = maximum_child_iterations;
  program->structured_store_count = store_count;

  std::string& source = program->source;
  source += StructuredParallelOptimizationMarker;
  source +=
      "#pragma readwrite_buffer BUFFER12\n"
      "#pragma readwrite_buffer BUFFER13\n"
      "void main(\n"
      "\tuniform unsigned int VuStoreAddresses[1] : BUFFER9,\n"
      "\tuniform int4 VuStoreValues[1] : BUFFER10,\n"
      "\tuniform unsigned int VuOuterState[16] : BUFFER12,\n"
      "\tuniform int4 VuPrivateMemory[1024] : BUFFER13,\n"
      "\tunsigned int VuInvocation : INDEX,\n"
      "\tout float4 VuPosition : POSITION,\n"
      "\tout float VuPointSize : PSIZE,\n"
      "\tout float4 VuResult : TEXCOORD0)\n"
      "{\n"
      "\tVuPosition = float4(2.0f, 2.0f, 2.0f, 1.0f);\n"
      "\tVuPointSize = 1.0f;\n"
      "\tVuResult = float4(0.0f);\n"
      "\tconst unsigned int VuOuterCount = VuOuterState[0];\n"
      "\tbool VuValid = VuOuterState[3] == 0u && VuOuterCount <= ";
  source += std::to_string(maximum_outer_iterations);
  source +=
      "u;\n"
      "\tconst unsigned int VuActiveEntries = VuOuterCount * ";
  source += std::to_string(maximum_child_iterations);
  source +=
      "u;\n"
      "\t#pragma branch (flatten: never)\n"
      "\tif (!VuValid || VuInvocation >= VuActiveEntries)\n"
      "\t{\n"
      "\t\tif (!VuValid)\n"
      "\t\t{\n"
      "\t\t\tVuOuterState[3] = " +
      std::to_string(static_cast<u32>(
          StructuredGeneratedRuntimeFailure::CommitMetadata)) +
      "u;\n"
      "\t\t\tVuOuterState[5] = VuOuterState[3];\n"
      "\t\t}\n"
      "\t\treturn;\n"
      "\t}\n";
  for (u32 store_index = 0; store_index < store_count; store_index++) {
    const std::string suffix = std::to_string(store_index);
    source += "\tconst unsigned int VuSlot" + suffix +
              " = VuInvocation * " + std::to_string(store_count) + "u + " +
              suffix + "u;\n";
    source += "\tconst unsigned int VuPacked" + suffix +
              " = VuStoreAddresses[VuSlot" + suffix + "];\n";
    source += "\t#pragma branch (flatten: never)\n"
              "\tif (VuPacked" + suffix + " != 4294967295u)\n"
              "\t{\n"
              "\t\tif ((VuPacked" + suffix + " & " +
              std::to_string(~StructuredStorePackedMask) +
              "u) != 0u || (VuPacked" + suffix + " & " +
              std::to_string(StructuredStoreLaneMask) + "u) == 0u)\n"
              "\t\t{\n"
              "\t\t\tVuOuterState[3] = " +
              std::to_string(static_cast<u32>(
                  StructuredGeneratedRuntimeFailure::CommitMetadata)) +
              "u;\n"
              "\t\t\tVuOuterState[5] = VuOuterState[3];\n"
              "\t\t}\n"
              "\t\telse\n"
              "\t\t{\n"
              "\t\t\tconst unsigned int VuAddress" + suffix +
              " = VuPacked" + suffix + " & 1023u;\n"
              "\t\t\tconst unsigned int VuMask" + suffix +
              " = (VuPacked" + suffix + " >> 16u) & 15u;\n"
              "\t\t\tconst int4 VuValue" + suffix +
              " = VuStoreValues[VuSlot" + suffix + "];\n"
              "\t\t\tint4 VuCurrent" + suffix +
              " = VuPrivateMemory[VuAddress" + suffix + "];\n"
              "\t\t\tif ((VuMask" + suffix +
              " & 8u) != 0u) VuCurrent" + suffix + ".x = VuValue" +
              suffix + ".x;\n"
              "\t\t\tif ((VuMask" + suffix +
              " & 4u) != 0u) VuCurrent" + suffix + ".y = VuValue" +
              suffix + ".y;\n"
              "\t\t\tif ((VuMask" + suffix +
              " & 2u) != 0u) VuCurrent" + suffix + ".z = VuValue" +
              suffix + ".z;\n"
              "\t\t\tif ((VuMask" + suffix +
              " & 1u) != 0u) VuCurrent" + suffix + ".w = VuValue" +
              suffix + ".w;\n"
              "\t\t\tVuPrivateMemory[VuAddress" + suffix +
              "] = VuCurrent" + suffix + ";\n"
              "\t\t}\n"
              "\t}\n";
  }
  source += "}\n";
  if (source.size() > MaximumGeneratedSourceBytes)
    return Fail(error, "generated structured-store commit exceeds source bound");
  if (error)
    error->clear();
  return true;
}

bool ApplyStructuredStoreJournalReference(
    const std::vector<StructuredStoreJournalEntry>& journal,
    u32 active_entry_count,
    std::array<std::array<u32, 4>, 1024>* vu_memory,
    std::string* error) {
  if (!vu_memory || active_entry_count > journal.size())
    return Fail(error, "structured-store journal extent is invalid");

  auto candidate = *vu_memory;
  for (u32 index = 0; index < active_entry_count; index++) {
    const StructuredStoreJournalEntry& entry = journal[index];
    if (entry.packed_address == 0xffffffffu)
      continue;
    if ((entry.packed_address & ~StructuredStorePackedMask) != 0 ||
        (entry.packed_address & StructuredStoreLaneMask) == 0) {
      return Fail(error, "structured-store journal metadata is invalid");
    }
    const u32 address = entry.packed_address & StructuredStoreAddressMask;
    const u32 lanes = (entry.packed_address >> 16u) & 0x0fu;
    for (u32 lane = 0; lane < 4; lane++) {
      if ((lanes & (0x8u >> lane)) != 0)
        candidate[address][lane] = entry.value[lane];
    }
  }
  *vu_memory = std::move(candidate);
  if (error)
    error->clear();
  return true;
}

bool ComputeStructuredChildIterationCount(
    const ParallelLoopKernel& transition_kernel,
    const NaturalLoop& child_loop, u32* count, std::string* error) {
  if (!count || child_loop.counter_reg == 0 ||
      child_loop.counter_reg >= transition_kernel.child_entry_vi_values.size() ||
      child_loop.counter_limit_reg >=
          transition_kernel.child_entry_vi_values.size() ||
      static_cast<LowerKind>(child_loop.branch_kind) != LowerKind::IBNE ||
      !child_loop.branch_taken_repeats ||
      (child_loop.counter_step != 1 && child_loop.counter_step != -1)) {
    return Fail(error, "structured child has no constant IBNE trip count");
  }
  const AffineViValue& counter =
      transition_kernel.child_entry_vi_values[child_loop.counter_reg];
  const AffineViValue zero_limit{0, 0, true};
  const AffineViValue& limit = child_loop.counter_limit_reg == 0
                                   ? zero_limit
                                   : transition_kernel.child_entry_vi_values[
                                         child_loop.counter_limit_reg];
  if (!counter.valid || !limit.valid || counter.base_vi != 0 ||
      limit.base_vi != 0) {
    return Fail(error,
                "structured child trip count depends on runtime VI state");
  }
  const u16 counter_value = static_cast<u16>(counter.offset);
  const u16 limit_value = static_cast<u16>(limit.offset);
  const u32 trip = child_loop.counter_step > 0
                       ? static_cast<u16>(limit_value - counter_value)
                       : static_cast<u16>(counter_value - limit_value);
  if (trip == 0 || trip > MaximumStructuredChildIterations) {
    return Fail(error, "structured child constant trip count is out of bounds");
  }
  *count = trip;
  if (error)
    error->clear();
  return true;
}

bool ComputeStructuredChildIterationStride(
    const ParallelLoopKernel& transition_kernel,
    const NaturalLoop& child_loop, u32* stride, std::string* error) {
  u32 trip = 0;
  if (!stride || !ComputeStructuredChildIterationCount(
                     transition_kernel, child_loop, &trip, error)) {
    return false;
  }
  u32 rounded = 1;
  while (rounded < trip)
    rounded <<= 1u;
  *stride = rounded;
  if (error)
    error->clear();
  return true;
}

bool GenerateStructuredLoopParallelDirectTfxCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 child_iteration_count, const DirectTfxContract& contract,
    GeneratedCgProgram* program, std::string* error) {
  if (!program)
    return Fail(error, "null generated structured direct-TFX output");
  u32 proven_child_iterations = 0;
  if (!ComputeStructuredChildIterationCount(
          child_kernel, child_loop, &proven_child_iterations, error)) {
    return false;
  }
  if (child_iteration_count != proven_child_iterations) {
    return Fail(error,
                "structured direct-TFX child count differs from PairPlan "
                "proof");
  }
  *program = {};
  CgEmitter emitter(child_kernel, &contract, program,
                    CgOutputKind::StructuredParallelDirectTfx,
                    &child_loop, nullptr, &boundary, nullptr,
                    maximum_outer_iterations, child_iteration_count);
  return emitter.Generate(error);
}

bool GenerateParallelTfxCg(const ParallelLoopKernel &kernel,
                           const DirectTfxContract &contract,
                           GeneratedCgProgram *program,
                           std::string *error) {
  if (!program)
    return Fail(error, "null generated TFX Cg output");
  *program = {};
  CgEmitter emitter(kernel, &contract, program, CgOutputKind::DirectTfx);
  return emitter.Generate(error);
}

bool GenerateLoopKernelDirectTfxCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    GeneratedCgProgram* program, std::string* error) {
  return GenerateLoopKernelDirectTfxCg(
      kernel, contract, GeneratedBatchVaryingLiveIns{}, program, error);
}

static bool RewriteSinkScheduledBatchUniformsAsLazyMacros(
    GeneratedCgProgram* program, std::string* error) {
  if (!program || !program->UsesLazySinkScheduledBatchUniforms()) {
    return true;
  }

  std::vector<std::string> aliases;
  aliases.reserve(program->BatchUniformVectorCount());
  for (const CgConstantInput& input : program->constant_inputs)
    aliases.push_back("VuConstant" + std::to_string(input.uniform_index));
  for (u32 reg = 1u; reg < 32u; reg++) {
    if ((program->vf_uniform_mask & (1u << reg)) == 0u)
      continue;
    aliases.push_back("VF" + (reg < 10u ? std::string("0") : std::string()) +
                      std::to_string(reg));
  }
  if (program->uses_acc_uniform)
    aliases.emplace_back("ACC");
  if (program->uses_q_uniform || program->uses_p_uniform ||
      program->uses_i_uniform || program->uses_gif_q_uniform) {
    aliases.emplace_back("VuBatchScalars");
  }

  for (const std::string& alias : aliases) {
    const std::string declaration = "\tconst float4 " + alias + " = ";
    const size_t begin = program->source.find(declaration);
    if (begin == std::string::npos ||
        program->source.find(declaration, begin + declaration.size()) !=
            std::string::npos) {
      return Fail(error,
                  "sink-scheduled batch live-in has no unique declaration");
    }
    const size_t value_begin = begin + declaration.size();
    const size_t end = program->source.find(";\n", value_begin);
    if (end == std::string::npos || end == value_begin)
      return Fail(error, "sink-scheduled batch live-in is malformed");
    const std::string value =
        program->source.substr(value_begin, end - value_begin);
    program->source.replace(begin, end + 2u - begin,
                            "#define " + alias + " (" + value + ")\n");
  }
  return true;
}

static bool GenerateLoopKernelDirectTfxCgImpl(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins,
    bool emit_private_output, GeneratedCgProgram* program,
    std::string* error, bool state_canary = false,
    const GeneratedCgProgram* varying_input_layout = nullptr
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
    , bool exact_depth_analysis = false
    , std::span<const u8> exact_fmac_nodes = {}
    , bool range_scaled_multiply = false
    , bool normalized_fmac_inputs = false
#endif
    ) {
  if (!program)
    return Fail(error, "null generated loop-kernel TFX output");

  GeneratedCgProgram generated;
  CgEmitter emitter(kernel, &contract, &generated,
                    CgOutputKind::LoopKernelDirectTfx);
  emitter.SetEmitLoopKernelPrivateOutput(emit_private_output);
  bool lean_native_output = !emit_private_output;
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
  // Offline raster resource controls must retain the validation numeric
  // profile. The ordinary lean product profile bypasses exact FMAC helpers.
  lean_native_output &= !exact_depth_analysis;
#endif
  emitter.SetNativeOutputOnlyProfile(lean_native_output);
  emitter.SetStateCanary(state_canary);
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
  emitter.SetExactDepthAnalysis(exact_depth_analysis, exact_fmac_nodes, range_scaled_multiply,
                               normalized_fmac_inputs);
#endif
  if (state_canary && !emit_private_output)
    return Fail(error, "state canary cannot omit its complete private journal");
  emitter.SetBatchVaryingLiveIns(
      &varying_live_ins, varying_input_layout);
  if (!emitter.Generate(error))
    return false;
  if (generated.execution_kind != GeneratedCgExecutionKind::DirectVuTfx ||
      generated.control_strategy != GeneratedCgControlStrategy::None ||
      generated.UsesStructuredSnapshotInputBuffers() ||
      generated.uses_structured_state_snapshots ||
      generated.uses_structured_memory_preflight ||
      generated.uses_structured_expression_scratch ||
      generated.uses_generated_direct_precompute ||
      generated.uses_structured_parallel_child_memory_store ||
      generated.uses_structured_parallel_direct_vu_tfx ||
      generated.uses_structured_store_commit ||
      generated.uses_structured_final_state ||
      generated.uses_final_state_output) {
    return Fail(error,
                "generated loop kernel inherited a non-atomic runtime ABI");
  }

  // The hardware-driven provider never admits the legacy INSTANCE shape.
  // Expanded flat INDEX roots are safe: their private journal is indexed by
  // the expanded shader-invocation domain, so duplicated source vertices do
  // not race on writable BUFFER2 state.
  if (generated.uses_flat_instance_inputs) {
    return Fail(error,
                "loop-kernel private store output rejects INSTANCE execution");
  }

  if (kernel.stores.empty() || kernel.stores.size() > MaximumValidationStores)
    return Fail(error, "generated loop kernel has an invalid private-store journal");
  generated.uses_loop_kernel_private_store_output = emit_private_output;
  generated.loop_kernel_private_store_count = emit_private_output
      ? static_cast<u8>(kernel.stores.size())
      : 0u;
  if (!emit_private_output) {
    generated.uses_loop_kernel_ftoi_probe_output = false;
    generated.loop_kernel_ftoi_probe_configuration_bits = 0u;
    generated.loop_kernel_ftoi_probe_store_index = 0u;
    generated.loop_kernel_ftoi_probe_lane = 0u;
    generated.loop_kernel_ftoi_probe_scale_offset = 0u;
    generated.loop_kernel_source_abi = generated.batch_varying_live_ins.Any()
        ? GeneratedLoopKernelPartialBatchProductCgAbiVersion
        : GeneratedLoopKernelProductCgAbiVersion;
  }

  if (state_canary) {
    generated.loop_kernel_source_abi = generated.batch_varying_live_ins.Any()
        ? GeneratedLoopKernelPartialStateCanaryCgAbiVersion
        : GeneratedLoopKernelStateCanaryCgAbiVersion;
  } else if (generated.uses_nested_iteration_grid && generated.uses_flat_index_inputs) {
    generated.loop_kernel_source_abi = emit_private_output
        ? (generated.batch_varying_live_ins.Any() ? GeneratedLoopKernelNestedFlatPartialCgAbiVersion
                                 : GeneratedLoopKernelNestedFlatCgAbiVersion)
        : (generated.batch_varying_live_ins.Any()
               ? GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion
               : GeneratedLoopKernelNestedFlatProductCgAbiVersion);
  }

  if (!RewriteSinkScheduledBatchUniformsAsLazyMacros(&generated, error))
    return false;

  static_assert(GeneratedLoopKernelCgAbiVersion == 26u);
  static_assert(GeneratedLoopKernelPartialBatchCgAbiVersion == 51u);
  static_assert(GeneratedLoopKernelProductCgAbiVersion == 52u);
  static_assert(GeneratedLoopKernelPartialBatchProductCgAbiVersion == 54u);
  // Exact store cones make Sony's optimized frontend expand every software
  // binary32 boundary. The 30,135-byte scalar ABI-23 fixture failed to finish
  // psp2cgc 13284 O1 in 180 seconds, while O0 completed in 16 seconds and
  // emitted one parallel 11,928-byte GXP with 72 temporaries, 38 SAs, and no
  // reported backing spill. Keep ordinary/native roots on the physically
  // exercised O1 path, but give an exact root its separately attested,
  // compiler-bounded class. Physical Shacc and runtime performance remain
  // mandatory private gates.
  std::string abi_marker =
      generated.emitted_software_f32_operation_count != 0u
          ? LoopKernelExactOptimizationMarker
          : "// VitaSX2 generated loop-kernel optimization ceiling O1\n";
  abi_marker +=
      "#define VITASX2_GPU_VU_LOOP_KERNEL_ABI ";
  abi_marker += std::to_string(generated.loop_kernel_source_abi);
  abi_marker +=
      "\n"
      "#define VITASX2_GPU_VU_STATIC_FLOW 1\n"
      "#define VITASX2_GPU_VU_NESTED_GRID ";
  abi_marker += generated.uses_nested_iteration_grid ? "1\n" : "0\n";
  abi_marker += "#define VITASX2_GPU_VU_NESTED_BATCH_INDEX ";
  abi_marker += generated.uses_nested_batch_index_inputs ? "1\n" : "0\n";
  abi_marker += "#define VITASX2_GPU_VU_NESTED_OUTER ";
  abi_marker += std::to_string(generated.nested_outer_iterations);
  abi_marker += "\n#define VITASX2_GPU_VU_NESTED_CHILD ";
  abi_marker += std::to_string(generated.nested_child_iterations);
  abi_marker += "\n#define VITASX2_GPU_VU_FTOI_PROBE ";
  abi_marker += generated.uses_loop_kernel_ftoi_probe_output ? "1\n" : "0\n";
  if (state_canary) {
    abi_marker +=
        "#define VITASX2_GPU_VU_DENSE_STATE_CANARY 1\n"
        "#define VITASX2_GPU_VU_PRIVATE_POINTS_NO_RASTER 1\n";
  }
  if (!emit_private_output) {
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
    if (exact_depth_analysis) {
      abi_marker +=
          "#define VITASX2_GPU_VU_RASTER_PRECISION_ANALYSIS 1\n";
    } else
#endif
    {
      abi_marker +=
          "#define VITASX2_GPU_VU_ATTESTED_NO_WRITE_PRODUCT 1\n";
      abi_marker +=
          "#define VITASX2_GPU_VU_NATIVE_SGX_OUTPUT_ONLY 1\n";
    }
  }
  if (generated.uses_dynamic_batch_uniform_index) {
    abi_marker +=
        "#define VITASX2_GPU_VU_DYNAMIC_BATCH_UNIFORM_INDEX 1\n";
  }
  if (generated.uses_sink_scheduled_outputs) {
    abi_marker += "#define VITASX2_GPU_VU_SINK_SCHEDULED_OUTPUTS 1\n";
  }
  if (generated.UsesLazySinkScheduledBatchUniforms()) {
    abi_marker += "#define VITASX2_GPU_VU_LAZY_BATCH_UNIFORMS 1\n";
  }
  if (generated.UsesLazySinkScheduledVaryingConstants()) {
    abi_marker +=
        "#define VITASX2_GPU_VU_LAZY_VARYING_CONSTANTS 1\n";
  }
  if (generated.batch_varying_live_ins.Any()) {
    abi_marker += "#define VITASX2_GPU_VU_PARTIAL_BATCH_LIVE_INS 1\n";
    abi_marker += "#define VITASX2_GPU_VU_BATCH_VARYING_CONSTANT_MASK ";
    abi_marker +=
        std::to_string(generated.batch_varying_live_ins.constant_mask);
    abi_marker += "\n#define VITASX2_GPU_VU_BATCH_VARYING_VF_MASK ";
    abi_marker += std::to_string(generated.batch_varying_live_ins.vf_mask);
    abi_marker += "\n#define VITASX2_GPU_VU_BATCH_VARYING_ACC ";
    abi_marker += generated.batch_varying_live_ins.acc ? "1\n" : "0\n";
    abi_marker += "#define VITASX2_GPU_VU_BATCH_VARYING_SCALARS ";
    abi_marker +=
        generated.batch_varying_live_ins.scalars ? "1\n" : "0\n";
  }
  generated.source.insert(0u, abi_marker);
  generated.generated_source_bytes =
      static_cast<u32>(generated.source.size());
  generated.execution_kind =
      GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx;
  generated.control_strategy = GeneratedCgControlStrategy::ParallelLoop;
  *program = std::move(generated);
  if (error)
    error->clear();
  return true;
}

bool GenerateLoopKernelDirectTfxCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins,
    GeneratedCgProgram* program, std::string* error,
    const GeneratedCgProgram* varying_input_layout) {
  return GenerateLoopKernelDirectTfxCgImpl(
      kernel, contract, varying_live_ins, true, program, error, false,
      varying_input_layout);
}

bool GenerateLoopKernelProductDirectTfxCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins,
    GeneratedCgProgram* program, std::string* error,
    const GeneratedCgProgram* varying_input_layout) {
  return GenerateLoopKernelDirectTfxCgImpl(
      kernel, contract, varying_live_ins, false, program, error, false,
      varying_input_layout);
}

bool GenerateLoopKernelStateCanaryCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins,
    GeneratedCgProgram* program, std::string* error,
    const GeneratedCgProgram* varying_input_layout) {
  return GenerateLoopKernelDirectTfxCgImpl(
      kernel, contract, varying_live_ins, true, program, error, true,
      varying_input_layout);
}

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
bool GenerateLoopKernelDepthPrecisionAnalysisCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins, bool state_canary,
    GeneratedCgProgram* program, std::string* error,
    const GeneratedCgProgram* varying_input_layout,
    std::span<const u8> exact_fmac_nodes, bool range_scaled_multiply,
    bool normalized_fmac_inputs, bool raster_output_only) {
  if (raster_output_only && state_canary)
    return Fail(error, "raster precision analysis cannot claim private state validation");
  if (!GenerateLoopKernelDirectTfxCgImpl(kernel, contract, varying_live_ins,
          !raster_output_only, program, error, state_canary, varying_input_layout, true,
          exact_fmac_nodes, range_scaled_multiply, normalized_fmac_inputs))
    return false;
  if (raster_output_only &&
      (program->uses_loop_kernel_private_store_output ||
       program->uses_loop_kernel_state_canary || !program->uses_tfx_uniforms ||
       program->source.find("#pragma readwrite_buffer") != std::string::npos))
    return Fail(error, "raster precision analysis inherited private output");
  program->source.insert(0u, "// Offline depth-precision resource analysis; not an attested provider.\n");
  program->generated_source_bytes = static_cast<u32>(program->source.size());
  return true;
}
#endif

} // namespace VitaGpuVu
