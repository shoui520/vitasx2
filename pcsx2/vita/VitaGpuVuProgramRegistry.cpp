// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuProgramRegistry.h"

#include "common/Console.h"
#include "common/Threading.h"
#include "vita/VitaGpuVuDirectProgram.h"
#include "vita/VitaGpuVuGeneratedUniversal.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace VitaGpuVu {
namespace {

struct ShaderKeyLess {
  bool operator()(const ShaderKey &left, const ShaderKey &right) const {
    return left.high < right.high ||
           (left.high == right.high && left.low < right.low);
  }
};

struct RegistryEntry {
  GeneratedProgramState state = GeneratedProgramState::Missing;
  GeneratedCgProgram metadata;
  GeneratedLoopKernelAttestationRecord loop_kernel_attestation;
  GeneratedGxpResourceAttestation resource_attestation =
      GeneratedGxpResourceAttestation::MissingInput;
};

GeneratedCgProgram CopyGeneratedProgramMetadataWithoutSource(
    GeneratedCgProgram* compiler_program) {
  if (!compiler_program)
    return {};

  // Cg text is compiler-queue input, not registry metadata.  Detach it while
  // copying so a 40--52 KiB generated root does not leave another full-capacity
  // std::string allocation in every registry entry.  Restore ownership before
  // returning so the non-blocking compiler submission below remains the sole
  // consumer of this RequestGeneratedProgram() argument.
  std::string compiler_source;
  compiler_source.swap(compiler_program->source);
  try {
    GeneratedCgProgram metadata = *compiler_program;
    compiler_program->source.swap(compiler_source);
    return metadata;
  } catch (...) {
    compiler_program->source.swap(compiler_source);
    throw;
  }
}

Threading::KernelMutex s_registry_mutex;
std::map<ShaderKey, RegistryEntry, ShaderKeyLess> s_registry;
ShaderCompiler *s_compiler = nullptr;
std::atomic<u64> s_requests{0};
std::atomic<u64> s_unavailable_requests{0};
std::atomic<u64> s_cache_hits{0};
std::atomic<u64> s_cache_misses{0};
std::atomic<u64> s_compiler_queue_retries{0};
std::atomic<u64> s_compile_successes{0};
std::atomic<u64> s_compile_failures{0};
std::atomic<u64> s_ready_programs{0};
std::atomic<u64> s_failed_programs{0};
std::atomic<u64> s_compiler_safety_rejections{0};
std::atomic<u64> s_compiler_in_flight_programs{0};
std::atomic<u64> s_compiler_in_flight_plans{0};
std::atomic<u64> s_compiler_idle_generation{0};

const char* GeneratedExecutionKindName(GeneratedCgExecutionKind kind) {
  switch (kind) {
  case GeneratedCgExecutionKind::DirectVuTfx:
    return "direct-vu-tfx";
  case GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx:
    return "generated-loop-kernel-direct-vu-tfx";
  case GeneratedCgExecutionKind::UniversalStateMachine:
    return "universal-state-machine";
  case GeneratedCgExecutionKind::UniversalDirectStateMachine:
    return "direct-state-machine";
  case GeneratedCgExecutionKind::UniversalCompactContinuation:
    return "compact-continuation";
  case GeneratedCgExecutionKind::StructuredStateSnapshots:
    return "structured-state";
  case GeneratedCgExecutionKind::StructuredMemoryPreflight:
    return "structured-preflight";
  case GeneratedCgExecutionKind::StructuredExpressionScratch:
    return "structured-expression-scratch";
  case GeneratedCgExecutionKind::StructuredFixedQpNumeric:
    return "structured-fixed-qp-numeric";
  case GeneratedCgExecutionKind::StructuredFixedFmacNumeric:
    return "structured-fixed-fmac-numeric";
  case GeneratedCgExecutionKind::StructuredParallelChildMemoryStore:
    return "structured-parallel-child-store";
  case GeneratedCgExecutionKind::StructuredParallelDirectVuTfx:
    return "structured-parallel-direct-vu-tfx";
  case GeneratedCgExecutionKind::StructuredStoreCommit:
    return "structured-store-commit";
  case GeneratedCgExecutionKind::StructuredFinalState:
    return "structured-final";
  }
  return "unknown";
}

const char* GeneratedControlStrategyName(
    GeneratedCgControlStrategy strategy) {
  switch (strategy) {
  case GeneratedCgControlStrategy::None:
    return "none";
  case GeneratedCgControlStrategy::PerPairStateMachine:
    return "per-pair";
  case GeneratedCgControlStrategy::BlockThreadedStateMachine:
    return "block-threaded";
  case GeneratedCgControlStrategy::StructuredLoop:
    return "structured-loop";
  case GeneratedCgControlStrategy::ParallelLoop:
    return "parallel-loop";
  }
  return "unknown";
}

constexpr u64 FnvPrime = 1099511628211ull;
constexpr u64 LowOffset = 14695981039346656037ull;
constexpr u64 HighOffset = 7809847782465536322ull;

void HashByte(u64 *hash, u8 value) {
  *hash ^= value;
  *hash *= FnvPrime;
}

ShaderKey HashSource(std::string_view source) {
  ShaderKey key{LowOffset, HighOffset};
  for (u32 shift = 0; shift < 32; shift += 8) {
    const u8 byte = static_cast<u8>(GeneratedProgramAbiVersion >> shift);
    HashByte(&key.low, byte);
    HashByte(&key.high, static_cast<u8>(byte ^ 0xa5u));
  }
  for (const char character : source) {
    const u8 byte = static_cast<u8>(character);
    HashByte(&key.low, byte);
    HashByte(&key.high, static_cast<u8>((byte << 1) | (byte >> 7)));
  }
  // Distinguish concatenations from the ABI prefix and make the empty-source
  // value an explicit invalid request rather than a useful cache entry.
  for (u32 shift = 0; shift < 64; shift += 8) {
    const u8 byte = static_cast<u8>(source.size() >> shift);
    HashByte(&key.low, byte);
    HashByte(&key.high, static_cast<u8>(byte ^ 0x5au));
  }
  return key;
}

bool IsCgIdentifierCharacter(char value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_';
}

bool IsCgWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
         value == '\f' || value == '\v';
}

size_t FindCgIdentifier(std::string_view source,
                        std::string_view identifier,
                        size_t offset = 0u) {
  while ((offset = source.find(identifier, offset)) !=
         std::string_view::npos) {
    const size_t end = offset + identifier.size();
    if ((offset == 0u ||
         !IsCgIdentifierCharacter(source[offset - 1u])) &&
        (end == source.size() ||
         !IsCgIdentifierCharacter(source[end]))) {
      return offset;
    }
    offset = end;
  }
  return std::string_view::npos;
}

size_t FindCgFloat4Declaration(std::string_view source,
                               std::string_view identifier,
                               size_t offset = 0u) {
  const std::string full =
      "\tconst float4 " + std::string(identifier) + " = ";
  const std::string compact =
      "const float4 " + std::string(identifier) + "=";
  const size_t full_offset = source.find(full, offset);
  const size_t compact_offset = source.find(compact, offset);
  if (full_offset == std::string_view::npos)
    return compact_offset;
  if (compact_offset == std::string_view::npos)
    return full_offset;
  return std::min(full_offset, compact_offset);
}

bool ContainsExactCgFloat4Declaration(std::string_view source,
                                      std::string_view identifier,
                                      std::string_view initializer) {
  const std::string full = "\tconst float4 " + std::string(identifier) +
      " = " + std::string(initializer) + ";\n";
  const std::string compact = "const float4 " + std::string(identifier) +
      "=" + std::string(initializer) + ";";
  return source.find(full) != std::string_view::npos ||
         source.find(compact) != std::string_view::npos;
}

// Compiler admission accepts the emitter's readable and whitespace-compacted
// forms. Match one complete assignment, not an identifier mentioned elsewhere
// in the shader. This is cold source validation, never descriptor preparation.
bool ContainsSingleCgAssignment(std::string_view source,
                                std::string_view assignment) {
  const size_t equal = assignment.find('=');
  if (equal == std::string_view::npos)
    return false;
  std::string compact(assignment);
  compact.erase(std::remove_if(compact.begin(), compact.end(), IsCgWhitespace),
                compact.end());
  const size_t offset = source.find(assignment);
  const size_t compact_offset = source.find(compact);
  const size_t first = offset == std::string_view::npos ? compact_offset : offset;
  if (first == std::string_view::npos)
    return false;
  // Also reject a second assignment with a different RHS. The LHS begins at
  // an identifier (all callers supply a generated output or index variable).
  std::string_view lhs = assignment.substr(0u, equal);
  while (!lhs.empty() && IsCgWhitespace(lhs.back()))
    lhs.remove_suffix(1u);
  u32 assignments = 0u;
  for (size_t next = 0u;
       (next = FindCgIdentifier(source, lhs, next)) != std::string_view::npos;) {
    next += lhs.size();
    size_t token = next;
    while (token < source.size() && IsCgWhitespace(source[token]))
      token++;
    if (token < source.size() && source[token] == '=' &&
        (token + 1u == source.size() || source[token + 1u] != '='))
      assignments++;
  }
  return assignments == 1u;
}

bool HasDenseStateCanarySourceContract(const GeneratedCgProgram& program) {
  if (!HasGeneratedLoopKernelStateCanaryContract(program))
    return false;
  const std::string& source = program.source;
  if (source.find("#define VITASX2_GPU_VU_DENSE_STATE_CANARY 1\n") == std::string::npos ||
      source.find("#define VITASX2_GPU_VU_PRIVATE_POINTS_NO_RASTER 1\n") == std::string::npos ||
      source.find("VuExpandedVertex") != std::string::npos ||
      source.find("VuFlat") != std::string::npos ||
      source.find("VuSinkPosition") != std::string::npos ||
      source.find("VuSinkColor") != std::string::npos ||
      source.find("VertexScaleOffset") != std::string::npos ||
      source.find("MaxDepth") != std::string::npos ||
      (source.find("vPointSize : PSIZE") == std::string::npos &&
       source.find("vPointSize:PSIZE") == std::string::npos))
    return false;
  // PSIZE is mandatory for GXM POINTS. Neither the original ADC topology nor
  // the raster cone is allowed to clip away a journal writer.
  for (const std::string_view assignment : {
           "vPosition = float4(0.0f, 0.0f, 0.0f, 1.0f);",
           "vPointSize = 1.0f;",
           "vTexFloat = float4(0.0f, 0.0f, 0.0f, 1.0f);",
           "vTexInt = float4(0.0f, 0.0f, 0.0f, 0.0f);",
           "vColor = float4(0.0f, 0.0f, 0.0f, 0.0f);"}) {
    if (!ContainsSingleCgAssignment(source, assignment))
      return false;
  }
  u32 writes = 0u;
  for (u32 store = 0u; store < program.loop_kernel_private_store_count; store++) {
    u32 lanes = 0u;
    for (u32 lane = 0u; lane < 4u; lane++) {
      const std::string prefix = "VuPrivateStoreWords[(VuInvocation * " +
          std::to_string(program.loop_kernel_private_store_count) + "u + " +
          std::to_string(store) + "u) * 4u + " + std::to_string(lane) + "u] = ";
      std::string compact(prefix);
      compact.erase(std::remove_if(compact.begin(), compact.end(), IsCgWhitespace), compact.end());
      const size_t full = source.find(prefix);
      const size_t packed = source.find(compact);
      if (full == std::string::npos && packed == std::string::npos)
        continue;
      if ((full != std::string::npos &&
           source.find(prefix, full + prefix.size()) != std::string::npos) ||
          (packed != std::string::npos &&
           source.find(compact, packed + compact.size()) != std::string::npos) ||
          (full != std::string::npos && packed != std::string::npos))
        return false;
      lanes++;
    }
    if (lanes == 0u)
      return false;
    writes += lanes;
  }
  // Every reference must be the BUFFER2 declaration or a unique bounded
  // original-invocation store above, never a read or an expanded/local index.
  u32 references = 0u;
  for (size_t offset = 0u;
       (offset = FindCgIdentifier(source, "VuPrivateStoreWords", offset)) != std::string::npos;
       offset += sizeof("VuPrivateStoreWords") - 1u)
    references++;
  return references == writes + 1u;
}

