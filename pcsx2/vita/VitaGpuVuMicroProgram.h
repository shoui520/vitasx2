// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>
#include <string>

struct VURegs;
namespace VitaVU {
struct GpuPairPlan;
}

namespace VitaGpuVu {

// Fixed, host-neutral input consumed by the universal GPU-VU executor. The
// opcode words retain every operand and immediate. Control is a serialization
// of VitaVU::GpuPairPlan, whose owner is the PCSX2-derived VU block compiler;
// this file never decodes a VU opcode independently.
struct UniversalPairMicroOp {
  u32 upper = 0;
  u32 lower = 0;
  u32 control = 0;
  // PCSX2 VUops.cpp::_VURegsNum dependency metadata, exported by the same
  // PairPlan analysis which feeds the A32 provider.  The fixed GPU executor
  // must not infer stalls or pipeline ownership from an independently grown
  // opcode decoder: these words are the serialized timing contract.
  u32 upper_vi_read = 0;
  u32 upper_vi_write = 0;
  u32 lower_vi_read = 0;
  u32 lower_vi_write = 0;
  // access0: write register, write mask, read-0 register, read-0 mask.
  // access1: read-1 register, read-1 mask, pipe kind, signed cycle latency.
  // The lower pipe byte uses its otherwise-zero upper five bits for the exact
  // VI backup register; control bit 31 says whether even register zero owns a
  // two-cycle branch-visibility window.
  u32 upper_vf_access0 = 0;
  u32 upper_vf_access1 = 0;
  u32 lower_vf_access0 = 0;
  u32 lower_vf_access1 = 0;
};

static_assert(sizeof(UniversalPairMicroOp) == 44);
static_assert(alignof(UniversalPairMicroOp) == alignof(u32));

// Version three serializes PCSX2's complete per-pair dependency, pipe-latency,
// and VI branch-visibility metadata in addition to making every VU1 numeric
// mode which can change PairPlan execution, Q/P arithmetic, or FMAC result
// policy—including Vita's controlled approximation tiers—part of
// configuration_bits. An old cache entry
// must never survive either semantic contract changing merely because the
// micro-memory bytes are unchanged.
inline constexpr u32 UniversalMicroProgramFormatVersion = 3;
inline constexpr u32 UniversalMicroProgramPairBytes = 8;
inline constexpr u32 UniversalMicroProgramPairCount = 2048;
inline constexpr u32 UniversalMicroProgramSourceBytes =
    UniversalMicroProgramPairCount * UniversalMicroProgramPairBytes;

// Immutable execution-policy identity carried beside every encoded source
// image. These bit positions are also consumed by the fixed GXP, so additions
// belong at this ABI boundary even when they do not change the 44-byte record.
inline constexpr u32 UniversalConfigurationAssumeScheduled = 1u << 0;
inline constexpr u32 UniversalConfigurationInstantQp = 1u << 1;
inline constexpr u32 UniversalConfigurationFlagHack = 1u << 2;
inline constexpr u32 UniversalConfigurationVu1Instant = 1u << 3;
inline constexpr u32 UniversalConfigurationOverflowClamp = 1u << 4;
inline constexpr u32 UniversalConfigurationExtraOverflowClamp = 1u << 5;
inline constexpr u32 UniversalConfigurationSignOverflowClamp = 1u << 6;
inline constexpr u32 UniversalConfigurationUnderflowClamp = 1u << 7;
inline constexpr u32 UniversalConfigurationRoundModeShift = 8;
inline constexpr u32 UniversalConfigurationRoundModeMask = 3u << 8;
inline constexpr u32 UniversalConfigurationDenormalsAreZero = 1u << 10;
inline constexpr u32 UniversalConfigurationFlushToZero = 1u << 11;
inline constexpr u32 UniversalConfigurationApproximateQ = 1u << 12;
inline constexpr u32 UniversalConfigurationApproximateP = 1u << 13;
inline constexpr u32 UniversalConfigurationApproximateFmac = 1u << 14;
inline constexpr u32 UniversalConfigurationApproximateConversions = 1u << 15;
inline constexpr u32 UniversalConfigurationKnownMask =
    UniversalConfigurationAssumeScheduled |
    UniversalConfigurationInstantQp |
    UniversalConfigurationFlagHack |
    UniversalConfigurationVu1Instant |
    UniversalConfigurationOverflowClamp |
    UniversalConfigurationExtraOverflowClamp |
    UniversalConfigurationSignOverflowClamp |
    UniversalConfigurationUnderflowClamp |
    UniversalConfigurationRoundModeMask |
    UniversalConfigurationDenormalsAreZero |
    UniversalConfigurationFlushToZero |
    UniversalConfigurationApproximateQ |
    UniversalConfigurationApproximateP |
    UniversalConfigurationApproximateFmac |
    UniversalConfigurationApproximateConversions;

struct UniversalMicroProgram {
  u32 format_version = UniversalMicroProgramFormatVersion;
  u32 configuration_bits = 0;
  u32 start_pc = 0;
  u32 valid_pair_count = 0;
  u32 invalid_pair_count = 0;
  std::array<UniversalPairMicroOp, UniversalMicroProgramPairCount> pairs{};
};

static_assert(UniversalMicroProgramSourceBytes == 16 * 1024);

// Serializes one PCSX2-owned PairPlan into the shared generated/fixed GPU ABI.
// This is the sole metadata packing contract: control-flow planning and both
// GPU code generators use it to attest that their ProgramAnalysis was derived
// from the exact encoded PairPlan rather than merely matching source words.
bool EncodeUniversalPairMicroOp(const VitaVU::GpuPairPlan& plan,
                                UniversalPairMicroOp* encoded,
                                std::string* error = nullptr);

// Encodes the complete 16 KiB VU1 micro-memory image. Undecodable words are
// represented by invalid records rather than rejecting unrelated reachable
// code; execution fails closed if control ever reaches one.
bool EncodeUniversalMicroProgram(const u8* micro, u32 micro_size,
                                 u32 start_pc,
                                 UniversalMicroProgram* program,
                                 std::string* error = nullptr);

// Encodes against a caller-owned immutable configuration snapshot. This is
// the command-epoch form: unlike the convenience entry point above, it never
// reads or mutates live EmuConfig while PairPlan records are being produced.
bool EncodeUniversalMicroProgramForConfiguration(
    const u8* micro, u32 micro_size, u32 start_pc,
    u32 configuration_bits, UniversalMicroProgram* program,
    std::string* error = nullptr);
u32 GetCurrentUniversalMicroProgramConfigurationBits();

struct UniversalMicroProgramCacheStatistics {
  u64 requests = 0;
  u64 hits = 0;
  u64 encodes = 0;
  u64 evictions = 0;
  u64 rejected_inputs = 0;
  u32 resident_programs = 0;
  u32 resident_source_bytes = 0;
  u32 resident_encoded_bytes = 0;
};

class UniversalMicroProgramHandle final {
public:
  UniversalMicroProgramHandle() = default;
  UniversalMicroProgramHandle(const UniversalMicroProgramHandle&) = delete;
  UniversalMicroProgramHandle& operator=(
      const UniversalMicroProgramHandle&) = delete;
  UniversalMicroProgramHandle(UniversalMicroProgramHandle&& other) noexcept;
  UniversalMicroProgramHandle& operator=(
      UniversalMicroProgramHandle&& other) noexcept;
  ~UniversalMicroProgramHandle();

