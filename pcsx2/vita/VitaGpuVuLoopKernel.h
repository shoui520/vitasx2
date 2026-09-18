// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "vita/VitaGpuVuProgram.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace VitaGpuVu {

enum class ScalarDomain : u8 {
  Raw,
  Float,
  SignedInt,
  UnsignedInt,
};

enum class ExpressionKind : u8 {
  ConstantFloat,
  ConstantSigned,
  ConstantUnsigned,
  InitialVf,
  InitialAcc,
  InitialQ,
  InitialP,
  InitialI,
  InvariantVf,
  InvariantAcc,
  InvariantQ,
  InvariantP,
  InvariantI,
  // One lane of a compact immutable qword table indexed by the enclosing
  // iteration. The table is materialized transactionally from architectural
  // entry VF state and/or immutable VU-memory qwords before submission; no VU
  // instruction body or serial GPU state pass participates.
  CompactOuterInput,
  // Transaction-private scalar published by an earlier compiler-bounded
  // generated module. This is a generated-code staging leaf, not a VU1
  // architectural register or a semantic admission shortcut.
  StructuredScratch,
  Memory,
  Add,
  Subtract,
  Multiply,
  // One VU FMAC operation whose binary32 result is architecturally visible
  // before a later VU instruction consumes it.  Keep these distinct from
  // host-side symbolic arithmetic so psp2cgc/ShaccCg cannot retain extra SGX
  // precision across PairPlan instruction boundaries.
  RoundedAdd,
  RoundedSubtract,
  // One VU multiply whose binary32 result must become observable before a
  // consuming add/subtract.  PCSX2 VUops.cpp::_vuOpMADD(), _vuOpMSUB(), and
  // _vuOPMSUB() all expose this operation boundary; keeping it in the shared
  // IR prevents psp2cgc/ShaccCg from contracting it into an SGX MAD.
  RoundedMultiply,
  Divide,
  Minimum,
  Maximum,
  Absolute,
  Negate,
  Reciprocal,
  SquareRoot,
  // PCSX2 VUops.cpp::_vuERSADD() first publishes the rounded x*x+y*y+z*z
  // intermediate before applying the configured reciprocal implementation.
  EfuSumXyzSquares,
  ArmApproximateReciprocal,
  // PCSX2 VUops.cpp::_vuESQRT() under the explicitly configured Vita
  // approximate-P tier.  This remains distinct from SquareRoot: the result is
  // the Cortex-A9 VRSQRTE/VRSQRTS estimate/refinement contract, not SGX sqrt.
  ArmApproximateSquareRoot,
  ReciprocalSquareRoot,
  FloatToInt,
  IntToFloat,
  Normalize,
  // Exact finite recurrence selected by the enclosing-loop invocation.  The
  // generated root evaluates the same rounded ADD chain as the PairPlan
  // transition and selects the value after `outer index` steps without a
  // runtime loop or mutable snapshot.  `immediate` is the attested outer
  // iteration count; operands 0/1 are the initial value and increment, while
  // reg records whether the carried value was the left (0) or right (1)
  // PairPlan operand. Multiple ADDs per enclosing iteration are represented
  // by nesting these finite roots, preserving every publication boundary. If
  // immediate has OuterRepeatedAddScheduledBit set, its remaining bits index
  // an exact finite per-outer publication schedule instead of the ordinary
  // `outer index` count.
  OuterRepeatedAdd,
};

constexpr u32 OuterRepeatedAddScheduledBit = 0x80000000u;

inline bool IsScheduledOuterRepeatedAdd(u32 immediate) {
  return (immediate & OuterRepeatedAddScheduledBit) != 0u;
}

inline u32 OuterRepeatedAddScheduleIndex(u32 immediate) {
  return immediate & ~OuterRepeatedAddScheduledBit;
}