bool ContainsCgKeywordCall(std::string_view source,
                           std::string_view keyword) {
  size_t offset = 0u;
  while ((offset = source.find(keyword, offset)) != std::string_view::npos) {
    const size_t end = offset + keyword.size();
    const bool left_boundary =
        offset == 0u || !IsCgIdentifierCharacter(source[offset - 1u]);
    const bool right_boundary =
        end == source.size() || !IsCgIdentifierCharacter(source[end]);
    size_t next = end;
    while (next < source.size() && IsCgWhitespace(source[next]))
      next++;
    if (left_boundary && right_boundary && next < source.size() &&
        source[next] == '(') {
      return true;
    }
    offset = end;
  }
  return false;
}

bool ContainsDynamicCgFlow(std::string_view source) {
  return ContainsCgKeywordCall(source, "if") ||
         ContainsCgKeywordCall(source, "for") ||
         ContainsCgKeywordCall(source, "while") ||
         ContainsCgKeywordCall(source, "switch") ||
         source.find('?') != std::string_view::npos ||
         source.find("&&") != std::string_view::npos ||
         source.find("||") != std::string_view::npos;
}

void RetireGeneratedPlanningWork() {
  const u64 previous =
      s_compiler_in_flight_plans.fetch_sub(1u, std::memory_order_acq_rel);
  if (previous == 1u &&
      s_compiler_in_flight_programs.load(std::memory_order_acquire) == 0u) {
    s_compiler_idle_generation.fetch_add(1u, std::memory_order_release);
  }
}

u32 StructuredFinalOutputVectorCount(const GeneratedCgProgram& program) {
  u32 vectors = 0u;
  for (u32 reg = 1u; reg < program.final_vf_lanes.size(); reg++)
    vectors += program.final_vf_lanes[reg] != 0u ? 1u : 0u;
  vectors += program.final_acc_lanes != 0u ? 1u : 0u;
  vectors += program.final_q ? 1u : 0u;
  vectors += program.final_p ? 1u : 0u;
  vectors += program.final_i ? 1u : 0u;
  return vectors;
}

bool HasGeneratedComputePointContract(const GeneratedCgProgram& program) {
  if (program.execution_kind == GeneratedCgExecutionKind::DirectVuTfx ||
      program.execution_kind ==
          GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx ||
      program.execution_kind ==
          GeneratedCgExecutionKind::StructuredParallelDirectVuTfx)
    return true;

  // Sony's sceGxmDraw contract requires every POINTS vertex program to
  // publish PSIZE.  Retail libGXM does not report the debug-only
  // INVALID_PRIMITIVE_TYPE diagnostic, so reject malformed generated source
  // before it can enter ShaccCg and independently recheck the compiled GXP at
  // registration.  The off-screen position and disabled fragment stage keep
  // writable-buffer compute roots out of TA/ISP raster work.
  const bool ordinary =
      program.source.find(" : POSITION") != std::string::npos &&
      program.source.find("PointSize : PSIZE") != std::string::npos &&
      program.source.find("PointSize = 1.0f") != std::string::npos &&
      program.source.find(
          "Position = float4(2.0f, 2.0f, 2.0f, 1.0f)") !=
          std::string::npos;
  const bool compact =
      program.source.find(":POSITION") != std::string::npos &&
      program.source.find("PointSize:PSIZE") != std::string::npos &&
      program.source.find("PointSize=1.0f") != std::string::npos &&
      program.source.find("Position=float4(2.0f,2.0f,2.0f,1.0f)") !=
          std::string::npos;
  return ordinary || compact;
}

bool HasExactStructuredScratchContract(const GeneratedCgProgram& program) {
  u32 write_count = 0u;
  bool has_dependency = false;
  for (u32 slot = 0u; slot < StructuredGeneratedScratchSlots; slot++) {
    const u32 bit = 1u << (slot % 32u);
    const bool reads =
        (program.structured_scratch_read_mask[slot / 32u] & bit) != 0u;
    const bool writes =
        (program.structured_scratch_write_mask[slot / 32u] & bit) != 0u;
    const std::string read_marker =
        "// VitaSX2 generated scratch read slot " + std::to_string(slot) +
        "\n";
    const std::string write_marker =
        "// VitaSX2 generated scratch write slot " + std::to_string(slot) +
        "\n";
    if ((program.source.find(read_marker) != std::string::npos) != reads ||
        (program.source.find(write_marker) != std::string::npos) != writes) {
      return false;
    }
    has_dependency |= reads || writes;
    write_count += writes ? 1u : 0u;
  }

  const bool expression_scratch =
      program.execution_kind ==
      GeneratedCgExecutionKind::StructuredExpressionScratch;
  if (expression_scratch) {
    if (write_count == 0u ||
        write_count != program.structured_scratch_output_count) {
      return false;
    }
  } else if (write_count != 0u ||
             program.structured_scratch_output_count != 0u) {
    return false;
  }

  const bool source_uses_scratch =
      program.source.find("VuScratchWords[") != std::string::npos;
  return !has_dependency || source_uses_scratch;
}

} // namespace

bool BeginGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity) {
  if (!record || !identity.IsValid())
    return false;

  if (!(record->identity == identity)) {
    // An old private draw may still retire after a cache/configuration change.
    // Never let the new identity steal its in-flight record; terminal records
    // are safely replaced because their exact identity remains in the draw.
    if (record->state == GeneratedLoopKernelAttestationState::PrivateTest)
      return false;
    *record = {};
    record->identity = identity;
  }
  if (record->state != GeneratedLoopKernelAttestationState::Unattested)
    return false;
  record->state = GeneratedLoopKernelAttestationState::PrivateTest;
  record->numeric_profile = GeneratedLoopKernelNumericProfile::None;
  record->rejection = GeneratedLoopKernelAttestationRejection::None;
  return true;
}

bool CompleteGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity,
    GeneratedLoopKernelNumericProfile numeric_profile,
    GeneratedLoopKernelAttestationRejection rejection) {
  if (!record || !identity.IsValid() || !(record->identity == identity) ||
      record->state != GeneratedLoopKernelAttestationState::PrivateTest) {
    return false;
  }

  const bool passed = rejection ==
      GeneratedLoopKernelAttestationRejection::None;
  if (passed ==
      (numeric_profile == GeneratedLoopKernelNumericProfile::None)) {
    return false;
  }
  record->state = passed ? GeneratedLoopKernelAttestationState::Passed
                         : GeneratedLoopKernelAttestationState::Rejected;
  record->numeric_profile = passed ? numeric_profile
                                   : GeneratedLoopKernelNumericProfile::None;
  record->rejection = rejection;
  return true;
}

bool PromoteGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity) {
  if (!record || !identity.IsValid() || !(record->identity == identity))
    return false;
  if (record->state == GeneratedLoopKernelAttestationState::Product)
    return true;
  if (record->state != GeneratedLoopKernelAttestationState::Passed ||
      record->numeric_profile == GeneratedLoopKernelNumericProfile::None ||
      record->rejection != GeneratedLoopKernelAttestationRejection::None) {
    return false;
  }
  record->state = GeneratedLoopKernelAttestationState::Product;
  return true;
}

bool CancelGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity) {
  if (!record || !identity.IsValid() || !(record->identity == identity) ||
      record->state != GeneratedLoopKernelAttestationState::PrivateTest) {
    return false;
  }
  record->state = GeneratedLoopKernelAttestationState::Unattested;
  record->numeric_profile = GeneratedLoopKernelNumericProfile::None;
  record->rejection = GeneratedLoopKernelAttestationRejection::None;
  return true;
}

const char* GeneratedLoopKernelAttestationStateName(
    GeneratedLoopKernelAttestationState state) {
  switch (state) {
  case GeneratedLoopKernelAttestationState::Unattested:
    return "unattested";
  case GeneratedLoopKernelAttestationState::PrivateTest:
    return "private-test";
  case GeneratedLoopKernelAttestationState::Passed:
    return "passed";
  case GeneratedLoopKernelAttestationState::Rejected:
    return "rejected";
  case GeneratedLoopKernelAttestationState::Product:
    return "product";
  }
  return "unknown";
}

const char* GeneratedLoopKernelNumericProfileName(
    GeneratedLoopKernelNumericProfile profile) {
  switch (profile) {
  case GeneratedLoopKernelNumericProfile::None:
    return "none";
  case GeneratedLoopKernelNumericProfile::ExactVu:
    return "exact-vu";
  case GeneratedLoopKernelNumericProfile::NativeSgxOutputOnly:
    return "native-sgx-output-only";
  }
  return "unknown";
}

const char* GeneratedLoopKernelAttestationRejectionName(
    GeneratedLoopKernelAttestationRejection rejection) {
  switch (rejection) {
  case GeneratedLoopKernelAttestationRejection::None:
    return "none";
  case GeneratedLoopKernelAttestationRejection::RetirementCancelled:
    return "retirement-cancelled";
  case GeneratedLoopKernelAttestationRejection::InvalidDescriptor:
    return "invalid-descriptor";
  case GeneratedLoopKernelAttestationRejection::OutputGuard:
    return "output-guard";
  case GeneratedLoopKernelAttestationRejection::ExactStoreMismatch:
    return "exact-store-mismatch";
  case GeneratedLoopKernelAttestationRejection::ExactCanonicalJournalMismatch:
    return "exact-canonical-journal-mismatch";
  case GeneratedLoopKernelAttestationRejection::PlayableStoreMismatch:
    return "playable-store-mismatch";
  case GeneratedLoopKernelAttestationRejection::ArchitecturalStateMismatch:
    return "architectural-state-mismatch";
  case GeneratedLoopKernelAttestationRejection::OutputOnlyClassificationMissing:
    return "output-only-classification-missing";
  case GeneratedLoopKernelAttestationRejection::NumericProbeMismatch:
    return "numeric-probe-mismatch";
  }
  return "unknown";
}

bool HasGeneratedLoopKernelStateCanaryContract(const GeneratedCgProgram& program) {
  const u64 invocations = static_cast<u64>(program.nested_outer_iterations) *
                          program.nested_child_iterations;
  return program.uses_loop_kernel_state_canary &&
      program.execution_kind == GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx &&
      program.control_strategy == GeneratedCgControlStrategy::ParallelLoop &&
      program.loop_kernel_source_abi == (program.batch_varying_live_ins.Any()
          ? GeneratedLoopKernelPartialStateCanaryCgAbiVersion
          : GeneratedLoopKernelStateCanaryCgAbiVersion) &&
      !program.requires_dynamic_entry_state && program.uses_nested_iteration_grid &&
      program.uses_nested_batch_index_inputs && program.uses_buffered_batch_inputs &&
      !program.uses_dynamic_batch_uniform_index && !program.uses_instance_indexed_batch_live_ins &&
      !program.uses_flat_index_inputs && !program.uses_flat_instance_inputs &&
      !program.uses_tfx_uniforms && program.uses_tfx_point_size &&
      !program.uses_tfx_uv_no_fog_interface && !program.uses_gif_q_uniform &&
      program.uses_loop_kernel_private_store_output && program.uses_sink_scheduled_outputs &&
      !program.uses_loop_kernel_ftoi_probe_output &&
      program.loop_kernel_private_store_count != 0u &&
      program.loop_kernel_private_store_count <= 8u &&
      invocations != 0u && invocations <= (1u << 16u);
}

