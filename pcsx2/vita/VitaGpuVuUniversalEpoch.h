// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/SingleWaiterProgressEvent.h"
#include "vita/VitaGpuVuCommandEpoch.h"
#include "vita/VitaGpuVuGeneratedUniversal.h"
#include "vita/VitaGpuVuMicroProgram.h"
#include "vita/VitaGpuVuShaderCompiler.h"
#include "vita/VitaGpuVuVifInput.h"
#include "vita/VitaVuBlockCompiler.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace VitaGpuVu {

struct UniversalGpuVuCommittedStateView {
  const u32* vf_words = nullptr;
  const u32* state_words = nullptr;
  const u8* vu_memory = nullptr;
  u64 sequence = 0;

  bool IsValid() const {
    return vf_words && state_words && vu_memory && sequence != 0;
  }
};

inline constexpr u32 UniversalGpuVuCompletionFormatVersion = 1;
// A universal command epoch may bind up to one 16 KiB contiguous payload
// window from the existing mapped ring. The builder proves that all captured
// spans actually occupy that window before binding it; BeginVuCommandEpoch
// only requires initial headroom and does not manufacture contiguity.
inline constexpr u32 UniversalGpuVuFixedPayloadWindowBytes = 16 * 1024;
// Physical r13 evidence showed that the 16,384 architectural malformed-loop
// bound is not also a safe SGX watchdog bound. Long Execute chains therefore
// remain one private transaction while the fixed GXP advances them through
// submissions no larger than this many pairs. Only terminal E-bit completion
// commits the generation or publishes PATH1 output.
inline constexpr u32 UniversalGpuVuWatchdogSafePairsPerSubmission = 128;
static_assert(UniversalGpuVuWatchdogSafePairsPerSubmission ==
              StructuredGeneratedTailPairBudget);
// Product promotion is intentionally narrower than semantic support.  The
// current direct PairPlan root executes one 128-pair source loop; allowing it
// to become a chain of dependent firmware jobs recreates the retired fixed
// interpreter's latency shape.  Until the whole-loop compiler retains an
// Execute inside one generated root, only one generated VU job and at most
// three preceding UNPACK publications are eligible.
inline constexpr u32 UniversalGpuVuMaximumGeneratedEntryFirmwareJobs = 4;
// Matches the generated transaction owner bound. Epoch records use a distinct
// fixed BSS pool because their large synchronization/descriptor object must
// never consume newlib heap on the accepted product path.
inline constexpr u32 UniversalGpuVuEpochPoolCapacity = 128u;

// The offline scheduled continuation removes command scanning and every
// unscheduled hazard walk from the fixed interpreter. It can therefore retain
// a materially larger bounded loop in one SGX invocation. The first command/
// UNPACK transition still uses the conservative fixed limit above.
inline constexpr u32 UniversalGpuVuCompactPairsPerSubmission = 256;
// Runtime-generated CFG roots bake PairPlans and dispatch once per basic
// block, so applying the retired interpreter's 128/256-pair slice to them
// manufactures dozens of firmware visibility jobs.  Bound generated work by
// the registered GXP's primary-program size instead.  This initial envelope is
// deliberately title-neutral: it estimates no more than 384 Ki primary USE
// instruction issues when the complete static root is charged once per owned
// semantic block.  The architectural Execute bound remains the hard ceiling.
// Physical BSpline r83 measured 57--58 firmware jobs and roughly 0.58 seconds
// for a 10,323-pair epoch under the old 256-pair slice; the resource-scaled
// policy is the hardware A/B replacement for that inherited interpreter shape.
inline constexpr u32 UniversalGpuVuGeneratedHotPrimaryInstructionBudget =
    384u * 1024u;

inline constexpr u32 UniversalGpuVuGeneratedHotPairLimit(
    u32 semantic_pair_count, u32 primary_instruction_count,
    u32 maximum_dynamic_pairs) {
  if (semantic_pair_count == 0u || primary_instruction_count == 0u ||
      maximum_dynamic_pairs == 0u ||
      maximum_dynamic_pairs > UniversalCommandEpochMaximumPairsPerExecute) {
    return 0u;
  }
  const u64 scaled_pairs =
      (static_cast<u64>(UniversalGpuVuGeneratedHotPrimaryInstructionBudget) *
       semantic_pair_count) /
      primary_instruction_count;
  const u64 at_least_one_block =
      scaled_pairs < semantic_pair_count ? semantic_pair_count : scaled_pairs;
  return static_cast<u32>(
      at_least_one_block < maximum_dynamic_pairs ? at_least_one_block
                                                 : maximum_dynamic_pairs);
}
// The fixed and compact GXPs are serial compatibility interpreters, and every
// dependent slice is a separate GXM core submission. One narrow conditional
// PATH1-commit job follows the final core in each bounded group. Physical
// BSpline r64 measured only about 120 firmware jobs
// per second. Prequeuing an epoch's complete 82--128-slice upper bound therefore
// stretched one ordinarily playable frame beyond 20 seconds while all ARM cores
// appeared idle. Keep one notification group small, then continue the unchanged
// private state generation from the GS worker after that notification. This is
// a scheduling/latency bound, not an architectural VU or epoch bound.
inline constexpr u32 UniversalGpuVuMaximumSerialCoreSubmissionsPerGroup = 4;
// Compiler-bounded structured roots contain no serial PairPlan dispatch loop:
// each data instance has already passed the per-invocation source/work bound,
// and the VDM distributes a complete independent grid.  They must not inherit
// the four-job latency limit required by the heavy fixed interpreter.  Queue a
// larger dependency stream in one scene, while the explicit preflight barrier
// below still prevents any effect root from being submitted before gate=1 is
// observed by the GS owner.
inline constexpr u32 UniversalGpuVuMaximumStructuredSubmissionsPerGroup = 32;
// A standalone transaction below one watchdog-safe VU slice cannot amortize
// its GXM submission, notification, state handoff, and possible provider
// transition on current hardware.  Physical BSpline r56 measured the exact
// seven-pair case: keeping those epochs on MTVU restored 35.6--38.7 guest
// VSync/s, while submitting them individually reduced the same workload to
// roughly 12--13 VSync/s without producing PATH1 output.  This is a temporary
// mixed-provider profitability boundary, not semantic admission.  Persistent
// cross-epoch batching may retire it once one completion covers enough work.
inline constexpr u32 UniversalGpuVuMinimumStandaloneDynamicPairs =
    UniversalGpuVuWatchdogSafePairsPerSubmission;

