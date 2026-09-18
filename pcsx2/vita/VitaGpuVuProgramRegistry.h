// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuCgGenerator.h"
#include "vita/VitaGpuVuShaderCompiler.h"

#include <functional>

namespace VitaGpuVu {

// Bump whenever generated source conventions or the GXM binding contract
// change. The key is exclusively generated-program content; it never contains
// a title, ELF, address, game, or known-program identity.
constexpr u32 GeneratedProgramAbiVersion = 55;

// Runtime ShaccCg executes inside VitaSX2's process. A malformed or merely
// optimizer-hostile root can therefore terminate the emulator instead of
// returning an ordinary compiler error. The old 24 KiB direct roots retained
// the complete VF file plus a 128-qword forwarding journal as per-invocation
// temporaries and faulted or spilled. ABI 34 writes the transaction-private
// source bank in place and lets Sony's O1 allocator choose the live VF subset.
// Physical ShaccCg 3.0 data-aborted on both 31,587- and 30,648-byte BSpline
// O1 roots, after a smaller root had already consumed 12.1 MiB of the private
// 16 MiB compiler arena. ABI 36 removes the dynamic four-lane commit helper
// from every PairPlan-known native FMAC write, replacing it with direct static
// vector/lane assignments. Its named Approximate-P profile also uses native
// SGX reciprocal/square-root arithmetic instead of inlining Cortex-A9 estimate
// refinement and binding its 2.5 KiB table. ABI 37 gives native SGX FTOI/ITOF
// their own named experimental identity. Resource A/B narrowed that profile
// to FTOI/ITOF: native MIN/MAX raised the captured loop from 118 to 124
// temporaries and introduced shader scratch, while exact bit-order MIN/MAX
// plus native conversion emits a 26,954-byte/137-pair source root. ABI 38
// selects O2 for this private in-place class: offline SDK 3.570 reduces the
// exact root from O1's 15,592-byte/1,712-primary-instruction GXP to a
// 14,768-byte/1,615-primary-instruction GXP with zero scratch at both levels;
// O3 instead requires 54,272 bytes of scratch per thread. Physical SDK 3.0
// ShaccCg must still prove that O2 avoids the O1 optimizer abort seen in ABI
// 37. ABI 40 selected exactly one generated module from the private TPC instead
// of submitting every module. ABI 41 split multi-block SCCs at canonical
// basic-block visibility boundaries after a 28,313-byte cyclic root
// prefetch-aborted ShaccCg 3.0 and exposed stale read/write-uniform VF state.
// Physical BSpline r85 then proved that this shape is not a performance
// provider: one Execute paid about 22 serialized module/firmware transitions.
// ABI 42 keeps every multi-block SCC in one automatically generated root,
// compacts its Cg before compiler admission, and retains cross-block VF values
// in shader-local state. Split cyclic bundles can still describe a compiler
// fallback, but product admission rejects them in favour of CPU MTVU. Keep
// every individual source strictly below the smallest observed in-process
// compiler abort. A 23,424-byte block-threaded root and a compact 22,051-byte
// direct-TFX root both prefetch-aborted inside SceShaccCg. Physical ShaccCg
// and GXM attestation still decide whether a result may replace CPU MTVU.
// This is a compiler-resource bound, never a semantic or workload whitelist.
// ABI 43 adds the nested-loop direct-TFX generated tier. Its dense invocation
// mapping uses compiler-safe multiply/shift division, and its source is
// compacted before Shacc admission. The root consumes transaction-private
// enclosing-loop snapshots and emits TFX varyings directly; it is not a fixed
// universal interpreter and never carries a workload identity.
// ABI 44 introduced compiler-bounded direct-expression cuts. ABI 45 carries
// every cut's exact BUFFER10 dependency stage in the immutable draw descriptor.
// Up to eight title-neutral producer modules may share one vertex firmware job
// when their read/write masks prove independence, followed by one fused
// generated VU+TFX consumer. Every cut remains a pure PairPlan expression
// boundary; CPU MTVU remains authoritative until the complete generated plan
// is compiled, registered, and resource-attested. ABI 46 adds the private
// loop-kernel DIV differential BUFFER3 contract; it is diagnostic output only
// and cannot satisfy product ownership. ABI 47 moves that private boundary to
// the first stored rounded-multiply/FTOI chain so physical evidence can
// distinguish FMAC input drift from conversion drift. ABI 48 makes that
// boundary genuinely exact on Series5XT: the emitted FTOI reconstructs its
// signed result from raw binary32 exponent/significand bits and never invokes
// Cg's optimization-sensitive native float-to-int conversion. Physical ABI
// 48 then exposed a Shacc 3.0 integer-predicate miscompile: three true range
// predicates became 0xffffffff before multiplication, collapsing the final
// mask to one bit. ABI 49 derives FTOI direction and range masks solely from
// integer sign bits, with no comparison-to-integer conversion in the helper.
// ABI 50 adds a second private BUFFER3 qword per probed invocation. It compares
// a lower-cost, floor-of-safe-magnitude FTOI candidate with the ABI-49 raw-bit
// owner and PCSX2 before that candidate may replace any architectural store.
inline constexpr size_t RuntimeGeneratedDirectCompilerSourceBytes =
    22u * 1024u;
inline constexpr u32 RuntimeGeneratedDirectCompilerExpressionCount = 512u;
// Static-flow loop kernels are a separately attested compiler class.  The
// captured 5x24 closed-form corpus compiles with patched Sony SDK 3.570 at O3
// to an 11,492-byte, parallel-mode, zero-spill GXP (90 temporaries, 1,197
// estimated cycles). ABI 13's packed FTOI lowering also compiled at O1 to a
// 19,340-byte, parallel-mode, zero-spill GXP, but physical Shacc execution
// disproved its component-wise integer-vector semantics. ABI 14 retained one
// full-qword assignment while using the scalar FTOI helper for each lane;
// physical execution disproved that constructor shape too. ABI 15 published
// every FTOI lane as a separate scalar SSA value, but physical execution
// showed that Shacc re-vectorized those values at the final int4 journal
// assignment. ABI 16 preserves the qword byte layout while publishing each
// lane through an independently addressed scalar BUFFER2 word. ABI 17 honors
// the configuration-keyed approximate-conversion profile for the product DAG,
// retaining the exact raw-bit conversion only in the private diagnostic probe.
// ABI 18 canonicalizes every comparison result to one bit before constructing
// an integer select mask. Physical ABI-17 disassembly proved that Shacc 3.0
// materializes true as 0xffffffff and that multiplying that value by -1 yields
// the one-bit mask 0x00000001, corrupting loop recurrence, DIV, Q/P, and clamp
// selection despite a structurally valid zero-spill GXP.
// ABI 19 retains that mask repair and adds the isolated playable-FTOI result;
// two int4 BUFFER3 records are written for each validation invocation.
// ABI 20 promotes the physically attested floor-of-safe-magnitude FTOI
// lowering into the static loop-kernel store DAG while retaining the exact
// raw-bit result in BUFFER3 as a private oracle boundary.
// ABI 21 reserves one host-zeroed batch-record vector and dynamically XORs it
// through each native rounded-multiply result. Pulled ABI-20 GXP disassembly
// proved that an ordinary raw-bit round trip was optimized away and 77 mad.f32
// instructions crossed PairPlan operation boundaries; the dynamic integer
// dependency is the compiler-visible Series5 rounding barrier.
// The 52 KiB ceiling accommodates the captured source shape without relaxing
// the smaller dynamic/state-machine compiler classes.
// Runtime Shacc/GXM registration and private physical comparison remain
// separate gates; this limit only permits that title-neutral static class to
// enter the asynchronous compiler queue while CPU MTVU stays authoritative.
inline constexpr size_t RuntimeGeneratedLoopKernelCompilerSourceBytes =
    52u * 1024u;
inline constexpr u32 RuntimeGeneratedLoopKernelCompilerExpressionCount = 768u;
// BUFFER3's private FTOI diagnostic retains the multiply operands and both
// conversion results until the final stores.  Physical ABI-24 disassembly
// shows that this tail is harmless for the 11-vector live-in root, while the
// 27-vector root reaches 122 temporaries and spills 4 KiB per thread.  The
// complete BUFFER2 journal plus exact architectural-state comparison remains
// the semantic attestation for larger roots; only the explanatory probe is
// omitted.  This is a Sony-compiler resource class, never a program-support
// or title admission rule.
inline constexpr u32
    RuntimeGeneratedLoopKernelDiagnosticProbeMaximumUniformVectors = 16u;
constexpr bool GeneratedLoopKernelDiagnosticProbeFitsCompilerInputClass(
    u32 uniform_vectors) {
  return uniform_vectors <=
         RuntimeGeneratedLoopKernelDiagnosticProbeMaximumUniformVectors;
}
// This is the architectural Execute bound carried by a generated loop/SCC
// root, not a promise that every such root is watchdog-safe on hardware.
// Product promotion independently requires a measured low-job plan and the
// exact compiled GXP resource/runtime attestation. Keeping the bound in the
// generated ABI lets ordinary loops stay inside one root instead of forcing a
// visibility job every 128 pairs.
inline constexpr u32 RuntimeGeneratedDirectMaximumDynamicPairs = 16384u;

// Physical Vita attestation for the in-process SDK 3.0 compiler plus offline
// SDK 3.570 optimizer characterization. A 12,277-byte/282-expression BSpline
// store root failed to finish O1 compilation after three minutes; forcing O0
// emitted a 10,132-cycle, 94-temp per-instance program. A 5,564-byte/
// 32-expression sink compiles immediately at O1 into a 109.5-cycle parallel
// program. Keep every outer-parallel root inside the smaller SSA envelope so
// the canonical DAG planner inserts scratch cuts before compiler entry. This
// is a compiler-resource classification only: CPU MTVU remains the semantic
// provider when it rejects.
inline constexpr size_t RuntimeStructuredParallelCompilerSourceBytes =
    8u * 1024u;
inline constexpr u32 RuntimeStructuredParallelCompilerExpressionCount = 96u;
// Direct native vector arithmetic has no scalar software-F32 helper graph.
// It may retain a larger architectural DAG while staying below the measured
// source shape that made O1 non-terminating before the helper simplification.
inline constexpr size_t RuntimeStructuredNativeCompilerSourceBytes =
    16u * 1024u;
inline constexpr u32 RuntimeStructuredNativeCompilerExpressionCount = 320u;
// A direct-TFX root has no writable-buffer sink, but it still enters the same
// in-process optimizer as every other generated root. Physical r81 proved
// that the compact 22,051-byte/491-expression BSpline root can prefetch-abort
// inside SceShaccCg before producing a GXP even though patched offline
// psp2cgc accepts it. Keep the final direct consumer in the already attested
// native-root envelope. The title-neutral DAG planner must publish pure
// boundary expressions through transaction-private scratch until the final
// root fits; increasing this ceiling or retrying another optimization level is
// not an eligible product fallback.
inline constexpr size_t RuntimeStructuredDirectCompilerSourceBytes =
    RuntimeStructuredNativeCompilerSourceBytes;
inline constexpr u32 RuntimeStructuredDirectCompilerExpressionCount =
    RuntimeStructuredNativeCompilerExpressionCount;
// A direct precompute root has no TFX tail and only publishes pure scalar
// values through BUFFER10. Lane-isomorphic outputs can therefore share one
// compact source even when their canonical scalar DAG contains more nodes
// than the final-root class. The 16 KiB source ceiling still applies, and the
// compiled GXP must independently attest zero spill/scratch allocation before
// product registration.
inline constexpr u32 RuntimeStructuredDirectPrecomputeCompilerExpressionCount =
    512u;
inline constexpr size_t RuntimeStructuredDirectPrecomputeCompilerSourceBytes =
    RuntimeStructuredDirectCompilerSourceBytes;
// Exact software-F32 roots carry a fixed helper prelude of roughly 9.5 KiB
// even when only a handful of semantic expressions remain. They compile at
// O0 under a separately attested resource class, so give that prelude enough
// room without reopening the much larger O1 SSA envelope above.
inline constexpr size_t RuntimeStructuredExactCompilerSourceBytes =
    12u * 1024u;
inline constexpr u32 RuntimeStructuredExactCompilerExpressionCount = 96u;
// Offline SDK 3.5/3.570 compiler characterization: a generated root with 49
// inlined exact F32 boundaries exceeded 30 seconds, while the standalone
// title-neutral add/multiply kernels compile in about 0.14 seconds. Keep each
// runtime root at a conservative initial ceiling; automatic DAG partitioning
// carries larger programs through multiple dependency-ordered modules.
inline constexpr u32 RuntimeStructuredSoftwareF32OperationCount = 24u;

// Compiler-safe source/SSA limits are not sufficient for writable structured
// sinks.  Physical BSpline r43 safely compiled a 19,056-byte scratch producer,
// then the 22,203-byte final root which published 31 VF vectors plus control
// state took the console/FTP services offline before returning a compiler
// result.  Bound architectural outputs independently so the title-neutral DAG
// planner emits several dependency-ordered store/final roots instead of ever
// submitting that wide sink to in-process ShaccCg.  These are compiler resource
// classes only; they do not decide semantic support.
inline constexpr u32 RuntimeStructuredStoreMaximumQwordWrites = 8u;
// The captured native BSpline DAG now uses branch-free lane-isomorphic
// normalization, which makes every four-vector O1 final sink compile and pass
// psp2shaderperf (the former VF21--VF24 sink reports 667.5 cycles). Repeating
// the title-neutral plan at widths five, six, and eight still produced one GXP
// at each width which psp2shaderperf could not decode within 60 seconds.
// Four-vector sinks therefore remain the measured compiler resource class;
// shared expression scratch retains common work. A fixed descriptor-driven
// final commit kernel can retire these extra single-invocation boundaries.
inline constexpr u32 RuntimeStructuredFinalMaximumOutputVectors = 4u;
// Sony's offline 3.570 compiler accepted the previously generated 88- and
// 116-output scratch roots, but psp2shaderperf reports both spilling to memory;
// the 116-output root alone reserves roughly 380 KiB of target memory. Source,
// expression and exact-F32 counts therefore do not fully describe this
// compiler resource class. Partition scratch publications before compilation
// as well. This bound is title-neutral and affects cache/source partitioning,
// never semantic support.
inline constexpr u32 RuntimeStructuredScratchMaximumOutputs = 64u;

// The serial structured-state root is a distinct Shacc resource class from
// the outer-parallel store/final roots above. Physical BSpline r26 faulted on
// one 16,862-byte/119-expression root, r29 faulted on a 10,671-byte slice, and
// r31 faulted on a 7,513-byte/57-expression root which merged two otherwise
// independent VF snapshot vectors. Runtime state roots therefore own at most
// one independently writable snapshot vector. This is a title-neutral
// compiler partition contract, not a byte-threshold retry: every root remains
// recurrence-closed and helper-pruned, while the fixed provider owns any
// indivisible closure outside the remaining conservative envelope.
inline constexpr size_t RuntimeStructuredStateCompilerSourceBytes =
    12u * 1024u;
inline constexpr u32 RuntimeStructuredStateCompilerExpressionCount = 64u;
inline constexpr u32 RuntimeStructuredStateSoftwareF32OperationCount = 8u;
inline constexpr u32 RuntimeStructuredStateMaximumSnapshotVectors = 1u;

// The memory preflight is a fixed-shape generated source with a bounded
// pointer-free descriptor. It is compiled and cached like every other hot-tier
// module; this limit prevents an accidental source substitution from reaching
// in-process ShaccCg.
inline constexpr size_t RuntimeStructuredPreflightCompilerSourceBytes =
    12u * 1024u;

// Metadata-only private POINTS interface. Does not grant source/resource
// admission or authorize rendering; source text may already be reclaimed.
bool HasGeneratedLoopKernelStateCanaryContract(const GeneratedCgProgram& program);
bool IsGeneratedProgramRuntimeCompilerShapeAttested(
    const GeneratedCgProgram& program);

enum class GeneratedProgramState : u8 {
  Missing,
  Queued,
  Compiled,
  Ready,
  Failed,
  Unavailable,
};

// A compiled GXP is only an artifact.  Generated loop kernels become an
// execution provider after the exact source/configuration contract has also
// completed one private physical comparison.  Keep that result beside the
// content-keyed registry entry so GS retirement can publish it once and MTVU
// can consume it without rebuilding the PairPlan epoch.
struct GeneratedLoopKernelAttestationIdentity {
  ShaderKey key{};
  u32 generated_program_abi = 0u;
  u32 loop_kernel_abi = 0u;
  u32 configuration_bits = 0u;
  u32 semantic_profile_key = 0u;

