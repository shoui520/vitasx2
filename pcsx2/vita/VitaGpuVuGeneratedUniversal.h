// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuCgGenerator.h"
#include "vita/VitaGpuVuInvocationPlan.h"
#include "vita/VitaGpuVuMicroProgram.h"
#include "vita/VitaGpuVuProgram.h"
#include "vita/VitaGpuVuShaderCompiler.h"
#include "vita/VitaGpuVuVifInput.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace VitaGpuVu {

class GpuVuDraw;
enum class GeneratedLoopKernelNumericProfile : u8;

// Operation families reachable from every Execute entry in one accepted
// command epoch. This is derived exclusively from PairPlan/CFG reachability;
// it is a generated-source pruning contract, never a semantic admission key.
struct GeneratedUniversalProfile {
  u32 upper_family_mask = 0;
  u32 lower_family_mask = 0;
  u32 reachable_pair_count = 0;
  // The first generated root consumes the Execute descriptor directly. Keep
  // the command-chain shape in the generated identity so a multi-Execute
  // epoch can never reuse this single-Execute entry contract.
  u32 execute_count = 1;
  // A bounded generated region publishes a continuation when TPC leaves its
  // PairPlan set. The enclosing transactional generation remains private and
  // the GS owner selects the successor module; ordinary complete-program
  // roots instead treat an unrepresented TPC as invalid metadata.
  bool continue_at_profile_exit = false;
  std::array<u64, UniversalMicroProgramPairCount / 64> reachable_pairs{};
};

// Canonical one-invocation control plan for the generated serial tier.  This
// is deliberately block threaded: one dynamic TPC lookup selects a complete
// PairPlan basic block, then all pairs in that block execute as static code.
// A loop backedge returns to the block selector, not to a per-pair opcode or
// body dispatcher.  ProgramAnalysis remains the PCSX2-derived owner of CFG,
// branch-delay, E-bit, and natural-loop semantics.
struct GeneratedBlockThreadedBlock {
  u32 analysis_block_index = 0;
  u32 start_pc = 0;
  std::vector<u32> pair_indices;
  std::vector<u32> successor_pcs;
  bool ends_program = false;
};

// A reducible CFG does not need to pay a dynamic TPC tree on every loop trip.
// This compact schedule is derived exclusively from StructuredControlPlan and
// references the same statically decoded blocks carried below. BeginLoop and
// EndLoop retain the guest loop inside one GXP invocation; they never split a
// backedge across firmware jobs.
enum class GeneratedLexicalControlOpKind : u8 {
  BeginLoop,
  Block,
  EndLoop,
};

struct GeneratedLexicalControlOp {
  GeneratedLexicalControlOpKind kind =
      GeneratedLexicalControlOpKind::Block;
  u32 block_index = 0;
  u32 loop_index = 0;
  u32 header_pc = 0;
};

struct GeneratedBlockThreadedPlan {
  u32 entry_block = 0;
  u32 reachable_pair_count = 0;
  u32 maximum_block_pair_count = 0;
  u32 conditional_block_count = 0;
  // Optional optimization proof. Generic block-threaded execution does not
  // require affine counters or lexical loops: its bounded PC dispatcher also
  // represents non-affine and irreducible-but-explicit control flow.
  StructuredControlPlan structured_control;
  bool structured_control_proven = false;
  std::vector<GeneratedLexicalControlOp> lexical_control;
  bool lexical_control_proven = false;
  // A continuation plan may deliberately exclude one predecessor region
  // which has already been summarized by another generated root. If guest
  // control unexpectedly returns there, the block dispatcher publishes an
  // unmatched-PC failure in private transactional state; no canonical effect
  // is committed. These fields are proof metadata, never admission by PC.
  u32 guarded_exit_pc = std::numeric_limits<u32>::max();
  u32 guarded_exit_count = 0;
  // A compiler-bounded transaction module may be prequeued beside mutually
  // exclusive CFG modules. It executes and publishes only when the private
  // generation's current TPC belongs to this plan; an unmatched module is a
  // literal no-effect draw. This is what permits one GXM visibility boundary
  // per dependency layer instead of one boundary per candidate block.
  bool transactional_entry_gate = false;
  // Maximum architectural pairs which one generated invocation may retain
  // locally before publishing a private continuation. Whole-program and
  // cyclic/SCC roots need the complete bounded Execute budget here; acyclic
  // compiler fragments retain the conservative short-root default. This is
  // emitted into the source and attested as part of the generated ABI.
  u32 maximum_dynamic_pairs_per_invocation = 128u;
  // Exactly one first-stage root consumes and validates the immutable Execute
  // descriptor. Successor SCC/DAG roots start only from a transaction-private
  // continuation generation; making each of them parse BUFFER4 duplicated a
  // large secondary/primary prelude in every runtime-compiled GXP.
  bool consumes_execute_entry = true;
  std::vector<GeneratedBlockThreadedBlock> blocks;
};

// Fails closed unless every reachable PairPlan, edge, delay slot, and exit is
// represented by one complete bounded CFG. Affine/structured loop proof is an
// optional optimization annotation, not a compatibility gate. Failure is
// non-mutating: output is assigned only after the complete plan validates.
bool BuildGeneratedBlockThreadedPlan(
    const UniversalMicroProgram& program, const ProgramAnalysis& analysis,
    GeneratedBlockThreadedPlan* plan, std::string* error = nullptr);

// Extracts the closed continuation beginning at entry_pc from an already
// validated complete plan. Edges back to guarded_exit_pc are represented as a
// transactional failure instead of re-executing the region summarized by a
// preceding generated root. Every other successor must remain represented and
// at least one E-bit exit must be reachable. Output is unchanged on failure.
bool BuildGeneratedBlockThreadedContinuationPlan(
    const UniversalMicroProgram& program,
    const GeneratedBlockThreadedPlan& complete_plan, u32 entry_pc,
    u32 guarded_exit_pc, GeneratedBlockThreadedPlan* plan,
    GeneratedUniversalProfile* profile, std::string* error = nullptr);

void AccumulateGeneratedUniversalProfile(
    u32 pair_index, const UniversalPairMicroOp& pair,
    GeneratedUniversalProfile* profile);

// Builds an exact-source serial state-machine root from the common PairPlan Cg
// semantic core. PairPlan kinds only remove unreachable operation families;
// they never decide whether the program is supported.
bool GenerateUniversalStateMachineCg(const UniversalMicroProgram& program,
                                     const GeneratedUniversalProfile& profile,
                                     GeneratedCgProgram* generated,
                                     std::string* error = nullptr);

// Emits the profitable serial tier: one dynamic selection per PairPlan basic
// block, followed by statically decoded pair bodies. Continuations occur only
// at block boundaries, so no GXM job can resume in the middle of an unrolled
// block. The fixed/per-pair interpreter is not involved.
bool GenerateBlockThreadedStateMachineCg(
    const UniversalMicroProgram& program,
    const GeneratedUniversalProfile& profile,
    const GeneratedBlockThreadedPlan& plan,
    GeneratedCgProgram* generated, std::string* error = nullptr);

// Compiler-bounded generated tier. A single-block loop remains local to one
// invocation. Multi-block SCCs are split at basic-block visibility boundaries:
// SGX read/write uniform buffers do not provide a shader-local VU register file
// across dynamically selected blocks, and ShaccCg 3.0 is not process-safe for
// the corresponding large roots. Leaving a represented PC set emits a private
// continuation and the product selects the next module from its private TPC.
// This partition is derived from the canonical CFG and carries no workload
// identity or semantic whitelist.
struct GeneratedUniversalRegionProgram {
  u32 block_index = 0;
  u32 start_pc = 0;
  // Every basic-block entry represented inside this compiler module. An SCC
  // may have several entries from the condensation DAG; descriptor edges must
  // resolve against this set rather than pretending one GXP equals one block.
  std::vector<u32> block_start_pcs;
  // Canonical PairPlan indices represented by this compiler-bounded module.
  // This is explicit bundle-planner metadata: coverage validation must never
  // infer a module's semantic extent from source text or a workload identity.
  std::vector<u32> pair_indices;
  std::vector<u32> successor_pcs;
  // This module belongs to a cyclic SCC. It must never be coalesced with an
  // acyclic neighbour. Several such modules may form a cycle; exactly one is
  // selected from the private transaction TPC at each visibility generation.
  bool cyclic = false;
  // The canonical SCC has a backedge wholly inside this compiler module. The
  // hot-bundle graph cannot see that edge because it only carries private-TPC
  // exits between modules; keep the distinction explicit so a local self-loop
  // is not mistaken for missing cross-module cycle ownership.
  bool owns_internal_cycle = false;
  bool ends_program = false;
  GeneratedUniversalProfile profile;
  GeneratedCgProgram generated;
};

bool GenerateUniversalControlFlowRegions(
    const UniversalMicroProgram& program, const ProgramAnalysis& analysis,
    std::vector<GeneratedUniversalRegionProgram>* regions,
    std::string* error = nullptr,
    GeneratedCgProgram* rejected_cyclic_candidate = nullptr);

// Product hot-tier ownership is intentionally a different ABI from the old
// fixed/structured validation bundle below. Every module in this descriptor
// has a runtime-generated source key; compatibility interpreters and offline
// numeric descriptors are structurally impossible to insert.
inline constexpr u32 GeneratedHotBundleMaximumModules = 20u;
// Product promotion starts with a deliberately low firmware-job envelope.
// The fixed/structured prototypes proved that semantically valid generated
// graphs with dozens of visibility jobs can lose to MTVU by orders of
// magnitude. These are performance gates, not semantic support limits.
// One active compiler module is submitted per private-TPC continuation. The
// descriptor can contain many CFG modules, but mutually exclusive modules are
// never charged or dispatched as execution jobs for the same continuation.
inline constexpr u32 GeneratedHotBundleMaximumProductExecutionJobs = 1u;
inline constexpr u32 GeneratedHotBundleMaximumProductFirmwareJobs = 6u;
inline constexpr u32 GeneratedHotBundleMinimumDynamicPairsPerExecutionJob =
    128u;

enum class GeneratedHotOutputRoute : u8 {
  RawPath1,
  TfxVertex,
  DirectVuTfx,
};

enum class GeneratedHotModuleRole : u8 {
  DirectFused,
  ControlFlow,
  StateSnapshot,
  MemoryPreflight,
  ExpressionScratch,
  PrivateMemoryStore,
  StoreCommit,
  FinalState,
  TailControl,
};

constexpr bool IsRuntimeGeneratedHotKind(GeneratedCgExecutionKind kind) {
  return kind == GeneratedCgExecutionKind::DirectVuTfx ||
         kind ==
             GeneratedCgExecutionKind::StructuredParallelDirectVuTfx ||
         kind == GeneratedCgExecutionKind::UniversalDirectStateMachine ||
         kind == GeneratedCgExecutionKind::StructuredStateSnapshots ||
         kind == GeneratedCgExecutionKind::StructuredMemoryPreflight ||
         kind == GeneratedCgExecutionKind::StructuredExpressionScratch ||
         kind == GeneratedCgExecutionKind::
                     StructuredParallelChildMemoryStore ||
         kind == GeneratedCgExecutionKind::StructuredStoreCommit ||
         kind == GeneratedCgExecutionKind::StructuredFinalState;
}

// Removes non-semantic whitespace and comments from deterministic generated
// Cg while preserving preprocessor directives and source-attestation markers.
// Runtime ShaccCg safety limits apply to this exact compact source, not to the
// readable diagnostic form emitted by the semantic generator.
void CompactRuntimeGeneratedProgramSource(GeneratedCgProgram* program);

