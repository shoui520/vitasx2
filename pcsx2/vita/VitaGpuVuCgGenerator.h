// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuLoopKernel.h"
#include "vita/VitaGpuVuVifInput.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace VitaGpuVu {

// Integer-only binary32 primitive mirrored by the structured generated Cg
// roots.  SGX543 native float arithmetic does not obey every PCSX2-selected
// VU1 rounding profile, so a generated transaction may use this explicit
// boundary instead of inheriting an undocumented shader-core rounding mode.
enum class GeneratedCgF32BinaryOperation : u8 {
  Add,
  Subtract,
  Multiply,
};

// The generated implementation covers the Vita product's named nearest-even,
// DAZ/FZ, standard-overflow-clamp profile. ApproximateFmac is a permission bit:
// exact generated roots retain the integer boundary, while playable generated
// roots may inline the separately attested normalized native-SGX result
// contract. The fixed native FMAC root remains a physical attestation owner;
// it is not the hot generated tier's required execution shape.
bool IsGeneratedCgSoftwareF32ConfigurationSupported(u32 configuration_bits);
bool IsGeneratedCgNativeF32ConfigurationSupported(u32 configuration_bits);
// Exact-Q generated roots cannot inherit Series5XT's FRCP-based lowering for
// Cg division.  This profile uses a bounded integer significand divider whose
// host mirror is compared with PCSX2's nearest-even VU DIV owner before the
// emitted GXP is physically attested.
bool IsGeneratedCgExactDivideConfigurationSupported(u32 configuration_bits);

// Host mirror of the emitted integer Cg primitive. This is not a replacement
// PCSX2 oracle: validation compares it against VUops.cpp under the same FPCR
// configuration, then physical private-transaction attestation compares the
// compiled GXP result again before any canonical commit.
bool EvaluateGeneratedCgSoftwareF32Reference(
    GeneratedCgF32BinaryOperation operation, u32 left_bits, u32 right_bits,
    u32 configuration_bits, u32* result_bits);
bool EvaluateGeneratedCgNativeF32Reference(
    GeneratedCgF32BinaryOperation operation, u32 left_bits, u32 right_bits,
    u32 configuration_bits, u32* result_bits);
bool EvaluateGeneratedCgExactDivideReference(
    u32 numerator_bits, u32 denominator_bits, u32 configuration_bits,
    u32* result_bits);
bool EvaluateGeneratedCgExactFloatToIntReference(
    u32 input_bits, u8 scale_offset, u32 configuration_bits,
    u32* result_bits);
bool EvaluateGeneratedCgPlayableFloatToIntReference(
    u32 input_bits, u8 scale_offset, u32 configuration_bits,
    u32* result_bits);

enum class GeneratedCgExecutionKind : u8 {
  DirectVuTfx,
  // Hardware-driven product namespace for one branch/static-flow loop body
  // mapped directly to exact GXM data instances.  Unlike the historical
  // structured direct kind, this root has no outer snapshot, expression
  // scratch, precompute, or continuation-buffer dependency.
  GeneratedLoopKernelDirectVuTfx,
  UniversalStateMachine,
  UniversalDirectStateMachine,
  UniversalCompactContinuation,
  StructuredStateSnapshots,
  StructuredMemoryPreflight,
  StructuredExpressionScratch,
  // Offline-compiled, title-neutral arithmetic stage. Runtime-generated
  // modules publish one operand into transactional scratch; this fixed stage
  // applies the configured Cortex-A9 Q/P estimate contract without exposing
  // ShaccCg to the compiler-hostile estimate/refinement root.
  StructuredFixedQpNumeric,
  // Offline-compiled exact FMAC arithmetic over transaction-private scratch.
  // One pointer-free descriptor batches a dependency-ordered ADD/SUB/MUL
  // chain, so runtime Shacc roots neither duplicate the software-F32 helpers
  // nor turn every architectural arithmetic boundary into a firmware job.
  StructuredFixedFmacNumeric,
  StructuredParallelChildMemoryStore,
  // One vertex invocation consumes one child-loop iteration from a serial
  // enclosing-loop snapshot and feeds the proven GIF vertex record directly
  // into TFX. This is the generated nested-loop performance tier: it avoids
  // both the serial PairPlan interpreter and transactional VU-memory export.
  StructuredParallelDirectVuTfx,
  StructuredStoreCommit,
  StructuredFinalState,
};