  constexpr bool IsValid() const {
    return (key.low != 0u || key.high != 0u) &&
           generated_program_abi == GeneratedProgramAbiVersion &&
           loop_kernel_abi != 0u;
  }

  bool operator==(
      const GeneratedLoopKernelAttestationIdentity& other) const {
    return key == other.key &&
           generated_program_abi == other.generated_program_abi &&
           loop_kernel_abi == other.loop_kernel_abi &&
           configuration_bits == other.configuration_bits &&
           semantic_profile_key == other.semantic_profile_key;
  }
};

enum class GeneratedLoopKernelAttestationState : u8 {
  Unattested,
  PrivateTest,
  Passed,
  Rejected,
  Product,
};

enum class GeneratedLoopKernelNumericProfile : u8 {
  None,
  ExactVu,
  NativeSgxOutputOnly,
};

// Physical Series5XT evidence for the first native-output-only product gate.
// This policy is deliberately operation/domain based rather than tied to a
// title, PC, source hash, or captured packet. It authorizes native SGX values
// only for the direct-output store lanes proven by physical comparison; all
// control/register successor state remains PairPlan-exact.
// The Series5XT private-store comparison observed a maximum three-ULP delta
// for Normalize-derived direct-output lanes while every architectural lane and
// the exact CPU journal remained identical.  Keep the physical bound exact:
// this is not permission to approximate values which can feed VU/GS control.
inline constexpr u32 NativeSgxOutputOnlyMaximumFloatUlp = 3u;
inline constexpr u32 NativeSgxOutputOnlyMaximumFixedIntegerDelta = 256u;
inline constexpr const char* NativeSgxOutputOnlyPolicyName =
  "native-sgx-normalize-ulp3+ftoi-delta256-v2";

struct GeneratedLoopKernelNativeOutputOnlyEvidence {
  u32 exact_mismatch_lanes = 0u;
  u32 non_output_only_mismatch_lanes = 0u;
  u32 unsupported_mismatch_lanes = 0u;
  u32 cpu_exact_journal_mismatch_lanes = 0u;
  u32 maximum_float_ulp_delta = 0u;
  u32 maximum_fixed_integer_delta = 0u;
  bool architectural_state_exact = false;
  bool numeric_probe_exact = false;
};

constexpr bool GeneratedLoopKernelNativeOutputOnlyCanaryPasses(
    const GeneratedLoopKernelNativeOutputOnlyEvidence& evidence) {
  return evidence.exact_mismatch_lanes != 0u &&
         evidence.non_output_only_mismatch_lanes == 0u &&
         evidence.unsupported_mismatch_lanes == 0u &&
         evidence.cpu_exact_journal_mismatch_lanes == 0u &&
         evidence.maximum_float_ulp_delta <=
             NativeSgxOutputOnlyMaximumFloatUlp &&
         evidence.maximum_fixed_integer_delta <=
             NativeSgxOutputOnlyMaximumFixedIntegerDelta &&
         evidence.architectural_state_exact && evidence.numeric_probe_exact;
}

enum class GeneratedLoopKernelAttestationRejection : u8 {
  None,
  RetirementCancelled,
  InvalidDescriptor,
  OutputGuard,
  ExactStoreMismatch,
  ExactCanonicalJournalMismatch,
  PlayableStoreMismatch,
  ArchitecturalStateMismatch,
  OutputOnlyClassificationMissing,
  NumericProbeMismatch,
};

struct GeneratedLoopKernelAttestationRecord {
  GeneratedLoopKernelAttestationIdentity identity{};
  GeneratedLoopKernelAttestationState state =
      GeneratedLoopKernelAttestationState::Unattested;
  GeneratedLoopKernelNumericProfile numeric_profile =
      GeneratedLoopKernelNumericProfile::None;
  GeneratedLoopKernelAttestationRejection rejection =
      GeneratedLoopKernelAttestationRejection::None;
};

// Pure transition owners are exposed so the ARMv7 differential suite can
// prove stale-key and stale-profile completions are non-mutating without
// creating a runtime compiler.
bool BeginGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity);
bool CompleteGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity,
    GeneratedLoopKernelNumericProfile numeric_profile,
    GeneratedLoopKernelAttestationRejection rejection);