inline constexpr bool UniversalGpuVuStandaloneDispatchIsProfitable(
    u32 dynamic_pair_upper_bound) {
  return dynamic_pair_upper_bound >=
      UniversalGpuVuMinimumStandaloneDynamicPairs;
}

inline constexpr bool UniversalGpuVuGeneratedEntryPlanIsProfitable(
    u32 dynamic_pair_upper_bound, u32 unpack_submissions) {
  return UniversalGpuVuStandaloneDispatchIsProfitable(
             dynamic_pair_upper_bound) &&
         dynamic_pair_upper_bound <=
             UniversalGpuVuWatchdogSafePairsPerSubmission &&
         unpack_submissions < UniversalGpuVuMaximumGeneratedEntryFirmwareJobs;
}
// One product transaction may retain two complete maximum-length Execute
// bounds. The owner submits only a small dependency-ordered job batch at once
// and chains later batches through the same private state generation, so this
// is a bounded transactional work limit rather than an immediate command-ring
// reservation or an architectural VU limit.
inline constexpr u32 UniversalGpuVuMaximumPairsPerSubmissionBatch =
    2u * UniversalCommandEpochMaximumPairsPerExecute;
inline constexpr u32 UniversalGpuVuMaximumSerialSubmissionsPerBatch =
    (UniversalGpuVuMaximumPairsPerSubmissionBatch +
     UniversalGpuVuWatchdogSafePairsPerSubmission - 1u) /
        UniversalGpuVuWatchdogSafePairsPerSubmission +
    UniversalCommandEpochMaximumCommands;
inline constexpr u32 UniversalGpuVuMaximumUnpackSubmissionsPerEpoch =
    UniversalCommandEpochMaximumCommands *
    ((UniversalGpuVuMaximumUnpackVectorsPerCommand +
      UniversalGpuVuUnpackVectorsPerSubmission - 1u) /
     UniversalGpuVuUnpackVectorsPerSubmission);
inline constexpr u32 UniversalGpuVuMaximumStructuredSubmissionsPerBatch =
    StructuredGeneratedFirmwareJobCount(
        UniversalGpuVuMaximumUnpackSubmissionsPerEpoch,
        StructuredGeneratedMaximumPartitionFirmwareJobs);
inline constexpr u32 UniversalGpuVuMaximumSubmissionsPerBatch =
    UniversalGpuVuMaximumSerialSubmissionsPerBatch >
            UniversalGpuVuMaximumStructuredSubmissionsPerBatch
        ? UniversalGpuVuMaximumSerialSubmissionsPerBatch
        : UniversalGpuVuMaximumStructuredSubmissionsPerBatch;

// A generated structured transaction is one ordered logical submission
// stream, but placing the complete stream in one GXM scene has the same
// unbounded firmware-queue shape as the fixed interpreter did before bounded
// continuation. Return the next title-neutral group size after a notification
// has proved completed_submissions visible. Zero is terminal or invalid. The
// private state generation and PATH1 record remain uncommitted between groups.
inline constexpr u32 UniversalGpuVuStructuredSubmissionGroupCount(
    u32 total_submissions, u32 completed_submissions,
    u32 preflight_barrier_submission = std::numeric_limits<u32>::max()) {
  if (total_submissions == 0u ||
      total_submissions > UniversalGpuVuMaximumStructuredSubmissionsPerBatch ||
      completed_submissions >= total_submissions) {
    return 0u;
  }
  const u32 remaining = total_submissions - completed_submissions;
  u32 group = remaining < UniversalGpuVuMaximumStructuredSubmissionsPerGroup
      ? remaining
      : UniversalGpuVuMaximumStructuredSubmissionsPerGroup;
  // The finalize job publishes the transactional memory-preflight gate. Never
  // prequeue a generated child-grid effect in the same firmware group: the GS
  // owner must first observe gate=1 at this notification boundary.
  if (completed_submissions < preflight_barrier_submission &&
      group > preflight_barrier_submission - completed_submissions) {
    group = preflight_barrier_submission - completed_submissions;
  }
  return group;
}

inline constexpr bool UniversalGpuVuStructuredSubmissionCursorIsValid(
    u32 total_submissions, u32 completed_submissions,
    u32 preflight_barrier_submission) {
  if (completed_submissions == 0u ||
      completed_submissions > total_submissions ||
      total_submissions == 0u ||
      total_submissions > UniversalGpuVuMaximumStructuredSubmissionsPerBatch) {
    return false;
  }
  if (completed_submissions == total_submissions ||
      completed_submissions == preflight_barrier_submission) {
    return true;
  }
  if (completed_submissions < preflight_barrier_submission) {
    return (completed_submissions %
            UniversalGpuVuMaximumStructuredSubmissionsPerGroup) == 0u;
  }
  return ((completed_submissions - preflight_barrier_submission) %
          UniversalGpuVuMaximumStructuredSubmissionsPerGroup) == 0u;
}

inline constexpr u32 UniversalGpuVuSerialSubmissionUpperBound(
    u32 dynamic_pair_upper_bound, u32 unpack_submissions,
    bool compact_continuation) {
  u32 execution_submissions = 0;
  if (dynamic_pair_upper_bound != 0) {
    if (compact_continuation) {
      const u32 after_fixed = dynamic_pair_upper_bound >
              UniversalGpuVuWatchdogSafePairsPerSubmission
          ? dynamic_pair_upper_bound -
                UniversalGpuVuWatchdogSafePairsPerSubmission
          : 0u;
      execution_submissions = 1u +
          (after_fixed + UniversalGpuVuCompactPairsPerSubmission - 1u) /
              UniversalGpuVuCompactPairsPerSubmission;
    } else {
      execution_submissions =
          (dynamic_pair_upper_bound +
           UniversalGpuVuWatchdogSafePairsPerSubmission - 1u) /
          UniversalGpuVuWatchdogSafePairsPerSubmission;
    }
  }
  const u32 total = execution_submissions + unpack_submissions;
  return total != 0u ? total : 1u;
}