// One VU-memory qword address expressed at the natural-loop entry. The
// invocation coefficient is in qwords and the complete address is reduced
// modulo VU1_MEMSIZE / 16 by the eventual stream binder.
struct AffineQwordAddress {
  u8 base_vi = 0;
  bool valid = false;
  s32 invocation_coefficient = 0;
  s32 qword_offset = 0;
  // Enclosing-loop coefficient for a flattened (outer, child) invocation
  // grid.  Existing one-dimensional kernels leave this zero.  Keeping both
  // coefficients in the common PairPlan IR lets one branchless GXP address
  // immutable VIF input directly, without an outer-state snapshot pass.
  s32 outer_invocation_coefficient = 0;

  constexpr AffineQwordAddress() = default;
  // Preserve the existing semantic argument order independently of compact
  // host member layout. No packed/unaligned accesses or narrower coefficients.
  constexpr AffineQwordAddress(u8 base, s32 coefficient, s32 offset,
                               bool is_valid, s32 outer_coefficient = 0)
      : base_vi(base), valid(is_valid), invocation_coefficient(coefficient),
        qword_offset(offset), outer_invocation_coefficient(outer_coefficient) {}

  bool operator==(const AffineQwordAddress &other) const {
    return base_vi == other.base_vi &&
           invocation_coefficient == other.invocation_coefficient &&
           qword_offset == other.qword_offset && valid == other.valid &&
           outer_invocation_coefficient ==
               other.outer_invocation_coefficient;
  }
};
static_assert(sizeof(AffineQwordAddress) == 16u);
static_assert(alignof(AffineQwordAddress) == alignof(s32));
static_assert(std::is_trivially_copyable_v<AffineQwordAddress>);

// One 16-bit VI value expressed at an enclosing-loop header.  The value is
// `(base(base_vi) + offset) & 0xffff`; base zero denotes a constant, 1--15
// denote the immutable entry VI snapshot, and the two values immediately
// above the architectural VI file denote the VIF TOP/ITOP values read by
// XTOP/XITOP.  Keeping those VIF values symbolic lets a generated root lower a
// complete MSCAL entry without executing its acyclic setup pairs on ARM.
// This is deliberately separate from AffineQwordAddress because it describes
// full VI state rather than a VU-memory address or child invocation stride.
inline constexpr u8 AffineViBaseVifTop = 16u;
inline constexpr u8 AffineViBaseVifItop = 17u;
inline constexpr u8 AffineViBaseCount = 18u;

struct AffineViValue {
  u8 base_vi = 0;
  s32 offset = 0;
  bool valid = false;

  bool operator==(const AffineViValue& other) const {
    return base_vi == other.base_vi && offset == other.offset &&
           valid == other.valid;
  }
};

// Evaluates only one bounded affine VI formula from immutable epoch inputs.
// It executes no guest instruction and is suitable for pre-effect admission
// and transactional successor checks.
bool EvaluateAffineViRuntimeValue(
    const AffineViValue& value, const std::array<u16, 16>& initial_vi,
    u16 vif_top, u16 vif_itop, u16* result);

// Resolves one affine VU-memory qword address from the immutable child-entry
// VI/VIF snapshot and the exact flattened invocation coordinates. VU1 memory
// wraps at 1,024 qwords. This performs no memory access or guest operation and
// is shared by pre-effect ownership proofs and descriptor construction.
bool EvaluateAffineQwordRuntimeAddress(
    const AffineQwordAddress& address,
    const std::array<u16, 16>& child_entry_vi, u16 vif_top, u16 vif_itop,
    u32 outer_iteration, u32 child_iteration, u16* qword);

struct ExpressionNode {
  ExpressionKind kind = ExpressionKind::ConstantFloat;
  ScalarDomain domain = ScalarDomain::Raw;
  u8 reg = 0;
  u8 lane = 0;
  std::array<u32, 3> operands{};
  AffineQwordAddress memory_address;
  u32 immediate = 0;
};
// Host-only IR: keys/encoded programs serialize named fields, never this
// object representation. Group the byte fields to avoid padding in every
// cold graph and retained runtime graph while keeping ARM word alignment.
static_assert(sizeof(ExpressionNode) == 36u);
static_assert(alignof(ExpressionNode) == alignof(u32));
static_assert(std::is_trivially_copyable_v<ExpressionNode>);

