// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuVifInput.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace VitaGpuVu {

// Phase-one direct descriptors are produced thousands of times per frame on a
// 496 MHz Cortex-A9. std::vector's empty object is cheap, but growing five
// separate vectors for every VU dispatch made allocator traffic part of the
// hot path. Keep the common generated-program shapes inline and retain a
// vector fallback so semantic coverage is not limited by an inline capacity.
template <typename T, size_t InlineCapacity>
class InlineDescriptorVector final {
  static_assert(InlineCapacity != 0);
  static_assert(std::is_nothrow_move_constructible_v<T>);
  static_assert(std::is_nothrow_move_assignable_v<T>);

public:
  InlineDescriptorVector() = default;
  InlineDescriptorVector(const InlineDescriptorVector&) = delete;
  InlineDescriptorVector& operator=(const InlineDescriptorVector&) = delete;
  InlineDescriptorVector(InlineDescriptorVector&&) = delete;
  InlineDescriptorVector& operator=(InlineDescriptorVector&&) = delete;

  size_t size() const { return m_size; }
  bool empty() const { return m_size == 0; }

  T* data() { return m_using_heap ? m_heap.data() : m_inline.data(); }
  const T* data() const {
    return m_using_heap ? m_heap.data() : m_inline.data();
  }
  T* begin() { return data(); }
  const T* begin() const { return data(); }
  T* end() { return data() + m_size; }
  const T* end() const { return data() + m_size; }

  T& operator[](size_t index) { return data()[index]; }
  const T& operator[](size_t index) const { return data()[index]; }
  T& back() { return (*this)[m_size - 1]; }
  const T& back() const { return (*this)[m_size - 1]; }

  void reserve(size_t requested) {
    if (requested > InlineCapacity)
      UseHeap(requested);
  }

  void push_back(const T& value) {
    EnsureSpaceForOne();
    if (m_using_heap)
      m_heap.push_back(value);
    else
      m_inline[m_size] = value;
    m_size++;
  }

  void push_back(T&& value) {
    EnsureSpaceForOne();
    if (m_using_heap)
      m_heap.push_back(std::move(value));
    else
      m_inline[m_size] = std::move(value);
    m_size++;
  }

  void clear() {
    if (m_using_heap)
      m_heap.clear();
    m_size = 0;
  }

private:
  void EnsureSpaceForOne() {
    if (!m_using_heap && m_size == InlineCapacity)
      UseHeap(InlineCapacity * 2);
  }

  void UseHeap(size_t requested) {
    if (m_using_heap) {
      m_heap.reserve(requested);
      return;
    }
    const size_t capacity =
        requested > InlineCapacity * 2 ? requested : InlineCapacity * 2;
    m_heap.reserve(capacity);
    for (size_t index = 0; index < m_size; index++)
      m_heap.push_back(std::move(m_inline[index]));
    m_using_heap = true;
  }

  std::array<T, InlineCapacity> m_inline{};
  std::vector<T> m_heap;
  size_t m_size = 0;
  bool m_using_heap = false;
};

enum class OutputLowering : u8 {
  DirectTfx,
  TfxVertexExport,
  RawPath1Export,
};

enum class ExecutionKind : u8 {
  GeneratedParallel,
  GeneratedSerial,
  UniversalInterpreter,
};

enum class PrimitiveBoundary : u8 {
  Native,
  InstanceIndexed,
  ExpandedIndexed,
  // A terminal PairPlan/GSState::VertexKick proof supplied the exact U16
  // line/triangle list after guest ADC restart semantics. INDEX remains the
  // original VU output vertex, so the generated root executes the same dense
  // original vertex domain while rasterization consumes only proven primitives.
  ExactPostLoopIndexed,
  // ADC-filtered flat line endpoints, in the shader's expanded INDEX domain.
  // VU store/successor ownership still covers the original invocation_count.
  // This boundary is no-write product only; sparse raster indices cannot
  // validate or publish a complete private GPU store journal.
  ExactPostLoopExpandedIndexed,
  // Non-committing state attestation: one POINTS invocation for each original
  // VU iteration, regardless of ADC. Never an authoritative rendering draw.
  PrivateStatePoints,
};

// Runtime-generated direct roots may depend on a small compiler-partitioned
// set of pure expression producers.  The descriptor carries only immutable
// generated-program identities and their dependency stages; BUFFER10 storage
// remains private to the GS submission which consumes the descriptor.
inline constexpr u32 GpuVuDirectPrecomputeMaximumPrograms = 8u;
inline constexpr u32 GpuVuDirectPrecomputeMaximumStages = 3u;

struct IntegerRect {
  s32 left = 0;
  s32 top = 0;
  s32 right = 0;
  s32 bottom = 0;
  bool valid = false;
  bool exact = false;
};

// One statically proven GS register write at this PATH1 ordering point. Dynamic
// A+D output is represented only by RawPath1Export.
struct StaticGsWrite {
  u64 value = 0;
  u8 address = 0;
};

// Generated roots consume memory from the owner which already contains the
// architecturally selected qword. RawInput is an immutable VIF-ring payload
// (including the bounded sparse fallback). CanonicalVuMemory is a qword which
// has remained unchanged since the current CPU-visible VU1 generation and can
// therefore be read directly from the permanently mapped VU region. The
// GXM submission owner rebases the generated root's existing BUFFER0 onto one
// homogeneous source for the complete invocation. CPU1 must never copy a
// wholly canonical range into the raw ring merely to make it GPU-readable;
// mixed owners conservatively use the compact transactional input path.
enum class StreamInputOwner : u8 {
  RawInput = 0,
  CanonicalVuMemory = 1,
};

struct StreamBinding {
  u32 payload_byte_offset = 0;
  u32 byte_stride = 0;
  u16 input_span = 0;
  u8 attribute_index = 0;
  StreamInputOwner owner = StreamInputOwner::RawInput;
  u32 outer_byte_stride = 0;
  u32 payload_byte_extent = 0;
};

enum class GeneratedInputWindowFailure : u8 {
  None,
  MissingBindings,
  InvalidCanonicalBinding,
  MixedOwners,
  InvalidBindingOwner,
  MissingPayloads,
  PayloadIndex,
  InvalidPayload,
  PayloadLogicalRange,
  MixedGeneration,
  InvalidBindingExtent,
  BindingOutsidePayload,
  BindingOutsideOwner,
  EmptyWindow,
  DeclaredWindow,
};

struct GeneratedInputWindow {
  uptr owner = 0;
  u32 slot = 0;
  u32 generation = 0;
  u32 required_first_qword = 0;
  u32 required_last_qword = 0;
  u32 bound_first_qword = 0;
  bool uses_raw = false;
  bool uses_canonical = false;
};

const char* GeneratedInputWindowFailureName(
    GeneratedInputWindowFailure failure);

bool ResolveGeneratedInputWindow(
    const RawVifPayloadRef* payloads, size_t payload_count,
    const StreamBinding* bindings, size_t binding_count,
    u32 raw_owner_qwords, u32 canonical_owner_qwords,
    u32 declared_qword_count, u32 maximum_relative_qword,
    GeneratedInputWindow* window,
    GeneratedInputWindowFailure* failure = nullptr);

struct VectorUniform {
  std::array<u32, 4> bits{};
  u8 register_index = 0;
};

struct ConstantUniform {
  std::array<u32, 4> bits{};
  u8 input_index = 0;
};