// With the current fixed VIF kernel, every UNPACK must publish its successor
// generation before the first universal VU core can enter Execute. Later groups
// resume the already active Execute and therefore contain no UNPACK jobs. A
// larger initial transition remains a pre-effect fallback until VIF command
// continuation itself is represented by the product scheduler.
inline constexpr u32 UniversalGpuVuInitialTransitionSubmissionCount(
    u32 unpack_submissions) {
  return unpack_submissions == std::numeric_limits<u32>::max()
      ? std::numeric_limits<u32>::max()
      : unpack_submissions + 1u;
}

inline constexpr bool UniversalGpuVuInitialTransitionFitsGroup(
    u32 unpack_submissions) {
  return UniversalGpuVuInitialTransitionSubmissionCount(unpack_submissions) <=
      UniversalGpuVuMaximumSerialCoreSubmissionsPerGroup;
}

// Returns the number of serial core submissions to place before one GXM
// notification. Zero means either terminal/no remaining work or invalid group
// metadata. The initial group includes every UNPACK and one conservative fixed
// Execute slice. A continuation group contains only Execute slices and may use
// the separately attested compact slice size.
inline constexpr u32 UniversalGpuVuSerialSubmissionGroupCount(
    u32 dynamic_pair_upper_bound, u32 executed_pairs, u32 unpack_submissions,
    bool compact_continuation, bool initial_group) {
  if (dynamic_pair_upper_bound == 0u ||
      executed_pairs > dynamic_pair_upper_bound) {
    return 0u;
  }
  if (initial_group) {
    if (executed_pairs != 0u ||
        !UniversalGpuVuInitialTransitionFitsGroup(unpack_submissions)) {
      return 0u;
    }
    const u32 total = UniversalGpuVuSerialSubmissionUpperBound(
        dynamic_pair_upper_bound, unpack_submissions, compact_continuation);
    return total < UniversalGpuVuMaximumSerialCoreSubmissionsPerGroup
        ? total
        : UniversalGpuVuMaximumSerialCoreSubmissionsPerGroup;
  }
  if (executed_pairs == 0u ||
      executed_pairs == dynamic_pair_upper_bound) {
    return 0u;
  }
  const u32 remaining = dynamic_pair_upper_bound - executed_pairs;
  const u32 pair_slice = compact_continuation
      ? UniversalGpuVuCompactPairsPerSubmission
      : UniversalGpuVuWatchdogSafePairsPerSubmission;
  const u32 required = 1u + (remaining - 1u) / pair_slice;
  return required < UniversalGpuVuMaximumSerialCoreSubmissionsPerGroup
      ? required
      : UniversalGpuVuMaximumSerialCoreSubmissionsPerGroup;
}

// Validate every continuation control word before the host clears stopReason or
// submits another job. This function is deliberately value-free and pure so a
// malformed private generation cannot partially mutate the transaction before
// CPU replay is selected.
inline constexpr bool UniversalGpuVuSerialContinuationMetadataIsValid(
    u32 phase, u32 command_cursor, u32 active_pairs_remaining,
    u32 executed_pairs, u32 execute_active, u32 submission_pair_limit,
    u32 command_count, u32 dynamic_pair_upper_bound,
    u32 expected_generated_pair_limit = 0u) {
  const bool pair_limit_valid = expected_generated_pair_limit != 0u
      ? submission_pair_limit == expected_generated_pair_limit &&
            expected_generated_pair_limit <=
                UniversalCommandEpochMaximumPairsPerExecute
      : submission_pair_limit == UniversalGpuVuWatchdogSafePairsPerSubmission ||
            submission_pair_limit == UniversalGpuVuCompactPairsPerSubmission;
  return phase == 1u && command_cursor <= command_count &&
      active_pairs_remaining != 0u && executed_pairs != 0u &&
      executed_pairs < dynamic_pair_upper_bound && execute_active == 1u &&
      active_pairs_remaining <= dynamic_pair_upper_bound - executed_pairs &&
      pair_limit_valid;
}

struct UniversalGpuVuAdditionalExecuteRequest {
  u32 start_pc = 0;
  u32 current_tpc = 0;
  u32 maximum_pairs = UniversalCommandEpochMaximumPairsPerExecute;
  u32 fbrst = 0;
  u16 vif_top = 0;
  u16 vif_itop = 0;
  bool resume = false;
  const std::vector<VifUnpackSpan>* unpacks = nullptr;
};

enum class UniversalGpuVuOutputRoute : u8 {
  RawPath1,
  TfxVertex,
  DirectVuTfx,
};

enum class UniversalGpuVuEpochStage : u32 {
  Prepared,
  Completing,
  Submitted,
  // The private GPU generation failed before commit. The prior canonical
  // state and output remain authoritative while MTVU replays the retained
  // input journal. This is deliberately nonterminal for the GS mailbox.
  GpuRejected,
  Accepted,
  CpuFallback,
  // The GS mailbox has consumed the ordering record and will no longer
  // dereference this descriptor. Only this stage permits owner destruction.
  Retired,
  Cancelled,
};

enum class UniversalGpuVuRejection : u32 {
  None,
  InvalidRequest,
  ProgramEncoding,
  IncompleteControlFlow,
  BranchInDelaySlot,
  EnabledDtObserver,
  MbitObserver,
  UnsupportedConfiguration,
  InvalidPairMetadata,
  UnsupportedUpper,
  UnsupportedLower,
  ApproximateQ,
  UnsupportedVifUnpack,
  CommandCapacity,
  PayloadGeneration,
  PayloadCapacity,
  InputUnavailable,
  StructuredBundleQuarantined,
  GeneratedArchitectureQuarantined,
  WatchdogWorkBudget,
  RuntimeInvalidPair,
  RuntimePairBudget,
  RuntimeInvalidPath1,
  RuntimeOutputCapacity,
  RuntimeTerminalXgkick,
  RuntimeStructuredPreflight,
  RuntimeStructuredAttestation,
  RuntimePredecessorFailed,
  GeneratedProgramPending,
  GeneratedProgramUnavailable,
  GeneratedProgramUnprofitable,
  SynchronousDispatchCost,
  EpochPoolCapacity,
  DeviceUnavailable,
  SubmissionFailed,
  Count,
};