struct GeneratedHotBundleModule {
  ShaderKey key{};
  GeneratedCgExecutionKind kind = GeneratedCgExecutionKind::DirectVuTfx;
  GeneratedHotModuleRole role = GeneratedHotModuleRole::ControlFlow;
  u16 stage = 0u;
  u16 reserved = 0u;
  u32 invocation_count = 0u;
  u32 semantic_pair_count = 0u;
  u32 entry_pc = 0u;
  u32 maximum_dynamic_pairs_per_invocation = 0u;
  // PairPlan PC entries accepted by this compiler module. Product dispatch
  // selects exactly one generated root from the private transaction TPC; it
  // does not submit every mutually exclusive CFG stage as a no-op draw.
  std::array<u64, UniversalMicroProgramPairCount / 64u> pc_entry_mask{};
  StructuredGeneratedScratchMask scratch_read_mask{};
  StructuredGeneratedScratchMask scratch_write_mask{};
  // Same-stage shared-control writes are legal only for transaction-gated
  // control modules: exactly one module can match the private TPC observed at
  // that visibility generation. Other generated module classes must publish
  // disjoint state or occupy a later dependency stage.
  bool transaction_gate = false;
  bool writes_shared_control = false;
  // Pair coverage is counted exactly once even when several generated roots
  // compute disjoint state/output slices for the same architectural pairs.
  // CFG modules own disjoint coverage; support/parallel sibling roots do not.
  bool owns_disjoint_pair_coverage = false;
};

struct GeneratedHotBundle {
  u64 program_identity = 0u;
  u64 cache_generation = 0u;
  u32 analysis_start_pc = 0u;
  u32 configuration_bits = 0u;
  GeneratedHotOutputRoute output_route = GeneratedHotOutputRoute::RawPath1;
  u32 module_count = 0u;
  u32 stage_count = 0u;
  u32 semantic_pair_count = 0u;
  u32 maximum_dynamic_pairs_per_invocation = 0u;
  // True when at least one guest backedge crosses a GXP/module boundary.
  // Such a bundle may be semantically complete, but each loop trip needs a
  // CPU-observed completion plus another firmware job. It is therefore a
  // compiler fallback artifact, never a low-job product candidate.
  bool has_cross_module_cycle = false;
  std::array<GeneratedHotBundleModule, GeneratedHotBundleMaximumModules>
      modules{};

  bool HasAtomicCompilerOwnership() const;
  bool StageModuleRange(u32 stage, u32* first_module,
                        u32* stage_module_count) const;
  bool ModuleForPc(u32 pc, u32* module_index) const;
  bool StageForPc(u32 pc, u32* stage) const;
  u32 DrawCount() const;
  u32 FirmwareJobCount(u32 unpack_jobs,
                       bool terminal_path1_commit = true) const;
  bool IsLowJobProductCandidate(u32 dynamic_pair_upper_bound,
                                u32 unpack_jobs) const;
  bool MatchesOwner(u64 expected_program_identity,
                    u32 expected_analysis_start_pc,
                    u32 expected_configuration_bits) const;
  bool MatchesLiveGeneration(const GeneratedHotBundle& live) const;
};

// Constructs a finite hot bundle from a complete CFG partition. Canonical
// PairPlan coverage is exact and disjoint, every successor resolves inside the
// bundle, and every source satisfies the in-process compiler contract. A
// cross-module cycle is legal only for modules explicitly derived from one
// cyclic SCC. Runtime execution is still bounded by the epoch pair budget and
// one active module is selected from the private TPC per continuation. Failure
// is non-mutating.
bool BuildGeneratedControlFlowHotBundleDescriptor(
    u64 program_identity, u64 cache_generation,
    const UniversalMicroProgram& program,
    const GeneratedBlockThreadedPlan& complete_plan,
    const std::vector<GeneratedUniversalRegionProgram>& regions,
    GeneratedHotBundle* bundle, std::string* error = nullptr);

enum class GeneratedUniversalRegionBundleState : u8 {
  Missing,
  Compiling,
  Ready,
  Failed,
  Unavailable,
};

// Starts/retries compilation of every compiler-bounded CFG region. Guest
// execution never waits for this service, and merely having some region GXPs
// ready never admits a partial bundle. The product owner may select the bundle
// only after Query reports Ready for every region.
bool RequestUniversalControlFlowRegionPrograms(
    u64 program_identity, const UniversalMicroProgram& program,
    const ProgramAnalysis& analysis);
bool PumpUniversalControlFlowRegionPrograms(u64 program_identity);
GeneratedUniversalRegionBundleState QueryUniversalControlFlowRegionPrograms(
    u64 program_identity, GeneratedHotBundle* bundle = nullptr);
bool FindUniversalControlFlowRegionProgram(
    u64 program_identity, u32 pc, ShaderKey* key);

struct GeneratedUniversalRegionBundleStatistics {
  u64 requests = 0;
  u64 cache_hits = 0;
  u64 generated_bundles = 0;
  u64 generation_failures = 0;
  u64 generated_modules = 0;
  u64 module_submit_attempts = 0;
  u64 module_submissions = 0;
  u64 module_submit_retries = 0;
  u64 module_compiler_safety_rejections = 0;
  u32 cached_bundles = 0;
  u32 ready_bundles = 0;
  u32 failed_bundles = 0;
  u32 pending_modules = 0;
  u32 pending_source_bytes = 0;
};

GeneratedUniversalRegionBundleStatistics
GetGeneratedUniversalRegionBundleStatistics();

// Hardware-driven product ABI.  This descriptor is intentionally unable to
// name the historical fixed interpreter, structured snapshot buffers, or a
// compiler-partition graph: one exact PairPlan-derived loop body owns one GXP
// and one direct-TFX draw.  The source marker emitted by
// GenerateLoopKernelDirectTfxCg() gives these roots a separate content-key
// namespace from every legacy generated artifact.
inline constexpr u32 GeneratedLoopKernelBundleAbiVersion =
    GeneratedLoopKernelCgAbiVersion;

enum class GeneratedLoopKernelOutputRoute : u8 {
  DirectVuTfx,
};

struct GeneratedLoopKernelBundle {
  u32 abi_version = GeneratedLoopKernelBundleAbiVersion;
  u64 program_identity = 0u;
  u64 cache_generation = 0u;
  u32 analysis_start_pc = 0u;
  u32 configuration_bits = 0u;
  u32 semantic_profile_key = 0u;
  ShaderKey kernel_key{};
  // Content key of the private canary whose physical comparison authorizes
  // this executable. Canary descriptors use their own key; lean ABI-29/30
  // descriptors retain that key while kernel_key names the no-journal GXP.
  ShaderKey attestation_key{};
  GeneratedLoopKernelOutputRoute output_route =
      GeneratedLoopKernelOutputRoute::DirectVuTfx;
  DirectTfxContract direct_tfx{};
  std::array<u32, 4> gif_tag{};

  u32 loop_index = 0u;
  u32 loop_entry_pc = 0u;
  u32 exact_invocation_count = 0u;
  u32 static_pair_body_count = 0u;
  u64 dynamic_semantic_pair_count = 0u;

  // Compact immutable input and live-in description.  These are mappings and
  // scalar/vector records, never a VU-memory or outer-register snapshot.
  u32 raw_input_mapping_count = 0u;
  u32 constant_qword_count = 0u;
  u32 uniform_vector_count = 0u;
  u32 vf_live_in_mask = 0u;
  u16 vi_live_in_mask = 0u;
  u8 scalar_live_in_mask = 0u;
  bool uses_flat_instance_inputs = false;
  bool uses_flat_index_inputs = false;
  bool uses_buffered_batch_inputs = false;
  bool uses_dynamic_batch_uniform_index = false;
  bool uses_instance_indexed_batch_live_ins = false;
  GeneratedBatchVaryingLiveIns batch_varying_live_ins{};
  u32 loop_kernel_source_abi = GeneratedLoopKernelCgAbiVersion;
  u32 flat_vertices_per_primitive = 0u;
  // Exact one-draw dispatch geometry for a flattened enclosing/child loop.
  // These fields describe immutable INDEX arithmetic only; no parent-state
  // snapshot or compiler-module dependency is representable by this ABI.
  bool uses_nested_iteration_grid = false;
  // ABI 26 batches only Executes whose fixed live-ins are bit-identical.
  // Varying raw-input bindings remain selected by the global INDEX domain.
  bool uses_nested_batch_index_inputs = false;
  u16 nested_outer_iterations = 0u;
  u16 nested_child_iterations = 0u;
  // A closed-form nested kernel owns the complete E-bit transaction, not just
  // the child body.  Its terminal ADC/XGKICK proof supplies an exact indexed
  // triangle list while the generated vertex domain remains one dense
  // outer-by-child grid.
  bool uses_closed_form_nested_loop = false;
  bool has_exact_post_loop_output = false;
  u16 compact_outer_table_count = 0u;
  u16 compact_outer_qword_count = 0u;
  u32 exact_index_count = 0u;
  u32 exact_primitive_count = 0u;
  u8 private_store_count = 0u;

  // O(1) PairPlan-derived successor formula.  It performs no guest pair and
  // is evaluated only after the private draw succeeds.
  u32 final_formula_node_count = 0u;
  u32 final_formula_alternative_count = 0u;
  u32 final_vi_write_mask = 0u;
  u32 unique_resume_pc = 0u;
  bool has_compact_final_state_formula = false;
  // Compilation and execution admission are deliberately distinct.  A
  // nested closed-form root may be compiled and resource-attested while CPU
  // MTVU remains authoritative until its complete VF/ACC/Q/P/I and VU-memory
  // successor is transactionally published or a measured compact host
  // summary is proven.
  bool requires_transactional_final_state = false;

  // Product-shape invariants are stored explicitly so stale/corrupt callers
  // cannot smuggle a multi-root architecture through a valid content key.
  u32 generated_root_count = 1u;
  u32 draw_count = 1u;
  u32 firmware_job_count = 1u;
  u32 precompute_root_count = 0u;
  u32 snapshot_bytes = 0u;
  u32 cpu_semantic_pair_count = 0u;
  u32 source_bytes = 0u;
  u32 expression_count = 0u;

  bool IsPrivateStateCanary() const {
    return loop_kernel_source_abi == GeneratedLoopKernelStateCanaryCgAbiVersion ||
           loop_kernel_source_abi == GeneratedLoopKernelPartialStateCanaryCgAbiVersion;
  }
  bool IsNoWriteProduct() const {
    return loop_kernel_source_abi == GeneratedLoopKernelProductCgAbiVersion ||
           loop_kernel_source_abi == GeneratedLoopKernelPartialBatchProductCgAbiVersion ||
           loop_kernel_source_abi == GeneratedLoopKernelNestedFlatProductCgAbiVersion ||
           loop_kernel_source_abi == GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion;
  }
  // Cold specialization: rebuild every executable input field from its own
  // source. A pruned state canary and raster product need not share a layout.
  // Retains the semantic transaction and, for products, its canary identity.
  // Does not grant source/compiler/resource or physical execution admission.
  bool ConfigureExecutableSourceMetadata(const GeneratedCgProgram& generated);
  // Dispatch-domain check only; never substitutes for the all-input PairPlan
  // capacity proof. Dense private state canaries must evaluate the complete
  // compiled grid, whereas a proven raster product may use an active prefix.
  bool HasCompatibleExecutableDispatchDomain(
      const GeneratedLoopKernelBundle& executable) const;
  bool HasAtomicCompilerOwnership() const;
  bool MatchesGeneratedProgram(const GeneratedCgProgram& generated) const;
  bool MatchesOwner(u64 expected_program_identity,
                    u32 expected_analysis_start_pc,
                    u32 expected_configuration_bits,
                    const std::array<u32, 4>& expected_gif_tag) const;
  bool MatchesLiveGeneration(const GeneratedLoopKernelBundle& live) const;
};