// One disjoint PairPlan store produced by a generated loop-kernel invocation.
// The shader writes entries in invocation-major/store-minor order to BUFFER2.
// During the first physical attestation gate, MTVU fills expected from its
// canonical post-execution VU memory and GXM compares after vertex completion.
// This is deliberately diagnostic successor evidence, not canonical CPU
// readback or product state publication.
struct PrivateStoreExpectation {
  // Selected PCSX2 CPU-provider result captured after the noncommitting
  // shadow execution.  This diagnoses the configured MTVU speedhack profile;
  // it is not automatically the canonical value when ApproximateFmac is set.
  std::array<u32, 4> expected{};
  // PCSX2-derived exact PairPlan arithmetic. Every lane which can become VU
  // memory or persistent state is compared against this value and may commit
  // only when it matches bit-for-bit.
  std::array<u32, 4> exact_profile_expected{};
  // Named playable SGX arithmetic model. It is diagnostic for persistent
  // stores and can become authoritative only for a separately proven
  // output-only direct-TFX cone.
  std::array<u32, 4> playable_profile_expected{};
  // Shared expression-DAG node which produced each lane. This is diagnostic
  // provenance only; support and product admission never depend on a node id.
  std::array<u32, 4> value_nodes{};
  std::array<u8, 4> value_kinds{};
  std::array<u8, 4> value_domains{};
  u16 address_qword = 0;
  u16 pair_pc = 0;
  u8 lane_mask = 0;
  u8 exact_profile_mask = 0;
  u8 playable_profile_mask = 0;
  // A native-SGX difference may be accepted only when a separate PairPlan
  // liveness proof establishes that the lane is consumed solely by this
  // direct-TFX output during the epoch and an exact canonical journal prevents
  // it from becoming persistent VU memory/state. Merely matching a GIF field
  // does not prove that property, so the default is zero.
  u8 output_only_mask = 0;
  u8 source_vf = 0;
};

struct PrivateArchitecturalStateExpectation {
  std::array<std::array<u32, 4>, 32> final_vf_values{};
  std::array<u32, 4> final_acc_values{};
  std::array<std::array<u32, 4>, 32> playable_final_vf_values{};
  std::array<u32, 4> playable_final_acc_values{};
  std::array<u16, 16> final_vi_values{};
  std::array<u8, 32> final_vf_lanes{};
  u32 final_vi_write_mask = 0u;
  u32 unique_resume_pc = 0u;
  u32 final_q_value = 0u;
  u32 final_p_value = 0u;
  u32 final_i_value = 0u;
  u32 playable_final_q_value = 0u;
  u32 playable_final_p_value = 0u;
  u32 playable_final_i_value = 0u;
  u8 final_acc_lanes = 0u;
  bool final_q = false;
  bool final_p = false;
  bool final_i = false;
  bool playable_profile_available = false;
};

enum class PrivateArchitecturalStateValueKind : u8 {
  Vf,
  Acc,
  Q,
  P,
  I,
  Vi,
  Tpc,
};

struct PrivateArchitecturalStateMismatch {
  PrivateArchitecturalStateValueKind kind =
      PrivateArchitecturalStateValueKind::Vf;
  u8 reg = 0u;
  u8 lane = 0u;
  u8 reserved = 0u;
  u32 actual = 0u;
  u32 exact_expected = 0u;
  u32 playable_expected = 0u;
};

inline constexpr u32 PrivateArchitecturalStateMismatchCapacity = 16u;

// Bounded oracle detail for a private generated-root attestation. This is
// populated before the GPU draw is queued, so it describes the compact
// successor formula versus PCSX2's completed CPU VU1 execution rather than a
// GPU readback. Keeping concrete destinations and words makes a failed
// canonical-state proof actionable without title/PC-specific admission.
struct PrivateArchitecturalStateComparison {
  std::array<PrivateArchitecturalStateMismatch,
             PrivateArchitecturalStateMismatchCapacity>
      exact_mismatches{};
  u32 exact_mismatch_count = 0u;
  u32 exact_mismatch_total = 0u;
};

const char* PrivateArchitecturalStateValueKindName(
    PrivateArchitecturalStateValueKind kind);

// Compact architectural destination for one qword emitted by the generated
// loop root. The GPU output remains invocation-major/store-minor; this table
// is the PairPlan-derived scatter map used only once vertex completion makes
// the private generation authoritative.
struct GeneratedLoopKernelStoreTarget {
  u16 address_qword = 0;
  u8 lane_mask = 0;
  u8 reserved = 0;
};

// PairPlan-derived destinations are invariant for one resolved generated
// transaction shape.  Keep that potentially hundreds-entry scatter map in a
// shared immutable layout instead of rebuilding and reallocating it for every
// hot Execute.  Per-Execute values remain in GeneratedLoopKernelTransaction.
struct GeneratedLoopKernelTransactionLayout final {
  static constexpr u32 MemoryWordCount = 16u * 1024u / sizeof(u32);
  static constexpr u32 MemoryQwordCount = 16u * 1024u / 16u;

  // PairPlan destinations are constructed once for one immutable runtime
  // shape, then shared by every hot transaction.  Seal() validates that shape
  // and derives dense ownership masks once; Configure() must not rescan
  // hundreds of identical BSpline destinations for every Execute.
  bool Seal();
  bool IsSealed() const {
    return sealed && sealed_store_target_count == store_targets.size() &&
           sealed_adc_patch_count == adc_patch_qwords.size();
  }

  std::vector<GeneratedLoopKernelStoreTarget> store_targets;
  std::vector<u16> adc_patch_qwords;
  std::array<u32, MemoryWordCount / 32u> store_word_masks{};
  std::array<u32, MemoryQwordCount / 32u> store_qword_masks{};
  // Qwords whose final transaction result replaces all four lanes. These do
  // not need a retained VIF owner materialized into CPU-private memory before
  // ownership moves to the generated transaction.
  std::array<u32, MemoryQwordCount / 32u> complete_write_qword_masks{};
  // Number of unique architectural lanes represented by store_word_masks.
  // A later transaction with this exact immutable layout can supersede a
  // fully-live, unobserved generation without scattering a new owner into
  // every lane on CPU1.
  u32 store_word_count = 0u;

private:
  size_t sealed_store_target_count = 0u;
  size_t sealed_adc_patch_count = 0u;
  bool sealed = false;
};

enum class GeneratedLoopKernelTransactionStage : u8 {
  Prepared,
  // The immediate GXM context accepted the draw which consumes this private
  // transaction.  CPU1 may continue to hold a speculative successor, but the
  // descriptor can no longer be replayed on CPU because libGXM owns its input
  // and output mappings until vertex completion.
  GsAccepted,
  GpuCompleted,
  Failed,
  Adopted,
};

enum class GeneratedLoopKernelTransactionFailure : u8 {
  None,
  Unspecified,
  DescriptorReleasedBeforeGsAcceptance,
  GsOutputRecordReleasedBeforeCompletion,
  GsRejectedBeforeEffects,
  GsSubmissionFailedAfterEffects,
  InvalidGpuOutputPublication,
  InvalidGpuCompletionPublication,
  CpuJournalAttachment,
  GpuRetirementOwnerFailure,
};

const char* GeneratedLoopKernelTransactionStageName(
    GeneratedLoopKernelTransactionStage stage);
const char* GeneratedLoopKernelTransactionFailureName(
    GeneratedLoopKernelTransactionFailure failure);

enum class GeneratedLoopKernelStoreCommitMode : u8 {
  PairPlanExact,
  DeferredPairPlanExact,
  GpuNativeOutputOnly,
};

// Immutable portion of a generated transaction.  A hot VIF chain can issue
// thousands of Executes which share this complete architectural shape; only
// retained input generations, captured dependency values, and final VI values
// differ.  Keeping the shape separate is also the prerequisite for replacing
// per-Execute completion records with one transactional chain owner.
struct GeneratedLoopKernelTransactionShape final {
  bool Seal();
  bool IsSealed() const { return sealed; }
  bool Matches(
      const std::shared_ptr<const GeneratedLoopKernelTransactionLayout>&
          candidate_layout,
      GeneratedLoopKernelStoreCommitMode candidate_store_commit_mode,
      GeneratedLoopKernelNumericProfile candidate_output_numeric_profile,
      const std::array<u8, 32>& candidate_final_vf_lanes,
      u8 candidate_final_acc_lanes, bool candidate_final_q,
      bool candidate_final_p, bool candidate_final_i,
      u32 candidate_final_vi_write_mask, u32 candidate_unique_resume_pc,
      u32 candidate_executed_pairs) const;