// Cache a terminal rejection only for the exact dispatch/output-contract key.
// Runtime GIF variants and entry states share VU microcode but remain separate
// generated providers; one failed key must never suppress a sibling key.
// Pending compilation or output attestation remains retryable.
constexpr bool ShouldCacheGeneratedProviderTerminalRejection(
    UniversalGpuVuRejection rejection, bool terminal_generated_provider_state,
    bool skip_single_attempt,
    bool wait_for_generated_program, bool wait_for_generated_compiler_idle,
    bool wait_for_generated_attestation) {
  return rejection == UniversalGpuVuRejection::GeneratedProgramUnavailable &&
         terminal_generated_provider_state && skip_single_attempt &&
         !wait_for_generated_program &&
         !wait_for_generated_compiler_idle &&
         !wait_for_generated_attestation;
}

// Development-time comparison between a completed private generated
// transaction and the exact CPU replay which remains authoritative.  The GPU
// bytes are captured before rejection, so the comparison never grants them
// guest-visible ownership.
struct UniversalGpuVuStructuredComparison {
  bool available = false;
  bool vf_exact = false;
  bool state_exact = false;
  bool memory_exact = false;
  bool path1_exact = false;
  u32 first_vf_word = std::numeric_limits<u32>::max();
  u32 gpu_vf_word = 0;
  u32 cpu_vf_word = 0;
  u32 first_state_word = std::numeric_limits<u32>::max();
  u32 gpu_state_word = 0;
  u32 cpu_state_word = 0;
  u32 first_memory_word = std::numeric_limits<u32>::max();
  u32 gpu_memory_word = 0;
  u32 cpu_memory_word = 0;
  u32 first_path1_word = std::numeric_limits<u32>::max();
  u32 gpu_path1_word = 0;
  u32 cpu_path1_word = 0;
  u32 gpu_path1_packets = 0;
  u32 gpu_path1_qwords = 0;
  u32 cpu_path1_bytes = 0;
  u32 gpu_executed_pairs = 0;

  bool Exact() const {
    return available && vf_exact && state_exact && memory_exact && path1_exact;
  }
};

enum class UniversalGpuVuStructuredBoundaryComponent : u8 {
  None,
  OuterCount,
  Vf,
  Acc,
  Vi,
  Q,
  P,
  I,
};

const char* UniversalGpuVuStructuredBoundaryComponentName(
    UniversalGpuVuStructuredBoundaryComponent component);

struct UniversalGpuVuStructuredBoundaryComparison {
  bool available = false;
  bool exact = false;
  u32 parent_entry_pc = 0;
  u32 child_entry_pc = 0;
  u32 gpu_outer_iterations = 0;
  u32 cpu_outer_iterations = 0;
  u32 cpu_parent_observations = 0;
  u32 cpu_executed_pairs = 0;
  u32 first_outer_iteration = std::numeric_limits<u32>::max();
  UniversalGpuVuStructuredBoundaryComponent component =
      UniversalGpuVuStructuredBoundaryComponent::None;
  u32 register_index = 0;
  u32 lane = 0;
  u32 gpu_word = 0;
  u32 cpu_word = 0;
  // Previous child-entry value for the same vector. This deliberately does
  // not claim to be an instruction operand: it localizes loop-carried numeric
  // drift without adding a workload-specific expression probe.
  bool prior_vector_available = false;
  std::array<u32, 4> prior_gpu_vector{};
  std::array<u32, 4> prior_cpu_vector{};
};

UniversalGpuVuStructuredBoundaryComparison
CompareStructuredGeneratedBoundarySnapshots(
    const StructuredGeneratedBundle& bundle, const u32* snapshot_words,
    const u32* outer_state_words, const u32* vi_snapshot_words,
    const VitaVU::Vu1StructuredBoundaryTrace& cpu_trace);

struct UniversalGpuVuPrivateStructuredResult;

const char* UniversalGpuVuRejectionName(UniversalGpuVuRejection rejection);
const char* UniversalGpuVuOutputRouteName(UniversalGpuVuOutputRoute route);

// Pointer-free record written only after a notification proves the private
// state/output generation complete. It is the transaction commit predicate;
// no VU state or PATH1 bytes become authoritative merely because a shader was
// submitted.
struct alignas(16) UniversalGpuVuCompletionRecord {
  u32 format_version = UniversalGpuVuCompletionFormatVersion;
  UniversalGpuVuEpochStage stage = UniversalGpuVuEpochStage::Prepared;
  UniversalGpuVuRejection rejection = UniversalGpuVuRejection::None;
  UniversalGpuVuOutputRoute output_route =
      UniversalGpuVuOutputRoute::RawPath1;
  u64 sequence = 0;
  u64 predecessor_sequence = 0;
  u32 final_tpc = 0;
  u32 executed_pairs = 0;
  u32 output_packet_count = 0;
  u32 output_qword_count = 0;
  u32 interrupt_flags = 0;
  u32 final_cycle = 0;
};

static_assert(sizeof(UniversalGpuVuCompletionRecord) == 64);

struct UniversalGpuVuEpochBuildRequest {
  const u8* micro = nullptr;
  u32 micro_size = 0;
  u32 start_pc = 0;
  u32 current_tpc = 0;
  u32 maximum_pairs = UniversalCommandEpochMaximumPairsPerExecute;
  u32 configuration_bits = 0;
  u32 fbrst = 0;
  u64 predecessor_sequence = 0;
  u16 vif_top = 0;
  u16 vif_itop = 0;
  std::array<u32, 4> vif_row{};
  std::array<u32, 4> vif_column{};
  std::array<u16, 16> initial_vi{};
  bool resume = false;
  const std::vector<VifUnpackSpan>* unpacks = nullptr;
  // Ordered MSCAL/MSCNT continuations which share the same immutable 16 KiB
  // micro image and configuration.  The fixed GXP executes every segment in
  // one invocation and publishes one transactional completion after the
  // terminal End command.  Empty retains the legacy single-Execute request.
  const std::vector<UniversalGpuVuAdditionalExecuteRequest>*
      additional_executes = nullptr;
};