  const UniversalMicroProgram* Get() const;
  // Stable only for this exact cached source/start/configuration instance.
  // It is an optimization key after the cache has performed its full source
  // comparison; it is never a semantic admission key.
  u64 Identity() const;
  const UniversalMicroProgram& operator*() const;
  const UniversalMicroProgram* operator->() const;
  explicit operator bool() const;

private:
  friend UniversalMicroProgramHandle PrepareUniversalMicroProgram(
      const u8*, u32, u32, std::string*);
  friend UniversalMicroProgramHandle PrepareUniversalMicroProgramForConfiguration(
      const u8*, u32, u32, u32, std::string*);
  explicit UniversalMicroProgramHandle(void* entry);
  void* m_entry = nullptr;
};

// Exact source bytes, start PC, format version, and execution configuration
// form the identity. Handles pin an entry across cache eviction without using
// Vita libstdc++'s non-atomic shared_ptr reference count.
UniversalMicroProgramHandle PrepareUniversalMicroProgram(
    const u8* micro, u32 micro_size, u32 start_pc,
    std::string* error = nullptr);
UniversalMicroProgramHandle PrepareUniversalMicroProgramForConfiguration(
    const u8* micro, u32 micro_size, u32 start_pc, u32 configuration_bits,
    std::string* error = nullptr);
void ClearUniversalMicroProgramCache();
UniversalMicroProgramCacheStatistics
GetUniversalMicroProgramCacheStatistics();
void ResetUniversalMicroProgramCacheStatistics();

enum class UniversalReferenceStepResult : u8 {
  PairCompleted,
  ProgramFinished,
  DbitObserver,
  TbitObserver,
  PairLimitReached,
  InvalidEncoding,
};

// Pair-local architectural output event captured from the same serialized
// PairPlan which drives the CPU reference body.  PATH1 remains delayed: this
// records only that the current pair queued XGKICK and the source address read
// by that pair.  The command-epoch transaction decides when the pending packet
// becomes visible and owns complete-packet publication.
struct UniversalReferencePairEvent {
  u32 xgkick_address = 0;
  bool queues_xgkick = false;
};

struct UniversalReferenceRunResult {
  UniversalReferenceStepResult result =
      UniversalReferenceStepResult::InvalidEncoding;
  u32 executed_pairs = 0;
  u32 stop_pc = 0;
  // Qword-level write provenance for the bounded private no-output bridge.
  // Other reference users leave this clear. It lets the generated owner
  // preserve direct canonical-memory bindings for every untouched qword
  // without comparing or copying the 16 KiB VU1 image.
  std::array<u32, 32> written_memory_qwords{};
};

struct UniversalFixedPipelineFmacEntry {
  u64 ready_cycle = 0;
  u8 upper_write = 0;
  u8 upper_mask = 0;
  u8 lower_write = 0;
  u8 lower_mask = 0;
};

struct UniversalFixedPipelineIaluEntry {
  u64 ready_cycle = 0;
  u32 write_mask = 0;
};

// Host model of the pipeline controller implemented by the fixed GXP. It is
// deliberately value-free: PairPlan controls issue/stall timing and the VI
// backup window, while the CPU reference and shader instruction bodies own
// arithmetic values. Complete-program admission begins with empty queues and
// calls FinishUniversalFixedPipeline() at the E-bit observation boundary.
struct UniversalFixedPipelineState {
  u64 cycle = 0;
  std::array<UniversalFixedPipelineFmacEntry, 4> fmac{};
  std::array<UniversalFixedPipelineIaluEntry, 4> ialu{};
  u64 fdiv_ready_cycle = 0;
  u64 efu_ready_cycle = 0;
  u8 fmac_count = 0;
  u8 ialu_count = 0;
  u8 fdiv_enabled = 0;
  u8 efu_enabled = 0;
  u8 vi_backup_cycles = 0;
  u8 vi_backup_register = 0;
};

// Pre-effect support result for one serialized pair against the current fixed
// GXP instruction body.  This is intentionally independent of title, PC,
// source bytes, and cache identity.  Supported means only that the pair's
// metadata, configuration, and instruction bodies are represented by the
// current fixed GXP; complete-epoch state/output admission remains a separate
// and stricter contract.
enum class UniversalFixedPairSupport : u8 {
  Supported,
  UnsupportedConfiguration,
  UnsupportedNumericConfiguration,
  InvalidMetadata,
  UnsupportedUpperBody,
  UnsupportedLowerBody,
  ApproximateQArithmetic,
  Count,
};

UniversalFixedPairSupport ClassifyUniversalFixedPairSupport(
    const UniversalPairMicroOp& op, u32 configuration_bits);
const char* UniversalFixedPairSupportName(UniversalFixedPairSupport support);

bool AdvanceUniversalFixedPipeline(
    const UniversalPairMicroOp& op, u32 configuration_bits,
    UniversalFixedPipelineState* state, std::string* error = nullptr);
void FinishUniversalFixedPipeline(UniversalFixedPipelineState* state);

// Validation oracle for the serialized format. Arithmetic and LSU bodies call
// PCSX2's VUmicroFast known-kind implementations; this function owns only the
// PairPlan-shaped simultaneous-pair, pipeline, branch-delay, and E-bit control
// around them. An enabled D/T interrupt is an observation boundary and is
// returned before the pair mutates state so the CPU owner can replay it.
UniversalReferenceStepResult ExecuteUniversalReferenceStep(
    VURegs* vu, const UniversalMicroProgram& program);
UniversalReferenceRunResult ExecuteUniversalReferenceProgram(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs);
// Explicit-FBRST forms let a serialized command epoch prove D/T boundaries
// without consulting worker-thread globals. The ordinary entry points retain
// PCSX2 provider behavior and supply the current canonical FBRST value.
UniversalReferenceStepResult ExecuteUniversalReferenceStepWithFbrst(
    VURegs* vu, const UniversalMicroProgram& program, u32 fbrst);
UniversalReferenceStepResult ExecuteUniversalReferenceStepWithFbrstAndEvent(
    VURegs* vu, const UniversalMicroProgram& program, u32 fbrst,
    UniversalReferencePairEvent* event);
UniversalReferenceRunResult ExecuteUniversalReferenceProgramWithFbrst(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs,
    u32 fbrst);

// Cached, compiler-derived formula for the small observer-free state-load
// programs which commonly connect two generated loop/output transactions.
// This is deliberately not a VU executor: BuildUniversalStateLoadFormula()
// proves one complete linear PairPlan path once, and the runtime evaluator
// performs only the resulting bounded assignments. It never advances a VU
// pipeline, dispatches an opcode, or executes a guest pair.
enum class UniversalStateLoadFormulaOpKind : u8 {
  Iaddiu,
  Lqi,
};

struct UniversalStateLoadFormulaOp {
  UniversalStateLoadFormulaOpKind kind =
      UniversalStateLoadFormulaOpKind::Iaddiu;
  u8 source_vi = 0;
  u8 destination = 0;
  u8 lane_mask = 0;
  u8 increment_source = 0;
  u16 immediate = 0;
  u16 reserved = 0;
};

inline constexpr u32 UniversalStateLoadFormulaFormatVersion = 1;
inline constexpr u32 UniversalStateLoadFormulaMaximumOps = 32;

struct UniversalStateLoadFormula {
  u32 format_version = UniversalStateLoadFormulaFormatVersion;
  u32 configuration_bits = 0;
  u32 start_pc = 0;
  u32 final_tpc = 0;
  u32 executed_pairs = 0;
  // MTVU starts each Execute with a scheduler-private cycle origin of zero.
  // This is the proved duration of the formula, not an absolute VU timestamp.
  u32 cycle_count = 0;
  u32 operation_count = 0;
  u32 vi_write_mask = 0;
  std::array<UniversalStateLoadFormulaOp,
             UniversalStateLoadFormulaMaximumOps>
      operations{};
};

struct UniversalStateLoadFormulaResult {
  std::array<std::array<u32, 4>, 32> vf_values{};
  std::array<u8, 32> vf_lane_masks{};
  std::array<u16, 16> vi{};
};

// PCSX2 VIF owner: MSCAL/MSCALF replace VU1 TPC with their immediate entry,
// while MSCNT resumes the canonical TPC. A compiled assignment formula has
// already proved its explicit start PC; only a resume is constrained by the
// preceding private generation's terminal TPC.
inline constexpr bool UniversalStateLoadFormulaEntryIsValid(
    bool resume, u32 formula_start_pc, u32 private_tpc) {
  return !resume || formula_start_pc == private_tpc;
}

using UniversalStateLoadReadMemoryWord = bool (*)(
    void* user, u16 qword, u8 lane, u32* value);

bool BuildUniversalStateLoadFormula(
    const UniversalMicroProgram& program, u32 maximum_pairs,
    UniversalStateLoadFormula* formula, std::string* error = nullptr);
bool EvaluateUniversalStateLoadFormula(
    const UniversalStateLoadFormula& formula,
    const std::array<u16, 16>& initial_vi,
    UniversalStateLoadReadMemoryWord read_memory_word, void* memory_user,
    UniversalStateLoadFormulaResult* result, std::string* error = nullptr);

// Executes only a bounded, linear state-load program (NOP/IADDIU/LQI) against
// a caller-owned VU1 image. The complete PairPlan path and terminal E-bit are
// proved before execution; dynamic LQI addresses are checked against the
// caller's unavailable-memory bitmap. Failure leaves *vu untouched and this
// helper never publishes VIF/GIF/MTVU effects. It is a temporary exact bridge
// between already queued generated transactions, not a GPU provider.
bool ExecuteUniversalPrivateStateLoadReference(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs,
    u32 fbrst, const u32* unavailable_memory_words,
    u32 unavailable_memory_word_count, UniversalReferenceRunResult* result,
    std::string* error = nullptr);

// Executes a bounded dynamic path against caller-owned scratch state while
// proving that it has no guest-visible output. D/T observers and XGKICK reject
// before their pair executes. Loads from words still owned by an in-flight GPU
// predecessor also reject, while stores make only their written lanes
// available to later pairs in this same scratch transaction. The caller must
// discard the scratch VU/memory/bitmap on failure; canonical VU/GIF/MTVU state
// is never touched by this helper.
bool ExecuteUniversalPrivateNoOutputReference(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs,
    u32 fbrst, u16 vif_top, u16 vif_itop,
    u32* unavailable_memory_words, u32 unavailable_memory_word_count,
    UniversalReferenceRunResult* result, std::string* error = nullptr);

}  // namespace VitaGpuVu