  std::shared_ptr<const GeneratedLoopKernelTransactionLayout> layout;
  std::array<u8, 32> final_vf_lanes{};
  u32 final_vi_write_mask = 0u;
  u32 unique_resume_pc = 0u;
  u32 executed_pairs = 0u;
  GeneratedLoopKernelStoreCommitMode store_commit_mode =
      GeneratedLoopKernelStoreCommitMode::PairPlanExact;
  GeneratedLoopKernelNumericProfile output_numeric_profile =
      GeneratedLoopKernelNumericProfile::None;
  u8 final_acc_lanes = 0u;
  bool final_q = false;
  bool final_p = false;
  bool final_i = false;

private:
  bool sealed = false;
};

class GeneratedLoopKernelTransaction;
inline constexpr u32 GeneratedLoopKernelTransactionPoolCapacity = 128u;
// A 128 x 8192-word BSS arena consumed 4 MiB even when the lean generated
// product had no BUFFER2 journal.  BSpline's largest physically attested
// canary needs 1440 words; retain a bounded 2048-word class and reject larger
// roots before effects until the product owner has a size-classed mapped
// journal arena.  This preserves the full asynchronous transaction count
// while returning 3 MiB to Vita's critically constrained general heap.
inline constexpr u32 GeneratedLoopKernelTransactionOutputWordsPerSlot = 2048u;
std::shared_ptr<GeneratedLoopKernelTransaction>
AcquireGeneratedLoopKernelTransaction(u32 preferred_output_words = 0u);

// A non-owning immutable word range. Product transactions use a fixed,
// process-lifetime journal arena, while host validation may retain a vector;
// exposing one view keeps retirement independent of either storage owner.
class GeneratedLoopKernelWordView final {
public:
  GeneratedLoopKernelWordView() = default;
  GeneratedLoopKernelWordView(const u32* words, size_t count)
      : m_words(words), m_count(count) {}

  const u32* data() const { return m_words; }
  size_t size() const { return m_count; }
  bool empty() const { return m_count == 0u; }
  const u32* begin() const { return m_words; }
  const u32* end() const { return m_words + m_count; }
  const u32& operator[](size_t index) const { return m_words[index]; }

  friend bool operator==(GeneratedLoopKernelWordView lhs,
                         const std::vector<u32>& rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }
  friend bool operator!=(GeneratedLoopKernelWordView lhs,
                         const std::vector<u32>& rhs) {
    return !(lhs == rhs);
  }

private:
  const u32* m_words = nullptr;
  size_t m_count = 0u;
};

struct GeneratedLoopKernelFinalStateValues final {
  std::array<std::array<u32, 4>, 32> vf{};
  std::array<u32, 4> acc{};
  u32 q = 0u;
  u32 p = 0u;
  u32 i = 0u;
};

// Closed-form loop analysis is bounded by VitaGpuVuEntrySlice.cpp's complete
// expression limit.  Real observers serialize on CPU1, so one process-lifetime
// workspace can serve every deferred successor/store owner without retaining
// two heap vectors per accepted Execute.
inline constexpr size_t GeneratedLoopKernelEvaluationWorkspaceCapacity =
    32768u;

struct GeneratedLoopKernelEvaluationWorkspace final {
  u32* values = nullptr;
  u8* states = nullptr;
  size_t capacity = 0u;

  bool IsValidFor(size_t count) const {
    return values && states && count <= capacity;
  }
};

// An exact successor formula captured from immutable PairPlan leaves.  The
// generated draw does not need these values to execute, so product ownership
// keeps the formula dormant across unobserved Executes and evaluates only the
// surviving lane owners at a real VIF/EE observer.  Implementations live with
// the PCSX2-derived loop IR; MTVU sees only this value-producing contract.
class GeneratedLoopKernelDeferredSuccessor {
public:
  virtual ~GeneratedLoopKernelDeferredSuccessor() = default;
  virtual bool Evaluate(GeneratedLoopKernelFinalStateValues* values,
                        GeneratedLoopKernelEvaluationWorkspace workspace,
                        std::string* error = nullptr) const = 0;
};

// Exact PairPlan store values whose output-only lanes need not be evaluated on
// every hot Execute.  The owner captures immutable architectural leaves before
// effects and evaluates the complete journal only if a later real observer
// reaches a lane which was not overwritten in the meantime.
class GeneratedLoopKernelDeferredStores {
public:
  virtual ~GeneratedLoopKernelDeferredStores() = default;
  virtual const GeneratedLoopKernelTransactionLayout* LayoutIdentity()
      const = 0;
  virtual const std::vector<GeneratedLoopKernelStoreTarget>& Targets() const = 0;
  // The observer supplies bounded process-lifetime storage. Product
  // materialization must not allocate a fresh vector after GPU ownership has
  // already been accepted; physical r177 exhausted newlib in that exact path.
  virtual bool Evaluate(u32* words, size_t word_capacity,
                        size_t* word_count,
                        GeneratedLoopKernelEvaluationWorkspace workspace,
                        std::string* error = nullptr) const = 0;
};

// One sequence-numbered successor generation shared by CPU1 and the GXM
// retirement owner. Exact mode retains a separately evaluated PairPlan store
// journal. The physically attested native profile may instead commit BUFFER2
// only for store lanes classified as direct-output-only; control/register
// successor state remains PairPlan-exact in both modes.
class GeneratedLoopKernelTransaction final {
public:
  GeneratedLoopKernelTransaction() = default;
  GeneratedLoopKernelTransaction(const GeneratedLoopKernelTransaction &) =
      delete;
  GeneratedLoopKernelTransaction &
  operator=(const GeneratedLoopKernelTransaction &) = delete;
  ~GeneratedLoopKernelTransaction();

  // Validation/construction convenience. Product hot paths use the shared
  // immutable-layout overload below.
  bool
  Configure(std::vector<GeneratedLoopKernelStoreTarget> store_targets,
            std::vector<u32> exact_store_words,
            std::vector<GeneratedLoopKernelStoreTarget> pre_loop_store_targets,
            std::vector<u32> pre_loop_store_words,
            std::vector<u16> adc_patch_qwords,
            GeneratedLoopKernelStoreCommitMode store_commit_mode,
            GeneratedLoopKernelNumericProfile output_numeric_profile,
            const std::array<u8, 32> &final_vf_lanes, u8 final_acc_lanes,
            bool final_q, bool final_p, bool final_i,
            const std::array<std::array<u32, 4>, 32> &final_vf_values,
            const std::array<u32, 4> &final_acc_values, u32 final_q_value,
            u32 final_p_value, u32 final_i_value,
            const std::array<u16, 16> &final_vi_values, u32 final_vi_write_mask,
            u32 unique_resume_pc, u32 executed_pairs,
            std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>
                deferred_successor = {},
            std::shared_ptr<const GeneratedLoopKernelDeferredStores>
                deferred_stores = {});
  bool
  Configure(std::shared_ptr<const GeneratedLoopKernelTransactionLayout> layout,
            std::vector<u32> exact_store_words,
            std::vector<GeneratedLoopKernelStoreTarget> pre_loop_store_targets,
            std::vector<u32> pre_loop_store_words,
            GeneratedLoopKernelStoreCommitMode store_commit_mode,
            GeneratedLoopKernelNumericProfile output_numeric_profile,
            const std::array<u8, 32> &final_vf_lanes, u8 final_acc_lanes,
            bool final_q, bool final_p, bool final_i,
            const std::array<std::array<u32, 4>, 32> &final_vf_values,
            const std::array<u32, 4> &final_acc_values, u32 final_q_value,
            u32 final_p_value, u32 final_i_value,
            const std::array<u16, 16> &final_vi_values, u32 final_vi_write_mask,
            u32 unique_resume_pc, u32 executed_pairs,
            std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>
                deferred_successor = {},
            std::shared_ptr<const GeneratedLoopKernelDeferredStores>
                deferred_stores = {});
  bool Configure(
      std::shared_ptr<const GeneratedLoopKernelTransactionShape> shape,
      std::vector<u32> exact_store_words,
      std::vector<GeneratedLoopKernelStoreTarget> pre_loop_store_targets,
      std::vector<u32> pre_loop_store_words,
      const std::array<u16, 16>& final_vi_values,
      std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>
          deferred_successor = {},
      std::shared_ptr<const GeneratedLoopKernelDeferredStores>
          deferred_stores = {});
  bool AttachReplayUnpacks(std::vector<VifUnpackSpan> *spans);
  std::vector<VifUnpackSpan> TakeReplayUnpacks();