// Source-key namespace for the hardware-driven one-root loop kernel.  This is
// intentionally independent of the historical generated-program ABI: bumping
// it invalidates only compact loop kernels and can never make a snapshot or
// compiler-module artifact satisfy the new bundle contract.
inline constexpr u32 GeneratedLoopKernelCgAbiVersion = 26;
// ABI 51 is the batch-varying private canary. ABI 26 is the first bounded
// private canary: its optional BUFFER3 FTOI probe exists only to classify a
// failed first hardware gate. Once adjacent Executes prove their varying
// live-ins, ABI 51 retains the exact BUFFER2 store journal but drops the
// diagnostic BUFFER3 output. The sink scheduler publishes store values at
// their final uses instead of keeping the complete store+raster DAG live until
// the epilogue, and varying vectors occupy the per-object tail of BUFFER1,
// eliminating the old ABI-33 BUFFER4 fetch path. ABI 51 is never a sustained
// executable after its private transaction passes; CPU MTVU or an already
// attested no-write product owns the interval until ABI 54 is ready. ABI 28 is
// retired because its Cg structure retained the total
// uniform count while the host packed only invariant uniforms; ABI 33 is
// retired because its permanent probe disabled sink scheduling and kept both
// BUFFER3 and BUFFER4 in every sustained exact draw. ABI 50 is retired because
// it still inherited the generic greater-than-16-uniform sink threshold; the
// physical BSpline root has eleven uniforms and therefore emitted the same
// epilogue-wide dependency graph. ABI 51 makes sink scheduling mandatory for
// this exact partial class independently of lazy uniform rewriting. Identifiers
// are never reused for a corrected generated-source contract.
inline constexpr u32 GeneratedLoopKernelPartialBatchCgAbiVersion = 51;
// No-write product roots are compiled only after the corresponding ABI-26/51
// private canary has passed. The canary attests exact control, addressing,
// input ownership, and the PairPlan state successor. The product has no
// writable VU state: its only SGX-visible effects are raster vertex outputs,
// so ABI-43/44 prunes dead state/store cones, uses the explicitly named
// native-SGX output-only arithmetic profile, and sink-schedules each raster
// result while leaving the wide batch-uniform set as lazy BUFFER reads. Exact
// integer indexing and invocation selection remain shared with the canary.
// ABI-37/38 is retired:
// preserving its complete exact store topology emitted a 1,971-primary-
// instruction BSpline GXP and reduced physical output to 11--18 FPS even with
// BUFFER2/3 absent. Separate base/partial identifiers keep every GXM binding
// layout in a distinct content-key namespace. ABI-39/40 is retired because its
// no-write product disabled the sink scheduler and kept the full raster DAG and
// batch live-in set live until the final output epilogue. ABI-41/42 is retired
// because every scheduled outer recurrence rebuilt the same outer-to-count
// selector inside each inlined helper call. ABI-43/44 publishes each exact
// integer schedule selection once per invocation, reuses it across native
// output-only multiply/add expressions, and preserves complete qword
// recurrences as float4 operations instead of four scalar copies. ABI-45/46
// additionally proves canonical +0 increments at generation time and removes
// their otherwise dead selector/multiply/add work from the output-only
// product; the exact private canary still executes those ordered additions.
// ABI-47/48 keeps that arithmetic but retires the product-only BUFFER4 split:
// partial-product varying live-ins now occupy a per-object tail in the already
// dynamically indexed BUFFER1 record.  This preserves record-zero invariant
// hoisting while removing the physical BUFFER4 path which cost about 105 ms
// per BSpline object on SGX in the r264 hardware run. ABI-47/48 is retired
// because its final-use sink scheduler was enabled only by the generic
// greater-than-16-uniform threshold during CgEmitter::Generate(); the product
// ABI was assigned afterward, so BSpline's eleven-uniform root retained its
// complete raster DAG until the epilogue despite the product contract claiming
// sink scheduling. ABI-52/53 makes final-use publication an unconditional part
// of the lean native-output product profile. ABI-54 replaces ABI-53's
// all-uniform macro rewrite with compiler-backed first-use declarations for
// only those constant qwords which vary in the inline per-object tail. This
// keeps invariant constants and varying VF/ACC/scalar records eager while
// shortening the varying constant live ranges seen by Sony's Cg allocator.
// Retired identifiers are never reused.
inline constexpr u32 GeneratedLoopKernelProductCgAbiVersion = 52;
inline constexpr u32 GeneratedLoopKernelPartialBatchProductCgAbiVersion = 54;
// Nested flat line strips use a dense expanded INDEX domain: two invocations
// per potential line, with independent geometry/provoking outer-child maps.
// The private journal uses that expanded domain, never the duplicated source
// vertex. Expanded private roots 55/57 remain compiler fixtures: ADC-filtered
// raster indices cannot certify the complete private journal. Products 56/58
// compile under their checked line-list ABI and may render only after their
// separate dense state canary (59/60) passes physical comparison.
inline constexpr u32 GeneratedLoopKernelNestedFlatCgAbiVersion = 55;
inline constexpr u32 GeneratedLoopKernelNestedFlatProductCgAbiVersion = 56;
inline constexpr u32 GeneratedLoopKernelNestedFlatPartialCgAbiVersion = 57;
inline constexpr u32 GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion = 58;
// Complete, non-rasterizing store attestation, one INDEX per VU iteration.
// Constant point outputs keep every indexed invocation inside the clip volume;
// fragment and depth writes must be disabled by the private submission owner.
// These keys may never stand in for an authoritative TFX executable.
inline constexpr u32 GeneratedLoopKernelStateCanaryCgAbiVersion = 59;
inline constexpr u32 GeneratedLoopKernelPartialStateCanaryCgAbiVersion = 60;

struct GeneratedBatchVaryingLiveIns final {
  u64 constant_mask = 0u;
  u32 vf_mask = 0u;
  bool acc = false;
  bool scalars = false;

  bool Any() const {
    return constant_mask != 0u || vf_mask != 0u || acc || scalars;
  }

  void Include(const GeneratedBatchVaryingLiveIns& other) {
    constant_mask |= other.constant_mask;
    vf_mask |= other.vf_mask;
    acc |= other.acc;
    scalars |= other.scalars;
  }

  bool Covers(const GeneratedBatchVaryingLiveIns& required) const {
    return (required.constant_mask & ~constant_mask) == 0u &&
           (required.vf_mask & ~vf_mask) == 0u &&
           (!required.acc || acc) && (!required.scalars || scalars);
  }

  bool operator==(const GeneratedBatchVaryingLiveIns& other) const {
    return constant_mask == other.constant_mask && vf_mask == other.vf_mask &&
           acc == other.acc && scalars == other.scalars;
  }
};

// Control shape emitted by the title-neutral GPU JIT. This is telemetry and
// compiler admission metadata, never a semantic support key. In particular,
// PerPairStateMachine identifies the slow transitional root which performs a
// dynamic TPC/body selection for every architectural pair; the profitable
// serial tier must become BlockThreadedStateMachine or a proven loop tier.
enum class GeneratedCgControlStrategy : u8 {
  None,
  PerPairStateMachine,
  BlockThreadedStateMachine,
  StructuredLoop,
  ParallelLoop,
};

// BUFFER10 is transaction-private spill storage after memory preflight. A
// 128-scalar row keeps compiler-bounded expression DAGs out of artificial
// 32-register rematerialization cycles while remaining only 2 MiB for the
// maximum 64 * 64 invocation grid. Slots never become canonical VU state.
inline constexpr u32 StructuredGeneratedScratchSlots = 128;
inline constexpr u32 StructuredGeneratedMaximumScratchInvocations = 64u * 64u;
inline constexpr u32 StructuredGeneratedScratchMaskWords =
    (StructuredGeneratedScratchSlots + 31u) / 32u;
using StructuredGeneratedScratchMask =
    std::array<u32, StructuredGeneratedScratchMaskWords>;
using StructuredFixedFmacScratchMask = StructuredGeneratedScratchMask;

inline constexpr u32 StructuredFixedQpNumericFormatVersion = 1;

enum class StructuredFixedQpNumericOperation : u32 {
  Invalid = 0,
  ArmApproximateReciprocal = 1,
  ArmApproximateSquareRoot = 2,
};

// One descriptor is bound as BUFFER0 for one fixed numeric partition. The
// operation reads/writes BUFFER10's transaction-private scalar scratch. No
// field identifies a title, source PC, or content hash.
struct alignas(16) StructuredFixedQpNumericDescriptor {
  u32 format_version = StructuredFixedQpNumericFormatVersion;
  StructuredFixedQpNumericOperation operation =
      StructuredFixedQpNumericOperation::Invalid;
  u32 input_slot = 0;
  u32 output_slot = 0;

  bool IsValid() const {
    return format_version == StructuredFixedQpNumericFormatVersion &&
           (operation ==
                StructuredFixedQpNumericOperation::ArmApproximateReciprocal ||
            operation ==
                StructuredFixedQpNumericOperation::ArmApproximateSquareRoot) &&
           input_slot < StructuredGeneratedScratchSlots &&
           output_slot < StructuredGeneratedScratchSlots &&
           input_slot != output_slot;
  }
};
static_assert(sizeof(StructuredFixedQpNumericDescriptor) == 16);

// Host mirror of the offline fixed numeric partition. Metadata and capacity
// are validated completely before the first scratch write, so a failed
// preflight is suitable for transactional provider admission tests.
bool ApplyStructuredFixedQpNumericReference(
    const StructuredFixedQpNumericDescriptor& descriptor,
    u32 active_invocations, u32* scratch_words,
    std::size_t scratch_word_count);