bool IsGeneratedProgramRuntimeCompilerShapeAttested(
    const GeneratedCgProgram& program) {
  // Compilation is not product promotion. Dense canaries may only execute via
  // the full-grid private owner; a linked no-write product still requires its
  // completed physical state comparison and final-GXP resource attestation.
  if (program.uses_loop_kernel_state_canary &&
      !HasGeneratedLoopKernelStateCanaryContract(program))
    return false;
  if (!HasGeneratedComputePointContract(program) ||
      !HasExactStructuredScratchContract(program))
    return false;

  if (program.execution_kind ==
      GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx) {
    const bool state_canary_source =
        program.loop_kernel_source_abi == GeneratedLoopKernelStateCanaryCgAbiVersion ||
        program.loop_kernel_source_abi == GeneratedLoopKernelPartialStateCanaryCgAbiVersion;
    const bool flat_product_source =
        program.loop_kernel_source_abi == GeneratedLoopKernelNestedFlatProductCgAbiVersion ||
        program.loop_kernel_source_abi == GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion;
    if (program.uses_loop_kernel_state_canary != state_canary_source ||
        (state_canary_source ? !HasDenseStateCanarySourceContract(program)
                            : (program.source.find("VITASX2_GPU_VU_DENSE_STATE_CANARY") != std::string::npos ||
                               program.source.find("VITASX2_GPU_VU_PRIVATE_POINTS_NO_RASTER") != std::string::npos)))
      return false;
    const bool has_abi_marker =
        (program.loop_kernel_source_abi == GeneratedLoopKernelCgAbiVersion ||
         program.loop_kernel_source_abi ==
             GeneratedLoopKernelPartialBatchCgAbiVersion ||
         program.loop_kernel_source_abi ==
             GeneratedLoopKernelProductCgAbiVersion ||
         program.loop_kernel_source_abi ==
             GeneratedLoopKernelPartialBatchProductCgAbiVersion ||
         state_canary_source || flat_product_source) &&
        program.source.find(
            "#define VITASX2_GPU_VU_LOOP_KERNEL_ABI " +
            std::to_string(program.loop_kernel_source_abi) + "\n") !=
        std::string::npos;
    const bool has_static_flow_marker =
        program.source.find("#define VITASX2_GPU_VU_STATIC_FLOW 1") !=
        std::string::npos;
    const bool dynamic_source_flow = ContainsDynamicCgFlow(program.source);
    constexpr std::string_view private_store_pragma =
        "#pragma readwrite_buffer BUFFER2";
    constexpr std::string_view ftoi_probe_pragma =
        "#pragma readwrite_buffer BUFFER3";
    constexpr std::string_view writable_pragma =
        "#pragma readwrite_buffer";
    const auto has_exactly_one = [&program](std::string_view token) {
      const size_t first = program.source.find(token);
      return first != std::string::npos &&
             program.source.find(token, first + token.size()) ==
                 std::string::npos;
    };
    const size_t private_store_pragma_offset =
        program.source.find(private_store_pragma);
    const size_t ftoi_probe_pragma_offset =
        program.source.find(ftoi_probe_pragma);
    u32 writable_pragma_count = 0u;
    for (size_t offset = program.source.find(writable_pragma);
         offset != std::string::npos;
         offset = program.source.find(writable_pragma,
                                      offset + writable_pragma.size())) {
      writable_pragma_count++;
    }
    const bool product_source =
        flat_product_source ||
        program.loop_kernel_source_abi ==
            GeneratedLoopKernelProductCgAbiVersion ||
        program.loop_kernel_source_abi ==
            GeneratedLoopKernelPartialBatchProductCgAbiVersion;
    const bool has_attested_no_write_product_marker =
        program.source.find(
            "#define VITASX2_GPU_VU_ATTESTED_NO_WRITE_PRODUCT 1") !=
        std::string::npos;
    const bool has_native_sgx_output_only_marker =
        program.source.find(
            "#define VITASX2_GPU_VU_NATIVE_SGX_OUTPUT_ONLY 1") !=
        std::string::npos;
    const bool attested_no_write_product_contract = product_source
        ? (has_attested_no_write_product_marker &&
           has_native_sgx_output_only_marker)
        : (!has_attested_no_write_product_marker &&
           !has_native_sgx_output_only_marker);
    const bool ftoi_probe_source_contract =
        program.uses_loop_kernel_ftoi_probe_output
            ? (ftoi_probe_pragma_offset != std::string::npos &&
               has_exactly_one(ftoi_probe_pragma) &&
               writable_pragma_count == 2u &&
               (program.source.find(
                    "VuFtoiProbeQwords[1] : BUFFER[3]") !=
                    std::string::npos ||
                program.source.find(
                    "VuFtoiProbeQwords[1]:BUFFER[3]") !=
                    std::string::npos) &&
               program.source.find("VuFtoiProbeQwords[") !=
                   program.source.rfind("VuFtoiProbeQwords[") &&
               program.source.find("VitaVuFloatToInt(") !=
                   std::string::npos &&
               program.source.find("VitaVuPlayableFloatToInt(") !=
                   std::string::npos &&
               (program.source.find(
                    "int VitaVuFloatToInt(float value, unsigned int scaleOffset)") !=
                    std::string::npos ||
                program.source.find(
                    "int VitaVuFloatToInt(float value,unsigned int scaleOffset)") !=
                    std::string::npos) &&
               program.source.find(
                   "int VitaVuFloatToInt(float value, float scale)") ==
                   std::string::npos &&
               program.source.find(
                   "int VitaVuFloatToInt(float value,float scale)") ==
                   std::string::npos &&
               (program.source.find(
                    "const int negativeShiftMask = binaryShift >> 31") !=
                    std::string::npos ||
                program.source.find(
                    "const int negativeShiftMask=binaryShift>>31") !=
                    std::string::npos) &&
               (program.source.find(
                    "const unsigned int exponentNonZeroBits = exponent | (0u - exponent)") !=
                    std::string::npos ||
                program.source.find(
                    "const unsigned int exponentNonZeroBits=exponent|(0u-exponent)") !=
                    std::string::npos) &&
               (program.source.find(
                    "const int finiteMask = exponentNonZeroMask & nonNegativeMask & lessThan31Mask") !=
                    std::string::npos ||
                program.source.find(
                    "const int finiteMask=exponentNonZeroMask&nonNegativeMask&lessThan31Mask") !=
                    std::string::npos) &&
               program.source.find("int(exponent != 0u) *") ==
                   std::string::npos &&
               program.source.find("int(exponent!=0u)*") ==
                   std::string::npos &&
               program.source.find(
                   "#define VITASX2_GPU_VU_FTOI_PROBE 1") !=
                   std::string::npos &&
               IsGeneratedCgExactDivideConfigurationSupported(
                   program.loop_kernel_ftoi_probe_configuration_bits) &&
               IsGeneratedCgNativeF32ConfigurationSupported(
                   program.loop_kernel_ftoi_probe_configuration_bits) &&
               (program.loop_kernel_ftoi_probe_configuration_bits &
                UniversalConfigurationApproximateConversions) != 0u &&
               program.loop_kernel_ftoi_probe_store_index <
                   program.loop_kernel_private_store_count &&
               program.loop_kernel_ftoi_probe_lane < 4u &&
               (program.loop_kernel_ftoi_probe_scale_offset == 0u ||
                program.loop_kernel_ftoi_probe_scale_offset == 4u ||
                program.loop_kernel_ftoi_probe_scale_offset == 12u ||
                program.loop_kernel_ftoi_probe_scale_offset == 15u))
            : (ftoi_probe_pragma_offset == std::string::npos &&
               writable_pragma_count == (product_source ? 0u : 1u) &&
               program.source.find("VuFtoiProbeQwords[") ==
                   std::string::npos &&
               program.source.find(
                   "#define VITASX2_GPU_VU_FTOI_PROBE 0") !=
                   std::string::npos &&
               program.loop_kernel_ftoi_probe_configuration_bits == 0u &&
               program.loop_kernel_ftoi_probe_store_index == 0u &&
               program.loop_kernel_ftoi_probe_lane == 0u &&
               program.loop_kernel_ftoi_probe_scale_offset == 0u);
    const bool private_store_source_contract =
        product_source
            ? (private_store_pragma_offset == std::string::npos &&
               program.source.find("VuPrivateStoreWords") ==
                   std::string::npos)
            : (private_store_pragma_offset != std::string::npos &&
               has_exactly_one(private_store_pragma) &&
               (program.source.find("VuPrivateStoreWords[1] : BUFFER[2]") !=
                    std::string::npos ||
                program.source.find("VuPrivateStoreWords[1]:BUFFER[2]") !=
                    std::string::npos));
    const bool forbidden_source =
        program.source.find("VuOuterSnapshots") != std::string::npos ||
        program.source.find("VuOuterViSnapshots") != std::string::npos ||
        program.source.find("VuOuterState") != std::string::npos ||
        program.source.find("VuScratchWords") != std::string::npos ||
        program.source.find("VuPrivateState") != std::string::npos ||
        program.source.find("VuStoreActive") != std::string::npos ||
        program.source.find("VITASX2_GPU_VU_ROUNDING_BARRIER") !=
            std::string::npos ||
        program.source.find("VuRM(") != std::string::npos ||
        program.source.find("VuRB") != std::string::npos ||
        program.source.find("BUFFER10") != std::string::npos ||
        program.source.find("BUFFER11") != std::string::npos ||
        program.source.find("BUFFER12") != std::string::npos ||
        program.source.find("BUFFER13") != std::string::npos ||
        (program.source.find(": INSTANCE") != std::string::npos ||
         program.source.find(":INSTANCE") != std::string::npos) ||
        // Shacc 3.0 materializes true integer predicates as 0xffffffff.  The
        // pre-ABI-18 idiom `int(predicate) * -1` therefore produces the
        // one-bit value 1 instead of an all-bits selection mask.  Reject both
        // formatted and compacted spellings even if stale source is paired
        // with otherwise plausible metadata.
        program.source.find(") * -1") != std::string::npos ||
        program.source.find(")*-1") != std::string::npos ||
        program.source.find("VITASX2_GPU_VU_DIRECT_PRECOMPUTE") !=
            std::string::npos;
    const bool scratch_mask =
        std::any_of(program.structured_scratch_read_mask.begin(),
                    program.structured_scratch_read_mask.end(),
                    [](u32 word) { return word != 0u; }) ||
        std::any_of(program.structured_scratch_write_mask.begin(),
                    program.structured_scratch_write_mask.end(),
                    [](u32 word) { return word != 0u; });
    const bool forbidden_metadata =
        program.UsesStructuredSnapshotInputBuffers() ||
        program.uses_structured_state_snapshots ||
        program.structured_state_control_owner ||
        program.structured_final_control_owner ||
        program.uses_structured_memory_preflight ||
        program.uses_structured_expression_scratch ||
        program.uses_generated_direct_precompute ||
        program.uses_structured_fixed_qp_numeric ||
        program.uses_structured_fixed_fmac_numeric ||
        program.uses_structured_parallel_child_memory_store ||
        program.uses_structured_parallel_direct_vu_tfx ||
        program.uses_structured_store_commit ||
        program.uses_structured_final_state ||
        program.uses_final_state_output ||
        program.structured_scratch_output_count != 0u || scratch_mask;
    const u64 nested_invocations =
        static_cast<u64>(program.nested_outer_iterations) *
        program.nested_child_iterations;
    const bool nested_grid_contract =
        program.uses_nested_iteration_grid &&
        program.uses_nested_batch_index_inputs &&
        program.nested_outer_iterations != 0u &&
        program.nested_child_iterations != 0u &&
        nested_invocations != 0u && nested_invocations <= (1u << 16u) &&
        !program.uses_flat_instance_inputs &&
        !program.uses_flat_index_inputs &&
        program.uses_buffered_batch_inputs &&
        program.flat_vertices_per_primitive == 0u &&
        program.flat_instance_vertex_step == 0u &&
        program.batch_primitives_per_draw == 0u &&
        !program.flat_strip_winding &&
        (program.source.find("VuInvocation : INDEX") != std::string::npos ||
         program.source.find("VuInvocation:INDEX") != std::string::npos);
    // PCSX2 GSState::VertexKick / GSRendererHW owns last-endpoint flat color.
    // The product emitter keeps geometry and color on independent outer/child
    // indices. Only this two-endpoint line-list layout is admitted here;
    // expanded private roots (55/57) remain compiler fixtures.
    const bool flat_product_grid_contract =
        flat_product_source && program.uses_nested_iteration_grid &&
        program.uses_nested_batch_index_inputs &&
        program.nested_outer_iterations != 0u && program.nested_child_iterations != 0u &&
        nested_invocations >= 2u && nested_invocations <= 32769u &&
        !program.uses_flat_instance_inputs && program.uses_flat_index_inputs &&
        program.uses_buffered_batch_inputs && program.uses_tfx_uniforms &&
        !program.uses_tfx_point_size && program.flat_vertices_per_primitive == 2u &&
        program.flat_instance_vertex_step == 1u &&
        program.batch_primitives_per_draw == nested_invocations - 1u &&
        !program.flat_strip_winding &&
        (program.source.find("VuExpandedVertex : INDEX") != std::string::npos ||
         program.source.find("VuExpandedVertex:INDEX") != std::string::npos) &&
        program.source.find("VuInvocation") == std::string::npos &&
        program.source.find("VuFlatOuterIteration") != std::string::npos &&
        program.source.find("VuFlatChildIteration") != std::string::npos &&
        ContainsSingleCgAssignment(program.source,
            "VuGlobalPrimitive = VuExpandedVertex >> 1u;") &&
        ContainsSingleCgAssignment(program.source,
            "VuLocalInvocation = VuLocalPrimitive + (VuExpandedVertex & 1u);") &&
        ContainsSingleCgAssignment(program.source,
            "VuFlatLocalInvocation = VuLocalPrimitive + 1u;");
    const std::string grid_source_contract =
        "#define VITASX2_GPU_VU_NESTED_GRID " +
        std::to_string(program.uses_nested_iteration_grid ? 1u : 0u) +
        "\n#define VITASX2_GPU_VU_NESTED_BATCH_INDEX " +
        std::to_string(program.uses_nested_batch_index_inputs ? 1u : 0u) +
        "\n#define VITASX2_GPU_VU_NESTED_OUTER " +
        std::to_string(program.nested_outer_iterations) +
        "\n#define VITASX2_GPU_VU_NESTED_CHILD " +
        std::to_string(program.nested_child_iterations) + "\n";
    std::string binding_source_contract = "// GXM binding: coefficients=";
    for (u32 index = 0u; index < program.memory_inputs.size(); index++) {
      if (index != 0u)
        binding_source_contract += ",";
      binding_source_contract += std::to_string(
          program.memory_inputs[index].address.invocation_coefficient);
    }
    binding_source_contract += " outerCoefficients=";
    for (u32 index = 0u; index < program.memory_inputs.size(); index++) {
      if (index != 0u)
        binding_source_contract += ",";
      binding_source_contract += std::to_string(
          program.memory_inputs[index]
              .address.outer_invocation_coefficient);
    }
    binding_source_contract +=
        " flatVertices=" +
        std::to_string(program.flat_vertices_per_primitive) +
        " flatStep=" + std::to_string(program.flat_instance_vertex_step) +
        " flatInstance=" +
        std::to_string(program.uses_flat_instance_inputs ? 1u : 0u) +
        " flatIndex=" +
        std::to_string(program.uses_flat_index_inputs ? 1u : 0u) +
        " batchPrimitives=" +
        std::to_string(program.batch_primitives_per_draw) +
        " bufferedBatch=" +
        std::to_string(program.uses_buffered_batch_inputs ? 1u : 0u) +
        " nestedGrid=" +
        std::to_string(program.uses_nested_iteration_grid ? 1u : 0u) +
        " nestedBatchIndex=" +
        std::to_string(program.uses_nested_batch_index_inputs ? 1u : 0u) +
        " nestedOuter=" +
        std::to_string(program.nested_outer_iterations) +
        " nestedChild=" +
        std::to_string(program.nested_child_iterations) +
        " batchRecordVectors=" +
        std::to_string(program.BatchRecordVectorCount());
    if (program.batch_varying_live_ins.Any()) {
      binding_source_contract +=
          " varyingConstants=" +
          std::to_string(program.batch_varying_live_ins.constant_mask) +
          " varyingVf=" +
          std::to_string(program.batch_varying_live_ins.vf_mask) +
          " varyingAcc=" +
          std::to_string(program.batch_varying_live_ins.acc ? 1u : 0u) +
          " varyingScalars=" +
          std::to_string(program.batch_varying_live_ins.scalars ? 1u : 0u) +
          " varyingVectors=" +
          std::to_string(program.BatchVaryingLiveInVectorCount());
    }
    if (program.uses_dynamic_batch_uniform_index)
      binding_source_contract += " dynamicBatchUniform=1";
    if (program.uses_sink_scheduled_outputs)
      binding_source_contract += " sinkScheduled=1";
    binding_source_contract +=
        " stripWinding=" +
        std::to_string(program.flat_strip_winding ? 1u : 0u) + "\n";
    const bool has_dynamic_batch_uniform_marker =
        program.source.find(
            "#define VITASX2_GPU_VU_DYNAMIC_BATCH_UNIFORM_INDEX 1") !=
        std::string::npos;
    const bool has_dynamic_batch_uniform_symbol =
        program.source.find("VuBatchUniformDraw") != std::string::npos;
    const bool dynamic_batch_uniform_contract =
        !program.uses_dynamic_batch_uniform_index &&
        program.BatchBindingVectorCount() ==
            program.BatchRawBindingVectorCount() &&
        !has_dynamic_batch_uniform_marker &&
        !has_dynamic_batch_uniform_symbol;
    const bool has_instance_batch_identity =
        program.source.find("VuBatchIdentity") != std::string::npos;
    const bool partial_batch_live_ins =
        program.batch_varying_live_ins.Any();
    const bool has_partial_batch_marker =
        program.source.find(
            "#define VITASX2_GPU_VU_PARTIAL_BATCH_LIVE_INS 1") !=
        std::string::npos;
    const std::string inline_varying_declaration =
        "float4 object_values[" +
        std::to_string(program.BatchVaryingLiveInVectorCount()) + "]";
    const bool has_inline_varying_tail =
        program.BatchVaryingLiveInVectorCount() != 0u &&
        program.source.find(inline_varying_declaration) !=
            std::string::npos &&
        program.source.find("VuBatchData[VuBatchDraw].object_values[") !=
            std::string::npos;
    const bool has_buffer4_varying =
        (program.source.find("VuBatchVarying[1] : BUFFER[4]") !=
             std::string::npos ||
         program.source.find("VuBatchVarying[1]:BUFFER[4]") !=
             std::string::npos) &&
        (program.source.find("VuBatchVarying[VuBatchDraw * ") !=
             std::string::npos ||
         program.source.find("VuBatchVarying[VuBatchDraw*") !=
             std::string::npos);
    const bool partial_batch_live_in_contract = partial_batch_live_ins
        ? (has_partial_batch_marker &&
           program.BatchVaryingLiveInVectorCount() != 0u &&
           program.BatchVaryingLiveInVectorCount() <= 4u &&
           (program.loop_kernel_source_abi == GeneratedLoopKernelPartialBatchCgAbiVersion ||
            program.loop_kernel_source_abi == GeneratedLoopKernelPartialBatchProductCgAbiVersion ||
            program.loop_kernel_source_abi == GeneratedLoopKernelPartialStateCanaryCgAbiVersion ||
            program.loop_kernel_source_abi == GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion) &&
           !has_buffer4_varying &&
           program.source.find("VuBatchVarying") == std::string::npos &&
           has_inline_varying_tail && program.UsesInlineBatchVaryingTail())
        : ((program.loop_kernel_source_abi ==
                GeneratedLoopKernelCgAbiVersion ||
            program.loop_kernel_source_abi ==
                GeneratedLoopKernelProductCgAbiVersion ||
            program.loop_kernel_source_abi == GeneratedLoopKernelStateCanaryCgAbiVersion ||
            program.loop_kernel_source_abi == GeneratedLoopKernelNestedFlatProductCgAbiVersion) &&
           !has_partial_batch_marker &&
           program.source.find("VuBatchVarying") == std::string::npos &&
           program.source.find(".object_values[") == std::string::npos &&
           program.BatchVaryingLiveInVectorCount() == 0u);
    const bool instance_batch_live_in_contract =
        !program.uses_instance_indexed_batch_live_ins &&
        !has_instance_batch_identity &&
        program.source.find("VuGlobalInvocation") == std::string::npos &&
        program.source.find("VITASX2_GPU_VU_INSTANCE_BATCH_LIVE_INS") ==
            std::string::npos &&
        program.BatchInstanceLiveInVectorCount() == 0u &&
        program.BatchRecordVectorCount() ==
            program.BatchBindingVectorCount() +
                program.BatchInvariantUniformVectorCount() +
                (program.UsesInlineBatchVaryingTail()
                     ? program.BatchVaryingLiveInVectorCount()
                     : 0u);
    const std::string invariant_uniform_declaration =
        "float4 uniforms[" +
        std::to_string(program.BatchInvariantUniformVectorCount()) + "]";
    const bool exact_batch_record_source_layout =
        (program.BatchInvariantUniformVectorCount() == 0u
            ? program.source.find("float4 uniforms[") == std::string::npos
            : program.source.find(invariant_uniform_declaration) !=
                  std::string::npos) &&
        (program.UsesInlineBatchVaryingTail()
             ? has_inline_varying_tail
             : program.source.find("float4 object_values[") ==
                   std::string::npos);
    const bool nested_batch_index_contract =
        program.uses_nested_iteration_grid
            ? (program.uses_nested_batch_index_inputs &&
               program.source.find(
                   "#define VITASX2_GPU_VU_NESTED_BATCH_INDEX 1") !=
                   std::string::npos &&
               program.source.find("VuBatchDraw") != std::string::npos &&
               program.source.find("VuLocalInvocation") !=
                   std::string::npos &&
               program.source.find(
                   "VuBatchData[VuBatchDraw].bindings[") !=
                   std::string::npos &&
               program.source.find(
                   "VuBatchData[VuBatchDraw].uniforms[") ==
                   std::string::npos &&
               (program.BatchInvariantUniformVectorCount() == 0u ||
                program.source.find("VuBatchData[0].uniforms[") !=
                    std::string::npos))
            : (!program.uses_nested_batch_index_inputs &&
               program.source.find(
                   "#define VITASX2_GPU_VU_NESTED_BATCH_INDEX 0") !=
                   std::string::npos);
    const bool has_sink_scheduled_marker =
        program.source.find(
            "#define VITASX2_GPU_VU_SINK_SCHEDULED_OUTPUTS 1") !=
        std::string::npos;
    const bool has_sink_scheduled_symbol =
        program.source.find("VuSinkPositionX") != std::string::npos;
    const bool has_lazy_batch_uniform_marker =
        program.source.find(
            "#define VITASX2_GPU_VU_LAZY_BATCH_UNIFORMS 1") !=
        std::string::npos;
    bool has_lazy_batch_uniform_aliases = true;
    for (const CgConstantInput& input : program.constant_inputs) {
      has_lazy_batch_uniform_aliases &=
          program.source.find("#define VuConstant" +
                              std::to_string(input.uniform_index) + " (") !=
          std::string::npos;
    }
    for (u32 reg = 1u; reg < 32u; reg++) {
      if ((program.vf_uniform_mask & (1u << reg)) == 0u)
        continue;
      has_lazy_batch_uniform_aliases &=
          program.source.find(
              "#define VF" +
              (reg < 10u ? std::string("0") : std::string()) +
              std::to_string(reg) + " (") != std::string::npos;
    }
    if (program.uses_acc_uniform) {
      has_lazy_batch_uniform_aliases &=
          program.source.find("#define ACC (") != std::string::npos;
    }
    if (program.uses_q_uniform || program.uses_p_uniform ||
        program.uses_i_uniform || program.uses_gif_q_uniform) {
      has_lazy_batch_uniform_aliases &=
          program.source.find("#define VuBatchScalars (") !=
          std::string::npos;
    }
    const bool has_eager_batch_uniform_alias =
        program.source.find("const float4 VuConstant") != std::string::npos ||
        program.source.find("const float4 VF") != std::string::npos ||
        program.source.find("const float4 ACC =") != std::string::npos ||
        program.source.find("const float4 VuBatchScalars =") !=
            std::string::npos;
    const bool partial_product_source =
        program.loop_kernel_source_abi ==
            GeneratedLoopKernelPartialBatchProductCgAbiVersion ||
        program.loop_kernel_source_abi == GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion;
    const bool has_lazy_varying_constant_marker =
        program.source.find(
            "#define VITASX2_GPU_VU_LAZY_VARYING_CONSTANTS 1") !=
        std::string::npos;
    const bool uses_lazy_varying_constants =
        program.UsesLazySinkScheduledVaryingConstants();
    bool lazy_varying_constant_contract =
        has_lazy_varying_constant_marker == uses_lazy_varying_constants;
    bool partial_product_eager_alias_contract = true;
    if (partial_product_source) {
      u32 varying_constant_slot = 0u;
      u32 invariant_constant_slot = 0u;
      for (u32 input_index = 0u;
           input_index < program.constant_inputs.size(); input_index++) {
        const CgConstantInput& input = program.constant_inputs[input_index];
        const std::string alias =
            "VuConstant" + std::to_string(input.uniform_index);
        const bool varying =
            input_index < 64u &&
            (program.batch_varying_live_ins.constant_mask &
             (1ull << input_index)) != 0u;
        if (varying) {
          const std::string declaration =
              "\tconst float4 " + alias +
              " = VuBatchData[VuBatchDraw].object_values[" +
              std::to_string(varying_constant_slot++) + "u];\n";
          const std::string compact_declaration =
              "const float4 " + alias +
              "=VuBatchData[VuBatchDraw].object_values[" +
              std::to_string(varying_constant_slot - 1u) + "u];";
          size_t declaration_offset = program.source.find(declaration);
          size_t declaration_size = declaration.size();
          if (declaration_offset == std::string::npos) {
            declaration_offset = program.source.find(compact_declaration);
            declaration_size = compact_declaration.size();
          }
          const size_t alias_offset = declaration_offset == std::string::npos
              ? std::string::npos
              : FindCgIdentifier(program.source, alias, declaration_offset);
          const size_t first_use = alias_offset == std::string::npos
              ? std::string::npos
              : FindCgIdentifier(
                    program.source, alias, alias_offset + alias.size());
          const size_t next_statement_end = declaration_offset ==
                                                     std::string::npos
              ? std::string::npos
              : program.source.find(';', declaration_offset +
                                             declaration_size);
          lazy_varying_constant_contract &=
              uses_lazy_varying_constants &&
              declaration_offset != std::string::npos &&
              FindCgFloat4Declaration(
                  program.source, alias,
                  declaration_offset + declaration_size) ==
                  std::string::npos &&
              FindCgIdentifier(program.source, alias) == alias_offset &&
              first_use != std::string::npos &&
              next_statement_end != std::string::npos &&
              first_use < next_statement_end &&
              program.source.find("#define " + alias + " (") ==
                  std::string::npos;
        } else {
          const std::string declaration =
              "\tconst float4 " + alias +
              " = VuBatchData[0].uniforms[" +
              std::to_string(invariant_constant_slot++) + "];\n";
          const std::string compact_declaration =
              "const float4 " + alias + "=VuBatchData[0].uniforms[" +
              std::to_string(invariant_constant_slot - 1u) + "];";
          size_t declaration_offset = program.source.find(declaration);
          size_t declaration_size = declaration.size();
          if (declaration_offset == std::string::npos) {
            declaration_offset = program.source.find(compact_declaration);
            declaration_size = compact_declaration.size();
          }
          partial_product_eager_alias_contract &=
              declaration_offset != std::string::npos &&
              FindCgFloat4Declaration(
                  program.source, alias,
                  declaration_offset + declaration_size) ==
                  std::string::npos &&
              program.source.find("#define " + alias + " (") ==
                  std::string::npos;
        }
      }
      const auto validate_eager_record =
          [&](const std::string& alias, bool varying) {
            std::string initializer;
            if (varying) {
              initializer =
                  "VuBatchData[VuBatchDraw].object_values[" +
                  std::to_string(varying_constant_slot++) + "u]";
            } else {
              initializer = "VuBatchData[0].uniforms[" +
                  std::to_string(invariant_constant_slot++) + "]";
            }
            partial_product_eager_alias_contract &=
                ContainsExactCgFloat4Declaration(
                    program.source, alias, initializer) &&
                program.source.find("#define " + alias + " (") ==
                    std::string::npos;
          };
      for (u32 reg = 1u; reg < 32u; reg++) {
        if ((program.vf_uniform_mask & (1u << reg)) == 0u)
          continue;
        const std::string alias =
            "VF" + (reg < 10u ? std::string("0") : std::string()) +
            std::to_string(reg);
        validate_eager_record(
            alias, (program.batch_varying_live_ins.vf_mask &
                    (1u << reg)) != 0u);
      }
      if (program.uses_acc_uniform)
        validate_eager_record("ACC", program.batch_varying_live_ins.acc);
      if (program.uses_q_uniform || program.uses_p_uniform ||
          program.uses_i_uniform || program.uses_gif_q_uniform) {
        validate_eager_record(
            "VuBatchScalars", program.batch_varying_live_ins.scalars);
      }
    } else {
      lazy_varying_constant_contract &=
          !has_lazy_varying_constant_marker &&
          !uses_lazy_varying_constants;
    }
    const bool requires_exact_partial_sink_schedule =
        program.RequiresExactPartialSinkSchedule();
    const bool uses_lazy_sink_uniforms =
        program.UsesLazySinkScheduledBatchUniforms();
    const bool sink_scheduled_contract =
        program.uses_sink_scheduled_outputs
            ? (program.uses_nested_iteration_grid &&
               (state_canary_source || product_source || requires_exact_partial_sink_schedule ||
                program.BatchUniformVectorCount() > GeneratedCgProgram::
                    SinkScheduledOutputUniformThreshold) &&
               !program.uses_loop_kernel_ftoi_probe_output &&
               has_sink_scheduled_marker && (state_canary_source ? !has_sink_scheduled_symbol
                                                               : has_sink_scheduled_symbol) &&
               (uses_lazy_sink_uniforms
                    ? (has_lazy_batch_uniform_marker &&
                       has_lazy_batch_uniform_aliases &&
                       !has_eager_batch_uniform_alias)
                    : !has_lazy_batch_uniform_marker))
            : (!has_sink_scheduled_marker && !has_sink_scheduled_symbol &&
               !has_lazy_batch_uniform_marker &&
               !requires_exact_partial_sink_schedule && !product_source);
    const bool flat_index_contract = flat_product_source ? flat_product_grid_contract :
        (nested_grid_contract || (!program.uses_nested_iteration_grid &&
         program.nested_outer_iterations == 0u &&
         program.nested_child_iterations == 0u &&
        (program.flat_vertices_per_primitive == 0u
            ? (!program.uses_flat_instance_inputs &&
               !program.uses_flat_index_inputs &&
               !program.uses_buffered_batch_inputs &&
               program.flat_instance_vertex_step == 0u &&
               program.batch_primitives_per_draw == 0u &&
               !program.flat_strip_winding)
            : (!program.uses_flat_instance_inputs &&
               program.uses_flat_index_inputs &&
               program.uses_buffered_batch_inputs &&
               (program.flat_vertices_per_primitive == 2u ||
                program.flat_vertices_per_primitive == 3u) &&
               program.flat_instance_vertex_step != 0u &&
               program.batch_primitives_per_draw != 0u &&
               (!program.flat_strip_winding ||
                program.flat_vertices_per_primitive == 3u) &&
               (program.source.find("VuExpandedVertex : INDEX") !=
                    std::string::npos ||
                program.source.find("VuExpandedVertex:INDEX") !=
                    std::string::npos)))));
    return GeneratedLoopKernelCgAbiVersion == 26u &&
           GeneratedLoopKernelPartialBatchCgAbiVersion == 51u &&
           GeneratedLoopKernelProductCgAbiVersion == 52u &&
           GeneratedLoopKernelPartialBatchProductCgAbiVersion == 54u &&
           GeneratedLoopKernelNestedFlatProductCgAbiVersion == 56u &&
           GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion == 58u &&
           GeneratedLoopKernelStateCanaryCgAbiVersion == 59u &&
           GeneratedLoopKernelPartialStateCanaryCgAbiVersion == 60u &&
           has_abi_marker &&
           has_static_flow_marker && !dynamic_source_flow &&
           attested_no_write_product_contract &&
           private_store_source_contract && ftoi_probe_source_contract &&
           dynamic_batch_uniform_contract &&
           instance_batch_live_in_contract && exact_batch_record_source_layout &&
           partial_batch_live_in_contract && nested_batch_index_contract &&
           sink_scheduled_contract && lazy_varying_constant_contract &&
           partial_product_eager_alias_contract &&
           program.uses_loop_kernel_private_store_output == !product_source &&
           (product_source
                ? program.loop_kernel_private_store_count == 0u
                : program.loop_kernel_private_store_count != 0u) &&
           program.loop_kernel_private_store_count <= 8u &&
           !forbidden_source && !forbidden_metadata &&
           flat_index_contract &&
           program.source.find(grid_source_contract) != std::string::npos &&
           program.source.find(binding_source_contract) !=
               std::string::npos &&
           program.control_strategy == GeneratedCgControlStrategy::ParallelLoop &&
           !program.requires_dynamic_entry_state &&
           program.semantic_pair_count != 0u &&
           program.generated_source_bytes == program.source.size() &&
           program.source.size() <=
               RuntimeGeneratedLoopKernelCompilerSourceBytes &&
           program.emitted_expression_count != 0u &&
           program.emitted_expression_count <=
               RuntimeGeneratedLoopKernelCompilerExpressionCount;
  }

  if (program.execution_kind ==
      GeneratedCgExecutionKind::UniversalDirectStateMachine) {
    const std::string dynamic_pair_marker =
        "// VitaSX2 generated maximum dynamic pairs " +
        std::to_string(program.maximum_dynamic_pairs_per_invocation);
    const bool lexical_control = program.control_strategy ==
        GeneratedCgControlStrategy::StructuredLoop;
    const char* const dynamic_pair_loop_counter =
        program.control_strategy ==
                GeneratedCgControlStrategy::BlockThreadedStateMachine
            ? "blockStep"
            : "pairStep";
    const std::string dynamic_pair_loop = lexical_control
        ? "// VitaSX2 generated lexical structured control"
        : std::string(dynamic_pair_loop_counter) + " = 0u; " +
              dynamic_pair_loop_counter + " < " +
              std::to_string(program.maximum_dynamic_pairs_per_invocation) +
              "u";
    const std::string compact_dynamic_pair_loop = lexical_control
        ? dynamic_pair_loop
        : std::string(dynamic_pair_loop_counter) + "=0u;" +
              dynamic_pair_loop_counter + "<" +
              std::to_string(program.maximum_dynamic_pairs_per_invocation) +
              "u";
    const std::string dynamic_pair_guard =
        "pairLimit > " +
        std::to_string(program.maximum_dynamic_pairs_per_invocation) + "u";
    const std::string compact_dynamic_pair_guard =
        "pairLimit>" +
        std::to_string(program.maximum_dynamic_pairs_per_invocation) + "u";
    const bool dynamic_pair_zero_guard =
        program.source.find("pairLimit == 0u") != std::string::npos ||
        program.source.find("pairLimit==0u") != std::string::npos;
    const std::string execute_entry_marker =
        "// VitaSX2 generated consumes Execute entry " +
        std::to_string(program.consumes_execute_entry ? 1u : 0u);
    const bool source_has_transaction_gate =
        (program.source.find(
             "const bool VuTransactionModuleActive =") !=
             std::string::npos ||
         program.source.find(
             "const bool VuTransactionModuleActive=") !=
             std::string::npos) &&
        (program.source.find("if (VuTransactionModuleActive)") !=
             std::string::npos ||
         program.source.find("if(VuTransactionModuleActive)") !=
             std::string::npos);
    const bool source_has_transaction_continuation = lexical_control
        ? program.source.find(
              "// VitaSX2 generated lexical transactional continuation") !=
              std::string::npos
        : (program.source.find("if (!matchedBlock) stopReason = 13u") !=
               std::string::npos ||
           program.source.find("if(!matchedBlock)stopReason=13u") !=
               std::string::npos);
    const bool transaction_gate_contract =
        source_has_transaction_gate ==
            program.uses_transactional_entry_gate &&
        source_has_transaction_continuation ==
            program.uses_transactional_entry_gate &&
        (!program.uses_transactional_entry_gate ||
         ((program.control_strategy ==
               GeneratedCgControlStrategy::BlockThreadedStateMachine ||
           program.control_strategy ==
               GeneratedCgControlStrategy::StructuredLoop) &&
          program.control_basic_block_count != 0u));
    const bool source_has_execute_entry_parse =
        program.source.find("const bool cleanEntryState") !=
            std::string::npos &&
        program.source.find("const bool cleanEnd") != std::string::npos &&
        program.source.find("const unsigned int commandBase") !=
            std::string::npos;
    const bool execute_entry_contract =
        source_has_execute_entry_parse == program.consumes_execute_entry;
    return program.source.size() <=
               RuntimeGeneratedDirectCompilerSourceBytes &&
           program.generated_source_bytes == program.source.size() &&
           program.emitted_expression_count <=
               RuntimeGeneratedDirectCompilerExpressionCount &&
           program.maximum_dynamic_pairs_per_invocation != 0u &&
           program.maximum_dynamic_pairs_per_invocation <=
               RuntimeGeneratedDirectMaximumDynamicPairs &&
           program.source.find(dynamic_pair_marker) != std::string::npos &&
           (program.source.find(dynamic_pair_loop) != std::string::npos ||
            program.source.find(compact_dynamic_pair_loop) !=
                std::string::npos) &&
           (program.source.find(dynamic_pair_guard) != std::string::npos ||
            program.source.find(compact_dynamic_pair_guard) !=
                std::string::npos) &&
           dynamic_pair_zero_guard &&
           program.source.find(execute_entry_marker) != std::string::npos &&
           transaction_gate_contract && execute_entry_contract;
  }

  if (program.execution_kind ==
      GeneratedCgExecutionKind::StructuredMemoryPreflight) {
    const StructuredMemoryPreflightData& preflight =
        program.structured_memory_preflight;
    const bool metadata_valid =
        program.uses_structured_memory_preflight &&
        preflight.header0[0] == StructuredMemoryPreflightFormatVersion &&
        preflight.header0[1] <= 1u &&
        preflight.header0[2] <= StructuredMemoryPreflightMaximumAccesses &&
        preflight.header0[3] + preflight.header1[0] ==
            preflight.header0[2] &&
        preflight.header1[1] != 0u && preflight.header1[1] < 16u &&
        preflight.header1[2] < 16u &&
        (preflight.header1[3] == 1u ||
         preflight.header1[3] == 0xffffffffu) &&
        preflight.header2[0] != 0u && preflight.header2[0] <= 64u &&
        preflight.header2[1] != 0u && preflight.header2[1] <= 1024u &&
        (preflight.header2[1] & (preflight.header2[1] - 1u)) == 0u &&
        preflight.header2[2] == 0u && preflight.header2[3] == 0u;
    return metadata_valid &&
        program.source.size() <=
            RuntimeStructuredPreflightCompilerSourceBytes &&
        program.generated_source_bytes == program.source.size() &&
        program.source.find(
            "uniform unsigned int4 VuPreflightMetadata[67] : BUFFER10") !=
            std::string::npos &&
        program.source.find(
            "uniform unsigned int VuCrossOuterAccesses[1024] : BUFFER9") !=
            std::string::npos &&
        program.source.find("#pragma readwrite_buffer BUFFER9") !=
            std::string::npos;
  }

  // These are dispatch-based compatibility interpreters, not runtime-JIT
  // program shapes. Keep them available to offline/host validation without
  // ever exposing their large dynamic-dispatch roots to in-process ShaccCg.
  if (program.execution_kind ==
          GeneratedCgExecutionKind::UniversalStateMachine ||
      program.execution_kind ==
          GeneratedCgExecutionKind::UniversalCompactContinuation) {
    return false;
  }

  // Source/resource consistency is part of compiler safety, not merely a
  // source-size heuristic.  Every title-neutral partition which reads or
  // writes transactional expression scratch must expose BUFFER10 in its Cg
  // signature before it can be queued to Shacc.  psp2cgc rejects an
  // undeclared VuScratchWords reference, and Shacc must never be asked to
  // diagnose malformed generated source in-process.
  if (program.source.find("VuScratchWords[") != std::string::npos &&
      program.source.find(
          "uniform int VuScratchWords[1] : BUFFER10") ==
          std::string::npos &&
      program.source.find(
          "uniform int VuScratchWords[1]:BUFFER10") ==
          std::string::npos) {
    return false;
  }
  const bool generated_direct_precompute_marker =
      program.source.find(
          "#define VITASX2_GPU_VU_DIRECT_PRECOMPUTE 1") !=
      std::string::npos;
  if (program.uses_generated_direct_precompute !=
          generated_direct_precompute_marker ||
      (program.uses_generated_direct_precompute &&
       program.execution_kind !=
           GeneratedCgExecutionKind::StructuredExpressionScratch)) {
    return false;
  }
  if (program.execution_kind ==
      GeneratedCgExecutionKind::StructuredStateSnapshots) {
    u32 snapshot_vectors = 0;
    for (u32 reg = 1; reg < program.child_entry_vf_lanes.size(); reg++)
      snapshot_vectors += program.child_entry_vf_lanes[reg] != 0 ? 1u : 0u;
    snapshot_vectors += program.child_entry_acc_lanes != 0 ? 1u : 0u;
    snapshot_vectors +=
        (program.child_entry_q || program.child_entry_p ||
         program.child_entry_i)
            ? 1u
            : 0u;
    // Runtime source compaction removes spaces around assignments. Attest the
    // semantic publication markers in either readable or compact lexical
    // form; source formatting must not change shared-control ownership.
    const bool publishes_outer_state =
        program.source.find("VuOuterState[0] = VuOuterExecuted") !=
            std::string::npos ||
        program.source.find("VuOuterState[0]=VuOuterExecuted") !=
            std::string::npos;
    const bool publishes_vi_snapshots =
        program.source.find("VuOuterViSnapshots[VuOuterIteration") !=
        std::string::npos;
    const bool control_contract = program.structured_state_control_owner
        ? publishes_outer_state && publishes_vi_snapshots
        : !publishes_outer_state && !publishes_vi_snapshots;
    return (snapshot_vectors != 0u ||
            program.structured_state_control_owner) &&
           snapshot_vectors <=
               RuntimeStructuredStateMaximumSnapshotVectors &&
           program.source.size() <=
               RuntimeStructuredStateCompilerSourceBytes &&
           program.emitted_expression_count <=
               RuntimeStructuredStateCompilerExpressionCount &&
           program.emitted_software_f32_operation_count <=
               RuntimeStructuredStateSoftwareF32OperationCount &&
           !program.uses_software_f32_resource_helpers && control_contract;
  }
  const bool structured_parallel =
      program.execution_kind ==
          GeneratedCgExecutionKind::StructuredExpressionScratch ||
      program.execution_kind ==
          GeneratedCgExecutionKind::StructuredParallelChildMemoryStore ||
      program.execution_kind ==
          GeneratedCgExecutionKind::StructuredParallelDirectVuTfx ||
      program.execution_kind == GeneratedCgExecutionKind::StructuredFinalState;
  const bool structured_child_grid =
      program.execution_kind ==
          GeneratedCgExecutionKind::StructuredExpressionScratch ||
      program.execution_kind ==
          GeneratedCgExecutionKind::StructuredParallelChildMemoryStore ||
      program.execution_kind ==
          GeneratedCgExecutionKind::StructuredParallelDirectVuTfx;
  const u64 structured_invocation_count =
      static_cast<u64>(program.maximum_structured_iterations) *
      program.maximum_structured_child_iterations;
  const bool structured_grid_bounds = !structured_parallel ||
      (program.maximum_structured_iterations != 0u &&
       program.maximum_structured_iterations <= 64u &&
       program.maximum_structured_child_iterations != 0u &&
       program.maximum_structured_child_iterations <= 64u &&
       structured_invocation_count <=
           StructuredGeneratedMaximumScratchInvocations);
  // These declarations are the generated-source half of the persistent slot
  // ABI in GSDeviceGXM.cpp. Sony permits dynamic user-buffer indexing beyond
  // the declared Cg array when the application registers the complete mapped
  // physical range, so BUFFER10 intentionally remains [1]; all statically
  // sized architectural banks must still match their host allocations.
  const bool structured_buffer_contract = !structured_parallel ||
      ((program.source.find(
            "uniform int4 VuRawQwords[1024] : BUFFER3") !=
            std::string::npos ||
        program.source.find(
            "uniform int4 VuRawQwords[1024]:BUFFER3") !=
            std::string::npos) &&
       (program.source.find(
            "uniform float4 VuOuterSnapshots[2304] : BUFFER11") !=
            std::string::npos ||
        program.source.find(
            "uniform float4 VuOuterSnapshots[2304]:BUFFER11") !=
            std::string::npos) &&
       (program.source.find(
            "uniform unsigned int VuOuterState[16] : BUFFER12") !=
            std::string::npos ||
        program.source.find(
            "uniform unsigned int VuOuterState[16]:BUFFER12") !=
            std::string::npos) &&
       (program.source.find(
            "uniform unsigned int4 VuOuterViSnapshots[256] : BUFFER13") !=
            std::string::npos ||
        program.source.find(
            "uniform unsigned int4 VuOuterViSnapshots[256]:BUFFER13") !=
            std::string::npos) &&
       (program.execution_kind !=
            GeneratedCgExecutionKind::StructuredExpressionScratch ||
        program.source.find(
            "uniform int VuScratchWords[1] : BUFFER10") !=
            std::string::npos ||
        program.source.find(
            "uniform int VuScratchWords[1]:BUFFER10") !=
            std::string::npos) &&
       (program.execution_kind != GeneratedCgExecutionKind::
            StructuredParallelChildMemoryStore ||
        program.source.find(
            "uniform int VuOutputWords[4096] : BUFFER8") !=
            std::string::npos ||
        program.source.find(
            "uniform int VuOutputWords[4096]:BUFFER8") !=
            std::string::npos));
  const bool structured_power_of_two_grid =
      (program.source.find(
           "const unsigned int VuChildIteration = VuInvocation &") !=
           std::string::npos ||
       program.source.find(
           "const unsigned int VuChildIteration=VuInvocation&") !=
           std::string::npos) &&
      (program.source.find(
           "const unsigned int VuOuterIteration = VuInvocation >>") !=
           std::string::npos ||
       program.source.find(
           "const unsigned int VuOuterIteration=VuInvocation>>") !=
           std::string::npos);
  const bool structured_dense_grid =
      (program.source.find(
           "const unsigned int VuOuterIteration = (VuInvocation *") !=
           std::string::npos ||
       program.source.find(
           "const unsigned int VuOuterIteration=(VuInvocation*") !=
           std::string::npos) &&
      (program.source.find(
           "const unsigned int VuChildIteration = VuInvocation - "
           "VuOuterIteration *") != std::string::npos ||
       program.source.find(
           "const unsigned int VuChildIteration=VuInvocation-"
           "VuOuterIteration*") != std::string::npos);
  const bool structured_child_grid_shape = !structured_child_grid ||
      ((structured_power_of_two_grid || structured_dense_grid) &&
       program.source.find("for (unsigned int VuChildIteration") ==
           std::string::npos);
  const bool structured_store_shape =
      program.execution_kind !=
          GeneratedCgExecutionKind::StructuredParallelChildMemoryStore ||
      program.structured_store_count <=
          RuntimeStructuredStoreMaximumQwordWrites;
  const bool structured_scratch_shape =
      program.execution_kind !=
          GeneratedCgExecutionKind::StructuredExpressionScratch ||
      (program.structured_scratch_output_count != 0u &&
       program.structured_scratch_output_count <=
           RuntimeStructuredScratchMaximumOutputs);
  const bool structured_final_shape =
      program.execution_kind != GeneratedCgExecutionKind::StructuredFinalState ||
      (StructuredFinalOutputVectorCount(program) <=
           RuntimeStructuredFinalMaximumOutputVectors &&
       program.structured_final_control_owner ==
           (program.source.find("#pragma readwrite_buffer BUFFER9") !=
            std::string::npos) &&
       (program.structured_final_control_owner ||
        (program.final_acc_lanes == 0u && !program.final_q &&
         !program.final_p && !program.final_i)));
  const bool structured_direct =
      program.execution_kind ==
      GeneratedCgExecutionKind::StructuredParallelDirectVuTfx;
  const bool structured_direct_precompute =
      program.uses_generated_direct_precompute;
  const bool native_f32 = program.uses_structured_native_f32;
  const bool exact_software_f32 =
      program.emitted_software_f32_operation_count != 0u;
  const size_t compiler_source_limit =
      structured_direct_precompute
      ? RuntimeStructuredDirectPrecomputeCompilerSourceBytes
      : structured_direct ? RuntimeStructuredDirectCompilerSourceBytes
      : native_f32 ? RuntimeStructuredNativeCompilerSourceBytes
      : exact_software_f32 ? RuntimeStructuredExactCompilerSourceBytes
                           : RuntimeStructuredParallelCompilerSourceBytes;
  const u32 compiler_expression_limit = structured_direct_precompute
      ? RuntimeStructuredDirectPrecomputeCompilerExpressionCount
      : structured_direct ? RuntimeStructuredDirectCompilerExpressionCount
      : native_f32 ? RuntimeStructuredNativeCompilerExpressionCount
      : exact_software_f32 ? RuntimeStructuredExactCompilerExpressionCount
                           : RuntimeStructuredParallelCompilerExpressionCount;
  return !structured_parallel ||
         (program.source.size() <= compiler_source_limit &&
          program.emitted_expression_count <= compiler_expression_limit &&
          program.emitted_software_f32_operation_count <=
              RuntimeStructuredSoftwareF32OperationCount &&
          !program.uses_software_f32_resource_helpers &&
          structured_grid_bounds && structured_buffer_contract &&
          structured_child_grid_shape &&
          structured_store_shape && structured_scratch_shape &&
          structured_final_shape);
}