// Source-construction inputs retained by Vita's bounded private replay and
// consumed by the native source-construction oracle.
struct GeneratedLoopKernelCandidateControlInputs final {
  std::array<u16, 16> initial_vi{};
  std::array<u32, 4> gif_tag{};
  u16 vif_top = 0u;
  u16 vif_itop = 0u;
};
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
// Calls the same pure source constructor as product admission, without
// registering or submitting a shader on the non-GXM validation board.
// Optional inspection returns the last semantically constructed source even
// when runtime admission rejects it; the boolean still reports admission.
// Optional variance is expressed in the base canary's input identities and
// follows the same partial-canary/product emitters as the asynchronous pump.
// Optional capacity control is independently constructed at the same source
// entry and checked by the runtime capacity proof and descriptor selector.
// It does not create registry readiness or physical attestation evidence.
bool ValidateGeneratedLoopKernelCandidateSource(
    const ProgramAnalysis& analysis, u32 configuration_bits,
    const std::array<u16, 16>& initial_vi, u16 vif_top, u16 vif_itop,
    const std::array<u32, 4>& gif_tag, GeneratedLoopKernelBundle* bundle,
    std::string* error, GeneratedCgProgram* inspected_source = nullptr,
    GeneratedCgProgram* inspected_product = nullptr,
    const GeneratedBatchVaryingLiveIns* varying_live_ins = nullptr,
    const GeneratedLoopKernelCandidateControlInputs* capacity_inputs = nullptr);
bool ValidateGeneratedLoopKernelCompilerMemoryReclaim();
bool ValidateGeneratedLoopKernelInputMapCache();
bool ValidateGeneratedLoopKernelDeferredCapture();
bool ValidateGeneratedLoopKernelPrivateReplay(const ProgramAnalysis& analysis,
    u32 configuration_bits, const char* path, u32 capture_id, std::string* error);
bool ValidateGeneratedLoopKernelExecutableSelection();
#endif

// One immutable pre-effect control result carried from live-contract
// resolution directly into descriptor construction.  The old path discarded
// this result, then replayed the same PairPlan prefix again for every hot
// Execute.  Store values remain private until the generated transaction
// completes successfully.
struct GeneratedLoopKernelLiveStore final {
  u16 address_qword = 0u;
  u8 lane_mask = 0u;
  u8 reserved = 0u;
  std::array<u32, 4> words{};
};

// Opaque, immutable runtime owners retained by one pre-effect live-contract
// resolution.  Their concrete PairPlan/runtime-plan type remains private to the
// generated compiler implementation.  Carrying the owners here prevents the
// product descriptor builder from reacquiring the compiler-cache mutex and
// rediscovering the same exact transaction plus compatible capacity roots.
// These are proof carriers only; no pointer identity decides semantic support.
struct GeneratedLoopKernelRuntimeCandidate final {
  GeneratedLoopKernelBundle descriptor;
  std::shared_ptr<const void> runtime_plan;
  std::shared_ptr<const GeneratedCgProgram> generated_override;
};

struct GeneratedLoopKernelLiveContract final {
  static constexpr u32 MaximumRuntimeCandidates = 16u;

  GeneratedLoopKernelBundle descriptor;
  u32 entry_iterations = 0u;
  u32 outer_iterations = 0u;
  std::vector<GeneratedLoopKernelLiveStore> pre_loop_stores;
  std::shared_ptr<const void> active_runtime_plan;
  std::shared_ptr<const GeneratedCgProgram> active_generated_override;
  // A hot BSpline resolution normally retains only two or three capacity
  // owners. Eagerly constructing all sixteen slots initialized and then moved
  // several kilobytes of descriptor state for every Execute even though live
  // control was already a cache hit. Disengaged optionals preserve the fixed,
  // allocation-free bound while constructing/moving only proven candidates.
  std::array<std::optional<GeneratedLoopKernelRuntimeCandidate>,
             MaximumRuntimeCandidates>
      runtime_candidates{};
  u32 runtime_candidate_count = 0u;
  bool control_proven = false;

  bool HasPreEffectProof() const {
    if (!control_proven || !descriptor.HasAtomicCompilerOwnership() ||
        entry_iterations == 0u || outer_iterations == 0u ||
        outer_iterations != descriptor.nested_outer_iterations ||
        !active_runtime_plan || runtime_candidate_count == 0u ||
        runtime_candidate_count > runtime_candidates.size()) {
      return false;
    }
    for (u32 index = 0u; index < runtime_candidate_count; index++) {
      if (!runtime_candidates[index].has_value())
        return false;
    }
    return true;
  }
};

// Builds the first title-neutral one-root form from a complete affine natural
// loop.  It reuses the canonical PairPlan/CFG/invocation analysis, emits no
// runtime effect, and assigns all outputs only after the complete descriptor,
// final-state formula, compiler shape, and source key validate.  Nested-loop
// rematerialization extends this same ABI; it must not call the historical
// snapshot/precompute builder.
bool BuildGeneratedLoopKernelBundleCandidate(
    u64 program_identity, u64 cache_generation,
    const ProgramAnalysis& analysis, u32 configuration_bits,
    const std::array<u32, 4>& gif_tag,
    GeneratedLoopKernelBundle* bundle,
    ParallelInvocationPlan* invocation_plan,
    GeneratedCgProgram* generated_program,
    std::string* error = nullptr);

enum class GeneratedLoopKernelBundleState : u8 {
  Missing,
  WaitingForOutputContract,
  // The observed epoch has no PATH1 output. It may still become a compact
  // generated state formula, so this is not a terminal compiler failure.
  StateOnly,
  // This request shape is intentionally outside the one-root query (for
  // example, a gathered multi-Execute epoch). The individual Execute remains
  // eligible and no provider hysteresis may be armed from this state.
  NotApplicable,
  Compiling,
  Ready,
  Failed,
  Unavailable,
};

constexpr bool GeneratedLoopKernelBundleStateIsTerminallyUnavailable(
    GeneratedLoopKernelBundleState state) {
  return state == GeneratedLoopKernelBundleState::Failed ||
         state == GeneratedLoopKernelBundleState::Unavailable;
}

// Bounded product cache for the one-root hardware-driven tier.  PairPlan
// analysis and source generation happen once per exact source/entry/profile;
// repeated MTVU epochs perform only an O(1) lookup while CPU MTVU remains the
// cold/pending provider.
bool RequestGeneratedLoopKernelBundle(
    u64 program_identity, const ProgramAnalysis& analysis,
    u32 configuration_bits,
    GeneratedLoopKernelBundle* bundle = nullptr,
    const std::array<u16, 16>* initial_vi = nullptr,
    u16 vif_top = 0u, u16 vif_itop = 0u);
bool PumpGeneratedLoopKernelBundle(u64 program_identity,
                                   u32 analysis_start_pc,
                                   u32 configuration_bits);
void PumpGeneratedLoopKernelBundles();
// Source construction and successor submission are compiler/planner work.
// Queue one coalesced pass on the existing low-priority worker; callers on
// MTVU/MTGS must never run PumpGeneratedLoopKernelBundles() synchronously.
bool RequestGeneratedLoopKernelBundlePump();
// Refresh only the bounded cache flags which mirror registry/resource and
// semantic-attestation state. This performs no source construction.
bool RefreshGeneratedLoopKernelExecutableState(const ShaderKey& key);

// GS retirement observes exact adjacent-descriptor variance after both draws
// have already passed semantic admission.  Feed that title-neutral shape back
// to the generated-program cache so it can compile a compact BUFFER4 variant
// asynchronously while ABI 26 remains the active provider.
bool RecordGeneratedLoopKernelBatchLiveInVariance(
    const ShaderKey& executable_key,
    const GeneratedBatchVaryingLiveIns& varying_live_ins);
// A compiler-ready optimization remains subordinate to its exact transaction
// owner. If physical retirement rejects that optimization's numeric/resource
// contract, stop selecting it and immediately resume the already-attested base
// executable instead of making CPU MTVU pay for the same epoch.
bool RejectGeneratedLoopKernelBatchExecutable(
    const ShaderKey& executable_key);
GeneratedLoopKernelBundleState QueryGeneratedLoopKernelBundle(
    u64 program_identity, u32 analysis_start_pc,
    u32 configuration_bits,
    GeneratedLoopKernelBundle* bundle = nullptr);

// Replays only the bounded PairPlan VI/control prefix from the Execute entry
// to the first closed-form parent-loop header. All memory comes through the
// immutable invocation callbacks, stores remain in a private journal, and no
// architectural state or output is published. This is the product pre-effect
// attestation used when a setup-loop trip count was loaded from VU memory.
bool EvaluateGeneratedLoopKernelEntryControl(
    const ProgramAnalysis& analysis,
    const ClosedFormNestedLoopProof& proof,
    const InvocationEvaluationContext& context,
    u32* summarized_entry_iterations, u32* outer_iterations,
    std::string* error = nullptr, u32* traced_operations = nullptr);

// Executes the same private PairPlan control machine without assuming one
// cached output contract's observed trip count. The walk is bounded by the
// greatest exact entry-prefix proof in the candidate set and returns the live
// entry/outer counts; callers must then match those counts to exactly one
// independently attested GIF/output contract before any GPU effect. This is
// the interpreter/dynarec-style dispatch discriminator, not semantic support
// selected by a source identity.
bool EvaluateGeneratedLoopKernelEntryControlDiscriminator(
    const ProgramAnalysis& analysis,
    const ClosedFormNestedLoopProof& control_machine,
    u32 maximum_entry_prefix_pair_count,
    const InvocationEvaluationContext& context,
    u32* summarized_entry_iterations, u32* outer_iterations,
    std::string* error = nullptr, u32* traced_operations = nullptr);

// Proves that a larger closed-form generated kernel computes the same active
// prefix as an exact-count kernel for one immutable epoch context. Every
// written qword address and lane is compared under both the exact PairPlan
// arithmetic profile and the named native-SGX playable profile. This executes
// no guest pair and publishes no state or output. A caller may use the larger
// executable only after this pre-effect proof succeeds; source identity or a
// matching loop shape alone is insufficient because a counted entry prelude
// can produce a different expression graph for each outer count.
bool ProveGeneratedLoopKernelCapacityPrefix(
    const ParallelLoopKernel& active_kernel,
    const ParallelLoopKernel& capacity_kernel,
    u32 active_outer_iterations, u32 child_iterations,
    u32* compared_store_lanes = nullptr,
    std::string* error = nullptr);

// Runtime differential for the structural proof above. This is retained by
// oracle validation and private attestation; the product hot path uses the
// context-independent structural result and does not reevaluate every vertex
// on Cortex-A9.
bool ProveGeneratedLoopKernelCapacityPrefixForContext(
    const ParallelLoopKernel& active_kernel,
    const ParallelLoopKernel& capacity_kernel,
    const InvocationEvaluationContext& context,
    u32 active_outer_iterations, u32 child_iterations,
    u32* compared_store_lanes = nullptr,
    std::string* error = nullptr);

// Resolves a cache-hint descriptor to the unique cached output/control
// contract selected by the immutable live VU state.  The last CPU-observed
// PATH1 tag is useful for creating cold cache entries, but cannot select a
// later GPU-owned transaction after CPU PATH1 execution stops.  This bounded
// pre-effect query executes no guest-visible operation and returns the exact
// generated-program key which must be used for attestation and dispatch.
bool ResolveGeneratedLoopKernelLiveContract(
    const GeneratedLoopKernelBundle& hint,
    const InvocationEvaluationContext& context,
    GeneratedLoopKernelLiveContract* resolved,
    std::string* error = nullptr);