// Scalar mirror shared by generated-loop final-state/profile evaluation and
// the fixed Q/P numeric partition. Keeping the ARM estimate/refinement owner
// here prevents a second approximation from drifting away from the Cg helper.
bool EvaluateGeneratedCgArmApproximateReference(
    StructuredFixedQpNumericOperation operation, u32 input_bits,
    u32* result_bits);

inline constexpr u32 StructuredFixedFmacNumericFormatVersion = 2;
inline constexpr u32 StructuredFixedFmacNumericMaximumOperations = 32;

enum class StructuredFixedFmacNumericOperation : u32 {
  Invalid = 0,
  Add = 1,
  Subtract = 2,
  Multiply = 3,
};

enum class StructuredFixedFmacOperandKind : u32 {
  Invalid = 0,
  Scratch = 1,
  Immediate = 2,
};

struct StructuredFixedFmacOperand {
  StructuredFixedFmacOperandKind kind =
      StructuredFixedFmacOperandKind::Invalid;
  // Scratch slot for Scratch, raw binary32 bits for Immediate.
  u32 value = 0;

  bool IsValid(const StructuredFixedFmacScratchMask& available_scratch_mask) const {
    const bool scratch_available = value < StructuredGeneratedScratchSlots &&
        (available_scratch_mask[value / 32u] &
         (1u << (value % 32u))) != 0u;
    return kind == StructuredFixedFmacOperandKind::Immediate ||
           (kind == StructuredFixedFmacOperandKind::Scratch &&
            scratch_available);
  }
};
static_assert(sizeof(StructuredFixedFmacOperand) == 8);

struct alignas(16) StructuredFixedFmacNumericRecord {
  StructuredFixedFmacNumericOperation operation =
      StructuredFixedFmacNumericOperation::Invalid;
  u32 output_slot = 0;
  StructuredFixedFmacOperand left{};
  StructuredFixedFmacOperand right{};
  std::array<u32, 2> reserved{};

  bool IsZero() const {
    return operation == StructuredFixedFmacNumericOperation::Invalid &&
           output_slot == 0u &&
           left.kind == StructuredFixedFmacOperandKind::Invalid &&
           left.value == 0u &&
           right.kind == StructuredFixedFmacOperandKind::Invalid &&
           right.value == 0u && reserved[0] == 0u && reserved[1] == 0u;
  }
};
static_assert(sizeof(StructuredFixedFmacNumericRecord) == 32);

// BUFFER0 descriptor for one fixed numeric stage. Operations execute in
// descriptor order independently for every outer/child invocation. The
// initial scratch mask plus the ordered publication walk makes use-before-
// produce metadata rejectable before the first private scratch write.
struct alignas(16) StructuredFixedFmacNumericDescriptor {
  u32 format_version = StructuredFixedFmacNumericFormatVersion;
  u32 operation_count = 0;
  u32 configuration_bits = 0;
  StructuredFixedFmacScratchMask initial_scratch_mask{};
  u32 reserved = 0;
  std::array<StructuredFixedFmacNumericRecord,
             StructuredFixedFmacNumericMaximumOperations>
      operations{};

  bool IsValid() const {
    if (format_version != StructuredFixedFmacNumericFormatVersion ||
        operation_count == 0u ||
        operation_count > operations.size() ||
        !IsGeneratedCgSoftwareF32ConfigurationSupported(configuration_bits)) {
      return false;
    }
    StructuredFixedFmacScratchMask available = initial_scratch_mask;
    for (u32 index = 0; index < operations.size(); index++) {
      const StructuredFixedFmacNumericRecord& record = operations[index];
      if (index >= operation_count) {
        if (!record.IsZero())
          return false;
        continue;
      }
      if ((record.operation != StructuredFixedFmacNumericOperation::Add &&
           record.operation !=
               StructuredFixedFmacNumericOperation::Subtract &&
           record.operation !=
               StructuredFixedFmacNumericOperation::Multiply) ||
          record.output_slot >= StructuredGeneratedScratchSlots ||
          !record.left.IsValid(available) ||
          !record.right.IsValid(available) || reserved != 0u ||
          record.reserved[0] != 0u ||
          record.reserved[1] != 0u) {
        return false;
      }
      available[record.output_slot / 32u] |=
          1u << (record.output_slot % 32u);
    }
    return true;
  }
};
static_assert(sizeof(StructuredFixedFmacNumericDescriptor) == 1056);

// Host candidate for the fixed numeric attestation GXP. ApproximateFmac
// selects the normalized native model; otherwise this uses exact nearest-even.
// The Sony/Series5 documentation does not guarantee native arithmetic's final
// rounding bit, so physical GXP attestation must establish this model before
// product promotion. Playable generated roots then inline that attested model
// rather than inserting a firmware job at every FMAC partition. The complete
// descriptor and destination capacity are validated before any
// transaction-private scratch mutation.
bool ApplyStructuredFixedFmacNumericReference(
    const StructuredFixedFmacNumericDescriptor& descriptor,
    u32 active_invocations, u32* scratch_words,
    std::size_t scratch_word_count);

// Runtime address metadata consumed by the fixed-shape structured-memory
// preflight.  The shader source is deliberately independent of the decoded
// program: PairPlan analysis emits this bounded block into each private
// transaction slot, and one compact title-neutral loop interprets it before
// any private-memory store job runs.  This prevents ShaccCg source size and
// compiler work from growing with the number of load/store relationships.
inline constexpr u32 StructuredMemoryPreflightFormatVersion = 1;
inline constexpr u32 StructuredMemoryPreflightMaximumAccesses = 64;
inline constexpr u32 StructuredMemoryPreflightStoreBit = 1u << 8;

struct alignas(16) StructuredMemoryPreflightAccess {
  u32 base_vi = 0;
  u32 invocation_coefficient = 0;
  u32 qword_offset = 0;
  // Bits 0--3 are the VU qword lane mask. Bit 8 classifies a store; clear
  // means load. All other bits are invalid metadata.
  u32 control = 0;
};
static_assert(sizeof(StructuredMemoryPreflightAccess) == 16);

struct alignas(16) StructuredMemoryPreflightData {
  // uint4 0: format, enabled, total access count, load count.
  std::array<u32, 4> header0{
      StructuredMemoryPreflightFormatVersion, 0, 0, 0};
  // uint4 1: store count, child counter VI, child limit VI, signed step.
  std::array<u32, 4> header1{};
  // uint4 2: maximum outer iterations, maximum child iterations, reserved.
  std::array<u32, 4> header2{};
  // Loads are serialized first, followed by stores. The shader validates the
  // partition before indexing it.
  std::array<StructuredMemoryPreflightAccess,
             StructuredMemoryPreflightMaximumAccesses>
      accesses{};
};
static_assert(sizeof(StructuredMemoryPreflightData) ==
              (3u + StructuredMemoryPreflightMaximumAccesses) * 16u);

// A structured generated transaction returns to the fixed core for the proven
// post-loop tail and terminal E-bit drain. This is the watchdog-safe size of
// each fixed continuation job, not a replacement architectural Execute budget.
// The generated state root preserves and debits the original command budget;
// longer exact tails continue through the same private generation.
inline constexpr u32 StructuredGeneratedTailPairBudget = 128;