class UniversalGpuVuEpoch final {
public:
  static void* operator new(std::size_t size);
  static void operator delete(void* pointer) noexcept;

  UniversalGpuVuEpoch(const UniversalGpuVuEpoch&) = delete;
  UniversalGpuVuEpoch& operator=(const UniversalGpuVuEpoch&) = delete;
  ~UniversalGpuVuEpoch();

  u64 Sequence() const { return m_completion.sequence; }
  u64 PredecessorSequence() const {
    return m_completion.predecessor_sequence;
  }
  UniversalGpuVuEpochStage Stage() const {
    return m_stage.load(std::memory_order_acquire);
  }
  UniversalGpuVuRejection Rejection() const {
    return m_rejection.load(std::memory_order_acquire);
  }
  void WaitForStageChange(UniversalGpuVuEpochStage observed) {
    m_stage_progress.WaitForChange(observed, [this]() { return Stage(); });
  }
  const UniversalGpuVuCompletionRecord& Completion() const {
    return m_completion;
  }
  const UniversalMicroProgram& Program() const { return *m_program; }
  u64 ProgramIdentity() const { return m_program.Identity(); }
  u32 AnalysisEntryPc() const { return m_analysis_entry_pc; }
  const UniversalEpochMicroOp* Commands() const { return m_commands.data(); }
  u32 CommandCount() const { return m_command_count; }
  u32 ExecuteCount() const { return m_execute_count; }
  std::span<const VifUnpackSpan> Unpacks() const {
    return {m_unpacks.data(), m_unpack_count};
  }
  u32 UnpackSubmissionCount() const { return m_unpack_submission_count; }
  bool CanUseIndependentUnpackSubmission(u32 submission) const;
  u32 IndependentUnpackSubmissionCount() const;
  u32 PayloadBaseOffset() const { return m_payload_base_offset; }
  u32 PayloadSize() const { return m_payload_size; }
  u32 PreflightPairCount() const { return m_preflight_pair_count; }
  u32 DynamicPairUpperBound() const { return m_dynamic_pair_upper_bound; }
  u32 ConfigurationBits() const { return m_configuration_bits; }
  const std::array<u32, 4>& InitialVifRow() const { return m_vif_row; }
  const std::array<u32, 4>& InitialVifColumn() const { return m_vif_column; }
  const ShaderKey& GeneratedProgramKey() const {
    return m_generated_program_key;
  }
  const StructuredGeneratedBundle& StructuredBundle() const {
    return m_structured_bundle;
  }
  const GeneratedHotBundle& GeneratedHotBundleDescriptor() const {
    return m_generated_hot_bundle;
  }
  const GeneratedNestedDirectBundle& GeneratedNestedDirectDescriptor() const {
    return m_generated_nested_direct;
  }
  const GeneratedLoopKernelBundle& GeneratedLoopKernelDescriptor() const {
    return m_generated_loop_kernel;
  }
  GeneratedLoopKernelBundleState GeneratedLoopKernelState() const {
    return m_generated_loop_kernel_state;
  }
  bool GeneratedRequestDeferred() const {
    return m_generated_request_deferred;
  }
  bool GeneratedNestedDirectOutputContractPending() const {
    return m_generated_nested_direct_output_contract_pending;
  }
  bool GeneratedLoopKernelOutputContractPending() const {
    return m_generated_loop_kernel_output_contract_pending;
  }
  u64 GeneratedRequestObservedCompilerIdle() const {
    return m_generated_request_observed_compiler_idle;
  }
  bool CanUseCompactScheduledContinuation() const {
    return m_execute_count == 1u &&
        (m_configuration_bits & UniversalConfigurationAssumeScheduled) != 0u;
  }
  bool RequiresRuntimePathProof() const { return m_runtime_path_proof; }
  uptr SubmissionNotificationAddress() const {
	return m_submission_notification_address.load(std::memory_order_acquire);
  }
  u32 SubmissionNotificationValue() const {
	return m_submission_notification_value.load(std::memory_order_acquire);
  }
  u32 SubmissionNotificationStartValue() const {
    return m_submission_notification_start_value.load(
        std::memory_order_acquire);
  }
  u32 SubmissionJobBase() const {
    return m_submission_job_base.load(std::memory_order_acquire);
  }
  u32 SubmissionJobCount() const {
    return m_submission_job_count.load(std::memory_order_acquire);
  }
  const UniversalRawPath1Export* RawPath1Output() const {
    return m_raw_path1_output;
  }
  const UniversalGpuVuCommittedStateView& CommittedState() const {
    return m_committed_state;
  }
  bool AcceptedStateAcquired() const {
    return m_accepted_state_acquired.load(std::memory_order_acquire);
  }
  u32 RejectedPath1Address() const { return m_rejected_path1_address; }
  const std::array<u32, 4>& RejectedPath1Tag() const {
    return m_rejected_path1_tag;
  }
  bool HasPrivateStructuredResult() const {
    return m_private_structured_result != nullptr;
  }