// Product hot-path form. The dispatch-cost cache already retains this exact
// source/configuration generation and the last attested executable key. Live
// resolution revalidates the complete cached transaction under the immutable
// invocation context, so reconstructing a full hint bundle first adds no
// semantic proof and needlessly scans the compiler cache twice per Execute.
bool ResolveGeneratedLoopKernelLiveContract(
    u64 program_identity, u32 analysis_start_pc, u32 configuration_bits,
    const ShaderKey& preferred_executable_key,
    const InvocationEvaluationContext& context,
    GeneratedLoopKernelLiveContract* resolved,
    std::string* error = nullptr);

// Constructs one immutable direct-TFX descriptor from compact live inputs and
// PairPlan-derived address/final-state formulas. It performs no VU pair and
// never allocates a snapshot/precompute buffer.
std::unique_ptr<GpuVuDraw> BuildGeneratedLoopKernelGpuVuDraw(
    const GeneratedLoopKernelBundle& bundle,
    const InvocationEvaluationContext& context,
    const std::vector<VifUnpackSpan>& spans, u32 pair_budget,
    GeneratedLoopKernelNumericProfile output_numeric_profile,
    std::string* error = nullptr);

// Product hot path: consumes the exact pre-effect proof returned by
// ResolveGeneratedLoopKernelLiveContract() instead of replaying PairPlan
// control during descriptor construction.
std::unique_ptr<GpuVuDraw> BuildGeneratedLoopKernelGpuVuDraw(
    GeneratedLoopKernelLiveContract live_contract,
    const InvocationEvaluationContext& context,
    const std::vector<VifUnpackSpan>& spans, u32 pair_budget,
    GeneratedLoopKernelNumericProfile output_numeric_profile,
    std::string* error = nullptr);

// Physical promotion gate for a compiler-ready closed-form kernel. The GPU
// owns the visible direct-TFX draw and publishes every proven PairPlan store
// to transaction-private BUFFER2; CPU MTVU still executes once as the oracle
// and fills the expected journal before queue publication. This function does
// not weaken product admission or claim zero CPU semantic pairs.
std::unique_ptr<GpuVuDraw>
BuildGeneratedLoopKernelPrivateComparisonGpuVuDraw(
    const GeneratedLoopKernelBundle& bundle,
    const InvocationEvaluationContext& context,
    const std::vector<VifUnpackSpan>& spans, u32 pair_budget,
    std::string* error = nullptr);

// Low-job runtime-generated nested-loop tier. The Cortex-A9 prepares one
// bounded descriptor-scale child-entry snapshot block from the same fixed
// PairPlan-derived expression program used by the oracle. Compiler-bounded
// pure expression producers publish transaction-private BUFFER10 values in
// at most three dependency stages before one generated direct root consumes
// the dense INDEX range and feeds the ordinary TFX interface. Independent
// producer modules in one stage share one GXM vertex job and one visibility
// boundary. This deliberately avoids both a serial SGX PairPlan interpreter
// and the many-job generated state prototype. It is
// a JIT artifact derived from PairPlans and an observed packed GIF contract,
// never a workload-specific shader or semantic admission key.
inline constexpr u32 GeneratedNestedDirectMaximumPrecomputeModules = 8u;
inline constexpr u32 GeneratedNestedDirectMaximumPrecomputeStages = 3u;
struct GeneratedNestedDirectBundle {
  u64 program_identity = 0u;
  u64 cache_generation = 0u;
  u32 analysis_start_pc = 0u;
  u32 configuration_bits = 0u;
  std::array<ShaderKey, GeneratedNestedDirectMaximumPrecomputeModules>
      precompute_keys{};
  std::array<u8, GeneratedNestedDirectMaximumPrecomputeModules>
      precompute_stages{};
  u32 precompute_module_count = 0u;
  u32 precompute_stage_count = 0u;
  ShaderKey direct_key{};
  DirectTfxContract direct_tfx{};
  std::array<u32, 4> gif_tag{};
  u32 parent_entry_pc = 0u;
  u32 child_entry_pc = 0u;
  u32 maximum_outer_iterations = 0u;
  u32 child_iteration_count = 0u;
  u32 semantic_pair_count = 0u;
  u32 parent_exit_pc = 0u;
  u32 unique_resume_pc = 0u;
  std::array<u16, 16> required_initial_vi{};
  u32 required_initial_vi_mask = 0u;
  bool uses_flat_instance_inputs = false;
  u32 flat_vertices_per_primitive = 0u;

  bool HasCompilerOwnership() const {
    if (program_identity == 0u || cache_generation == 0u ||
        precompute_module_count > precompute_keys.size() ||
        precompute_stage_count >
            GeneratedNestedDirectMaximumPrecomputeStages ||
        ((precompute_module_count == 0u) !=
         (precompute_stage_count == 0u)) ||
        (direct_key.low == 0u && direct_key.high == 0u) ||
        direct_tfx.vertex_count == 0u || maximum_outer_iterations == 0u ||
        child_iteration_count == 0u || semantic_pair_count == 0u ||
        parent_exit_pc > 0x3ff8u || (parent_exit_pc & 7u) != 0u ||
        unique_resume_pc > 0x4000u || (unique_resume_pc & 7u) != 0u ||
        required_initial_vi[0] != 0u ||
        (required_initial_vi_mask & ~0xfffeu) != 0u) {
      return false;
    }
    std::array<bool, GeneratedNestedDirectMaximumPrecomputeStages>
        represented_stages{};
    for (u32 index = 0u; index < precompute_module_count; index++) {
      if (precompute_keys[index].low == 0u &&
          precompute_keys[index].high == 0u)
        return false;
      if (precompute_stages[index] >= precompute_stage_count ||
          (index != 0u &&
           precompute_stages[index] < precompute_stages[index - 1u]))
        return false;
      represented_stages[precompute_stages[index]] = true;
    }
    for (u32 stage = 0u; stage < precompute_stage_count; stage++) {
      if (!represented_stages[stage])
        return false;
    }
    return true;
  }
  bool MatchesInitialVi(const u16* initial_vi) const {
    if (!initial_vi)
      return false;
    for (u32 reg = 1u; reg < required_initial_vi.size(); reg++) {
      if ((required_initial_vi_mask & (1u << reg)) != 0u &&
          initial_vi[reg] != required_initial_vi[reg]) {
        return false;
      }
    }
    return true;
  }
  bool MatchesOwner(u64 expected_identity, u32 expected_entry_pc,
                    u32 expected_configuration_bits) const {
    return HasCompilerOwnership() && program_identity == expected_identity &&
           analysis_start_pc == expected_entry_pc &&
           configuration_bits == expected_configuration_bits;
  }
  bool MatchesLiveGeneration(const GeneratedNestedDirectBundle& live) const {
    return cache_generation == live.cache_generation &&
           live.MatchesOwner(program_identity, analysis_start_pc,
                             configuration_bits) &&
           precompute_module_count == live.precompute_module_count &&
           precompute_stage_count == live.precompute_stage_count &&
           precompute_keys == live.precompute_keys &&
           precompute_stages == live.precompute_stages &&
           direct_key == live.direct_key && gif_tag == live.gif_tag;
  }
};

enum class GeneratedNestedDirectBundleState : u8 {
  Missing,
  WaitingForOutputContract,
  Compiling,
  Ready,
  Failed,
  Unavailable,
};

// A CPU-MTVU cold execution may publish the first complete packed PATH1 tag
// for this exact PairPlan/configuration identity. The tag supplies dynamic GS
// contract data only; PairPlan proofs still decide semantic support.
void RecordGeneratedNestedDirectPath1Tag(
    u64 program_identity, const std::array<u32, 4>& gif_tag,
    const std::array<u16, 16>* initial_vi = nullptr,
    u16 vif_top = 0u, u16 vif_itop = 0u);
void RecordGeneratedNestedDirectNoPath1(u64 program_identity);

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
bool ValidateGeneratedColdPath1Reporting();
#endif
// A source identity may legitimately alternate between PATH1-producing and
// state-only executions. This query reports the latter observation without
// discarding any independently compiled PATH1 contract variants.
bool HasGeneratedNoPath1Observation(u64 program_identity);

bool BuildGeneratedNestedDirectBundleCandidate(
    u64 program_identity, const ProgramAnalysis& analysis,
    u32 configuration_bits, const std::array<u16, 16>& initial_vi,
    const std::array<u32, 4>& gif_tag,
    GeneratedNestedDirectBundle* bundle,
    std::vector<GeneratedCgProgram>* precompute_programs,
    GeneratedCgProgram* direct_program, std::string* error = nullptr);

// Builds the one-draw hot descriptor after every generated root is ready.
// The complete V4-32 journal is materialized into a private mapped input
// generation, fixed structured control is evaluated before publication, and
// the remaining VI-only tail is proven/evaluated without executing VU
// arithmetic. Any unsupported state or control form returns null before the
// draw or canonical VI/TPC state is published.
std::unique_ptr<GpuVuDraw> BuildGeneratedNestedDirectGpuVuDraw(
    const GeneratedNestedDirectBundle& bundle,
    const InvocationEvaluationContext& context,
    const void* canonical_vu_memory, u32 canonical_vu_memory_size,
    const std::vector<VifUnpackSpan>& spans, u32 pair_budget,
    std::string* error = nullptr);

// Generates and asynchronously requests the one direct hot GXP. initial_vi is
// used only to resolve the compact invocation plan's affine pointer bases; the
// GXP source remains content keyed and reusable. Failure leaves CPU MTVU
// authoritative and has no guest effect.
bool RequestGeneratedNestedDirectBundle(
    u64 program_identity, const ProgramAnalysis& analysis,
    u32 configuration_bits, const std::array<u16, 16>& initial_vi,
    GeneratedNestedDirectBundle* bundle = nullptr);
bool PumpGeneratedNestedDirectBundle(u64 program_identity);
void PumpGeneratedNestedDirectBundles();
GeneratedNestedDirectBundleState QueryGeneratedNestedDirectBundle(
    u64 program_identity, GeneratedNestedDirectBundle* bundle = nullptr);

// Legacy diagnostic structured lowering selected solely from the canonical
// PairPlan CFG and dependence proofs. The compiler-bounded generated roots are
// one transactional validation bundle; partial compiler readiness is never
// enough for admission. Structured architectural recurrence is serialized as
// data for one offline fixed evaluator. Runtime ShaccCg owns only independently
// parallel scratch/store/final roots, selected from semantic destination and
// expression-DAG boundaries rather than a workload identity. Store and final
// scratch producer/consumer pairs occupy one dependency-ordered stream, so a
// consumed scratch cut is retired before its slots are reused. The stream bound
// is deliberately independent of Shacc's per-root arena: it lets a large DAG
// become many compiler-safe roots without retrying an unsafe root or increasing
// the compiler arena. This dependency-heavy shape is not a product provider;
// CPU MTVU remains authoritative while the low-job generated compiler improves.
inline constexpr u32 StructuredGeneratedMaximumPartitionModuleCount = 256;
inline constexpr u32 StructuredGeneratedMaximumCompilerModuleCount =
    StructuredGeneratedMaximumPartitionModuleCount;