// First-failure code published in StructuredGeneratedBundle's outer-state
// word 3, with word 5 retaining a diagnostic duplicate. These are
// transactional preflight failures: no canonical VU state or PATH1 output has
// been committed when one is reported. Keeping support and reason in one
// authoritative word prevents a partially observed pair of stores from ever
// looking like an unclassified runtime rejection.
enum class StructuredGeneratedRuntimeFailure : u32 {
  None = 0,
  EntryPc = 1,
  ContinuationState = 2,
  ChildTripCount = 3,
  LoadAfterStore = 4,
  IntraIterationLoadStoreAlias = 5,
  StoreQwordAlias = 6,
  PairBudget = 7,
  OuterIterationBound = 8,
  CommitMetadata = 9,
  CrossOuterMemoryAlias = 10,
};

// Transaction-private ABI shared by the generated state/final roots, the
// fixed preflight root, and the GS completion owner. Word 6 preserves the
// current Execute command's remaining architectural pair budget, word 15 says
// whether the structured transaction was reached (zero means a preceding fixed
// entry job already completed the epoch), and words 8/9 preserve the fixed
// runway's counters. Writable-buffer execution must never depend on a
// read/modify/write surviving duplicate or replayed draw scheduling.
inline constexpr u32 StructuredGeneratedOuterStateWords = 16;
inline constexpr u32 StructuredGeneratedOuterRemainingPairBudgetWord = 6;
inline constexpr u32 StructuredGeneratedOuterBasePairWord = 8;
inline constexpr u32 StructuredGeneratedOuterBaseCycleWord = 9;
inline constexpr u32 StructuredGeneratedOuterFinalTripWord = 10;
inline constexpr u32 StructuredGeneratedOuterFinalFailureWord = 11;
inline constexpr u32 StructuredGeneratedOuterPreflightGateWord = 12;
inline constexpr u32 StructuredGeneratedOuterPreflightFailureWord = 13;
inline constexpr u32 StructuredGeneratedOuterPreflightTripWord = 14;
inline constexpr u32 StructuredGeneratedOuterTransactionGateWord = 15;

inline const char* StructuredGeneratedRuntimeFailureName(u32 value) {
  switch (static_cast<StructuredGeneratedRuntimeFailure>(value)) {
  case StructuredGeneratedRuntimeFailure::None:
    return "none";
  case StructuredGeneratedRuntimeFailure::EntryPc:
    return "entry-pc";
  case StructuredGeneratedRuntimeFailure::ContinuationState:
    return "continuation-state";
  case StructuredGeneratedRuntimeFailure::ChildTripCount:
    return "child-trip-count";
  case StructuredGeneratedRuntimeFailure::LoadAfterStore:
    return "load-after-store";
  case StructuredGeneratedRuntimeFailure::IntraIterationLoadStoreAlias:
    return "intra-iteration-load-store-alias";
  case StructuredGeneratedRuntimeFailure::StoreQwordAlias:
    return "store-qword-alias";
  case StructuredGeneratedRuntimeFailure::PairBudget:
    return "pair-budget";
  case StructuredGeneratedRuntimeFailure::OuterIterationBound:
    return "outer-iteration-bound";
  case StructuredGeneratedRuntimeFailure::CommitMetadata:
    return "commit-metadata";
  case StructuredGeneratedRuntimeFailure::CrossOuterMemoryAlias:
    return "cross-outer-memory-alias";
  }
  return "unknown";
}

struct CgMemoryInput {
  static constexpr u32 OrdinaryVuMemory = 0xffffffffu;
  static constexpr u32 PackedCompactOuterTables = 0xfffffffeu;

  AffineQwordAddress address;
  u32 attribute_index = 0;
  // OrdinaryVuMemory denotes an immutable VU-memory span. The packed marker
  // denotes one contiguous transaction-private block containing every
  // reachable CompactOuterInputTable; one base binding then serves all table
  // loads and avoids consuming one descriptor/stream slot per VF qword.
  u32 compact_outer_table = OrdinaryVuMemory;
  // Bit N means the flat instance root declares VertexN for this logical
  // stream. Geometry dependencies require every primitive lane; a
  // color-only dependency requires only the final/provoking lane.
  u8 flat_attribute_vertex_mask = 0;
};

// One descriptor-scale VU-memory qword which is invariant across every
// invocation in the draw. It is uploaded once through the vertex default
// uniform buffer, never repeated as a per-vertex stream.
struct CgConstantInput {
  AffineQwordAddress address;
  u32 uniform_index = 0;
};

struct GeneratedCgProgram {
  static constexpr u32 RawInputBufferQwords =
      GeneratedRawInputBufferQwords;
  static constexpr u32 MaximumBatchDraws = 4096;
  // Sony's gxm/memory.h explicitly permits a dynamically indexed user
  // uniform buffer to access beyond its shader-declared size at runtime. Keep
  // the declaration at the skinning sample's proven 64-vector scale: declaring
  // the complete 2 MiB ring caused runtime ShaccCg to exhaust its work heap and
  // return a fatal internal error even though offline psp2cgc accepted it.
  static constexpr u32 DeclaredBufferVectors =
      GeneratedRawInputDeclaredQwords;
  // Fixed BUFFER1 indices remain cheaper for the characterized 27-vector
  // nested root. A host-zeroed dynamic record selector moved its live-ins into
  // primary code but increased physical scratch from 4 KiB to 69 KiB; sink
  // scheduling reduced that only to 44 KiB. Reserve dynamic indexing for roots
  // beyond the fixed 32-vector class, where secondary capacity can no longer be
  // assumed. This is a compiler-resource class, not semantic admission.
  static constexpr u32 MaximumStaticBatchUniformVectors = 32;
  // Begin shortening terminal-output live ranges before the fixed-uniform
  // ceiling. Physical ABI-24 evidence identified the first problematic class
  // at 27 vectors, while the established 11-vector root remains unchanged.
  static constexpr u32 SinkScheduledOutputUniformThreshold = 16;

  GeneratedCgExecutionKind execution_kind =
      GeneratedCgExecutionKind::DirectVuTfx;