bool PromoteGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity);
bool CancelGeneratedLoopKernelAttestationRecord(
    GeneratedLoopKernelAttestationRecord* record,
    const GeneratedLoopKernelAttestationIdentity& identity);

GeneratedLoopKernelAttestationRecord QueryGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity);
bool BeginGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity);
bool CompleteGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity,
    GeneratedLoopKernelNumericProfile numeric_profile,
    GeneratedLoopKernelAttestationRejection rejection =
        GeneratedLoopKernelAttestationRejection::None);
bool PromoteGeneratedLoopKernelAttestationToProduct(
    const GeneratedLoopKernelAttestationIdentity& identity);
bool CancelGeneratedLoopKernelAttestation(
    const GeneratedLoopKernelAttestationIdentity& identity);

const char* GeneratedLoopKernelAttestationStateName(
    GeneratedLoopKernelAttestationState state);
const char* GeneratedLoopKernelNumericProfileName(
    GeneratedLoopKernelNumericProfile profile);
const char* GeneratedLoopKernelAttestationRejectionName(
    GeneratedLoopKernelAttestationRejection rejection);

constexpr bool GeneratedProgramStateIsPending(GeneratedProgramState state) {
  return state == GeneratedProgramState::Queued ||
         state == GeneratedProgramState::Compiled;
}