  void SetSequence(u64 sequence) { m_sequence = sequence; }
  u64 Sequence() const { return m_sequence; }
  void SetValidationCanary(bool enabled) { m_validation_canary = enabled; }
  bool IsValidationCanary() const { return m_validation_canary; }
  u32 StoreEntryCount() const {
    return m_layout ? static_cast<u32>(m_layout->store_targets.size()) : 0u;
  }
  u32 OutputWordCount() const { return StoreEntryCount() * 4u; }
  u32 OutputBytes() const { return OutputWordCount() * sizeof(u32); }

  bool CanClaimGpuOutputOwner() const;
  bool ClaimGpuOutputOwner();
  void ReleaseDescriptorOwner();
  bool PublishGpuOutput(const u32 *words, u32 word_count);
  bool PublishGpuCompletion();
  void MarkFailed(GeneratedLoopKernelTransactionFailure failure =
                      GeneratedLoopKernelTransactionFailure::Unspecified,
                  const char* detail = nullptr);
  void WaitForTerminal();
  bool MarkAdopted();
  const std::vector<VifUnpackSpan>& ReplayUnpacks() const {
    return m_replay_unpacks;
  }
  bool HasReplayUnpackJournal() const { return m_replay_unpacks_attached; }
  bool RecoverReplayUnpacksBeforeGsAcceptance(
      std::vector<VifUnpackSpan>* spans);

  GeneratedLoopKernelTransactionStage Stage() const {
    return m_stage.load(std::memory_order_acquire);
  }
  GeneratedLoopKernelTransactionFailure Failure() const {
    return m_failure.load(std::memory_order_acquire);
  }
  const char* FailureDetail() const {
    return m_failure_detail.load(std::memory_order_acquire);
  }
  const std::vector<GeneratedLoopKernelStoreTarget> &StoreTargets() const {
    return m_layout->store_targets;
  }
  const std::vector<u16> &AdcPatchQwords() const {
    return m_layout->adc_patch_qwords;
  }
  const std::array<u32, GeneratedLoopKernelTransactionLayout::MemoryWordCount /
                            32u>&
  StoreWordMasks() const {
    return m_layout->store_word_masks;
  }
  const std::array<u32,
                   GeneratedLoopKernelTransactionLayout::MemoryQwordCount /
                       32u>&
  StoreQwordMasks() const {
    return m_layout->store_qword_masks;
  }
  const std::array<u32,
                   GeneratedLoopKernelTransactionLayout::MemoryQwordCount /
                       32u>&
  CompleteWriteQwordMasks() const {
    return m_layout->complete_write_qword_masks;
  }
  const GeneratedLoopKernelTransactionLayout* LayoutIdentity() const {
    return m_layout.get();
  }
  u32 StoreWordCount() const {
    return m_layout ? m_layout->store_word_count : 0u;
  }
  GeneratedLoopKernelWordView OutputWords() const {
    return {m_output_words_data, m_output_words_size};
  }
  const std::vector<u32> &ExactStoreWords() const {
    return m_exact_store_words;
  }
  GeneratedLoopKernelStoreCommitMode StoreCommitMode() const {
    return m_shape ? m_shape->store_commit_mode : m_store_commit_mode;
  }
  bool UsesGpuNativeOutputOnlyStoreCommit() const {
    return StoreCommitMode() ==
           GeneratedLoopKernelStoreCommitMode::GpuNativeOutputOnly;
  }
  bool UsesDeferredPairPlanStoreCommit() const {
    return StoreCommitMode() ==
           GeneratedLoopKernelStoreCommitMode::DeferredPairPlanExact;
  }
  GeneratedLoopKernelWordView CommittedStoreWords() const {
    return UsesGpuNativeOutputOnlyStoreCommit()
        ? OutputWords()
        : GeneratedLoopKernelWordView(m_exact_store_words.data(),
                                      m_exact_store_words.size());
  }
  const char *StoreCommitModeName() const {
    return UsesGpuNativeOutputOnlyStoreCommit()
               ? "gpu-native-output-only"
               : (UsesDeferredPairPlanStoreCommit()
                      ? "pairplan-exact-deferred"
                      : "pairplan-exact");
  }
  u32 PreLoopStoreEntryCount() const {
    return static_cast<u32>(m_pre_loop_store_targets.size());
  }
  const std::vector<GeneratedLoopKernelStoreTarget> &
  PreLoopStoreTargets() const {
    return m_pre_loop_store_targets;
  }
  const std::vector<u32> &PreLoopStoreWords() const {
    return m_pre_loop_store_words;
  }
  GeneratedLoopKernelNumericProfile OutputNumericProfile() const {
    return m_shape ? m_shape->output_numeric_profile :
                     m_output_numeric_profile;
  }
  const std::array<u8, 32> &FinalVfLanes() const {
    return m_shape ? m_shape->final_vf_lanes : m_final_vf_lanes;
  }
  u8 FinalAccLanes() const {
    return m_shape ? m_shape->final_acc_lanes : m_final_acc_lanes;
  }
  bool FinalQ() const { return m_shape ? m_shape->final_q : m_final_q; }
  bool FinalP() const { return m_shape ? m_shape->final_p : m_final_p; }
  bool FinalI() const { return m_shape ? m_shape->final_i : m_final_i; }
  const std::array<std::array<u32, 4>, 32> &FinalVfValues() const {
    return m_final_vf_values;
  }
  const std::array<u32, 4> &FinalAccValues() const {
    return m_final_acc_values;
  }
  u32 FinalQValue() const { return m_final_q_value; }
  u32 FinalPValue() const { return m_final_p_value; }
  u32 FinalIValue() const { return m_final_i_value; }
  const std::array<u16, 16> &FinalViValues() const { return m_final_vi_values; }
  u32 FinalViWriteMask() const {
    return m_shape ? m_shape->final_vi_write_mask : m_final_vi_write_mask;
  }
  u32 UniqueResumePc() const {
    return m_shape ? m_shape->unique_resume_pc : m_unique_resume_pc;
  }
  u32 ExecutedPairs() const {
    return m_shape ? m_shape->executed_pairs : m_executed_pairs;
  }
  bool HasDeferredSuccessor() const {
    return static_cast<bool>(m_deferred_successor);
  }
  const std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>&
  DeferredSuccessor() const {
    return m_deferred_successor;
  }
  const std::shared_ptr<const GeneratedLoopKernelDeferredStores>&
  DeferredStores() const {
    return m_deferred_stores;
  }
  bool ReleaseTransferredDeferredStores() {
    if (!UsesDeferredPairPlanStoreCommit() || !m_deferred_stores ||
        !m_configured) {
      return false;
    }
    m_deferred_stores.reset();
    return true;
  }
  bool ReleaseTransferredDeferredSuccessor() {
    if (!m_deferred_successor || !m_configured) {
      return false;
    }
    m_deferred_successor.reset();
    return true;
  }

private:
  friend std::shared_ptr<GeneratedLoopKernelTransaction>
  AcquireGeneratedLoopKernelTransaction(u32 preferred_output_words);
  void AttachFixedOutputStorage(u32* words, u32 capacity_words);
  bool ResizeOutputWords(u32 word_count);
  void ResetForReuse(bool preserve_output_words);