// Physical r34 exhausted the shared 256 KiB vertex-USSE patcher heap while
// registering roughly the 29th small structured root. Until generated-program
// LRU/pinning can budget exact measured USSE bytes across bundles, retain a
// conservative per-bundle ceiling below that observed failure. The ABI-22
// 64-output partition produces 19 spill-free compiler roots whose complete
// offline GXP payload is 67,324 bytes, effectively the same as the previous
// 16-root/66,896-byte plan, so count alone must not reject it. This limits an
// optional optimization only; CPU MTVU remains the cold/unavailable owner.
inline constexpr u32 StructuredGeneratedMaximumResidentProgramsPerBundle = 20;
inline constexpr u32 StructuredGeneratedMaximumOuterIterations = 64;
// Keep the fixed recurrence evaluator below the firmware watchdog without
// materializing state on ARM. Offline SDK 3.570 compilation and
// psp2shaderperf produce the same 11,788-byte/1,433-cycle dynamic-loop root
// for 4, 8, 16, 32, and 64-outer source bounds. Use a conservative 16-outer
// continuation: one invocation remains far shorter than the rejected 10,132-
// cycle generated point root, while twelve dependency/no-op firmware jobs are
// removed from every bounded transaction. Private recurrence and a monotonic
// slice cursor remain in BUFFER9; no runtime trip count is assumed here.
inline constexpr u32 StructuredGeneratedStateOuterIterationsPerJob = 16;
inline constexpr u32 StructuredGeneratedFixedStateJobCount =
    (StructuredGeneratedMaximumOuterIterations +
     StructuredGeneratedStateOuterIterationsPerJob - 1u) /
    StructuredGeneratedStateOuterIterationsPerJob;
// INDEX values encode (slice,module) without an integer divide, which Sony's
// sce_vp_psp2 profile does not support for arbitrary denominators.
inline constexpr u32 StructuredGeneratedFixedStateIndexStride = 64;
static_assert((StructuredGeneratedFixedStateIndexStride &
               (StructuredGeneratedFixedStateIndexStride - 1u)) == 0u);
inline constexpr u32 StructuredGeneratedControlStage = 0;
inline constexpr u32 StructuredGeneratedFixedStateStage = 1;
inline constexpr u32 StructuredGeneratedFixedStateStageEnd =
    StructuredGeneratedFixedStateStage +
    StructuredGeneratedFixedStateJobCount;
inline constexpr u32 StructuredGeneratedPreflightBuildStage =
    StructuredGeneratedFixedStateStageEnd;
inline constexpr u32 StructuredGeneratedPreflightReduceStage =
    StructuredGeneratedPreflightBuildStage + 1u;
inline constexpr u32 StructuredGeneratedPreflightFinalizeStage =
    StructuredGeneratedPreflightReduceStage + 1u;
inline constexpr u32 StructuredGeneratedPartitionStageBase =
    StructuredGeneratedPreflightFinalizeStage + 1u;
// A legacy structured validation bundle remains one private transaction, but
// its generated
// partitions are continued through a bounded logical firmware-job stream.
// This is a diagnostic capacity, not an opcode or workload admission rule.
inline constexpr u32 StructuredGeneratedMaximumPartitionFirmwareJobs = 2048;

inline constexpr bool IsStructuredGeneratedFixedStateStage(u32 stage) {
  return stage >= StructuredGeneratedFixedStateStage &&
         stage < StructuredGeneratedFixedStateStageEnd;
}

inline constexpr u32 StructuredGeneratedFixedStateSlice(u32 stage) {
  return IsStructuredGeneratedFixedStateStage(stage)
             ? stage - StructuredGeneratedFixedStateStage
             : std::numeric_limits<u32>::max();
}

// Bound legacy diagnostic work per SGX vertex invocation independently of any
// title or source identity. Product execution never descends to a full fixed
// interpreter; any generated plan outside the profitable envelope stays on
// CPU MTVU.
inline constexpr u32 StructuredGeneratedMaximumStateModuleWork = 2048;
inline constexpr u32 StructuredGeneratedMaximumPreflightAccessWork = 2048;

inline constexpr u32 StructuredGeneratedStageCount(
    u32 partition_firmware_job_count) {
  return StructuredGeneratedPartitionStageBase +
         partition_firmware_job_count;
}

inline constexpr u32 StructuredGeneratedCompilerModuleCount(
    u32 partition_module_count) {
  return partition_module_count;
}

// Pointer-free expression bytecode consumed by the offline fixed structured
// control/state GXPs. The control job publishes the bounded loop shape once;
// one state invocation then owns each recurrence-closed snapshot vector. The
// descriptor contains no source identity, title, PC whitelist, or generated
// Cg text.
inline constexpr u32 FixedStructuredStateFormatVersion = 2;
inline constexpr u32 FixedStructuredStateHeaderWords = 32;
inline constexpr u32 FixedStructuredStateViEntryWords = 4;
inline constexpr u32 FixedStructuredStateModuleWords = 10;
inline constexpr u32 FixedStructuredStateExpressionWords = 8;
inline constexpr u32 FixedStructuredStateBindingWords = 2;
inline constexpr u32 FixedStructuredStateMaximumModules = 34;
inline constexpr u32 FixedStructuredStateMaximumExpressionsPerModule = 96;
inline constexpr u32 FixedStructuredStateMaximumRecurrenceSlots = 32;
inline constexpr u32 FixedStructuredStateMaximumBindingsPerModule = 32;
inline constexpr u32 FixedStructuredStateMaximumWords = 32 * 1024;
static_assert(StructuredGeneratedFixedStateIndexStride >=
              FixedStructuredStateMaximumModules);

// Fixed structured workspace shared by the offline control/state/preflight
// kernels. During preflight, BUFFER10 exposes exactly one 256 KiB occupancy
// plane: the largest writable-buffer shape demonstrated in Sony's shader
// compiler guide. BUFFER9 is rebound to a disjoint auxiliary plane for
// failures, immutable metadata, control snapshots, and per-module expression
// scratch. No fixed preflight shader accesses BUFFER10 beyond word 65535, and
// no invocation shares writable occupancy scratch with another invocation.
// Later generated partitions rebind BUFFER10 to the separately bounded 2 MiB
// expression-scratch allocation in GSDeviceGXM.cpp.
inline constexpr u32 StructuredGeneratedPreflightOuterCount =
    StructuredGeneratedMaximumOuterIterations;
inline constexpr u32 StructuredGeneratedPreflightAddressCount = 1024;
inline constexpr u32 StructuredGeneratedPreflightOccupancyOffset = 0;
inline constexpr u32 StructuredGeneratedPreflightOccupancyWords =
    StructuredGeneratedPreflightOuterCount *
    StructuredGeneratedPreflightAddressCount;
inline constexpr u32 StructuredGeneratedPreflightOuterFailureOffset = 0;
inline constexpr u32 StructuredGeneratedPreflightOuterTripOffset =
    StructuredGeneratedPreflightOuterFailureOffset +
    StructuredGeneratedPreflightOuterCount;
inline constexpr u32 StructuredGeneratedPreflightAddressFailureOffset =
    StructuredGeneratedPreflightOuterTripOffset +
    StructuredGeneratedPreflightOuterCount;
inline constexpr u32 StructuredGeneratedPreflightModuleFailureOffset =
    StructuredGeneratedPreflightAddressFailureOffset +
    StructuredGeneratedPreflightAddressCount;
inline constexpr u32 StructuredGeneratedPreflightMetadataOffset =
    (StructuredGeneratedPreflightModuleFailureOffset +
     FixedStructuredStateMaximumModules + 63u) & ~63u;
inline constexpr u32 StructuredGeneratedPreflightMetadataWords =
    sizeof(StructuredMemoryPreflightData) / sizeof(u32);
inline constexpr u32 StructuredGeneratedPreflightWorkspaceWords =
    StructuredGeneratedPreflightOccupancyWords;
inline constexpr u32 StructuredGeneratedControlInitialParentViOffset =
    (StructuredGeneratedPreflightMetadataOffset +
     StructuredGeneratedPreflightMetadataWords + 63u) & ~63u;
inline constexpr u32 StructuredGeneratedControlParentViOffset =
    StructuredGeneratedControlInitialParentViOffset + 16u;
inline constexpr u32 StructuredGeneratedControlChildViOffset =
    StructuredGeneratedControlParentViOffset + 16u;
inline constexpr u32 StructuredGeneratedParentViSnapshotOffset =
    (StructuredGeneratedControlChildViOffset + 16u + 63u) & ~63u;
inline constexpr u32 StructuredGeneratedParentViSnapshotWords =
    StructuredGeneratedPreflightOuterCount * 16u;
inline constexpr u32 StructuredGeneratedStateScratchOffset =
    (StructuredGeneratedParentViSnapshotOffset +
     StructuredGeneratedParentViSnapshotWords + 63u) & ~63u;
inline constexpr u32 StructuredGeneratedStateScratchWordsPerModule =
    FixedStructuredStateMaximumRecurrenceSlots +
    FixedStructuredStateMaximumExpressionsPerModule + 1u;
inline constexpr u32 StructuredGeneratedStateProgressWord =
    FixedStructuredStateMaximumRecurrenceSlots +
    FixedStructuredStateMaximumExpressionsPerModule;
inline constexpr u32 StructuredGeneratedAuxiliaryWords =
    StructuredGeneratedStateScratchOffset +
    FixedStructuredStateMaximumModules *
        StructuredGeneratedStateScratchWordsPerModule;

enum FixedStructuredStateHeaderWord : u32 {
  FixedStateHeaderFormat = 0,
  FixedStateHeaderTotalWords = 1,
  FixedStateHeaderModuleCount = 2,
  FixedStateHeaderModuleOffset = 3,
  FixedStateHeaderViOffset = 4,
  FixedStateHeaderEntryPc = 5,
  FixedStateHeaderMaximumOuterIterations = 6,
  FixedStateHeaderMaximumChildIterations = 7,
  FixedStateHeaderParentPrefixPairs = 8,
  FixedStateHeaderChildPairs = 9,
  FixedStateHeaderSuffixPairs = 10,
  FixedStateHeaderPairUpperBound = 11,
  FixedStateHeaderDemandedViMask = 12,
  FixedStateHeaderParentLiveViMask = 13,
  FixedStateHeaderOuterCounterReg = 14,
  FixedStateHeaderOuterLimitReg = 15,
  FixedStateHeaderChildCounterReg = 16,
  FixedStateHeaderChildLimitReg = 17,
  FixedStateHeaderChildCounterStep = 18,
  FixedStateHeaderOuterBranch = 19,
  FixedStateHeaderOuterBranchTakenRepeats = 20,
  FixedStateHeaderOuterCounterStep = 21,
  FixedStateHeaderConfigurationBits = 22,
  // Number of dependency-ordered fixed-state jobs which publish one complete
  // recurrence generation.  The fixed evaluator and preflight finalizer both
  // validate this field, so changing the watchdog-safe slice geometry cannot
  // silently leave the finalizer waiting for a stale hard-coded count.
  FixedStateHeaderStateSliceCount = 23,
};

enum FixedStructuredStateModuleWord : u32 {
  FixedStateModuleExpressionOffset = 0,
  FixedStateModuleExpressionCount = 1,
  FixedStateModuleRecurrenceSlotCount = 2,
  FixedStateModuleBindingOffset = 3,
  FixedStateModuleBindingCount = 4,
  FixedStateModuleUpdateOffset = 5,
  FixedStateModuleUpdateCount = 6,
  FixedStateModuleOutputOffset = 7,
  FixedStateModuleOutputCount = 8,
  FixedStateModuleOutputVector = 9,
};

struct FixedStructuredStateProgram {
  std::vector<u32> words;
  u32 module_count = 0;
  u32 expression_count = 0;
  u32 maximum_module_expressions = 0;
  u32 maximum_recurrence_slots = 0;

  bool IsValid() const;
};

// Serializes one PairPlan-proven nested-loop recurrence into the pointer-free
// bytecode consumed by both the offline GXP and the host oracle. This is a
// semantic lowering API, not a runtime-compiler or workload admission API.
bool BuildFixedStructuredStateProgram(
    const ParallelLoopKernel& transition_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 parent_entry_pc,
    u32 parent_prefix_pair_count, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, u32 configuration_bits,
    FixedStructuredStateProgram* program, std::string* error = nullptr);