// A cached cold-provider dependency must stop polling once the compiler or
// registration has made the key terminal. Missing is terminal here because a
// wait is installed only after this exact key was observed Queued/Compiled;
// seeing it disappear means the registry generation was invalidated.
constexpr bool GeneratedProgramStateIsTerminallyUnavailable(
    GeneratedProgramState state) {
  return state == GeneratedProgramState::Missing ||
         state == GeneratedProgramState::Failed ||
         state == GeneratedProgramState::Unavailable;
}

struct ProgramRegistryStatistics {
  u64 requests = 0;
  u64 unavailable_requests = 0;
  u64 cache_hits = 0;
  u64 cache_misses = 0;
  u64 compiler_queue_retries = 0;
  u64 compile_successes = 0;
  u64 compile_failures = 0;
  u64 ready_programs = 0;
  u64 failed_programs = 0;
  u64 compiler_safety_rejections = 0;
  ShaderCompilerStatistics compiler;
};

// Stable 128-bit content key used only to coalesce/invalidate generated GXP.
ShaderKey MakeGeneratedProgramKey(const GeneratedCgProgram &program);

// GSDeviceGXM owns the compiler lifetime. These calls merely expose its
// bounded asynchronous queue to the EE producer; compilation remains
// serialized on the low-priority compiler thread.
bool AttachGeneratedProgramCompiler(ShaderCompiler *compiler);
void DetachGeneratedProgramCompiler(ShaderCompiler *compiler);