  std::string source;
  std::vector<CgMemoryInput> memory_inputs;
  std::vector<CgConstantInput> constant_inputs;
  std::vector<CompactOuterInputTable> compact_outer_inputs;
  u32 vf_uniform_mask = 0;
  u16 vi_uniform_mask = 0;
  std::array<u8, 32> stable_initial_vf_lanes{};
  // Demanded entry lanes which are stable only because the region's writes to
  // them are idempotent self-clamps. The descriptor path must verify each
  // runtime seed against its bound before accepting a draw.
  std::vector<ClampStableLane> clamp_stable_lanes;
  u8 stable_initial_acc_lanes = 0;
  u32 emitted_expression_count = 0;
  // Number of nontrivial software binary32 boundaries retained by this root.
  // Source bytes and SSA-node count alone do not predict Sony compiler work:
  // a 16 KiB root with 49 exact FMAC operations already exceeds the bounded
  // optimizer envelope while similarly sized integer/control roots compile in
  // milliseconds.  The title-neutral module planner partitions on this value
  // before handing source to ShaccCg.
  u32 emitted_software_f32_operation_count = 0;
  // The named playable profile emits native SGX ADD/SUB/MUL boundaries.
  // Keep this explicit so compiler-resource admission can distinguish compact
  // native vector arithmetic from exact helper-heavy or plain control roots.
  bool uses_structured_native_f32 = false;
  // ARM-estimate Q/P nodes currently expand several exact arithmetic
  // boundaries plus dynamically indexed estimate-table control inside one
  // opaque expression.  They require a fixed numeric module or finer shared
  // IR before they are safe runtime-compiler roots; shrinking an enclosing
  // source does not partition this internal dependency graph.
  bool uses_software_f32_resource_helpers = false;
  // General generated-VU telemetry distinguishes the number of reachable
  // architectural source pairs from the number of emitted code bodies after
  // title-neutral PairPlan coalescing. Other generated roots leave these zero.
  u32 semantic_pair_count = 0;
  GeneratedCgControlStrategy control_strategy =
      GeneratedCgControlStrategy::None;
  u32 control_basic_block_count = 0;
  u32 control_maximum_block_pair_count = 0;
  u32 control_natural_loop_count = 0;
  // Compiler-bounded CFG module contract. The source may share a GXM job with
  // mutually exclusive modules only when an unmatched TPC causes no writable
  // buffer store. Whole-program generated roots leave this false.
  bool uses_transactional_entry_gate = false;
  // True only for the generated root which consumes the epoch's immutable
  // Execute descriptor. Transaction-gated successor roots consume the private
  // continuation state directly and must not duplicate command parsing.
  bool consumes_execute_entry = false;
  u32 maximum_dynamic_pairs_per_invocation = 0;
  u32 generated_source_bytes = 0;
  u32 generated_prelude_bodies = 0;
  u32 generated_upper_bodies = 0;
  u32 generated_lower_bodies = 0;
  u32 generated_terminal_bodies = 0;
  // Title-neutral operation-family inventory used by generated universal
  // roots. Bits describe semantic source retained by preprocessing; they are
  // compiler telemetry only and never participate in support admission.
  u32 semantic_upper_family_mask = 0;
  u32 semantic_lower_family_mask = 0;
  u8 flat_vertices_per_primitive = 0;
  u8 flat_instance_vertex_step = 0;
  u16 batch_primitives_per_draw = 0;
  bool stable_initial_q = false;
  bool stable_initial_p = false;
  bool stable_initial_i = false;
  bool requires_dynamic_entry_state = false;
  bool uses_acc_uniform = false;
  bool uses_q_uniform = false;
  bool uses_p_uniform = false;
  bool uses_i_uniform = false;
  bool uses_gif_q_uniform = false;
  // BUFFER7 is the immutable 640-word ARM reciprocal/reciprocal-square-root
  // estimate table shared with the fixed universal executor.  The expression
  // generator only requests it for an explicit approximate-P configuration.
  bool uses_arm_estimate_table = false;
  bool uses_tfx_uniforms = false;
  bool uses_tfx_point_size = false;
  // The direct GIF contract proves TME=1/FST=1/FGE=0, so the generated root
  // and its linked fragment variants exchange packed UV only. This omits the
  // dead STQ/fog TEXCOORD which the generic runtime-selector ABI must retain.
  bool uses_tfx_uv_no_fog_interface = false;
  bool uses_flat_instance_inputs = false;
  // Hardware-driven loop kernels flatten each primitive into one ordinary
  // INDEX domain.  Keeping this separate from the legacy INSTANCE ABI lets
  // registration and submission reject any root which would force Sony's
  // compiler into per-instance execution mode.
  bool uses_flat_index_inputs = false;
  bool uses_buffered_batch_inputs = false;
  // BUFFER1 contains one extra host-zeroed int4 binding when this is true.
  // Uniforms are read through that runtime selector even though the selected
  // record is always zero. The dynamic address prevents Sony's compiler from
  // hoisting every fixed-index live-in into the secondary register file.
  bool uses_dynamic_batch_uniform_index = false;
  // Physical ABI-27 evidence rejected carrying every Execute live-in through
  // one INSTANCE_16BIT stream: the 11-vector hot root acquired 17 KiB of
  // scratch and failed product admission. Keep this false in ABI 26 while the
  // title-neutral variance census identifies a compiler-bounded partial
  // live-in layout.
  bool uses_instance_indexed_batch_live_ins = false;
  // Title-neutral physical variance feedback may specialize only the vectors
  // which differ across adjacent executions of the same generated root. They
  // live in a compact BUFFER4 table indexed by the already-proven global
  // INDEX-derived batch id. This preserves Series5 vertex parallelism and
  // avoids both fixed-live-in draw splitting and INSTANCE-mode serialization.
  GeneratedBatchVaryingLiveIns batch_varying_live_ins{};
  u32 loop_kernel_source_abi = GeneratedLoopKernelCgAbiVersion;
  bool uses_loop_kernel_state_canary = false;
  // High-pressure loop roots publish each terminal TFX/store sink as soon as
  // its SSA value is emitted. This preserves one atomic draw and one copy of
  // every expression while shortening primary-register live ranges for
  // Sony's Series5 allocator. It changes scheduling only, never semantics or
  // the GXM binding layout.
  bool uses_sink_scheduled_outputs = false;
  // One ordinary INDEX domain flattened from an exact enclosing/child grid.
  // It is distinct from flat-shading primitive expansion: the native GIF
  // topology still consumes one output vertex per INDEX, while memory inputs
  // use the independently proven outer and child affine coefficients.
  bool uses_nested_iteration_grid = false;
  // Consecutive compatible Executes share one draw when their fixed live-ins
  // are bit-identical. Their varying raw-input bindings remain indexed through
  // BUFFER1.
  bool uses_nested_batch_index_inputs = false;
  u16 nested_outer_iterations = 0;
  u16 nested_child_iterations = 0;
  // ABI 10+ writes every statically proven loop store to a dense, disjoint
  // transaction-private BUFFER2 journal in the same invocation which emits
  // the direct-TFX vertex.  This is not a producer module: it adds no draw,
  // visibility boundary, snapshot, or dynamic control flow.
  bool uses_loop_kernel_private_store_output = false;
  u8 loop_kernel_private_store_count = 0;
  // Private physical-attestation output for the first stored FTOI fed by a
  // rounded multiply in a DIV-bearing loop kernel. BUFFER3 records two int4
  // qwords per invocation: both multiply operands, its native Series5 result,
  // an exact raw-bit FTOI of that result, and a lower-cost branchless playable
  // candidate. The ordinary BUFFER2 journal supplies the emitted native
  // conversion and CPU oracle value. This is never canonical and may be bound
  // only while CPU MTVU remains the transaction owner.
  bool uses_loop_kernel_ftoi_probe_output = false;
  u32 loop_kernel_ftoi_probe_configuration_bits = 0;
  u8 loop_kernel_ftoi_probe_store_index = 0;
  u8 loop_kernel_ftoi_probe_lane = 0;
  u8 loop_kernel_ftoi_probe_scale_offset = 0;
  bool flat_strip_winding = false;
  // A generated structured-loop state root publishes only the child-exit
  // values proven live at its enclosing-loop boundary.  These masks describe
  // that scratch ABI; they are not a complete canonical VU-state snapshot.
  std::array<u8, 32> final_vf_lanes{};
  std::array<u8, 32> child_entry_vf_lanes{};
  u16 child_entry_vi_mask = 0;
  u8 final_acc_lanes = 0;
  u8 child_entry_acc_lanes = 0;
  bool final_q = false;
  bool final_p = false;
  bool final_i = false;
  bool child_entry_q = false;
  bool child_entry_p = false;
  bool child_entry_i = false;
  bool uses_final_state_output = false;
  bool uses_structured_state_snapshots = false;
  // Exactly one independently compiled recurrence slice publishes shared VI
  // snapshots and control metadata. Other slices write only disjoint
  // VF/ACC/Q/P/I snapshot destinations, so they can share one GXM job without
  // racing writable-buffer stores.
  bool structured_state_control_owner = false;
  // Exactly one final-state slice publishes scalar/VI/control state through
  // BUFFER9. Sibling VF slices write disjoint BUFFER8 destinations and may
  // share its GXM job.
  bool structured_final_control_owner = false;
  bool uses_structured_memory_preflight = false;
  bool uses_structured_expression_scratch = false;
  // Pure expression scratch generated specifically as a dependency of one
  // fused direct VU+TFX root. Its source carries a matching ABI marker so the
  // registry can apply the separately attested low-job compiler class without
  // broadening ordinary structured scratch admission.
  bool uses_generated_direct_precompute = false;
  bool uses_structured_fixed_qp_numeric = false;
  bool uses_structured_fixed_fmac_numeric = false;
  bool uses_structured_parallel_child_memory_store = false;
  bool uses_structured_parallel_direct_vu_tfx = false;
  bool uses_structured_store_commit = false;
  bool uses_structured_final_state = false;
  // Number of independent transaction-private values published by one
  // compiler-bounded scratch root. This is planner/telemetry metadata; the Cg
  // source and shader key remain the executable contract.
  u32 structured_scratch_output_count = 0;
  // Exact transaction-private BUFFER10 dependencies. These masks are derived
  // from reachable shared-IR leaves and roots and are embedded into the Cg
  // source contract. The hot-bundle planner uses them to coalesce independent
  // draws and inserts a visibility boundary only across a real RAW/WAR/WAW
  // dependency.
  StructuredGeneratedScratchMask structured_scratch_read_mask{};
  StructuredGeneratedScratchMask structured_scratch_write_mask{};
  StructuredMemoryPreflightData structured_memory_preflight{};
  u32 maximum_structured_iterations = 0;
  u32 maximum_structured_child_iterations = 0;
  u32 structured_store_count = 0;
  u32 structured_entry_pc = 0;
  u32 structured_tail_pc = 0;
  u32 structured_parent_prefix_pairs = 0;
  u32 structured_child_pairs = 0;
  u32 structured_suffix_pairs = 0;
  u32 structured_pair_upper_bound = 0;
  // Descriptor-only metadata for StructuredFixedQpNumeric. Such a module has
  // no runtime Cg source or generated-program key; the product binds this
  // immutable descriptor to the shared offline GXP.
  StructuredFixedQpNumericDescriptor structured_fixed_qp_numeric{};
  // Descriptor-only exact ADD/SUB/MUL batch bound to one shared offline GXP.
  StructuredFixedFmacNumericDescriptor structured_fixed_fmac_numeric{};