ShaderKey MakeGeneratedProgramKey(const GeneratedCgProgram &program) {
  return program.source.empty() ? ShaderKey{} : HashSource(program.source);
}

bool AttachGeneratedProgramCompiler(ShaderCompiler *compiler) {
  if (!compiler)
    return false;
  std::lock_guard lock(s_registry_mutex);
  if (s_compiler && s_compiler != compiler)
    return false;
  s_compiler = compiler;
  return true;
}

void DetachGeneratedProgramCompiler(ShaderCompiler *compiler) {
  std::lock_guard lock(s_registry_mutex);
  if (s_compiler == compiler)
    s_compiler = nullptr;
}

bool RequestGeneratedProgram(GeneratedCgProgram program, ShaderKey *key) {
  if (!key || program.source.empty())
    return false;
  s_requests.fetch_add(1, std::memory_order_relaxed);
  *key = MakeGeneratedProgramKey(program);
  if (key->low == 0 && key->high == 0)
    return false;

  // Source is the persistent cache identity, but metadata is the admission
  // contract for that source. Re-attest it before a cache hit can coalesce the
  // request: stale or corrupted dynamic bounds must not borrow a previously
  // registered GXP merely because their source bytes happen to match.
  const bool compiler_shape_attested =
      IsGeneratedProgramRuntimeCompilerShapeAttested(program);

  // Keep compiler ownership stable through the non-blocking Submit(). Detach()
  // uses the same mutex, so shutdown cannot invalidate the borrowed pointer
  // between lookup and queue insertion.
  std::lock_guard lock(s_registry_mutex);
  ShaderCompiler *const compiler = s_compiler;
  if (!compiler) {
    s_unavailable_requests.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const auto existing = s_registry.find(*key);
  if (existing != s_registry.end()) {
    if (!compiler_shape_attested) {
      s_compiler_safety_rejections.fetch_add(1,
                                             std::memory_order_relaxed);
      return false;
    }
    s_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return existing->second.state != GeneratedProgramState::Failed;
  }

  if (!compiler_shape_attested) {
    RegistryEntry entry;
    entry.state = GeneratedProgramState::Failed;
    entry.metadata = CopyGeneratedProgramMetadataWithoutSource(&program);
    s_registry.emplace(*key, std::move(entry));
    s_cache_misses.fetch_add(1, std::memory_order_relaxed);
    s_failed_programs.fetch_add(1, std::memory_order_relaxed);
    s_compiler_safety_rejections.fetch_add(1,
                                           std::memory_order_relaxed);
    const bool direct_state_machine =
        program.execution_kind ==
        GeneratedCgExecutionKind::UniversalDirectStateMachine;
    const bool loop_kernel =
        program.execution_kind ==
        GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx;
    const bool structured_direct =
        program.execution_kind ==
        GeneratedCgExecutionKind::StructuredParallelDirectVuTfx;
    const bool native_f32 = program.uses_structured_native_f32;
    const bool exact_software_f32 =
        program.emitted_software_f32_operation_count != 0u;
    const size_t compiler_source_limit = loop_kernel
        ? RuntimeGeneratedLoopKernelCompilerSourceBytes
        : direct_state_machine
        ? RuntimeGeneratedDirectCompilerSourceBytes
        : structured_direct ? RuntimeStructuredDirectCompilerSourceBytes
        : native_f32 ? RuntimeStructuredNativeCompilerSourceBytes
        : exact_software_f32 ? RuntimeStructuredExactCompilerSourceBytes
                             : RuntimeStructuredParallelCompilerSourceBytes;
    const u32 compiler_expression_limit = loop_kernel
        ? RuntimeGeneratedLoopKernelCompilerExpressionCount
        : direct_state_machine
        ? RuntimeGeneratedDirectCompilerExpressionCount
        : structured_direct ? RuntimeStructuredDirectCompilerExpressionCount
        : native_f32 ? RuntimeStructuredNativeCompilerExpressionCount
        : exact_software_f32 ? RuntimeStructuredExactCompilerExpressionCount
                             : RuntimeStructuredParallelCompilerExpressionCount;
    Console.Warning(
        "GPU-VU: generated compiler-safety rejection kind=%s "
        "key=%016llx%016llx source_bytes=%u/%u expressions=%u/%u "
        "software_f32=%u/%u native_f32=%u resource_helpers=%u "
        "scratch_outputs=%u/%u "
        "stores=%u/%u final_vectors=%u/%u; pre-effect CPU MTVU remains "
        "authoritative.",
        GeneratedExecutionKindName(program.execution_kind),
        static_cast<unsigned long long>(key->high),
        static_cast<unsigned long long>(key->low),
        static_cast<u32>(program.source.size()),
        static_cast<u32>(compiler_source_limit),
        program.emitted_expression_count,
        compiler_expression_limit,
        program.emitted_software_f32_operation_count,
        RuntimeStructuredSoftwareF32OperationCount,
        static_cast<u32>(program.uses_structured_native_f32),
        static_cast<u32>(program.uses_software_f32_resource_helpers),
        program.structured_scratch_output_count,
        RuntimeStructuredScratchMaximumOutputs,
        program.structured_store_count,
        RuntimeStructuredStoreMaximumQwordWrites,
        StructuredFinalOutputVectorCount(program),
        RuntimeStructuredFinalMaximumOutputVectors);
    return true;
  }

  RegistryEntry entry;
  entry.state = GeneratedProgramState::Queued;
  entry.metadata = CopyGeneratedProgramMetadataWithoutSource(&program);
  s_registry.emplace(*key, std::move(entry));
  s_cache_misses.fetch_add(1, std::memory_order_relaxed);

  // This line is deliberately emitted before handing source ownership to
  // ShaccCg. A fatal compiler-module fault cannot publish a CompileResult, so
  // the durable log must still identify the structural generated tier which
  // entered the compiler. Keys remain cache identities only.
  Console.WriteLn(
      "GPU-VU: generated compile request kind=%s key=%016llx%016llx "
      "source_bytes=%u source_pairs=%u bodies=%u/%u/%u/%u "
      "control=%s blocks=%u max_block_pairs=%u loops=%u pair_limit=%u "
      "uniform_vectors=%u binding_vectors=%u dynamic_uniform=%u "
      "instance_live_ins=%u batch_varying_vectors=%u source_abi=%u "
      "sink_scheduled=%u ftoi_probe=%u.",
      GeneratedExecutionKindName(program.execution_kind),
      static_cast<unsigned long long>(key->high),
      static_cast<unsigned long long>(key->low),
      static_cast<u32>(program.source.size()), program.semantic_pair_count,
      program.generated_prelude_bodies, program.generated_upper_bodies,
      program.generated_lower_bodies, program.generated_terminal_bodies,
      GeneratedControlStrategyName(program.control_strategy),
      program.control_basic_block_count,
      program.control_maximum_block_pair_count,
      program.control_natural_loop_count,
      program.maximum_dynamic_pairs_per_invocation,
      program.BatchUniformVectorCount(),
      program.BatchBindingVectorCount(),
      static_cast<u32>(program.uses_dynamic_batch_uniform_index),
      static_cast<u32>(program.uses_instance_indexed_batch_live_ins),
      program.BatchVaryingLiveInVectorCount(),
      program.loop_kernel_source_abi,
      static_cast<u32>(program.uses_sink_scheduled_outputs),
      static_cast<u32>(program.uses_loop_kernel_ftoi_probe_output));

  const GeneratedGxpExecutionRequirement execution_requirement =
      program.execution_kind ==
              GeneratedCgExecutionKind::GeneratedLoopKernelDirectVuTfx
          ? GeneratedGxpExecutionRequirement::ParallelStaticFlow
          : GeneratedGxpExecutionRequirement::Any;
  if (compiler->Submit(*key, std::move(program.source),
                       execution_requirement)) {
    // RequestGeneratedProgram() still owns s_registry_mutex here. A compiler
    // result cannot be published and registered before this root contributes
    // to the generation-wide in-flight count.
    s_compiler_in_flight_programs.fetch_add(1,
                                            std::memory_order_release);
    return true;
  }

  // A full bounded queue is retryable. Remove only the entry installed by
  // this request; an independently completed result cannot exist because
  // Submit() rejected it.
  const auto it = s_registry.find(*key);
  if (it != s_registry.end() &&
      it->second.state == GeneratedProgramState::Queued) {
    s_registry.erase(it);
  }
  s_compiler_queue_retries.fetch_add(1, std::memory_order_relaxed);
  return false;
}

GeneratedProgramState QueryGeneratedProgram(const ShaderKey &key) {
  std::lock_guard lock(s_registry_mutex);
  if (!s_compiler)
    return GeneratedProgramState::Unavailable;
  const ShaderCompiler::State compiler_state = s_compiler->GetState();
  if (compiler_state == ShaderCompiler::State::Stopped ||
      compiler_state == ShaderCompiler::State::Stopping ||
      compiler_state == ShaderCompiler::State::Unavailable) {
    return GeneratedProgramState::Unavailable;
  }
  const auto it = s_registry.find(key);
  return it == s_registry.end() ? GeneratedProgramState::Missing
                                : it->second.state;
}

bool GeneratedProgramHasProductResourceAttestation(const ShaderKey& key) {
  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(key);
  return it != s_registry.end() &&
         it->second.state == GeneratedProgramState::Ready &&
         it->second.resource_attestation ==
             GeneratedGxpResourceAttestation::Accepted;
}

GeneratedLoopKernelAttestationRecord QueryGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity) {
  GeneratedLoopKernelAttestationRecord unattested;
  unattested.identity = identity;
  if (!identity.IsValid())
    return unattested;

  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(identity.key);
  if (it == s_registry.end() ||
      it->second.state != GeneratedProgramState::Ready ||
      !(it->second.loop_kernel_attestation.identity == identity)) {
    return unattested;
  }
  return it->second.loop_kernel_attestation;
}