struct LoopStore {
  u32 pair_pc = 0;
  AffineQwordAddress address;
  std::array<u32, 4> values{};
  u8 write_mask = 0;
  u8 source_vf = 0;
};

enum class CompactQwordSourceKind : u8 {
  InitialVf,
  Memory,
};

struct CompactQwordSource {
  CompactQwordSourceKind kind = CompactQwordSourceKind::InitialVf;
  u8 reg = 0;
  AffineQwordAddress memory_address;

  bool operator==(const CompactQwordSource& other) const {
    return kind == other.kind && reg == other.reg &&
           memory_address == other.memory_address;
  }
};

// One qword per enclosing iteration. Sources are copied into the immutable
// mapped input generation once, then addressed as one ordinary raw-qword
// binding by the generated GXP.
struct CompactOuterInputTable {
  std::vector<CompactQwordSource> sources;

  bool operator==(const CompactOuterInputTable& other) const {
    return sources == other.sources;
  }
};

// Number of exact rounded ADD publications applied by each enclosing-loop
// invocation after selecting its compact raw source. This handles sliding VU
// register windows whose publication depth saturates or otherwise changes in
// a finite, statically proven pattern without a CPU semantic precompute pass.
struct OuterRepeatedAddSchedule {
  std::vector<u8> add_counts;

  bool operator==(const OuterRepeatedAddSchedule& other) const {
    return add_counts == other.add_counts;
  }
};

struct StructuredScratchOutput {
  u32 expression = 0;
  u8 slot = 0;
};

struct ViEvolutionPairStates {
  std::array<std::vector<s32>, 16> prefix;
  // Symbolic value before each pair.  Unlike prefix, this also represents a
  // register reset/derivation inside the loop (for example VI6=VI0 followed
  // by four LQI operations).  It is still restricted to one architectural VI
  // base plus an invocation coefficient and constant offset.
  std::array<std::vector<AffineQwordAddress>, 16> address_state;
};

struct ViEvolution {
  std::array<s32, 16> step{};
  u32 affine_mask = 0xffff;
  u16 written_mask = 0;
  // AnalyzeViEvolution publishes these immutable per-pair states once.
  // Kernel slicing/composition copies retain that owner instead of cloning
  // all 32 vectors. Steps/masks remain per-kernel values; no live VI or VU
  // memory is shared here. PCSX2 VUops/VUmicroFast still own their derivation.
  std::shared_ptr<const ViEvolutionPairStates> pair_states;

  std::span<const s32> PrefixForRegister(u32 reg) const {
    return pair_states ? std::span<const s32>(pair_states->prefix[reg]) :
                         std::span<const s32>{};
  }
  std::span<const AffineQwordAddress> AddressesForRegister(u32 reg) const {
    return pair_states ?
        std::span<const AffineQwordAddress>(pair_states->address_state[reg]) :
        std::span<const AffineQwordAddress>{};
  }
};

// One VF lane whose every write in the reachable region is an idempotent
// self-clamp against a hardwired VF00 lane, i.e. `vfN.l = max(vfN.l, c)` or
// `mini`. Such a lane is a usable entry uniform exactly when the runtime seed
// already satisfies the clamp: the region is then a no-op on that lane, so the
// CPU snapshot can never become stale behind an accepted GPU draw. The
// descriptor path verifies the seed once per draw; nothing is assumed about
// the program's identity.
struct ClampStableLane {
  u8 reg = 0;
  u8 lane = 0;
  u32 bound_bits = 0;
  bool minimum = false;

  bool operator==(const ClampStableLane &other) const {
    return reg == other.reg && lane == other.lane &&
           bound_bits == other.bound_bits && minimum == other.minimum;
  }
};