  std::atomic<GeneratedLoopKernelTransactionStage> m_stage{
      GeneratedLoopKernelTransactionStage::Prepared};
  std::atomic<GeneratedLoopKernelTransactionFailure> m_failure{
      GeneratedLoopKernelTransactionFailure::None};
  std::atomic<const char*> m_failure_detail{nullptr};
  std::atomic_bool m_gpu_output_owner{false};
  std::shared_ptr<const GeneratedLoopKernelTransactionShape> m_shape;
  std::shared_ptr<const GeneratedLoopKernelTransactionLayout> m_layout;
  std::vector<VifUnpackSpan> m_replay_unpacks;
  bool m_replay_unpacks_attached = false;
  std::vector<u32> m_exact_store_words;
  std::vector<GeneratedLoopKernelStoreTarget> m_pre_loop_store_targets;
  std::vector<u32> m_pre_loop_store_words;
  // Product pool entries point at a fixed process-lifetime arena. Directly
  // constructed oracle transactions use the vector fallback. This removes a
  // several-KiB newlib allocation from every accepted Execute without making
  // output capacity unbounded.
  std::vector<u32> m_output_words_fallback;
  u32* m_output_words_data = nullptr;
  u32 m_output_words_size = 0u;
  u32 m_output_words_capacity = 0u;
  std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>
      m_deferred_successor;
  std::shared_ptr<const GeneratedLoopKernelDeferredStores> m_deferred_stores;
  std::array<u8, 32> m_final_vf_lanes{};
  std::array<std::array<u32, 4>, 32> m_final_vf_values{};
  std::array<u32, 4> m_final_acc_values{};
  std::array<u16, 16> m_final_vi_values{};
  u64 m_sequence = 0;
  u32 m_final_vi_write_mask = 0;
  u32 m_unique_resume_pc = 0;
  u32 m_executed_pairs = 0;
  u32 m_final_q_value = 0;
  u32 m_final_p_value = 0;
  u32 m_final_i_value = 0;
  u8 m_final_acc_lanes = 0;
  GeneratedLoopKernelStoreCommitMode m_store_commit_mode =
      GeneratedLoopKernelStoreCommitMode::PairPlanExact;
  GeneratedLoopKernelNumericProfile m_output_numeric_profile =
      GeneratedLoopKernelNumericProfile::None;
  bool m_final_q = false;
  bool m_final_p = false;
  bool m_final_i = false;
  bool m_validation_canary = false;
  bool m_configured = false;
};

// Shadow-only reference for the generated loop kernel's one rounded-MUL/FTOI
// boundary. The values are evaluated from the same immutable entry state and
// PairPlan expression graph before CPU replay. They let retirement distinguish
// an incorrect GPU live-in from native Series5 arithmetic drift; none of these
// words are uploaded to the shader or accepted as canonical VU state.
struct FtoiProbeExpectation {
  u32 left = 0;
  u32 right = 0;
  u32 product = 0;
  u32 converted = 0;
  // Counterfactual host-only values used to identify which enclosing-loop
  // lowering class first departs from the PCSX2 CPU oracle. Each value freezes
  // only the named outer-dependent input at iteration zero; none is uploaded
  // to the GXP or accepted as architectural state.
  u32 compact_outer_zero_converted = 0;
  u32 outer_memory_zero_converted = 0;
  u32 repeated_add_zero_converted = 0;
  u32 all_outer_zero_converted = 0;
  u32 left_node = 0;
  u32 right_node = 0;
  u32 product_node = 0;
  u32 ftoi_node = 0;
};

enum ScalarUniformMask : u32 {
  ScalarUniformQ = 1u << 0,
  ScalarUniformP = 1u << 1,
  ScalarUniformI = 1u << 2,
  ScalarUniformGifQ = 1u << 3,
};

struct ScalarUniforms {
  u32 present = 0;
  u32 q = 0;
  u32 p = 0;
  u32 i = 0;
  u32 gif_q = 0;
};

// Concatenate one object's exact primitive indices into its global shader
// INDEX domain (original or expanded). This helper is shared by the GXM owner and
// differential tests so an index wrap can never select another transaction's
// batch record.
bool RebaseExactIndicesForGpuVuBatch(const u16* local_indices,
    size_t index_count, u32 invocation_count, u32 object_index,
    u16* global_indices);

// Variable-capacity generated batches reserve one fixed compiler invocation
// domain per object while each transaction contributes only its active exact
// indices. This keeps the shader's batch-index division constant and preserves
// primitive order without executing inactive vertices.
bool RebaseExactIndicesForGpuVuVariableBatch(const u16* local_indices,
    size_t index_count, u32 active_invocation_count,
    u32 batch_invocation_stride, u32 object_index, u16* global_indices);

// Private loop-store output uses the same fixed compiler invocation domain as
// variable-capacity exact-index batching.  Each object reserves one complete
// capacity stride so VuInvocation remains a direct GXP write index, while the
// transaction publishes only the active prefix after retirement.
struct GeneratedLoopKernelPrivateOutputBatchLayout {
  u32 object_count = 0;
  u32 capacity_invocations = 0;
  u32 stores_per_invocation = 0;
  u32 payload_stride_bytes = 0;
  u32 probe_stride_bytes = 0;
  u32 payload_total_bytes = 0;
  u32 probe_total_bytes = 0;
  u32 guard_offset_bytes = 0;
  u32 allocation_bytes = 0;
};

struct GeneratedLoopKernelPrivateOutputSlice {
  u32 payload_offset_bytes = 0;
  u32 payload_bytes = 0;
  u32 entry_count = 0;
  u32 probe_offset_bytes = 0;
  u32 probe_bytes = 0;
  u32 probe_count = 0;
};

// Exact writable-buffer bounds for the INDEX domain submitted to one
// generated loop-kernel draw.  BUFFER2 stores one four-word vector per
// private store and BUFFER3 stores two four-word probe vectors per shader
// invocation.  Keeping this proof pointer-free lets the host validation suite
// exercise the same arithmetic which guards the final GXM call.
struct GeneratedLoopKernelPrivateOutputWriteExtent {
  u32 invocation_first = 0;
  u32 invocation_last = 0;
  u32 payload_capacity_words = 0;
  u32 payload_maximum_write_word = 0;
  u32 probe_capacity_words = 0;
  u32 probe_maximum_write_word = 0;
};

bool ComputeGeneratedLoopKernelPrivateOutputBatchLayout(
    u32 object_count, u32 capacity_invocations,
    u32 stores_per_invocation, bool has_ftoi_probe, u32 guard_bytes,
    GeneratedLoopKernelPrivateOutputBatchLayout* layout);

bool ResolveGeneratedLoopKernelPrivateOutputBatchSlice(
    const GeneratedLoopKernelPrivateOutputBatchLayout& layout,
    u32 object_index, u32 active_invocations,
    GeneratedLoopKernelPrivateOutputSlice* slice);

bool ResolveGeneratedLoopKernelPrivateOutputWriteExtent(
    u32 invocation_first, u32 invocation_last,
    u32 stores_per_invocation, u32 payload_capacity_words,
    u32 probe_capacity_words,
    GeneratedLoopKernelPrivateOutputWriteExtent* extent);

// GXM binds the complete statically declared Cg uniform-buffer array, not
// merely the elements reached by the generated expression graph. Rebase one
// logical qword interval so both the requested data and the declared window
// remain inside one mapped owner. Failure is pre-effect and leaves the output
// unchanged.
bool ResolveGeneratedBufferWindow(u32 required_first_qword,
    u32 required_last_qword, u32 owner_qword_count,
    u32 declared_qword_count, u32 maximum_relative_qword,
    u32* bound_first_qword);