// Moves source into the compiler queue and retains only resource metadata.
// Duplicate content is coalesced. This call never waits for compilation.
bool RequestGeneratedProgram(GeneratedCgProgram program, ShaderKey *key);
GeneratedProgramState QueryGeneratedProgram(const ShaderKey &key);

// Runs PairPlan/source partitioning on the same low-priority worker which
// owns ShaccCg. The identity is exact source/configuration provenance used
// only to coalesce duplicate planning requests; it never decides support.
bool RequestGeneratedProgramPlanning(u64 identity,
                                     std::function<void()> work,
                                     std::function<void()> cancel = {});
bool IsGeneratedProgramPlanning(u64 identity);

// Monotonic token advanced only when the GS owner has consumed a completed
// compiler result, pumped every dependency-ordered structured bundle, and no
// generated root remains in flight. MTVU uses this lock-free idle transition
// to wake a structured request deliberately deferred behind another bundle.
// A per-root completion token would make unrelated identities re-analyze once
// for every module in a large bundle.
u64 GetGeneratedProgramCompilerIdleGeneration();
bool GeneratedProgramCompilerHasInFlightWork();

// Shared product/test policy for the two transient generated dependencies: an
// exact key becoming Ready, or a true compiler-idle transition after the
// generation observed at a pre-effect deferral. A missing key with an
// unchanged idle generation is intentionally not ready; this is the case which previously
// stranded BSpline in the permanent MTVU dispatch-cost cache.
constexpr bool GeneratedDispatchDependencyIsReady(
    bool wait_for_generated_program,
    GeneratedProgramState generated_program_state,
    bool wait_for_compiler_idle,
    u64 observed_compiler_idle_generation,
    u64 current_compiler_idle_generation,
    bool compiler_has_in_flight_work) {
  return (wait_for_generated_program &&
          generated_program_state == GeneratedProgramState::Ready) ||
         (wait_for_compiler_idle && !compiler_has_in_flight_work &&
          observed_compiler_idle_generation !=
              current_compiler_idle_generation);
}