bool BeginGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity) {
  if (!identity.IsValid())
    return false;
  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(identity.key);
  return it != s_registry.end() &&
         it->second.state == GeneratedProgramState::Ready &&
         BeginGeneratedLoopKernelAttestationRecord(
             &it->second.loop_kernel_attestation, identity);
}

bool CompleteGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity,
    GeneratedLoopKernelNumericProfile numeric_profile,
    GeneratedLoopKernelAttestationRejection rejection) {
  if (!identity.IsValid())
    return false;
  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(identity.key);
  return it != s_registry.end() &&
         it->second.state == GeneratedProgramState::Ready &&
         CompleteGeneratedLoopKernelAttestationRecord(
             &it->second.loop_kernel_attestation, identity,
             numeric_profile, rejection);
}

bool PromoteGeneratedLoopKernelAttestationToProduct(
    const GeneratedLoopKernelAttestationIdentity& identity) {
  if (!identity.IsValid())
    return false;
  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(identity.key);
  return it != s_registry.end() &&
         it->second.state == GeneratedProgramState::Ready &&
         it->second.resource_attestation ==
             GeneratedGxpResourceAttestation::Accepted &&
         PromoteGeneratedLoopKernelAttestationRecord(
             &it->second.loop_kernel_attestation, identity);
}