// Replays the generated BUFFER0 expression at final record-build time.
// Success proves every qword in the affine/grid extent remains inside both the
// host-proven window and the compiler-safe relative-index domain.
bool ValidateGeneratedBufferReadWindow(u64 absolute_byte_offset,
    u32 payload_byte_extent, u32 bound_first_qword,
    u32 required_last_qword, u32 maximum_relative_qword,
    u32* relative_base_qword);

// A compact owner has no surrounding 2 MiB input-ring slot. Pad its mapped
// storage to the complete Cg declaration so speculative/PDS-side accesses
// cannot cross into the following transfer-arena allocation.
bool ResolveGeneratedCompactStorageQwords(u32 logical_qword_count,
    u32 declared_qword_count, u32* storage_qword_count);

// Descriptor-scale register and constant inputs are immutable for one direct
// MSCAL/MSCNT chain when the resume slice has no current-memory constant
// inputs. Keep them in one allocation shared by every compact continuation
// descriptor instead of copying roughly 700 bytes for every 34-vertex chunk.
// A chain with dynamic resume-time constants receives a distinct block through
// the general builder, preserving the same semantic contract.
struct GpuVuUniformBlock final {
#if defined(__vita__)
  static void* operator new(std::size_t size);
  static void operator delete(void* pointer) noexcept;
  static void operator delete(void* pointer, std::size_t size) noexcept;
#endif

  void Retain() {
    m_references.fetch_add(1, std::memory_order_relaxed);
  }
  void Release() {
    if (m_references.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete this;
  }

  // Generated roots currently admit at most 32 constant qwords.  Keeping that
  // compiler-bounded shape in the fixed uniform-block pool avoids promoting
  // the 27-qword BSpline transaction through newlib on every Execute.  The
  // vector fallback remains available if a later ABI raises the limit.
  InlineDescriptorVector<ConstantUniform, 32> constant_uniforms;
  InlineDescriptorVector<VectorUniform, 16> vf_uniforms;

private:
  std::atomic<u32> m_references{1};
};

// Vita's libstdc++ may select non-atomic shared_ptr reference counts. This
// explicit intrusive owner crosses MTVU and GS with an architectural atomic
// count, matching PreparedProgramReference's ownership rule.
class GpuVuUniformBlockRef final {
public:
  GpuVuUniformBlockRef() = default;
  static GpuVuUniformBlockRef Adopt(GpuVuUniformBlock* block) {
    GpuVuUniformBlockRef result;
    result.m_block = block;
    return result;
  }
  explicit GpuVuUniformBlockRef(GpuVuUniformBlock* block)
      : m_block(block) {
    if (m_block)
      m_block->Retain();
  }
  GpuVuUniformBlockRef(const GpuVuUniformBlockRef& other)
      : GpuVuUniformBlockRef(other.m_block) {}
  GpuVuUniformBlockRef& operator=(const GpuVuUniformBlockRef& other) {
    if (this == &other)
      return *this;
    GpuVuUniformBlockRef replacement(other);
    Swap(replacement);
    return *this;
  }
  GpuVuUniformBlockRef(GpuVuUniformBlockRef&& other) noexcept
      : m_block(std::exchange(other.m_block, nullptr)) {}
  GpuVuUniformBlockRef& operator=(GpuVuUniformBlockRef&& other) noexcept {
    if (this == &other)
      return *this;
    GpuVuUniformBlockRef replacement(std::move(other));
    Swap(replacement);
    return *this;
  }
  ~GpuVuUniformBlockRef() {
    if (m_block)
      m_block->Release();
  }

  GpuVuUniformBlock* Get() const { return m_block; }
  explicit operator bool() const { return m_block != nullptr; }
  void Swap(GpuVuUniformBlockRef& other) noexcept {
    std::swap(m_block, other.m_block);
  }

private:
  GpuVuUniformBlock* m_block = nullptr;
};

struct FinalStatePublication {
  u32 vf_mask = 0;
  u32 vi_mask = 0;
  bool acc = false;
  bool q = false;
  bool p = false;
  bool i = false;
  bool flags = false;
  bool vif = false;
  bool memory = false;

  bool IsRequired() const;
};

// Immutable, sequence-numbered handoff from the EE/VIF producer to the
// GS/GXM-owning thread. PairPlan consumes the full VIF UNPACK records before
// construction; AddInputPayload() retains only the immutable raw byte owners
// selected by the resulting stream bindings until GPU completion/rejection.
class GpuVuDraw final {
public:
  GpuVuDraw() = default;
  GpuVuDraw(const GpuVuDraw &) = delete;
  GpuVuDraw &operator=(const GpuVuDraw &) = delete;
  GpuVuDraw(GpuVuDraw &&) = delete;
  GpuVuDraw &operator=(GpuVuDraw &&) = delete;
  ~GpuVuDraw();

#if defined(__vita__)
  static void* operator new(std::size_t size);
  static void operator delete(void* pointer) noexcept;
  static void operator delete(void* pointer, std::size_t size) noexcept;
#endif

  bool AddInputPayload(const RawVifPayloadRef &payload);
  // Installs one generated BUFFER0 payload/stream plan as a transaction. Every
  // payload is retained and both destination collections are reserved before
  // either logical collection changes. Failure leaves both collections empty.
  bool SetGeneratedInputPlan(
      const RawVifPayloadRef* payloads, size_t payload_count,
      const StreamBinding* bindings, size_t binding_count,
      bool require_generated_window = true);
  // Some exact generated kernels read a small, statically addressed subset of
  // VU memory which is not wholly backed by the current epoch's raw UNPACK
  // spans. Retain only those proven qwords in shader-address layout. The GS
  // owner copies this compact immutable table into its mapped transfer arena;
  // this is neither a full VU-memory snapshot nor semantic CPU execution.
  bool SetCompactRawInputWords(std::vector<u32> words);
  bool Validate(std::string *error) const;
  // The MTVU owner calls this once after assigning the ordering sequence and
  // immediately before publishing the immutable descriptor. The GS owner may
  // then trust all descriptor-wide bounds and layout checks without walking
  // the same spans and uniforms again on the 496 MHz worker thread.
  bool ValidateForQueue(std::string *error);
  // Generated product descriptors are assembled exclusively from an immutable
  // compiler-owned shape whose vectors, indices, stream bounds, and output
  // layout were checked while that shape was built.  Rewalking those same
  // arrays for every hot Execute made validation proportional to guest output
  // size.  This seals only the changing transaction/sequence boundary after
  // the generated builder has already checked its dynamic payload guards.
  // Other descriptor producers continue through ValidateForQueue().
  bool SealGeneratedLoopKernelProductForQueue(
      u64 sequence, std::string *error = nullptr);
  bool WasValidatedForQueue() const { return m_validated_for_queue; }
  const GeneratedInputWindow* GeneratedInputWindowProof() const {
    return m_generated_input_window_valid ? &m_generated_input_window : nullptr;
  }