  bool SetSubmissionNotification(uptr address, u32 value,
                                 u32 start_value = 0, u32 job_base = 0,
                                 u32 job_count = 1);
  bool SetContinuationSubmissionNotification(uptr address, u32 value,
                                             u32 start_value = 0,
                                             u32 job_base = 0,
                                             u32 job_count = 1);
  void SetRejectedPath1Diagnostic(u32 address,
                                  const std::array<u32, 4>& tag);
  bool CapturePrivateStructuredResult(
      const u32* vf_words, const u32* state_words, const u8* vu_memory,
      const UniversalRawPath1Export* raw_path1,
      const u32* structured_snapshot_words,
      const u32* structured_outer_state_words,
      const u32* structured_vi_snapshot_words);
  UniversalGpuVuStructuredComparison ComparePrivateStructuredResult(
      const VURegs* cpu_vu, const u8* cpu_path1, u32 cpu_path1_bytes) const;
  UniversalGpuVuStructuredBoundaryComparison
  ComparePrivateStructuredBoundaryTrace(
      const VitaVU::Vu1StructuredBoundaryTrace& cpu_trace) const;
	void WaitForSubmissionProgress(u32 observed_notification_value) {
		m_stage_progress.WaitForChange(observed_notification_value, [this]() {
			return Stage() == UniversalGpuVuEpochStage::Submitted ?
				SubmissionNotificationValue() : 0u;
		});
	}
  bool MarkSubmitted();
  bool MarkAccepted(u32 final_tpc, u32 executed_pairs,
                    u32 output_packet_count, u32 output_qword_count,
                    u32 interrupt_flags, u32 final_cycle,
                    const UniversalRawPath1Export* raw_path1 = nullptr,
                    UniversalGpuVuCommittedStateView committed_state = {});
  // Acceptance has two ordered consumers. The VU worker first adopts the
  // completion-published mapped generation; only then may the GS mailbox
  // consume PATH1 and retire the descriptor. This keeps a product slot from
  // becoming reusable while CPU ownership still names its mapped state.
  bool MarkAcceptedStateAcquired();
  bool MarkGpuRejected(UniversalGpuVuRejection rejection,
                       u32 executed_pairs);
  bool MarkCpuFallback(UniversalGpuVuRejection rejection,
                       u32 executed_pairs);
  bool MarkRetired();
  void Cancel();

private:
  friend std::unique_ptr<UniversalGpuVuEpoch> PrepareUniversalGpuVuEpoch(
      const UniversalGpuVuEpochBuildRequest&, UniversalGpuVuRejection*,
      std::string*, u32*, u32*);
  UniversalGpuVuEpoch() = default;

  std::atomic<UniversalGpuVuEpochStage> m_stage{
      UniversalGpuVuEpochStage::Prepared};
  std::atomic<UniversalGpuVuRejection> m_rejection{
      UniversalGpuVuRejection::None};
  Threading::SingleWaiterProgressEvent m_stage_progress;
  UniversalGpuVuCompletionRecord m_completion{};
  UniversalMicroProgramHandle m_program;
  std::array<UniversalEpochMicroOp,
             UniversalCommandEpochMaximumCommands>
      m_commands{};
  // Every UNPACK already counts against the fixed command ABI. Keep its
  // retained payload descriptors inside the epoch object so accepting a hot
  // generated chain never allocates another newlib vector after preflight.
  std::array<VifUnpackSpan, UniversalCommandEpochMaximumCommands> m_unpacks{};
  u32 m_unpack_count = 0;
  u32 m_command_count = 0;
  u32 m_execute_count = 0;
  u32 m_unpack_submission_count = 0;
  u32 m_payload_base_offset = 0;
  u32 m_payload_size = 0;
  u32 m_preflight_pair_count = 0;
  u32 m_dynamic_pair_upper_bound = 0;
  u32 m_analysis_entry_pc = 0;
  u32 m_configuration_bits = 0;
  std::array<u32, 4> m_vif_row{};
  std::array<u32, 4> m_vif_column{};
  ShaderKey m_generated_program_key{};
  StructuredGeneratedBundle m_structured_bundle{};
  GeneratedHotBundle m_generated_hot_bundle{};
  GeneratedNestedDirectBundle m_generated_nested_direct{};
  GeneratedLoopKernelBundle m_generated_loop_kernel{};
  GeneratedLoopKernelBundleState m_generated_loop_kernel_state =
      GeneratedLoopKernelBundleState::Missing;
  u64 m_generated_request_observed_compiler_idle = 0;
  bool m_generated_request_deferred = false;
  bool m_generated_nested_direct_output_contract_pending = false;
  bool m_generated_loop_kernel_output_contract_pending = false;
  bool m_runtime_path_proof = false;
	std::atomic<uptr> m_submission_notification_address{0};
	std::atomic<u32> m_submission_notification_value{0};
  std::atomic<u32> m_submission_notification_start_value{0};
  std::atomic<u32> m_submission_job_base{0};
  std::atomic<u32> m_submission_job_count{0};
  const UniversalRawPath1Export* m_raw_path1_output = nullptr;
  UniversalGpuVuCommittedStateView m_committed_state{};
  std::atomic_bool m_accepted_state_acquired{false};
  u32 m_rejected_path1_address = 0;
  std::array<u32, 4> m_rejected_path1_tag{};
  std::unique_ptr<UniversalGpuVuPrivateStructuredResult>
      m_private_structured_result;
};

std::unique_ptr<UniversalGpuVuEpoch> PrepareUniversalGpuVuEpoch(
    const UniversalGpuVuEpochBuildRequest& request,
    UniversalGpuVuRejection* rejection = nullptr,
    std::string* error = nullptr,
    u32* classified_pair_count = nullptr,
    u32* dynamic_pair_upper_bound = nullptr);