struct ParallelLoopKernel {
  u32 loop_index = 0;
  u32 header_pc = 0;
  u32 latch_pc = 0;
  u32 pair_count = 0;
  u32 maximum_backedge_distance = 0;
  u32 collapsed_idempotent_recurrences = 0;
  // Immutable semantic identity used while slicing Q/P producers.  Direct
  // legacy callers leave this zero and therefore cannot accidentally acquire
  // a speedhack-specific expression.  The general generated tier supplies the
  // UniversalMicroProgram identity explicitly.
  u32 configuration_bits = 0;
  // Opt-in physical diagnostic only. Product kernels leave this false so an
  // exact software FTOI probe cannot become per-vertex hot-path work. Host
  // validation enables it on a copied kernel when the probe ABI itself needs
  // coverage.
  bool enable_private_ftoi_probe = false;
  // Exact flattened dispatch geometry. Both fields are zero for the ordinary
  // one-dimensional loop kernel. A grid is valid only when their product is
  // the packed GIF vertex count and every enclosing recurrence has already
  // been reduced to immutable inputs or finite expression nodes.
  u32 outer_iteration_count = 0;
  u32 child_iteration_count = 0;
  ViEvolution vi;
  std::vector<ExpressionNode> expressions;
  std::vector<LoopStore> stores;
  std::vector<CompactOuterInputTable> compact_outer_inputs;
  std::vector<OuterRepeatedAddSchedule> outer_repeated_add_schedules;
  // Generated compiler partition only. Each output is written for every
  // active outer/child iteration and consumed after a GXM job boundary by a
  // later module in the same private transaction.
  std::vector<StructuredScratchOutput> structured_scratch_outputs;
  std::array<std::array<u32, 4>, 32> final_vf_values{};
  std::array<u32, 4> final_acc_values{};
  u32 final_q_value = 0;
  u32 final_p_value = 0;
  u32 final_i_value = 0;
  std::array<u8, 32> final_vf_lanes{};
  u8 final_acc_lanes = 0;
  bool final_q = false;
  bool final_p = false;
  bool final_i = false;
  // Child-entry state after executing the enclosing-loop prefix. Structured
  // generated roots publish these sparse values for the parallel child job;
  // they are intentionally distinct from final_* values carried into the next
  // parent iteration.
  std::array<std::array<u32, 4>, 32> child_entry_vf_values{};
  std::array<u32, 4> child_entry_acc_values{};
  u32 child_entry_q_value = 0;
  u32 child_entry_p_value = 0;
  u32 child_entry_i_value = 0;
  std::array<u8, 32> child_entry_vf_lanes{};
  u8 child_entry_acc_lanes = 0;
  bool child_entry_q = false;
  bool child_entry_p = false;
  bool child_entry_i = false;
  std::array<AffineViValue, 16> child_entry_vi_values{};
  u16 child_entry_vi_mask = 0;
  std::array<u8, 32> stable_initial_vf_lanes{};
  std::vector<ClampStableLane> clamp_stable_lanes;
  u8 stable_initial_acc_lanes = 0;
  bool stable_initial_q = false;
  bool stable_initial_p = false;
  bool stable_initial_i = false;
  bool acyclic_entry_inlined = false;
  bool enclosing_prefix_inlined = false;
  bool enclosing_suffix_inlined = false;
  bool enclosing_final_state_pass_through = false;
  bool enclosing_final_vi_state_pass_through = false;
  bool requires_dynamic_entry_state = false;
  bool has_true_recurrence = false;
  bool has_unsupported_expression = false;
  bool independent_store_values = false;
  bool independent_final_state = false;
  bool independent_child_entry_state = false;
  bool independent_child_entry_vi = false;
};