  const InlineDescriptorVector<RawVifPayloadRef, 4>& InputPayloads() const {
    return m_input_payloads;
  }
  // GS/GXM calls this only after a draw has been encoded and every descriptor
  // field has been consumed.  It transfers the descriptor's existing ring
  // reference into the scene-retirement owner without a second, fallible
  // generation lookup after GPU-visible effects have begun.
  bool TransferInputPayloadOwnership(
      size_t index, RawVifPayloadRef* destination);
  const std::vector<u32>& CompactRawInputWords() const {
    return m_compact_raw_input_words;
  }
  bool HasCompactRawInputs() const {
    return !m_compact_raw_input_words.empty();
  }
  const RawVifPayloadRef* StructuredDirectInput() const {
    return m_structured_direct_input_span < m_input_payloads.size()
        ? &m_input_payloads[m_structured_direct_input_span]
        : nullptr;
  }
  bool SetStructuredDirectInput(const RawVifPayloadRef& payload);
  bool ConfigurePrivateStoreJournal(
      u8 stores_per_invocation,
      std::vector<PrivateStoreExpectation> expectations = {});
  bool ConfigureGeneratedLoopKernelTransaction(
      u8 stores_per_invocation,
      std::shared_ptr<GeneratedLoopKernelTransaction> transaction,
      bool requires_gpu_private_store_output = true);
  bool ConfigureFtoiProbeExpectations(
      std::vector<FtoiProbeExpectation> expectations);
  bool ConfigurePrivateArchitecturalStateExpectation(
      PrivateArchitecturalStateExpectation expectation);
  bool ComparePrivateArchitecturalState(
      const u32* vf_words, const u32* acc_words,
      u32 q, u32 p, u32 i,
      const std::array<u16, 16>& vi_values, u32 tpc_bytes,
      u32* mismatch_lanes = nullptr,
      u32* playable_mismatch_lanes = nullptr,
      PrivateArchitecturalStateComparison* comparison = nullptr);
  bool ConfigureGeneratedLoopKernelAttestation(
      GeneratedLoopKernelAttestationIdentity identity);
  // A no-write product has its own content/program key but inherits the
  // physical result of the private canary from which it was generated. Keep
  // this distinct from canary attachment so an ordinary draw cannot silently
  // attach an unrelated attestation identity.
  bool ConfigureGeneratedLoopKernelProductAttestation(
      GeneratedLoopKernelAttestationIdentity canary_identity);
  // Selects the only legal attestation relationship for an authoritative
  // generated executable. Writable exact roots attest themselves; no-write
  // products inherit the distinct private-canary identity from which their
  // source was derived.
  bool ConfigureGeneratedLoopKernelExecutableAttestation(
      GeneratedLoopKernelAttestationIdentity canary_identity,
      bool uses_private_store_output);
  bool CapturePrivateStoreExpected(const void* vu_memory, size_t bytes);
  bool SetExactPostLoopIndices(std::vector<u16> indices);
  // Cold private validation only: one POINT per original VU iteration, never
  // ADC-filtered geometry or an authoritative state/rendering transaction.
  bool ConfigurePrivateStatePoints();
  bool IsPrivateStateCanary() const {
    return primitive_boundary == PrimitiveBoundary::PrivateStatePoints;
  }
  // Generated products already retain the immutable PairPlan runtime which
  // owns this index list.  Reference that list directly instead of allocating
  // and copying hundreds of u16s for every hot Execute on MTVU.
  bool SetSharedExactPostLoopIndices(
      const std::vector<u16>& indices,
      std::shared_ptr<const void> lifetime_owner);
  u32 ExactIndicesPerPrimitive() const;
  // Size of the legal shader INDEX address range, not the number of executed
  // vertices (ADC may leave holes). Canonical state uses invocation_count.
  u32 ShaderIndexDomainCount() const;
  bool HasExpandedExactIndices() const {
    return primitive_boundary == PrimitiveBoundary::ExactPostLoopExpandedIndexed;
  }
  bool HasPrivateStoreJournal() const {
    return m_requires_gpu_private_store_output && private_store_count != 0u;
  }
  // Native/nested roots execute one shader invocation per VU iteration.
  // Expanded flat roots execute one shader invocation per expanded index and
  // therefore require the larger, collision-free private journal domain.
  u32 PrivateStoreInvocationCount() const {
    return primitive_boundary == PrimitiveBoundary::ExpandedIndexed
        ? index_count
        : invocation_count;
  }
  bool HasPrivateStoreComparison() const {
    return cpu_shadow_private_compare &&
           !private_store_expectations.empty();
  }
  bool HasGeneratedLoopKernelTransaction() const {
    return static_cast<bool>(m_generated_loop_kernel_transaction);
  }
  const std::shared_ptr<GeneratedLoopKernelTransaction>&
  GeneratedLoopKernelTransactionOwner() const {
    return m_generated_loop_kernel_transaction;
  }
  const std::vector<PrivateStoreExpectation>& PrivateStoreExpectations() const {
    return private_store_expectations;
  }
  const std::vector<FtoiProbeExpectation>& FtoiProbeExpectations() const {
    return ftoi_probe_expectations;
  }
  const GeneratedLoopKernelAttestationIdentity&
  GeneratedLoopKernelAttestation() const {
    return m_generated_loop_kernel_attestation;
  }
  bool HasGeneratedLoopKernelAttestation() const {
    return m_generated_loop_kernel_attestation.IsValid();
  }
  bool PrivateArchitecturalStateCompared() const {
    return m_private_architectural_state_compared;
  }
  bool PrivateArchitecturalStateExact() const {
    return m_private_architectural_state_exact;
  }
  u32 PrivateArchitecturalStateMismatchLanes() const {
    return m_private_architectural_state_mismatch_lanes;
  }
  bool PrivateArchitecturalStatePlayableProfileMatches() const {
    return m_private_architectural_state_playable_profile_matches;
  }
  u32 PrivateArchitecturalStatePlayableMismatchLanes() const {
    return m_private_architectural_state_playable_mismatch_lanes;
  }
  const std::vector<u16>& ExactIndices() const {
    return m_shared_exact_indices ? *m_shared_exact_indices : exact_indices;
  }
  bool HasExactIndices() const { return !ExactIndices().empty(); }
  bool UsesQuarantinedSnapshotArchitecture() const {
    return StructuredDirectInput() != nullptr ||
           precompute_program_count != 0 || precompute_stage_count != 0;
  }

  ShaderKey program;
  std::array<ShaderKey, GpuVuDirectPrecomputeMaximumPrograms>
      precompute_programs{};
  std::array<u8, GpuVuDirectPrecomputeMaximumPrograms>
      precompute_stages{};
  u8 precompute_program_count = 0;
  u8 precompute_stage_count = 0;
  DirectTfxContract direct_tfx;
  std::array<u32, 4> gif_tag{};
  // The generated input ABI is bounded to sixteen memory expressions.  Most
  // BSpline transactions bind five or ten streams, so four inline entries
  // forced one heap allocation for every accepted Execute.  Put the complete
  // current ABI in the fixed GpuVuDraw pool while retaining overflow support.
  InlineDescriptorVector<StreamBinding, 16> streams;
  // Entry-slice roots commonly lift one 4x4 matrix from sixteen fixed VU
  // qwords. Keep that complete descriptor set inline: promoting every MSCNT
  // continuation through malloc would put allocator traffic back into the
  // 496 MHz worker hot path.
  const InlineDescriptorVector<ConstantUniform, 32>& ConstantUniforms() const;
  const InlineDescriptorVector<VectorUniform, 16>& VfUniforms() const;
  const GpuVuUniformBlockRef& UniformBlock() const {
    return m_uniform_block;
  }
  void SetUniformBlock(GpuVuUniformBlockRef block) {
    m_uniform_block = std::move(block);
  }
  std::array<u32, 4> acc_uniform{};
  ScalarUniforms scalar_uniforms;
  std::array<std::array<float, 4>, 3> vertex_scale_offset{};
  float max_depth = 0.0f;
  InlineDescriptorVector<StaticGsWrite, 4> static_gs_writes;
  IntegerRect target_bounds;
  IntegerRect texture_bounds;
  // PairPlan-derived descriptor-scale exit state. MTVU publishes these VI
  // values after the immutable draw handoff succeeds, before it releases the
  // ordinary VU completion flag. They are not GPU readback requirements.
  std::array<u16, 16> final_vi_values{};
  u32 final_vi_write_mask = 0;
  // Exact post-E byte PC proven by the PairPlan CFG. This remains descriptor
  // state on the MTVU owner; the generated vertex root never needs to publish
  // it back from SGX.
  u32 unique_resume_pc = 0;
  FinalStatePublication final_state;
  u64 ordering_sequence = 0;
  // Bounded private comparison evidence only; never an execution/admission key.
  u64 private_replay_capture_id = 0;
  // PairPlan-backed dynamic work actually owned by this descriptor. This is
  // telemetry/accounting only: it never participates in semantic admission or
  // identifies a program. Structured generated draws publish it so hardware
  // evidence can distinguish a real VU bypass from shader preparation alone.
  u32 executed_pair_count = 0;
  u32 invocation_count = 0;
  u32 vertex_count = 0;
  u32 primitive_count = 0;
  u32 index_count = 0;
  // ABI-10 generated loop kernels publish every PairPlan store to one private
  // writable BUFFER2. A zero count means the registered root must not bind or
  // write that buffer. The physical oracle gate additionally carries one
  // expectation per private-domain invocation/store entry.
  u8 private_store_count = 0;
  bool cpu_shadow_private_compare = false;
  OutputLowering lowering = OutputLowering::DirectTfx;
  ExecutionKind execution = ExecutionKind::GeneratedParallel;
  PrimitiveBoundary primitive_boundary = PrimitiveBoundary::Native;