  // Host input-mode validation shared by GXM registration and native compiler
  // regressions. This grants no shader/resource/transaction attestation.
  bool HasValidDirectTfxInputMode() const;
  // Packed outer tables are BUFFER inputs, not per-vertex attributes. Validate
  // their marker separately while counting the ordinary attribute layout.
  bool GetDirectTfxVertexAttributeCount(u32* attribute_count) const;

  // These generated stages consume the PairPlan-derived child-entry state
  // through BUFFER11--13.  Their vf_uniform_mask/uses_*_uniform fields still
  // describe semantic snapshot dependencies, but they are not default-uniform
  // parameters and must never be looked up or uploaded as VFxx/ACC/Q/P/I.
  bool UsesStructuredSnapshotInputBuffers() const {
    return execution_kind ==
               GeneratedCgExecutionKind::StructuredExpressionScratch ||
           execution_kind == GeneratedCgExecutionKind::
                                 StructuredParallelChildMemoryStore ||
           execution_kind == GeneratedCgExecutionKind::
                                 StructuredParallelDirectVuTfx ||
           execution_kind == GeneratedCgExecutionKind::StructuredFinalState;
  }

  u32 BatchRawBindingVectorCount() const {
    return (static_cast<u32>(memory_inputs.size()) + 3u) / 4u;
  }
  u32 BatchBindingVectorCount() const {
    return BatchRawBindingVectorCount() +
           (uses_dynamic_batch_uniform_index ? 1u : 0u);
  }
  u32 BatchUniformVectorCount() const {
    u32 vectors = static_cast<u32>(constant_inputs.size());
    for (u32 reg = 1; reg < 32; reg++)
      vectors += (vf_uniform_mask & (1u << reg)) != 0;
    vectors += uses_acc_uniform ? 1u : 0u;
    vectors += (uses_q_uniform || uses_p_uniform || uses_i_uniform ||
                uses_gif_q_uniform)
                   ? 1u
                   : 0u;
    return vectors;
  }
  static u32 CountMaskBits(u64 bits) {
    u32 count = 0u;
    while (bits != 0u) {
      count += static_cast<u32>(bits & 1u);
      bits >>= 1u;
    }
    return count;
  }
  u32 BatchVaryingLiveInVectorCount() const {
    return CountMaskBits(batch_varying_live_ins.constant_mask) +
           CountMaskBits(batch_varying_live_ins.vf_mask) +
           static_cast<u32>(batch_varying_live_ins.acc) +
           static_cast<u32>(batch_varying_live_ins.scalars);
  }
  u32 BatchInvariantUniformVectorCount() const {
    const u32 varying = BatchVaryingLiveInVectorCount();
    const u32 total = BatchUniformVectorCount();
    return varying <= total ? total - varying : 0u;
  }
  bool UsesInlineBatchVaryingTail() const {
    return (loop_kernel_source_abi ==
                GeneratedLoopKernelPartialBatchCgAbiVersion ||
            loop_kernel_source_abi ==
                GeneratedLoopKernelPartialBatchProductCgAbiVersion ||
            loop_kernel_source_abi ==
                GeneratedLoopKernelNestedFlatPartialCgAbiVersion ||
            loop_kernel_source_abi ==
                GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion ||
            loop_kernel_source_abi ==
                GeneratedLoopKernelPartialStateCanaryCgAbiVersion) &&
           batch_varying_live_ins.Any();
  }
  bool RequiresExactPartialSinkSchedule() const {
    return loop_kernel_source_abi ==
               GeneratedLoopKernelPartialBatchCgAbiVersion ||
           loop_kernel_source_abi ==
               GeneratedLoopKernelNestedFlatPartialCgAbiVersion ||
           loop_kernel_source_abi ==
               GeneratedLoopKernelPartialStateCanaryCgAbiVersion;
  }
  bool UsesLazySinkScheduledBatchUniforms() const {
    return uses_sink_scheduled_outputs &&
           loop_kernel_source_abi !=
               GeneratedLoopKernelPartialBatchProductCgAbiVersion &&
           loop_kernel_source_abi !=
               GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion &&
           BatchUniformVectorCount() > SinkScheduledOutputUniformThreshold;
  }
  bool UsesLazySinkScheduledVaryingConstants() const {
    return uses_sink_scheduled_outputs &&
           (loop_kernel_source_abi ==
                GeneratedLoopKernelPartialBatchProductCgAbiVersion ||
            loop_kernel_source_abi ==
                GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion) &&
           UsesInlineBatchVaryingTail() &&
           batch_varying_live_ins.constant_mask != 0u;
  }
  u32 BatchRecordVectorCount() const {
    return BatchBindingVectorCount() +
           (uses_instance_indexed_batch_live_ins ? 0u
                                                 : BatchInvariantUniformVectorCount()) +
           (UsesInlineBatchVaryingTail()
                ? BatchVaryingLiveInVectorCount()
                : 0u);
  }
  u32 BatchInstanceLiveInVectorCount() const {
    return 0u;
  }
};