// Dependence result for the serial prefix from an enclosing loop header to a
// child-loop header.  `prior_*` is the demanded subset which still depends on
// values carried from the preceding parent iteration.  Empty prior masks prove
// that the child value kernel may be parameterized by parent/child induction
// and memory inputs instead of serializing the complete parent VF state.
struct EnclosingLoopEntryIndependence {
  u32 child_loop = 0;
  u32 parent_loop = 0;
  std::vector<u32> prefix_blocks;
  std::array<u8, 32> demanded_vf_lanes{};
  std::array<u8, 32> prior_vf_lanes{};
  // Architectural VI values required at the child header.  This includes
  // every affine VU-memory base and the canonical child-loop counter/limit;
  // bit zero is never set because VI0 is hardwired zero.
  u16 demanded_vi_mask = 0;
  u16 prior_vi_mask = 0;
  // Reverse liveness at the enclosing-loop header. These are the child final
  // values which must be fed into the next parent iteration; all other child
  // register results are dead at the fusion boundary.
  std::array<u8, 32> parent_live_vf_lanes{};
  u16 parent_live_vi_mask = 0;
  u8 demanded_acc_lanes = 0;
  u8 prior_acc_lanes = 0;
  u8 parent_live_acc_lanes = 0;
  bool demanded_q = false;
  bool demanded_p = false;
  bool demanded_i = false;
  bool prior_q = false;
  bool prior_p = false;
  bool prior_i = false;
  bool parent_live_q = false;
  bool parent_live_p = false;
  bool parent_live_i = false;
  bool independent_of_prior_parent_state = false;
};

// Structural contract for replacing repeated executions of a nested child
// loop with generated state/journal roots.  Intermediate parent suffixes may
// be summarized only when they have one acyclic path to the canonical parent
// latch, contain no output observer, and do not alter state live at the next
// child entry.  The final iteration is never summarized: final_resume_pc is
// the real child-exit successor at which an exact generated/fixed tail resumes.
struct StructuredLoopTailProof {
  u32 child_loop = 0;
  u32 parent_loop = 0;
  u32 final_resume_block = 0;
  u32 final_resume_pc = 0;
  u32 parent_exit_block = 0;
  u32 parent_exit_pc = 0;
  std::vector<u32> suffix_blocks;
  u32 summarized_pair_count = 0;
  u32 parent_counter_write_count = 0;
  bool exact_final_resume_proven = false;
  bool intermediate_suffix_proven = false;
};

struct StructuredMemoryDependenceProof {
  u32 load_sites = 0;
  u32 store_sites = 0;
  u32 compared_addresses = 0;
  bool constant_address_bases = false;
  bool requires_runtime_preflight = false;
  // Loads which alias a store in the same logical child iteration remain
  // conservative: the expression DAG does not retain instruction ordering.
  bool intra_iteration_access_order_safe = false;
  // Outer invocations may execute concurrently only when their complete
  // read/write qword sets are disjoint wherever either side writes.
  bool cross_outer_accesses_independent = false;
};

// Exact finite reduction of one counted enclosing loop and its counted child
// loop.  The generated vertex root consumes the flattened (outer, child)
// domain directly; no parent-state snapshot, precompute root, or serial SGX
// interpreter participates.  final_parent_vi_values names the state at the
// proven parent exit seam.  The later tail/observer proof remains responsible
// for extending that state through the post-loop region to the E-bit resume.
struct ClosedFormNestedLoopProof {
  u32 child_loop = 0;
  u32 parent_loop = 0;
  u32 outer_iteration_count = 0;
  u32 child_iteration_count = 0;
  // Exact dynamic pairs from the MSCAL/MSCNT entry through the first parent
  // header.  A finite counted setup loop is unrolled in the compile-time
  // expression graph; no CPU semantic pair or GPU serial interpreter is
  // introduced.
  u32 entry_prefix_pair_count = 0;
  u32 summarized_entry_loop = std::numeric_limits<u32>::max();
  u32 summarized_entry_loop_iterations = 0;
  AffineViValue summarized_entry_counter_value;
  AffineViValue summarized_entry_limit_value;
  StructuredLoopTailProof tail;
  AffineViValue parent_counter_entry_value;
  AffineViValue parent_counter_limit_value;
  std::array<AffineViValue, 16> final_parent_vi_values{};
  u16 final_parent_vi_mask = 0;
  u8 parent_counter_reg = 0;
  u8 parent_counter_limit_reg = 0;
  u8 parent_branch_kind = 0;
  u8 summarized_entry_counter_reg = 0;
  u8 summarized_entry_limit_reg = 0;
  u8 summarized_entry_branch_kind = 0;
  s32 parent_counter_step = 0;
  s32 summarized_entry_counter_step = 0;
  // Some resumed programs derive the finite setup-loop counter from an
  // immutable ILW/ILWR input before entering the loop.  Such control is not
  // representable by AffineViValue alone.  The generated root still unrolls
  // the observed finite count, but product admission must replay the bounded
  // PairPlan VI/control slice against the immutable epoch memory before any
  // GPU effect instead of trusting the cached output contract.
  bool entry_loop_control_requires_pairplan_preflight = false;
  bool entry_loop_trip_count_requires_runtime_attestation = false;
  bool parent_trip_count_requires_runtime_attestation = false;
  bool parent_entry_reduced = false;
  bool child_entry_reduced = false;
  bool final_parent_state_reduced = false;
};