  // VitaGsMailbox links consecutive direct PATH1 descriptors without a
  // per-dispatch allocation. The GS owner clears this before normal descriptor
  // validation and ownership transfer.
  GpuVuDraw* path1_next = nullptr;
  // Empty VU dispatches immediately before this draw still own ordered PATH1
  // credits (PCSX2 MTVU.cpp::ExecuteRingBuffer). They require no descriptor,
  // GIF packet, or GPU work. MTVU writes this before queue release; only MTGS
  // changes it afterwards when a reservation prefix ends inside the gap.
  u32 path1_leading_no_output_count = 0;

private:
  bool IsExactPostLoopIndexShapeValid(std::span<const u16> indices,
                                    bool expanded) const;
  // Generic affine vertex programs commonly use position, normal, texture,
  // and one auxiliary stream. Keeping four references inline avoids one heap
  // promotion per IGA-style three-stream dispatch on the 496 MHz MTVU core.
  InlineDescriptorVector<RawVifPayloadRef, 4> m_input_payloads;
  std::vector<u32> m_compact_raw_input_words;
  GpuVuUniformBlockRef m_uniform_block;
  std::vector<PrivateStoreExpectation> private_store_expectations;
  std::vector<FtoiProbeExpectation> ftoi_probe_expectations;
  PrivateArchitecturalStateExpectation
      m_private_architectural_state_expectation;
  GeneratedLoopKernelAttestationIdentity
      m_generated_loop_kernel_attestation;
  std::shared_ptr<GeneratedLoopKernelTransaction>
      m_generated_loop_kernel_transaction;
  std::vector<u16> exact_indices;
  const std::vector<u16>* m_shared_exact_indices = nullptr;
  std::shared_ptr<const void> m_shared_exact_indices_owner;
  u16 m_structured_direct_input_span = std::numeric_limits<u16>::max();
  bool m_private_store_expected_captured = false;
  bool m_private_architectural_state_configured = false;
  bool m_private_architectural_state_compared = false;
  bool m_private_architectural_state_exact = false;
  bool m_private_architectural_state_playable_profile_matches = false;
  u32 m_private_architectural_state_mismatch_lanes = 0u;
  u32 m_private_architectural_state_playable_mismatch_lanes = 0u;
  bool m_validated_for_queue = false;
  bool m_requires_gpu_private_store_output = false;
  GeneratedInputWindow m_generated_input_window{};
  size_t m_generated_input_payload_count = 0u;
  size_t m_generated_input_stream_count = 0u;
  bool m_generated_input_window_valid = false;
  bool m_generated_input_plan_installed = false;
};

// User-declared GXM uniform-buffer addresses persist until explicitly
// replaced. A generated batch must therefore never derive BUFFER2 ownership
// from its first descriptor while admitting a later descriptor with the
// opposite writable-output contract. The store count and comparison policy
// have their own batch checks; this helper names the physical binding
// invariant shared by GS grouping and final GXM preflight.
inline bool HasSamePrivateStoreBufferBinding(
    const GpuVuDraw& left, const GpuVuDraw& right) {
  return left.HasPrivateStoreJournal() == right.HasPrivateStoreJournal();
}

// A generated GXM submission ticket may be cancelled only while the draw is
// still provably pre-effect. Once libGXM has accepted any draw command, even a
// later API error leaves firmware ownership uncertain and the process-lifetime
// notification thread must retain the bounded fail-stop.
inline constexpr bool CanCancelGpuVuPreNotificationWatchdog(
    bool armed_here, bool retained, bool encoded_any) {
  return armed_here && !retained && !encoded_any;
}

u64 NextGpuVuOrderingSequence();

// This becomes true only when CPU0 can retain the immutable VIF epoch, omit
// CpuVU1->Execute(), and queue its GpuVuDraw at the matching PATH1 position.
// Cold shader preparation must not consume MTVU time before that handoff
// exists.
bool IsDirectDrawAdmissionConnected();

// PhyreEngine's GXM resource contract: notification values are monotonically
// increasing modulo 2^32, and any later completed value retires an older one.
bool HasCompletedNotificationValue(u32 completed, u32 required);

// Why one VU1 dispatch could not become a direct GPU draw. These are semantic
// groups derived from the analysis and the invocation's own state, never a
// title, program, hash or instruction-sequence identity.
enum class AdmissionFailure {
  Disconnected,
  NoProgramToken,
  BuildFailed,
  QueueRejected,
  NoInputSpans,
  NoReadyCandidate,
  TagMismatch,
  SeedUnstable,
  GeometryFailed,
  InputResolveFailed,
};
inline constexpr size_t AdmissionFailureCount = 10;

struct DrawStatistics {
  u64 queued = 0;
  u64 consumed = 0;
  u64 rejected = 0;
  u64 generated_parallel_invocations = 0;
  u64 generated_serial_invocations = 0;
  u64 interpreter_invocations = 0;
  u64 fused_vertices = 0;
  u64 fused_primitives = 0;
  u64 tfx_vertex_exports = 0;
  u64 raw_path1_exports = 0;
  u64 retirement_batches = 0;
  u64 retired_draws = 0;
  u64 retirement_ring_waits = 0;
  u64 notification_waits = 0;
  u64 descriptor_pool_waits = 0;
  u64 descriptor_pool_in_use = 0;
  u64 peak_descriptor_pool_in_use = 0;
  u32 descriptor_pool_capacity = 0;
  u32 descriptor_size = 0;
  u64 uniform_pool_waits = 0;
  u64 uniform_pool_in_use = 0;
  u64 peak_uniform_pool_in_use = 0;
  u32 uniform_pool_capacity = 0;
  u32 uniform_block_size = 0;
  u64 live_draws = 0;
  u64 peak_live_draws = 0;
  // Phase accounting: what the CPU still executed and still had to publish.
  u64 cpu_vu1_executions = 0;
  u64 cpu_path1_packets = 0;
  u64 cpu_path1_bytes = 0;
  u64 encoded_objects = 0;
  std::array<u64, 10> admission_failures{};
};

void RecordGpuVuDrawQueued();
void RecordGpuVuDrawConsumed();
void RecordGpuVuDrawRejected();
void RecordCpuVu1Execution(u32 path1_packet_bytes);
void RecordDirectAdmissionFailure(AdmissionFailure reason);
void RecordGpuVuObjectsEncoded(u64 count);
void RecordGpuVuDrawExecuted(const GpuVuDraw &draw);
void RecordGpuVuRetirementBatch();
void RecordGpuVuDrawsRetired(u64 count);
void RecordGpuVuRetirementRingWait();
void RecordGpuVuNotificationWait();
DrawStatistics GetGpuVuDrawStatistics();

} // namespace VitaGpuVu