struct UniversalGpuVuEpochStatistics {
  u64 prepared = 0;
  u64 preflight_accepted = 0;
  u64 submitted = 0;
  u64 accepted = 0;
  u64 cpu_fallbacks = 0;
  u64 accepted_pairs = 0;
  u64 accepted_path1_packets = 0;
  u64 accepted_path1_qwords = 0;
  u64 rejected_pairs = 0;
  u64 cpu_vu_calls = 0;
  u64 async_epochs_queued = 0;
  u64 async_state_acquired = 0;
  u64 async_pending_max = 0;
  // Wall-time attribution is accumulated only while product performance
  // telemetry is enabled. Values are microseconds and split CPU1 preparation
  // from GXM completion and ordered PATH1 retirement.
  u64 preflight_wall_us = 0;
  u64 program_prepare_wall_us = 0;
  u64 static_preflight_lookup_wall_us = 0;
  u64 epoch_allocation_wall_us = 0;
  u64 control_analysis_wall_us = 0;
  u64 pair_validation_wall_us = 0;
  u64 payload_encode_wall_us = 0;
  u64 mailbox_wait_wall_us = 0;
  u64 notification_wait_wall_us = 0;
  u64 retirement_wait_wall_us = 0;
  u64 cpu_fallback_wall_us = 0;
  u64 cpu_materialize_wall_us = 0;
  u64 cpu_unpack_replay_wall_us = 0;
  u64 cpu_path1_finish_wall_us = 0;
  u64 cpu_completion_publish_wall_us = 0;
  u64 worker_attempt_wall_us = 0;
  u64 path1_retirement_wall_us = 0;
  // Generated-provider CPU1 costs. These are kept separate from canonical
  // CPU VU fallback counters: a generated epoch can report cpu_vu_calls=0
  // while still spending substantial ARM time constructing descriptors,
  // advancing a speculative successor, or executing a temporary state bridge.
  u64 generated_live_contract_resolutions = 0;
  u64 generated_live_contract_resolution_wall_us = 0;
  u64 generated_live_control_cache_hits = 0;
  u64 generated_live_control_cache_misses = 0;
  u64 generated_descriptor_builds = 0;
  u64 generated_descriptor_build_wall_us = 0;
  // Successful generated descriptors are split at stable architectural
  // boundaries so physical Vita runs can distinguish cache/proof work from
  // input packing and successor construction.  These fields are attribution
  // only; they never participate in admission.
  u64 generated_descriptor_cache_wall_us = 0;
  u64 generated_descriptor_runtime_proof_wall_us = 0;
  u64 generated_descriptor_input_pack_wall_us = 0;
  u64 generated_descriptor_store_layout_wall_us = 0;
  u64 generated_descriptor_final_state_wall_us = 0;
  u64 generated_descriptor_transaction_wall_us = 0;
  u64 generated_descriptor_transaction_acquire_wall_us = 0;
  u64 generated_descriptor_transaction_capture_wall_us = 0;
  u64 generated_descriptor_transaction_configure_wall_us = 0;
  u64 generated_private_state_advances = 0;
  u64 generated_private_state_advance_wall_us = 0;
  u64 generated_private_same_layout_replacements = 0;
  u64 generated_private_covered_layout_replacements = 0;
  u64 generated_private_covered_owner_slots = 0;
  u64 generated_private_register_owner_replacements = 0;
  u64 generated_private_register_owner_replacement_slots = 0;
  u64 generated_private_register_owner_remaps = 0;
  u64 generated_private_bridge_calls = 0;
  u64 generated_private_bridge_pairs = 0;
  u64 generated_private_bridge_wall_us = 0;
  u64 generated_state_formula_calls = 0;
  u64 generated_state_formula_logical_pairs = 0;
  u64 generated_state_formula_operations = 0;
  u64 generated_state_formula_wall_us = 0;
  u64 generated_hot_execute_calls = 0;
  u64 generated_hot_execute_wall_us = 0;
  u64 generated_queue_calls = 0;
  u64 generated_queue_wall_us = 0;
  u64 generated_gather_calls = 0;
  u64 generated_gather_executes = 0;
  u64 generated_gather_wall_us = 0;
  u64 generated_publication_calls = 0;
  u64 generated_publication_draws = 0;
  u64 generated_publication_wall_us = 0;
  u64 generated_retirement_polls = 0;
  u64 generated_retirement_poll_wall_us = 0;
  u64 generated_mtvu_execute_records = 0;
  u64 generated_mtvu_execute_record_wall_us = 0;
  u64 generated_mtvu_vif_records = 0;
  u64 generated_mtvu_vif_record_wall_us = 0;
  u64 generated_mtvu_other_records = 0;
  u64 generated_mtvu_other_record_wall_us = 0;
  u64 generated_mtvu_housekeeping_calls = 0;
  u64 generated_mtvu_housekeeping_wall_us = 0;
  u64 generated_batch_commits = 0;
  u64 generated_batch_commit_wall_us = 0;
  u64 generated_batch_drain_waits = 0;
  u64 generated_batch_drain_wait_wall_us = 0;
  u64 generated_batch_drain_polls = 0;
  u64 mtvu_execute_queue_samples = 0;
  u64 mtvu_execute_queue_age_us = 0;
  u64 mtvu_execute_queue_age_max_us = 0;
  u64 mtvu_execute_outstanding_max = 0;
  u64 mtvu_queue_used_words_max = 0;
  u64 cpu0_mtvu_wait_wall_us = 0;
  u64 cpu0_mtvu_ring_wait_wall_us = 0;
  u64 cpu0_execute_budget_waits = 0;
  u64 cpu0_execute_budget_wait_wall_us = 0;
  u64 static_preflight_cache_hits = 0;
  u64 static_preflight_cache_misses = 0;
	// Counts Execute records for which MTVU deliberately avoided walking the
	// following queue because this exact entry/configuration had not yet proved
	// enough single-submission budget headroom.
	u64 mtvu_multi_execute_gather_suppressed = 0;
	u64 mtvu_dispatch_cache_hits = 0;
	u64 mtvu_dispatch_cache_misses = 0;
	u64 generated_product_hot_dispatch_hits = 0;
	u64 generated_product_hot_dispatch_misses = 0;
	u64 continuation_groups = 0;
	u64 continuation_submissions = 0;
  // Published only after the terminal notification proves a transactional
  // epoch complete. Residency is submission-to-observed-completion wall time;
  // it includes GS-worker polling delay and is an upper bound rather than an
  // invented SGX hardware-utilization counter.
  u64 universal_provider_epochs = 0;
  u64 universal_provider_jobs = 0;
  u64 generated_provider_epochs = 0;
  u64 generated_provider_jobs = 0;
  u64 gpu_residency_samples = 0;
  u64 gpu_residency_wall_us = 0;
  u64 gpu_residency_wall_us_max = 0;
  u64 mtvu_path1_queue_samples = 0;
  u64 mtvu_path1_queue_age_us = 0;
  u64 mtvu_path1_queue_age_max_us = 0;
  std::array<u64, static_cast<std::size_t>(
                      UniversalGpuVuRejection::Count)>
      rejections{};
};