bool CancelGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity) {
  if (!identity.IsValid())
    return false;
  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(identity.key);
  return it != s_registry.end() &&
         CancelGeneratedLoopKernelAttestationRecord(
             &it->second.loop_kernel_attestation, identity);
}

bool RequestGeneratedProgramPlanning(u64 identity,
                                     std::function<void()> work,
                                     std::function<void()> cancel) {
  if (identity == 0u || !work)
    return false;
  std::lock_guard lock(s_registry_mutex);
  ShaderCompiler* const compiler = s_compiler;
  if (!compiler)
    return false;
  if (compiler->HasPlanning(identity))
    return true;

  std::function<void()> wrapped = [work = std::move(work)]() mutable {
    struct Completion {
      ~Completion() { RetireGeneratedPlanningWork(); }
    } completion;
    work();
  };
  std::function<void()> cancelled = [cancel = std::move(cancel)]() mutable {
    struct Completion {
      ~Completion() { RetireGeneratedPlanningWork(); }
    } completion;
    if (cancel)
      cancel();
  };
  // Construct closures before taking ownership: allocation failure must not
  // strand the registry's lock-free in-flight/idle-generation accounting.
  s_compiler_in_flight_plans.fetch_add(1u, std::memory_order_release);
  if (compiler->SubmitPlanning(identity, std::move(wrapped),
                               std::move(cancelled)))
    return true;
  RetireGeneratedPlanningWork();
  return false;
}