bool ExecuteFixedStructuredStateReference(
    const FixedStructuredStateProgram& program,
    const u32* vf_words, std::size_t vf_word_count,
    const u32* state_words, std::size_t state_word_count,
    const u32* vu_memory_words, std::size_t vu_memory_word_count,
    u32* snapshot_words, std::size_t snapshot_word_count,
    u32* outer_state_words, std::size_t outer_state_word_count,
    u32* vi_snapshot_words, std::size_t vi_snapshot_word_count,
    std::string* error = nullptr);
// A structured root begins at a PairPlan-proven loop header, while an MSCAL
// may enter through an ordinary serial prefix. Keep the entry transition on
// an offline serial semantic owner and queue a small, title-neutral runway. Every job
// stops before the exact target PC; jobs after an early arrival are no-effect
// target checks. Longer prefixes fail transactionally before publication.
inline constexpr u32 StructuredGeneratedEntryJobCount = 4;
// BUFFER11 stores 36 float4 values for each of 64 outer iterations: ACC at
// vector 0, VF1--VF31 at their register indices, Q/P/I at vector 32, and three
// reserved vectors. BUFFER13 stores four uint4 VI groups per outer iteration.
// Keep these dimensions shared with the transactional capture/oracle owner.
inline constexpr u32 StructuredGeneratedSnapshotVectorCount = 2304;
inline constexpr u32 StructuredGeneratedSnapshotWordCount =
    StructuredGeneratedSnapshotVectorCount * 4u;
inline constexpr u32 StructuredGeneratedViSnapshotVectorCount = 256;
inline constexpr u32 StructuredGeneratedViSnapshotWordCount =
    StructuredGeneratedViSnapshotVectorCount * 4u;

// One cacheable mapped input record consumed by BUFFER3/11/12/13 of the
// generated nested direct root. Keeping the offsets public makes the producer
// and the GXM binder share one compile-time ABI instead of duplicating byte
// arithmetic.
inline constexpr u32 GeneratedNestedDirectMemoryWordOffset = 0u;
inline constexpr u32 GeneratedNestedDirectMemoryWords = 1024u * 4u;
inline constexpr u32 GeneratedNestedDirectSnapshotWordOffset =
    GeneratedNestedDirectMemoryWordOffset + GeneratedNestedDirectMemoryWords;
inline constexpr u32 GeneratedNestedDirectOuterStateWordOffset =
    GeneratedNestedDirectSnapshotWordOffset +
    StructuredGeneratedSnapshotWordCount;
inline constexpr u32 GeneratedNestedDirectViSnapshotWordOffset =
    GeneratedNestedDirectOuterStateWordOffset +
    StructuredGeneratedOuterStateWords;
inline constexpr u32 GeneratedNestedDirectInputWordCount =
    GeneratedNestedDirectViSnapshotWordOffset +
    StructuredGeneratedViSnapshotWordCount;
inline constexpr u32 GeneratedNestedDirectInputBytes =
    GeneratedNestedDirectInputWordCount * sizeof(u32);
static_assert((GeneratedNestedDirectSnapshotWordOffset & 3u) == 0u &&
              (GeneratedNestedDirectOuterStateWordOffset & 3u) == 0u &&
              (GeneratedNestedDirectViSnapshotWordOffset & 3u) == 0u);

// A structured transaction contains one fixed control draw, bounded parallel
// fixed-state slices, three offline preflight draws, a bounded continuation
// stream for every compiler-bounded scratch/store/final module, and one fixed-
// universal tail draw. Runtime Shacc never owns the fixed stages. Keeping this
// boundary shared prevents the fixed tail from being routed through undersized
// structured scratch storage.
inline constexpr bool IsStructuredGeneratedModuleStage(
    u32 stage, u32 generated_stage_count) {
  return stage < generated_stage_count;
}

inline constexpr bool IsStructuredGeneratedTailStage(
    u32 stage, u32 generated_stage_count) {
  return stage == generated_stage_count;
}

inline constexpr bool IsStructuredGeneratedPartitionKind(
    GeneratedCgExecutionKind kind) {
  return kind == GeneratedCgExecutionKind::StructuredStateSnapshots ||
         kind == GeneratedCgExecutionKind::StructuredExpressionScratch ||
         kind == GeneratedCgExecutionKind::StructuredFixedQpNumeric ||
         kind == GeneratedCgExecutionKind::StructuredFixedFmacNumeric ||
         kind ==
             GeneratedCgExecutionKind::StructuredParallelChildMemoryStore ||
         kind == GeneratedCgExecutionKind::StructuredFinalState;
}

inline constexpr bool StructuredGeneratedPartitionUsesChildGridInvocations(
    GeneratedCgExecutionKind kind) {
  return kind == GeneratedCgExecutionKind::StructuredExpressionScratch ||
         kind ==
             GeneratedCgExecutionKind::StructuredParallelChildMemoryStore;
}

inline constexpr bool StructuredGeneratedPartitionUsesNumericInvocations(
    GeneratedCgExecutionKind kind) {
  return kind == GeneratedCgExecutionKind::StructuredFixedQpNumeric ||
         kind == GeneratedCgExecutionKind::StructuredFixedFmacNumeric;
}

// Runtime-generated scratch/store roots use one independent SGX data instance
// per proven-independent (outer, child) point. Sony's SGX543/GXM guides state
// that dynamic flow forces per-instance execution and that independent data
// instances are the unit distributed by the VDM. Never put the child trip loop
// back inside one vertex invocation: that serial shape both defeats MP4
// distribution and exposed the firmware thread-timeout path on physical Vita.
// These title-neutral weights describe one finite point body; they never
// classify semantic support.  The compiler-bounded root is the watchdog unit.
// Do not turn the sum of independent data-instance work into an artificial
// draw split: the VDM owns scheduling of those instances across the SGX cores.
inline constexpr u32 StructuredGeneratedSoftwareF32WorkWeight = 16;
inline constexpr u32 StructuredGeneratedFixedQpInvocationWork = 128;
inline constexpr u32 StructuredGeneratedMaximumPartitionInvocationWork = 2048;
// SGX543's 256 resident data instances are an occupancy figure, not a draw
// limit. Sony's sceGxmDraw() contract takes a 32-bit index count, and the
// Series5 VDM index-list count field is 22 bits. The product identity index
// stream covers 65536 u16 vertices, while a structured grid is bounded to only
// 64*64 points. Let VDM schedule that complete independent grid in one draw;
// splitting it according to aggregate ALU work manufactures dependency-free
// firmware jobs and discards the MP4's normal scheduling.
inline constexpr u32 StructuredGeneratedMaximumParallelInvocationsPerJob =
    64u * 64u;
inline constexpr u32 StructuredGeneratedMaximumNumericInvocationsPerJob =
    64u * 64u;

inline constexpr u32 StructuredGeneratedSaturatingAdd(u32 left, u32 right) {
  return left > std::numeric_limits<u32>::max() - right
             ? std::numeric_limits<u32>::max()
             : left + right;
}

inline constexpr u32 StructuredGeneratedSaturatingMultiply(u32 left,
                                                            u32 right) {
  return left != 0u &&
                 right > std::numeric_limits<u32>::max() / left
             ? std::numeric_limits<u32>::max()
             : left * right;
}

inline constexpr u32 StructuredGeneratedDivideRoundUp(u32 value,
                                                       u32 divisor) {
  return value == 0u || divisor == 0u
             ? 0u
             : 1u + (value - 1u) / divisor;
}

struct StructuredGeneratedPartitionSchedule {
  u32 invocation_count = 0;
  u32 invocations_per_job = 0;
  u32 firmware_job_count = 0;
  u32 invocation_work = 0;
  u32 maximum_job_work = 0;

  bool IsValid() const {
    return invocation_count != 0u && invocations_per_job != 0u &&
           invocations_per_job <= invocation_count &&
           firmware_job_count == StructuredGeneratedDivideRoundUp(
                                     invocation_count,
                                     invocations_per_job) &&
           invocation_work != 0u && maximum_job_work != 0u;
  }
};

struct StructuredGeneratedPartitionStage {
  u32 partition_index = std::numeric_limits<u32>::max();
  u32 partition_job_index = std::numeric_limits<u32>::max();
  u32 invocation_offset = 0u;
  u32 invocation_count = 0u;
  u32 invocation_work = 0u;

  bool IsValid() const {
    return partition_index != std::numeric_limits<u32>::max() &&
           partition_job_index != std::numeric_limits<u32>::max() &&
           invocation_count != 0u && invocation_work != 0u;
  }
};

inline constexpr StructuredGeneratedPartitionSchedule
BuildStructuredGeneratedPartitionSchedule(
    GeneratedCgExecutionKind kind, u32 maximum_outer_iterations,
    u32 maximum_child_iterations, u32 expression_count,
    u32 software_f32_operation_count,
    u32 fixed_fmac_operation_count) {
  StructuredGeneratedPartitionSchedule schedule;
  if (!IsStructuredGeneratedPartitionKind(kind) ||
      kind == GeneratedCgExecutionKind::StructuredStateSnapshots) {
    return schedule;
  }

  u32 maximum_invocations_per_job = 1u;
  if (StructuredGeneratedPartitionUsesChildGridInvocations(kind)) {
    schedule.invocation_count = StructuredGeneratedSaturatingMultiply(
        maximum_outer_iterations, maximum_child_iterations);
    if (schedule.invocation_count == 0u || expression_count == 0u) {
      return {};
    }
    const u32 software_work = StructuredGeneratedSaturatingMultiply(
        software_f32_operation_count,
        StructuredGeneratedSoftwareF32WorkWeight);
    const u32 expression_work = StructuredGeneratedSaturatingAdd(
        expression_count, software_work);
    // One invocation evaluates exactly one child point. Child count belongs
    // in the grid extent, never in the per-thread work estimate.
    schedule.invocation_work = expression_work;
    maximum_invocations_per_job =
        StructuredGeneratedMaximumParallelInvocationsPerJob;
  } else if (StructuredGeneratedPartitionUsesNumericInvocations(kind)) {
    schedule.invocation_count = StructuredGeneratedSaturatingMultiply(
        maximum_outer_iterations, maximum_child_iterations);
    if (schedule.invocation_count == 0u)
      return {};
    if (kind == GeneratedCgExecutionKind::StructuredFixedFmacNumeric) {
      if (fixed_fmac_operation_count == 0u)
        return {};
      schedule.invocation_work = StructuredGeneratedSaturatingMultiply(
          fixed_fmac_operation_count,
          StructuredGeneratedSoftwareF32WorkWeight);
    } else {
      schedule.invocation_work =
          StructuredGeneratedFixedQpInvocationWork;
    }
    maximum_invocations_per_job =
        StructuredGeneratedMaximumNumericInvocationsPerJob;
  } else if (kind == GeneratedCgExecutionKind::StructuredFinalState) {
    schedule.invocation_count = 1u;
    schedule.invocation_work = std::max(expression_count, 1u);
  } else {
    return {};
  }

  if (schedule.invocation_work == 0u ||
      schedule.invocation_work >
          StructuredGeneratedMaximumPartitionInvocationWork) {
    return {};
  }
  schedule.invocations_per_job =
      std::min(schedule.invocation_count, maximum_invocations_per_job);
  schedule.firmware_job_count = StructuredGeneratedDivideRoundUp(
      schedule.invocation_count, schedule.invocations_per_job);
  schedule.maximum_job_work = StructuredGeneratedSaturatingMultiply(
      schedule.invocation_work, schedule.invocations_per_job);
  return schedule;
}

// Sony's libgxm writable-uniform contract requires a job boundary only when a
// later draw consumes an earlier draw's writes. A generated partition may be a
// scratch producer or its immediate consumer, and the following producer may
// reuse the same bounded slots. Flush every dependency-ordered partition for
// the initial exact owner. Proven-independent adjacent roots can be coalesced
// later without changing this descriptor order.
inline constexpr bool StructuredGeneratedStageNeedsFlush(u32 stage,
                                                          u32 stage_count) {
  return stage <= stage_count;
}