// Evaluates the two finite counted-control proofs from immutable epoch VI and
// VIF inputs. It executes no guest pair. The returned counts are the exact
// setup-loop and enclosing-loop dimensions which a static generated root must
// own; callers compare them with the descriptor before any GPU effect.
bool EvaluateClosedFormNestedLoopRuntimeControl(
    const ClosedFormNestedLoopProof& proof,
    const std::array<u16, 16>& initial_vi, u16 vif_top, u16 vif_itop,
    u32* summarized_entry_iterations, u32* outer_iterations,
    std::string* error = nullptr);

// Builds the steady-state semantic kernel of one natural loop. Values are
// sliced backward through architectural VF/ACC/Q/P/I state and across a
// bounded number of backedges. A slice is parallel only when it terminates in
// affine VU-memory loads or invariant VU state. This is dependence analysis;
// ShaccCg remains the whole-program SSA/register allocator.
bool BuildParallelLoopKernel(const ProgramAnalysis &program, u32 loop_index,
                             ParallelLoopKernel *kernel, std::string *error);

bool BuildParallelLoopKernelForConfiguration(
    const ProgramAnalysis& program, u32 loop_index, u32 configuration_bits,
    ParallelLoopKernel* kernel, std::string* error);

// Extends the same backwards PairPlan slice with only the child state lanes
// proven live by the enclosing-loop boundary. Values are after one logical
// iteration and are parameterized by the ordinary invocation index; evaluating
// them at trip_count-1 yields the exact loop-exit state when the slice remains
// bounded and recurrence-free.
bool BuildParallelLoopKernelWithFinalStateForConfiguration(
    const ProgramAnalysis& program, u32 loop_index, u32 configuration_bits,
    const std::array<u8, 32>& final_vf_lanes, u8 final_acc_lanes,
    bool final_q, bool final_p, bool final_i, ParallelLoopKernel* kernel,
    std::string* error);

// Replaces loop-entry invariant leaves with the PairPlan-defined arithmetic
// and fixed-address VU-memory loads in the acyclic region from the external
// entry to the natural-loop header. Every pair reads one input snapshot and
// commits lower then upper writes, preserving simultaneous-pair semantics and
// upper priority. Values which genuinely originate outside the program remain
// Initial* leaves; lanes written anywhere in the program are marked unstable
// so direct admission cannot reuse a stale seed across invocations.
bool InlineAcyclicEntrySlice(const ProgramAnalysis &program, u32 loop_index,
                            ParallelLoopKernel *kernel,
                            std::string *error);

// Proves only the value-dependence seam between one parent loop iteration and
// its child.  Address evolution, cross-iteration output aliasing, pair/output
// bounds, and generated shader compilation remain separate admission proofs.
bool ProveEnclosingLoopEntryIndependence(
    const ProgramAnalysis& program, const ParallelLoopKernel& child_kernel,
    EnclosingLoopEntryIndependence* proof, std::string* error = nullptr);

// Proves the exact resumption seam after the final generated child iteration
// and the safety of summarizing only intermediate parent suffixes.  This is a
// structural/PairPlan proof; no source identity participates.
bool ProveStructuredLoopTailResume(
    const ProgramAnalysis& program, const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    StructuredLoopTailProof* proof, std::string* error = nullptr);