UniversalGpuVuEpochStatistics GetUniversalGpuVuEpochStatistics();
void ResetUniversalGpuVuEpochStatistics();
void RecordUniversalGpuVuCpuFallback(u32 pairs);
void RecordUniversalGpuVuPreflightCpuFallback(u32 pairs);
void RecordUniversalGpuVuMailboxWait(u64 wall_us);
void RecordUniversalGpuVuNotificationWait(u64 wall_us);
void RecordUniversalGpuVuRetirementWait(u64 wall_us);
void RecordUniversalGpuVuCpuFallbackTime(u64 wall_us);
void RecordUniversalGpuVuCpuMaterializeTime(u64 wall_us);
void RecordUniversalGpuVuCpuUnpackReplayTime(u64 wall_us);
void RecordUniversalGpuVuCpuPath1FinishTime(u64 wall_us);
void RecordUniversalGpuVuCpuCompletionPublishTime(u64 wall_us);
void RecordUniversalGpuVuWorkerAttemptTime(u64 wall_us);
void RecordUniversalGpuVuPath1RetirementTime(u64 wall_us);
void RecordGeneratedLoopKernelLiveContractResolution(u64 wall_us);
void RecordGeneratedLoopKernelLiveControlCache(bool hit);
void RecordGeneratedLoopKernelDescriptorBuild(u64 wall_us);
void RecordGeneratedLoopKernelDescriptorStages(
    u64 cache_wall_us, u64 runtime_proof_wall_us,
    u64 input_pack_wall_us, u64 store_layout_wall_us,
    u64 final_state_wall_us, u64 transaction_wall_us,
    u64 transaction_acquire_wall_us, u64 transaction_capture_wall_us,
    u64 transaction_configure_wall_us);
void RecordGeneratedLoopKernelPrivateStateAdvance(u64 wall_us);
void RecordGeneratedLoopKernelPrivateSameLayoutReplacement();
void RecordGeneratedLoopKernelPrivateCoveredLayoutReplacement(
    u32 owner_slots);
void RecordGeneratedLoopKernelPrivateRegisterOwnerUpdate(
    bool replaced_owner, u32 owner_slots);
void RecordGeneratedLoopKernelPrivateBridge(u32 pairs, u64 wall_us);
void RecordGeneratedLoopKernelStateFormula(u32 logical_pairs, u32 operations,
                                           u64 wall_us);
void RecordGeneratedLoopKernelHotExecute(u64 wall_us);
void RecordGeneratedLoopKernelQueue(u64 wall_us);
void RecordGeneratedLoopKernelGather(u32 executes, u64 wall_us);
void RecordGeneratedLoopKernelPublication(u32 draws, u64 wall_us);
void RecordGeneratedLoopKernelRetirementPoll(u64 wall_us);
void RecordGeneratedLoopKernelMtvuExecuteRecord(u64 wall_us);
void RecordGeneratedLoopKernelMtvuVifRecord(u64 wall_us);
void RecordGeneratedLoopKernelMtvuOtherRecord(u64 wall_us);
void RecordGeneratedLoopKernelMtvuHousekeeping(u64 wall_us);
void RecordGeneratedLoopKernelBatchCommit(u64 wall_us);
void RecordGeneratedLoopKernelBatchDrainWait(u64 wall_us, u64 polls);
void RecordUniversalGpuVuMtvuExecuteQueueAge(u64 wall_us);
void RecordUniversalGpuVuMtvuExecuteOutstanding(u64 count);
void RecordUniversalGpuVuMtvuQueueUsedWords(u64 words);
void RecordUniversalGpuVuCpu0Wait(u64 wall_us);
void RecordUniversalGpuVuCpu0RingWait(u64 wall_us);
void RecordUniversalGpuVuCpu0ExecuteBudgetWait(u64 wall_us);
void RecordUniversalGpuVuMtvuMultiExecuteGatherSuppressed();
void RecordUniversalGpuVuMtvuDispatchCacheLookup(bool hit);
void RecordGeneratedLoopKernelProductHotDispatch(bool hit);
void RecordUniversalGpuVuCachedPreflightRejection(
    UniversalGpuVuRejection rejection);
void RecordUniversalGpuVuProductPolicyCpuFallback(
    UniversalGpuVuRejection rejection);
void RecordUniversalGpuVuContinuationGroup(u32 core_submissions);
void RecordUniversalGpuVuProviderCompletion(bool generated, u32 jobs,
                                             u64 gpu_residency_wall_us);
// Counts a real generated-provider GXM firmware boundary. Generated epoch
// completion is recorded separately because one boundary can retire a batch.
void RecordGeneratedGpuVuFirmwareSubmission();
// One runtime-generated direct transaction became authoritative without a
// CPU VU call. This shares the universal provider's accepted pair counters so
// frame telemetry cannot mistake compiler completion for execution.
void RecordGeneratedLoopKernelAccepted(u32 executed_pairs);
void RecordUniversalGpuVuMtvuPath1QueueAge(u64 wall_us);
void RecordUniversalGpuVuAsyncQueued(u32 pending_count);

// The legacy transaction owner publishes whether its mapped resources exist.
// This is not permission to select one of its rejected serial, structured, or
// snapshot execution providers.
void SetUniversalGpuVuDeviceAvailable(bool available);
bool IsUniversalGpuVuDeviceAvailable();

// Product admission remains closed until the hardware-driven generated loop
// kernel has its own compact descriptor, resource certificate, and physical
// performance attestation. CPU MTVU owns cold/pending execution meanwhile;
// legacy universal and nested-direct owners remain diagnostics only.
void SetGeneratedLoopKernelProductAdmissionEnabled(bool enabled);
bool IsGeneratedLoopKernelProductAdmissionEnabled();

// MTVU's cheap dispatch policy runs before an epoch reaches the GXM thread.
// Publish the fixed compact continuation capability separately so that a hot
// dynamic epoch is not rejected using the serial interpreter's job estimate
// after the offline compact provider has been registered.
void SetUniversalGpuVuCompactProviderAvailable(bool available);
bool IsUniversalGpuVuCompactProviderAvailable();

// PCSX2 owner: VU1microInterp.cpp::_vu1FinishProgram() and
// VUops.cpp::_vuFlushAll(). The view is published only after the GXM
// notification and complete transaction validation prove a terminal idle
// generation. It may be copied to VU1 only at a real observer/provider switch.
bool MaterializeUniversalGpuVuCommittedState(
    const UniversalGpuVuCommittedStateView& view, VURegs* vu);

}  // namespace VitaGpuVu