inline constexpr u32 StructuredGeneratedFirmwareJobCount(
    u32 unpack_jobs, u32 partition_firmware_job_count,
    u32 entry_jobs = StructuredGeneratedEntryJobCount) {
  // UNPACK + serial entry + control + bounded state slices + preflight
  // build/reduce/finalize + ordered partitions + fixed tail. Every dependency
  // stage is one firmware job.
  return unpack_jobs + entry_jobs + StructuredGeneratedPartitionStageBase +
         1u + partition_firmware_job_count;
}

// Generated compatibility is never selected merely because every compiler
// root fits Shacc.  A bundle also has to reduce dependency-ordered firmware
// jobs relative to the fixed compact owner for the same PairPlan-derived
// transaction bound.  UNPACK jobs are common to both providers and may be
// included by callers without changing the comparison.  Strictly fewer jobs
// leaves no ambiguous equal-cost promotion and bounds source construction
// before any runtime compiler request is created.
inline constexpr u32 StructuredGeneratedMaximumProfitablePartitionJobs(
    u32 fixed_firmware_jobs, u32 unpack_jobs = 0u,
    u32 entry_jobs = StructuredGeneratedEntryJobCount) {
  const u32 generated_without_partitions =
      StructuredGeneratedFirmwareJobCount(unpack_jobs, 0u, entry_jobs);
  if (fixed_firmware_jobs <= generated_without_partitions)
    return 0u;
  const u32 strict_budget =
      fixed_firmware_jobs - generated_without_partitions - 1u;
  return std::min(strict_budget,
                  StructuredGeneratedMaximumPartitionFirmwareJobs);
}

inline constexpr bool StructuredGeneratedFirmwareJobsAreProfitable(
    u32 fixed_firmware_jobs, u32 unpack_jobs,
    u32 partition_firmware_job_count,
    u32 entry_jobs = StructuredGeneratedEntryJobCount) {
  return partition_firmware_job_count != 0u &&
         partition_firmware_job_count <=
             StructuredGeneratedMaximumProfitablePartitionJobs(
                 fixed_firmware_jobs, unpack_jobs, entry_jobs) &&
         StructuredGeneratedFirmwareJobCount(
             unpack_jobs, partition_firmware_job_count, entry_jobs) <
             fixed_firmware_jobs;
}

inline constexpr u32 StructuredGeneratedEffectiveFixedFirmwareJobs(
    u32 structural_fixed_firmware_jobs,
    u32 observed_fixed_firmware_jobs) {
  return observed_fixed_firmware_jobs != 0u
             ? std::min(structural_fixed_firmware_jobs,
                        observed_fixed_firmware_jobs)
             : structural_fixed_firmware_jobs;
}

struct StructuredGeneratedBundle {
  // Immutable provenance for the PairPlan analysis which produced every key
  // below.  Shader content keys identify compiler/cache artifacts, but cannot
  // prove that a bundle belongs to the Execute entry currently being queued.
  // The product owner must attest all three fields before the first GPU effect.
  u64 program_identity = 0;
  // Host-cache publication generation. This is not a semantic support key and
  // never reaches a shader; it prevents an already-queued epoch from using an
  // artifact snapshot after the live cache owner was quarantined or replaced.
  u64 cache_generation = 0;
  u32 analysis_start_pc = 0;
  u32 configuration_bits = 0;
  std::array<ShaderKey, StructuredGeneratedMaximumPartitionModuleCount>
      partition_keys{};
  std::array<GeneratedCgExecutionKind,
             StructuredGeneratedMaximumPartitionModuleCount>
      partition_kinds{};
  std::array<StructuredFixedQpNumericDescriptor,
             StructuredGeneratedMaximumPartitionModuleCount>
      partition_fixed_qp_numeric{};
  // Exact FMAC descriptors remain in the immutable cache-owned module stream
  // and are copied into a private GXM slot before submission. Keeping only a
  // compact count here prevents every queued epoch and empty cache entry from
  // inlining 256 * 1040 bytes of descriptor storage.
  std::array<u8, StructuredGeneratedMaximumPartitionModuleCount>
      partition_fixed_fmac_operation_counts{};
  // Shared-IR execution-cost metadata. It is derived from the emitted module,
  // not a shader hash or source identity, and expands one compiler module into
  // as many bounded firmware jobs as its invocation shape requires.
  std::array<u32, StructuredGeneratedMaximumPartitionModuleCount>
      partition_expression_counts{};
  std::array<u32, StructuredGeneratedMaximumPartitionModuleCount>
      partition_software_f32_operation_counts{};
  u32 partition_module_count = 0;
  u32 partition_firmware_job_count = 0;
  // Number of recurrence-closed snapshot vectors interpreted by the fixed
  // offline state stage. No state module is submitted to ShaccCg.
  u32 state_module_count = 0;
  u32 scratch_module_count = 0;
  u32 fixed_qp_numeric_module_count = 0;
  u32 fixed_fmac_numeric_module_count = 0;
  u32 memory_store_module_count = 0;
  u32 final_module_count = 0;
  u32 entry_pc = 0;
  u32 child_entry_pc = 0;
  u32 tail_pc = 0;
  u32 maximum_outer_iterations = 0;
  u32 maximum_child_iterations = 0;
  u32 store_count = 0;
  u32 child_pair_count = 0;
  u32 parent_prefix_pair_count = 0;
  u32 suffix_pair_count = 0;
  // Sparse child-entry state written by the generated state root. These masks
  // are the exact BUFFER11/13 contract and allow a PCSX2 interpreter replay to
  // identify the first divergent outer iteration without comparing dead data.
  std::array<u8, 32> child_entry_vf_lanes{};
  u16 child_entry_vi_mask = 0;
  u8 child_entry_acc_lanes = 0;
  bool child_entry_q = false;
  bool child_entry_p = false;
  bool child_entry_i = false;
  // Maximum semantic pairs summarized by the generated roots. This comes
  // from the title-neutral CFG/loop bounds and is distinct from the fixed
  // interpreter's 16K continuation budget.
  u32 pair_upper_bound = 0;
  // Provider-neutral admission evidence. UNPACK work is omitted because it is
  // identical for fixed and generated execution. These values are derived
  // from PairPlan bounds and the current fixed compact scheduling contract;
  // they are performance metadata, never semantic support keys.
  u32 predicted_fixed_firmware_jobs = 0;
  u32 predicted_generated_firmware_jobs = 0;
  u32 maximum_profitable_partition_jobs = 0;
  u32 maximum_partition_invocations_per_job = 0;
  u32 maximum_partition_invocation_work = 0;
  u32 maximum_partition_job_work = 0;
  u32 unique_compiler_program_count = 0;
  u32 fixed_state_descriptor_words = 0;
  u32 fixed_state_expression_count = 0;
  u32 maximum_fixed_state_module_expressions = 0;
  u32 maximum_fixed_state_recurrence_slots = 0;
  u32 preflight_source_bytes = 0;
  u32 partition_source_bytes = 0;
  u32 maximum_partition_source_bytes = 0;
  std::array<u32, StructuredGeneratedMaximumPartitionModuleCount>
      partition_module_source_bytes{};
  bool runtime_memory_preflight = false;
  // Immutable PairPlan-derived metadata for the compact title-neutral
  // preflight root. It is copied into BUFFER10 of the private generation
  // before submission and never participates in semantic admission by hash.
  StructuredMemoryPreflightData memory_preflight{};
  // Scratch and store modules use one invocation per (outer, child) point.
  // Runtime preflight must publish gate=1 before any such module is submitted.
  bool parallel_child_memory_store = false;

  u32 TransactionPairUpperBound() const {
    constexpr u32 fixed_edges = 2u * StructuredGeneratedTailPairBudget;
    return pair_upper_bound != 0 &&
                   pair_upper_bound <=
                       std::numeric_limits<u32>::max() - fixed_edges
               ? pair_upper_bound + fixed_edges
               : 0u;
  }

  // Key ownership is deliberately narrower than complete descriptor
  // validity. Fixed numeric kernels are offline product assets and must not
  // carry a runtime compiler key; every generated partition must carry one.
  // Keeping this check separate lets planner diagnostics distinguish missing
  // compiler ownership from a later profitability or metadata rejection.
  bool HasCompilerKeyOwnership() const {
    if (partition_module_count == 0u ||
        partition_module_count > partition_keys.size()) {
      return false;
    }
    for (u32 index = 0u; index < partition_module_count; index++) {
      const GeneratedCgExecutionKind kind = partition_kinds[index];
      if (!IsStructuredGeneratedPartitionKind(kind) ||
          kind == GeneratedCgExecutionKind::StructuredStateSnapshots) {
        return false;
      }
      const bool fixed_numeric =
          kind == GeneratedCgExecutionKind::StructuredFixedQpNumeric ||
          kind == GeneratedCgExecutionKind::StructuredFixedFmacNumeric;
      const bool has_key = partition_keys[index].high != 0u ||
                           partition_keys[index].low != 0u;
      if (fixed_numeric == has_key)
        return false;
    }
    return true;
  }

  bool HasKeys() const {
    return HasCompilerKeyOwnership() &&
           state_module_count != 0 &&
           state_module_count <= FixedStructuredStateMaximumModules &&
           maximum_outer_iterations != 0u &&
           maximum_outer_iterations <=
               StructuredGeneratedPreflightOuterCount &&
           maximum_fixed_state_module_expressions != 0u &&
           maximum_fixed_state_module_expressions <=
               StructuredGeneratedMaximumStateModuleWork /
                   maximum_outer_iterations &&
           fixed_state_descriptor_words >= FixedStructuredStateHeaderWords &&
           fixed_state_descriptor_words <= FixedStructuredStateMaximumWords &&
           partition_module_count != 0 &&
           partition_module_count <= partition_keys.size() &&
           partition_firmware_job_count != 0u &&
           partition_firmware_job_count <=
               StructuredGeneratedMaximumPartitionFirmwareJobs &&
           maximum_profitable_partition_jobs != 0u &&
           maximum_profitable_partition_jobs <=
               StructuredGeneratedMaximumPartitionFirmwareJobs &&
           partition_firmware_job_count <=
               maximum_profitable_partition_jobs &&
           maximum_partition_invocations_per_job != 0u &&
           maximum_partition_invocation_work != 0u &&
           maximum_partition_job_work != 0u &&
           predicted_fixed_firmware_jobs != 0u &&
           predicted_generated_firmware_jobs ==
               StructuredGeneratedFirmwareJobCount(
                   0u, partition_firmware_job_count) &&
           StructuredGeneratedFirmwareJobsAreProfitable(
               predicted_fixed_firmware_jobs, 0u,
               partition_firmware_job_count) &&
           unique_compiler_program_count != 0u &&
           unique_compiler_program_count <=
               StructuredGeneratedMaximumResidentProgramsPerBundle &&
           scratch_module_count + fixed_qp_numeric_module_count +
                   fixed_fmac_numeric_module_count +
                   memory_store_module_count +
                   final_module_count ==
               partition_module_count &&
           memory_store_module_count != 0 && final_module_count != 0 &&
           parallel_child_memory_store && runtime_memory_preflight &&
           memory_preflight.header0[2] <=
               StructuredMemoryPreflightMaximumAccesses &&
           (memory_preflight.header0[2] == 0u ||
            maximum_child_iterations <=
                StructuredGeneratedMaximumPreflightAccessWork /
                    memory_preflight.header0[2]) &&
           [&]() {
             u32 actual_partition_jobs = 0u;
             u32 actual_maximum_invocations = 0u;
             u32 actual_maximum_invocation_work = 0u;
             u32 actual_maximum_job_work = 0u;
             for (u32 index = 0; index < partition_module_count; index++) {
               const GeneratedCgExecutionKind kind = partition_kinds[index];
               if (!IsStructuredGeneratedPartitionKind(kind) ||
                   kind == GeneratedCgExecutionKind::StructuredStateSnapshots)
                 return false;
               const bool fixed_qp = kind ==
                   GeneratedCgExecutionKind::StructuredFixedQpNumeric;
               const bool fixed_fmac = kind ==
                   GeneratedCgExecutionKind::StructuredFixedFmacNumeric;
               const bool has_key = partition_keys[index].high != 0 ||
                                    partition_keys[index].low != 0;
               if (fixed_qp) {
                 if (has_key || !partition_fixed_qp_numeric[index].IsValid())
                   return false;
               } else if (fixed_fmac) {
                 const u32 operation_count =
                     partition_fixed_fmac_operation_counts[index];
                 if (has_key || operation_count == 0u ||
                     operation_count >
                         StructuredFixedFmacNumericMaximumOperations)
                   return false;
               } else if (!has_key) {
                 return false;
               }
               if (!fixed_fmac &&
                   partition_fixed_fmac_operation_counts[index] != 0u)
                 return false;
               const StructuredGeneratedPartitionSchedule schedule =
                   BuildStructuredGeneratedPartitionSchedule(
                       kind, maximum_outer_iterations,
                       maximum_child_iterations,
                       partition_expression_counts[index],
                       partition_software_f32_operation_counts[index],
                       partition_fixed_fmac_operation_counts[index]);
               if (!schedule.IsValid() ||
                   schedule.firmware_job_count >
                       StructuredGeneratedMaximumPartitionFirmwareJobs ||
                   actual_partition_jobs >
                       StructuredGeneratedMaximumPartitionFirmwareJobs -
                           schedule.firmware_job_count) {
                 return false;
               }
               actual_partition_jobs += schedule.firmware_job_count;
               actual_maximum_invocations = std::max(
                   actual_maximum_invocations,
                   schedule.invocations_per_job);
               actual_maximum_invocation_work = std::max(
                   actual_maximum_invocation_work,
                   schedule.invocation_work);
               actual_maximum_job_work = std::max(
                   actual_maximum_job_work, schedule.maximum_job_work);
             }
             return actual_partition_jobs == partition_firmware_job_count &&
                    actual_maximum_invocations ==
                        maximum_partition_invocations_per_job &&
                    actual_maximum_invocation_work ==
                        maximum_partition_invocation_work &&
                    actual_maximum_job_work == maximum_partition_job_work;
           }();
  }