// Compiler completion is only an artifact state. Product execution also
// requires the descriptor to own every guest-visible successor through the
// sequence-numbered transaction. Keeping this distinction explicit prevents a
// compiler-only root from waking MTVU into an expensive per-invocation
// construction/rejection loop while CPU MTVU is still authoritative.
constexpr bool GeneratedLoopKernelProductExecutionIsReady(
    GeneratedProgramState generated_program_state,
    bool is_no_write_product,
    bool has_atomic_compiler_ownership,
    bool has_transactional_successor_owner,
    bool has_product_resource_attestation,
    GeneratedLoopKernelAttestationState attestation_state) {
  return generated_program_state == GeneratedProgramState::Ready &&
         is_no_write_product &&
         has_atomic_compiler_ownership &&
         has_transactional_successor_owner &&
         has_product_resource_attestation &&
         attestation_state == GeneratedLoopKernelAttestationState::Product;
}

// A compiler-ready partial-batch root is an optional private canary, not a new
// semantic provider.  GeneratedLoopKernelOptionalExecutableMayOverrideBase()
// includes the passed state because the asynchronous pump uses it to derive
// the matching no-write product.  Hot executable selection must instead use
// GeneratedLoopKernelOptionalCanaryNeedsPrivateAttestation(): once the canary
// passes, it must never replace a smaller already-attested product while the
// derived product is compiling.  Physical BSpline r266/r267 otherwise ran the
// 18 KiB, roughly 2,100-cycle exact canary in large sustained GXM jobs.
constexpr bool GeneratedLoopKernelOptionalExecutableMayOverrideBase(
    GeneratedProgramState generated_program_state,
    bool has_product_resource_attestation,
    GeneratedLoopKernelAttestationState attestation_state) {
  return generated_program_state == GeneratedProgramState::Ready &&
         (attestation_state ==
              GeneratedLoopKernelAttestationState::Unattested ||
          attestation_state ==
              GeneratedLoopKernelAttestationState::PrivateTest ||
          (has_product_resource_attestation &&
           (attestation_state ==
                GeneratedLoopKernelAttestationState::Passed ||
           attestation_state ==
                GeneratedLoopKernelAttestationState::Product)));
}

constexpr bool GeneratedLoopKernelOptionalCanaryNeedsPrivateAttestation(
    GeneratedProgramState generated_program_state,
    GeneratedLoopKernelAttestationState attestation_state) {
  return generated_program_state == GeneratedProgramState::Ready &&
         (attestation_state ==
              GeneratedLoopKernelAttestationState::Unattested ||
          attestation_state ==
              GeneratedLoopKernelAttestationState::PrivateTest);
}

// Semantic attestation and GXP resource attestation are independent gates.
// Scratch-spill artifacts may execute one transactional private canary, but
// only a zero-scratch/static-flow artifact may own sustained product work.
bool GeneratedProgramHasProductResourceAttestation(const ShaderKey& key);

// GS-thread-only completion handoff. Registration and patching remain owned by
// GSDeviceGXM; the registry never calls libGXM.
bool PollGeneratedProgramCompile(CompileResult *result,
                                 GeneratedCgProgram *metadata);
void CompleteGeneratedProgramRegistration(const ShaderKey &key, bool succeeded);
ProgramRegistryStatistics GetGeneratedProgramRegistryStatistics();

// Called after the GS context has drained and generated programs are released.
void ClearGeneratedProgramRegistry();

} // namespace VitaGpuVu