// Shader-local constant ordinals are not VU identities. Build this bounded
// projection once from PairPlan affine addresses; VF/ACC/scalars retain their
// architectural identities. No source strings, heap allocation or address
// comparisons are needed when projecting subsequent observations on the A9.
// Inputs absent from the destination are omitted (e.g. pruned store-only
// inputs, or constants lowered to per-object raw inputs at another capacity).
// This is NOT an all-input equivalence proof: capacity reuse still requires
// the PairPlan prefix proof and batching still compares invariant values.
class GeneratedBatchInputIdentityMap final {
public:
  bool Configure(const GeneratedCgProgram& source,
                 const GeneratedCgProgram& destination,
                 std::string* error = nullptr);
  bool Project(const GeneratedBatchVaryingLiveIns& source,
               GeneratedBatchVaryingLiveIns* destination,
               std::string* error = nullptr) const;

private:
  std::array<u8, 32> m_constant_destinations{};
  u32 m_source_constant_mask = 0u;
  u32 m_source_vf_mask = 0u;
  u32 m_shared_vf_mask = 0u;
  bool m_source_acc = false;
  bool m_shared_acc = false;
  bool m_source_scalars = false;
  bool m_shared_scalars = false;
  bool m_valid = false;
};

// Emits a vertex program which evaluates the proven parallel semantic slice
// and exposes each VU qword store as a varying. This is the debug/oracle root
// used to inspect ShaccCg allocation and compare generated values. The final
// fused root reuses the same expression emitter and replaces these store
// varyings with the existing TFX tail.
bool GenerateParallelStoreValidationCg(const ParallelLoopKernel &kernel,
                                       GeneratedCgProgram *program,
                                       std::string *error);

// Emits the child-loop exit values requested by the enclosing-loop liveness
// proof to writable GPU scratch buffers.  Binding one invocation to the final
// affine iteration computes the exact recurrence-free child result without
// executing its PairPlans serially.  This is the state half of the structured
// generated provider; ordered store/PATH1 publication remains a separate
// admission contract.
bool GenerateParallelFinalStateCg(const ParallelLoopKernel& kernel,
                                  GeneratedCgProgram* program,
                                  std::string* error);

// Emits one bounded invocation which retains the enclosing loop's carried
// state locally, applies the proven parent-prefix transition repeatedly, and
// writes one sparse child-entry snapshot per outer iteration. This first
// structured form requires the child to preserve every boundary-live value;
// more general child transitions remain a separate lowering.
bool GenerateStructuredLoopStateCg(
    const ParallelLoopKernel& transition_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 parent_entry_pc,
    u32 parent_prefix_pair_count, u32 maximum_iterations,
    u32 maximum_child_iterations,
    GeneratedCgProgram* program, std::string* error);

// Compiler-bounded form of the same serial parent-state transition. The
// selected child-entry destinations are published by this root; the planner
// automatically retains the transitive parent-state recurrence closure needed
// to produce those destinations across every outer iteration. Different roots
// write disjoint VF/ACC/scalar snapshot locations. Exactly one slice must own
// shared VI/control publication; the others can be queued beside it without
// overlapping writable-buffer stores.
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
    bool publish_shared_control = true);

// Builds the recurrence-closed semantic slice used by both the generated Cg
// diagnostic emitter and the offline fixed structured-state evaluator.  The
// latter serializes this shared PairPlan-derived graph as data, so runtime
// ShaccCg never owns architectural state recurrence.
bool BuildStructuredLoopStateSliceKernel(
    const ParallelLoopKernel& transition_kernel,
    const std::array<u8, 32>& child_vf_lanes, u8 child_acc_lanes,
    bool child_q, bool child_p, bool child_i,
    ParallelLoopKernel* slice, std::string* error = nullptr);

// Checks the concrete child-entry VI snapshots produced by the serial state
// root before any private-memory invocation runs. The preflight permits
// sequential dependencies among child iterations owned by one invocation,
// but rejects any access conflict between concurrently executing outer
// invocations. Keeping this bounded address proof in its own root prevents
// ShaccCg 3.0 from compiling the large mutable transition DAG and the nested
// memory walk as one O0 program.
bool GenerateStructuredLoopMemoryPreflightCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, bool enable_runtime_checks,
    GeneratedCgProgram* program, std::string* error);

// Emits one invocation per bounded outer iteration. Each invocation consumes
// its private child-entry snapshot and executes child iterations serially,
// making a store visible to later loads in that same child chain. Different
// outer invocations remain parallel only after preflight proves that their
// read/write qwords do not conflict. BUFFER3 is the private transactional
// VU-memory generation; canonical state and PATH1 remain unpublished.
bool GenerateStructuredLoopParallelChildMemoryStoreCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, GeneratedCgProgram* program,
    std::string* error);

// Emits a compact direct-output root for a nested parallel child loop. The
// preceding serial state root publishes one immutable child-entry snapshot
// per enclosing iteration; this root maps the dense INDEX range to exact
// (outer, child) coordinates and lowers the child store expressions directly
// to the existing VU+TFX interface. The child trip count must be a proven
// compile-time constant so no inactive holes enter GS primitive assembly.
bool GenerateStructuredLoopParallelDirectTfxCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 child_iteration_count, const DirectTfxContract& contract,
    GeneratedCgProgram* program, std::string* error = nullptr);