bool IsGeneratedProgramPlanning(u64 identity) {
  if (identity == 0u)
    return false;
  std::lock_guard lock(s_registry_mutex);
  return s_compiler && s_compiler->HasPlanning(identity);
}

u64 GetGeneratedProgramCompilerIdleGeneration() {
  return s_compiler_idle_generation.load(std::memory_order_acquire);
}

bool GeneratedProgramCompilerHasInFlightWork() {
  return s_compiler_in_flight_programs.load(std::memory_order_acquire) != 0u ||
         s_compiler_in_flight_plans.load(std::memory_order_acquire) != 0u;
}

bool PollGeneratedProgramCompile(CompileResult *result,
                                 GeneratedCgProgram *metadata) {
  if (!result || !metadata)
    return false;

  ShaderCompiler *compiler = nullptr;
  {
    std::lock_guard lock(s_registry_mutex);
    compiler = s_compiler;
  }
  if (!compiler || !compiler->Poll(result))
    return false;

  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(result->key);
  if (it == s_registry.end())
    return false;
  it->second.state = GeneratedProgramState::Compiled;
  it->second.resource_attestation = result->gxp_resource_attestation;
  (result->succeeded ? s_compile_successes : s_compile_failures)
      .fetch_add(1, std::memory_order_relaxed);
  *metadata = it->second.metadata;
  return true;
}