  u32 GeneratedStageCount() const {
    return StructuredGeneratedStageCount(partition_firmware_job_count);
  }

  u32 CompilerModuleCount() const {
    return StructuredGeneratedCompilerModuleCount(partition_module_count);
  }

  ShaderKey ReadyKey() const {
    if (!HasKeys())
      return {};
    for (u32 index = partition_module_count; index != 0; index--) {
      const ShaderKey key = partition_keys[index - 1u];
      if (key.high != 0 || key.low != 0)
        return key;
    }
    return {};
  }

  StructuredGeneratedPartitionSchedule PartitionSchedule(
      u32 partition_index) const {
    if (partition_index >= partition_module_count)
      return {};
    return BuildStructuredGeneratedPartitionSchedule(
        partition_kinds[partition_index], maximum_outer_iterations,
        maximum_child_iterations,
        partition_expression_counts[partition_index],
        partition_software_f32_operation_counts[partition_index],
        partition_fixed_fmac_operation_counts[partition_index]);
  }

  u32 PartitionInvocationCount(u32 partition_index) const {
    return PartitionSchedule(partition_index).invocation_count;
  }

  bool MapPartitionStage(u32 stage,
                         StructuredGeneratedPartitionStage* mapped) const {
    if (!mapped)
      return false;
    *mapped = {};
    if (stage < StructuredGeneratedPartitionStageBase ||
        stage >= GeneratedStageCount()) {
      return false;
    }
    u32 ordinal = stage - StructuredGeneratedPartitionStageBase;
    for (u32 partition = 0u; partition < partition_module_count;
         partition++) {
      const StructuredGeneratedPartitionSchedule schedule =
          PartitionSchedule(partition);
      if (!schedule.IsValid())
        return false;
      if (ordinal >= schedule.firmware_job_count) {
        ordinal -= schedule.firmware_job_count;
        continue;
      }
      const u32 offset = ordinal * schedule.invocations_per_job;
      if (offset >= schedule.invocation_count)
        return false;
      mapped->partition_index = partition;
      mapped->partition_job_index = ordinal;
      mapped->invocation_offset = offset;
      mapped->invocation_count = std::min(
          schedule.invocations_per_job,
          schedule.invocation_count - offset);
      mapped->invocation_work = StructuredGeneratedSaturatingMultiply(
          schedule.invocation_work, mapped->invocation_count);
      return mapped->IsValid();
    }
    return false;
  }

  bool MatchesOwner(u64 expected_program_identity,
                    u32 expected_analysis_start_pc,
                    u32 expected_configuration_bits) const {
    return program_identity != 0 &&
           program_identity == expected_program_identity &&
           analysis_start_pc == expected_analysis_start_pc &&
           configuration_bits == expected_configuration_bits;
  }

  bool MatchesLiveGeneration(const StructuredGeneratedBundle& live) const {
    if (cache_generation == 0u ||
        cache_generation != live.cache_generation ||
        !live.MatchesOwner(program_identity, analysis_start_pc,
                           configuration_bits) ||
        partition_module_count != live.partition_module_count) {
      return false;
    }
    for (u32 index = 0; index < partition_module_count; index++) {
      if (!(partition_keys[index] == live.partition_keys[index]) ||
          partition_kinds[index] != live.partition_kinds[index]) {
        return false;
      }
    }
    return true;
  }
};

// Physical fixed-provider cost for one exact source/entry/configuration and
// VIF transition shape.  UNPACK submissions are common to fixed and generated
// execution, so fixed_firmware_jobs deliberately excludes them.  The profile
// is a profitability input only; it never decides semantic support.
struct StructuredGeneratedFixedCostObservation {
  u32 observed_pairs = 0;
  u32 fixed_firmware_jobs = 0;
};

// Proves that every reachable E-bit or external exit from this analysis entry
// passes through target_block. A generated structured transaction may be
// prequeued only under this proof; shader-side repair after an earlier fixed
// entry job terminates would violate transactional no-effect ownership.
bool StructuredEntryDominatesEveryExit(const ProgramAnalysis& analysis,
                                       u32 target_block);

// Title-neutral generated-module planners shared by product bundle ownership
// and exhaustive/offline compiler validation. State slices are recurrence-
// closed and have disjoint writable outputs; exactly one owns shared VI/control
// publication, allowing all slices to be queued in one dependency stage.
bool BuildCompilerBoundedStructuredStateModules(
    const ParallelLoopKernel& transition_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 parent_entry_pc,
    u32 parent_prefix_pair_count, u32 maximum_outer_iterations,
    u32 maximum_child_iterations,
    std::vector<GeneratedCgProgram>* modules,
    std::string* error = nullptr);

bool BuildCompilerBoundedStructuredStoreModules(
    const ParallelLoopKernel& kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, u32 maximum_outer_iterations,
    u32 maximum_child_iterations,
    std::vector<GeneratedCgProgram>* modules, u32* scratch_module_count,
    std::string* error = nullptr,
    u32 maximum_partition_modules =
        StructuredGeneratedMaximumPartitionModuleCount);

bool BuildCompilerBoundedStructuredFinalModules(
    const ParallelLoopKernel& kernel,
    const EnclosingLoopEntryIndependence& boundary,
    const NaturalLoop& child_loop, const NaturalLoop& enclosing_loop,
    const StructuredLoopTailProof& tail, u32 maximum_outer_iterations,
    u32 maximum_child_iterations,
    std::vector<GeneratedCgProgram>* modules, u32* scratch_module_count,
    std::string* error = nullptr,
    u32 maximum_partition_modules =
        StructuredGeneratedMaximumPartitionModuleCount);

// Builds one complete PairPlan-derived generated-provider candidate without
// touching the compiler registry or product cache. This is the deterministic
// planner/oracle seam used to validate arbitrary captured microprograms on
// ARMv7 before any ShaccCg or GXM effect exists. The final flag is diagnostic
// only: it may return generated sources from a resident-capacity rejection so
// offline Sony tools can characterize them. Product callers leave it false;
// rejected modules own no key, submission, private-state, or guest effect.
bool BuildCompilerBoundedStructuredCandidate(
    const ProgramAnalysis& analysis, u32 loop_index, u32 configuration_bits,
    StructuredGeneratedBundle* descriptor,
    std::vector<GeneratedCgProgram>* modules, u64* score,
    std::string* error = nullptr, u32 fixed_firmware_job_budget = 0u,
    bool preserve_capacity_rejected_modules = false);

enum class StructuredGeneratedBundleState : u8 {
  Missing,
  Compiling,
  Ready,
  Failed,
  Unavailable,
};

bool RequestStructuredGeneratedBundle(
    u64 program_identity, const ProgramAnalysis& analysis,
    u32 configuration_bits, u32 fixed_firmware_job_budget = 0u,
    StructuredGeneratedBundle* bundle = nullptr);
StructuredGeneratedBundleState QueryStructuredGeneratedBundle(
    u64 program_identity, StructuredGeneratedBundle* bundle = nullptr);
void RecordStructuredGeneratedFixedCost(
    u64 program_identity, u32 execute_count, u32 unpack_submission_count,
    u32 observed_pairs, u32 fixed_firmware_jobs);
bool QueryStructuredGeneratedFixedCost(
    u64 program_identity, u32 execute_count, u32 unpack_submission_count,
    StructuredGeneratedFixedCostObservation* observation);
// Permanently quarantine a cached bundle whose private GPU transaction failed
// before commit.  Later epochs retain the exact CPU/fixed-core fallback path
// without repeatedly paying the same generated submission and replay cost.
bool RejectStructuredGeneratedBundle(u64 program_identity);
bool CopyStructuredFixedStateProgram(u64 program_identity, u32* destination,
                                     std::size_t destination_words,
                                     u32* copied_words = nullptr);
bool CopyStructuredFixedFmacNumericDescriptors(
    u64 program_identity, StructuredFixedFmacNumericDescriptor* destination,
    std::size_t destination_count, u32* copied_descriptors = nullptr);
// Called after the GS owner registers one generated root. Advances every
// cached structured bundle by at most one dependency without requiring MTVU
// to rebuild a statically cached epoch.
void PumpStructuredGeneratedBundles();

// Non-blocking hot-tier request. CPU MTVU remains authoritative while ShaccCg
// compiles and the GS owner registers this exact generated key. ProgramAnalysis
// is the PCSX2-derived control owner and must exactly match the serialized
// PairPlans before any source is handed to the runtime compiler.
enum class UniversalStateMachineRequestCacheState : u8 {
  Missing,
  Unavailable,
  Keyed,
};

// Cheap hot-path lookup performed before rebuilding ProgramAnalysis. Keyed
// includes compiling and registered programs; QueryGeneratedProgram() owns
// that later state distinction. Unavailable is a cached, title-neutral source
// or compiler-shape decision and leaves CPU MTVU authoritative.
UniversalStateMachineRequestCacheState
LookupUniversalStateMachineProgram(
    u64 program_identity, const GeneratedUniversalProfile& profile,
    ShaderKey* key = nullptr);

bool RequestUniversalStateMachineProgram(
    u64 program_identity, const UniversalMicroProgram& program,
    const ProgramAnalysis& analysis,
    const GeneratedUniversalProfile& profile,
    ShaderKey* key);

void ClearUniversalStateMachineProgramCache();

} // namespace VitaGpuVu