// Emits one invocation per bounded outer/child slot. The state root proves
// unique qword ownership before this stage, allowing each invocation to apply
// its fixed store sites without a cross-invocation read/modify/write race.
// BUFFER13 is the private transactional VU-memory generation; no canonical
// state or PATH1 output is published by this stage.
bool GenerateStructuredLoopStoreCommitCg(
    u32 maximum_outer_iterations, u32 maximum_child_iterations,
    u32 store_count, GeneratedCgProgram* program, std::string* error);

// Evaluates the complete recurrence-free child exit state for only the final
// outer/child iteration. It consumes the private snapshots and committed VU
// memory, then publishes canonical VF/ACC/Q/P/I into BUFFER1/2. VI/control and
// the post-loop tail remain later stages of the same private transaction.
bool GenerateStructuredLoopFinalStateCg(
    const ParallelLoopKernel& complete_child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, GeneratedCgProgram* program,
    std::string* error);

// Compiler-bounded form of the same final-state publication. VF slices write
// disjoint canonical registers; exactly one control slice publishes
// ACC/Q/P/I/VI and resumes the exact fixed-GXP tail. All slices consume the
// same immutable snapshots and committed private VU memory.
bool GenerateStructuredLoopFinalStateSliceCg(
    const ParallelLoopKernel& complete_child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, u32 vf_register_mask,
    bool publish_control_state, GeneratedCgProgram* program,
    std::string* error);

// Emits an already-sliced final-state kernel produced by the compiler-budget
// partitioner. Unlike the public canonical slicer above, this entry accepts
// zeroed unselected destinations and StructuredScratch leaves, but retains the
// same private-generation/output contract and never changes semantic support.
bool GenerateStructuredLoopPreparedFinalStateCg(
    const ParallelLoopKernel& prepared_slice,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, bool publish_control_state,
    GeneratedCgProgram* program, std::string* error = nullptr);

struct StructuredStoreJournalEntry {
  u32 packed_address = 0xffffffffu;
  std::array<u32, 4> value{};
};

// Host oracle for the generated commit stage. Validation is deliberately
// performed against a copy so malformed metadata cannot partially alter the
// caller's private generation before rejection.
bool ApplyStructuredStoreJournalReference(
    const std::vector<StructuredStoreJournalEntry>& journal,
    u32 active_entry_count,
    std::array<std::array<u32, 4>, 1024>* vu_memory,
    std::string* error = nullptr);

// Emits one compiler-partition producer. The kernel's
// structured_scratch_outputs are the only roots; later modules consume them
// through ExpressionKind::StructuredScratch after a bounded GXM job boundary.
bool GenerateStructuredLoopExpressionScratchCg(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, GeneratedCgProgram* program,
    std::string* error = nullptr);

// Returns a power-of-two dispatch stride when the child IBNE trip count is a
// nonzero constant after enclosing-prefix VI lowering. Runtime-dependent trip
// counts remain unsupported by this compact journal allocation contract.
bool ComputeStructuredChildIterationCount(
    const ParallelLoopKernel& transition_kernel,
    const NaturalLoop& child_loop, u32* count,
    std::string* error = nullptr);

bool ComputeStructuredChildIterationStride(
    const ParallelLoopKernel& transition_kernel,
    const NaturalLoop& child_loop, u32* stride,
    std::string* error = nullptr);

// Emits the primary playable root: the VU semantic slice feeds the existing
// TFX vertex tail directly, with no VU output buffer or CPU vertex expansion.
bool GenerateParallelTfxCg(const ParallelLoopKernel &kernel,
                           const DirectTfxContract &contract,
                           GeneratedCgProgram *program,
                           std::string *error);

// Emits the same PairPlan-derived static expression body as the proven direct
// path, but under the hardware-driven loop-kernel ABI/key namespace.  The
// result is one direct-TFX root with compact raw-input/default-uniform live-ins
// and no snapshot, scratch, precompute, or continuation ABI. Transactional
// BUFFER2 stores and the optional private DIV differential on BUFFER3 are
// outputs of that same invocation, never producer jobs or canonical state.
// Runtime ownership and final-state formulas are validated by
// GeneratedLoopKernelBundle before this source may be selected.
bool GenerateLoopKernelDirectTfxCg(const ParallelLoopKernel& kernel,
                                   const DirectTfxContract& contract,
                                   GeneratedCgProgram* program,
                                   std::string* error = nullptr);
bool GenerateLoopKernelDirectTfxCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins,
    GeneratedCgProgram* program, std::string* error = nullptr,
    const GeneratedCgProgram* varying_input_layout = nullptr);

// Evaluates every loop-store cone on the dense outer/child grid, without a
// raster cone or ADC filtering. Only the private BUFFER2 journal is meaningful;
// the fixed point/varying outputs must never be rendered. The caller retains
// the original output contract and PairPlan successor proofs independently.
bool GenerateLoopKernelStateCanaryCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins,
    GeneratedCgProgram* program, std::string* error = nullptr,
    const GeneratedCgProgram* varying_input_layout = nullptr);

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
// Conservative producer fact, for the supported nearest/DAZ/FZ/clamp profile.
// Native FMAC, raw leaves and zero-trip recurrence results are not normalized
// by construction. The caller must separately prove the numeric profile.
bool IsGeneratedCgNormalizedF32Result(const ExpressionNode& node, bool exact_fmac);
// Offline mirror of the range-scaled SGX multiply candidate. The floating
// product assumption still needs physical instruction/output attestation.
bool EvaluateGeneratedCgRangeScaledMultiplyReference(
    u32 left_bits, u32 right_bits, u32 configuration_bits, u32* result_bits);
// Offline compiler/resource experiment only. No Vita caller or provider can
// select this source before independent oracle and physical validation.
// raster_output_only removes the private journal, never its correctness gate.
// A full-size zero mask gives a matched numeric raster control; an empty mask
// selects the complete proven depth cone. Neither is the lean product profile.
bool GenerateLoopKernelDepthPrecisionAnalysisCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins, bool state_canary,
    GeneratedCgProgram* program, std::string* error,
    const GeneratedCgProgram* varying_input_layout,
    std::span<const u8> exact_fmac_nodes = {}, bool range_scaled_multiply = false,
    bool normalized_fmac_inputs = false, bool raster_output_only = false);
#endif

// Emits the compiler-generated product executable for a previously attested
// loop contract. Persistent VU state remains transactionally derived from the
// shared PairPlan successor; this shader publishes raster output only and has
// no writable diagnostic buffers.
// When varying_input_layout is supplied, varying_live_ins uses that source
// layout's ordinals and is projected after resource discovery, before source
// emission. Otherwise the mask already names this generated root's inputs.
bool GenerateLoopKernelProductDirectTfxCg(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const GeneratedBatchVaryingLiveIns& varying_live_ins,
    GeneratedCgProgram* program, std::string* error = nullptr,
    const GeneratedCgProgram* varying_input_layout = nullptr);

} // namespace VitaGpuVu