void CompleteGeneratedProgramRegistration(const ShaderKey &key,
                                          bool succeeded) {
  bool completed = false;
  bool completed_in_flight_root = false;
  {
    std::lock_guard lock(s_registry_mutex);
    const auto it = s_registry.find(key);
    if (it == s_registry.end())
      return;
    completed_in_flight_root =
        it->second.state == GeneratedProgramState::Queued ||
        it->second.state == GeneratedProgramState::Compiled;
    it->second.state =
        succeeded ? GeneratedProgramState::Ready
                  : GeneratedProgramState::Failed;
    (succeeded ? s_ready_programs : s_failed_programs)
        .fetch_add(1, std::memory_order_relaxed);
    completed = true;
  }
  if (completed)
    PublishDirectProgramRegistration(key, succeeded);
  if (completed)
    RefreshGeneratedLoopKernelExecutableState(key);
  if (completed)
    RequestGeneratedLoopKernelBundlePump();
  if (completed)
    PumpGeneratedNestedDirectBundles();
  if (completed)
    PumpStructuredGeneratedBundles();
  if (completed_in_flight_root) {
    // Pump before retiring this root. If any structured bundle submits its
    // successor, the count never transiently reaches zero and unrelated
    // deferred identities remain on their O(1) MTVU fallback cache path.
    const u64 previous = s_compiler_in_flight_programs.fetch_sub(
        1u, std::memory_order_acq_rel);
    if (previous == 1u &&
        s_compiler_in_flight_plans.load(std::memory_order_acquire) == 0u) {
      s_compiler_idle_generation.fetch_add(1,
                                           std::memory_order_release);
    }
  }
}

ProgramRegistryStatistics GetGeneratedProgramRegistryStatistics() {
  ProgramRegistryStatistics stats;
  stats.requests = s_requests.load(std::memory_order_relaxed);
  stats.unavailable_requests =
      s_unavailable_requests.load(std::memory_order_relaxed);
  stats.cache_hits = s_cache_hits.load(std::memory_order_relaxed);
  stats.cache_misses = s_cache_misses.load(std::memory_order_relaxed);
  stats.compiler_queue_retries =
      s_compiler_queue_retries.load(std::memory_order_relaxed);
  stats.compile_successes = s_compile_successes.load(std::memory_order_relaxed);
  stats.compile_failures = s_compile_failures.load(std::memory_order_relaxed);
  stats.ready_programs = s_ready_programs.load(std::memory_order_relaxed);
  stats.failed_programs = s_failed_programs.load(std::memory_order_relaxed);
  stats.compiler_safety_rejections =
      s_compiler_safety_rejections.load(std::memory_order_relaxed);
  std::lock_guard lock(s_registry_mutex);
  if (s_compiler)
    stats.compiler = s_compiler->GetStatistics();
  return stats;
}

void ClearGeneratedProgramRegistry() {
  std::lock_guard lock(s_registry_mutex);
  s_registry.clear();
}

} // namespace VitaGpuVu