// Classifies memory dependencies for a parallel (outer, child) generated
// lowering. Loads and stores must be disjoint across the complete child grid;
// dynamic address sets are checked by a transactional GPU preflight before
// effects. Same-iteration aliases remain unsupported because the expression
// DAG does not retain architectural load/store pair ordering.
bool ProveStructuredLoopMemoryIndependence(
    const ParallelLoopKernel& child_kernel,
    const EnclosingLoopEntryIndependence& boundary,
    u32 maximum_outer_iterations, u32 maximum_child_iterations,
    StructuredMemoryDependenceProof* proof, std::string* error = nullptr);

// Rewrites the child's invariant entry leaves through the unique acyclic
// prefix from its enclosing-loop header. The resulting expression graph is
// one exact outer-state transition: Initial* leaves are the parent-header
// generation and final roots are the child-exit values live at the next
// parent iteration. Repetition and output publication remain the generated
// structured-Cg owner's responsibility.
bool InlineEnclosingLoopPrefixSlice(
    const ProgramAnalysis& program,
    const EnclosingLoopEntryIndependence& proof,
    ParallelLoopKernel* kernel, std::string* error = nullptr);

// Extends a canonical child-exit slice through the proven parent suffix, then
// retains only values live at the next parent iteration. This makes omitted
// intermediate suffix arithmetic part of the generated transition while the
// final architectural suffix still executes from tail.final_resume_pc.
bool InlineEnclosingLoopSuffixSlice(
    const ProgramAnalysis& program,
    const EnclosingLoopEntryIndependence& boundary,
    const StructuredLoopTailProof& tail,
    ParallelLoopKernel* kernel, std::string* error = nullptr);

// Builds the hardware-driven nested-loop IR used by the runtime PairPlan GXP
// JIT.  Every child invariant is replaced by a finite closed-form expression
// of the architectural entry state and immutable VU-memory input.  Affine
// memory roots carry separate outer/child coefficients and rounded carried ADD
// chains become OuterRepeatedAdd nodes.  The routine is transactional: failure
// leaves both outputs unchanged and therefore remains suitable for pre-effect
// product admission.
bool BuildClosedFormNestedLoopKernelForConfiguration(
    const ProgramAnalysis& program, u32 child_loop_index,
    u32 configuration_bits, u32 outer_iteration_count,
    u32 child_iteration_count, ParallelLoopKernel* kernel,
    ClosedFormNestedLoopProof* proof, std::string* error = nullptr);

// Builds the same source-facing closed-form kernel as the routine above while
// also retaining a complete architectural VF/ACC/Q/P/I successor formula.
// The source-facing kernel deliberately remains liveness-pruned at the parent
// backedge so existing generated-program keys and compiler artifacts do not
// change.  `successor_kernel` is host metadata only: it closes the final outer
// iteration through the enclosing suffix and is used for transactional state
// attestation/commit after the generated draw completes.
bool BuildClosedFormNestedLoopKernelAndSuccessorForConfiguration(
    const ProgramAnalysis& program, u32 child_loop_index,
    u32 configuration_bits, u32 outer_iteration_count,
    u32 child_iteration_count, ParallelLoopKernel* kernel,
    ParallelLoopKernel* successor_kernel,
    ClosedFormNestedLoopProof* proof, std::string* error = nullptr,
    const char** allocation_stage = nullptr);

// The complete successor initially shares the source-facing flattened graph,
// including store and child-entry expressions which retirement never reads.
// Retain only the transitive closure of architectural VF/ACC/Q/P/I final
// roots. This is an in-place transactional rewrite: failure restores the
// original kernel and reports a named pre-effect error.
bool CompactLoopKernelFinalStateGraph(
    ParallelLoopKernel* kernel, u32* original_expression_count = nullptr,
    std::string* error = nullptr);

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
// Differentially validate the cold append-only graph index against the exact
// snapshot-key map, including collisions, growth, duplicates and capacity.
bool ValidateAppendOnlyExpressionGraphIndex();
#endif

} // namespace VitaGpuVu
