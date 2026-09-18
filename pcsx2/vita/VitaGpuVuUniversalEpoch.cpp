// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuUniversalEpoch.h"

#include "vita/VitaGpuVuGeneratedUniversal.h"
#include "vita/VitaGpuVuProgramRegistry.h"

#include "VU.h"
#include "VUmicroFast.h"
#include "common/Console.h"
#include "common/Timer.h"
#include "vita/VitaGpuVuProgram.h"
#include "vita/VitaPerformanceTelemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <tuple>
#include <utility>
#include <vector>

namespace VitaGpuVu {

struct UniversalGpuVuPrivateStructuredResult {
  std::array<u32, 33u * 4u> vf{};
  std::array<u32, UniversalGpuVuStateWordCount> state{};
  std::array<u8, VU1_MEMSIZE> memory{};
  std::array<u32, StructuredGeneratedSnapshotWordCount> snapshots{};
  std::array<u32, StructuredGeneratedOuterStateWords> outer_state{};
  std::array<u32, StructuredGeneratedViSnapshotWordCount> vi_snapshots{};
  std::vector<u32> path1_words;
  u32 path1_packets = 0;
};

namespace {

std::atomic<u64> s_next_sequence{1};
std::atomic<u64> s_prepared{0};
std::atomic<u64> s_preflight_accepted{0};
std::atomic<u64> s_submitted{0};
std::atomic<u64> s_accepted{0};
std::atomic<u64> s_cpu_fallbacks{0};
std::atomic<u64> s_accepted_pairs{0};
std::atomic<u64> s_accepted_path1_packets{0};
std::atomic<u64> s_accepted_path1_qwords{0};
std::atomic<u64> s_rejected_pairs{0};
std::atomic<u64> s_cpu_vu_calls{0};
std::atomic<u64> s_async_epochs_queued{0};
std::atomic<u64> s_async_state_acquired{0};
std::atomic<u64> s_async_pending_max{0};
std::atomic<u64> s_preflight_wall_us{0};
std::atomic<u64> s_program_prepare_wall_us{0};
std::atomic<u64> s_static_preflight_lookup_wall_us{0};
std::atomic<u64> s_epoch_allocation_wall_us{0};
std::atomic<u64> s_control_analysis_wall_us{0};
std::atomic<u64> s_pair_validation_wall_us{0};
std::atomic<u64> s_payload_encode_wall_us{0};
std::atomic<u64> s_mailbox_wait_wall_us{0};
std::atomic<u64> s_notification_wait_wall_us{0};
std::atomic<u64> s_retirement_wait_wall_us{0};
std::atomic<u64> s_cpu_fallback_wall_us{0};
std::atomic<u64> s_cpu_materialize_wall_us{0};
std::atomic<u64> s_cpu_unpack_replay_wall_us{0};
std::atomic<u64> s_cpu_path1_finish_wall_us{0};
std::atomic<u64> s_cpu_completion_publish_wall_us{0};
std::atomic<u64> s_worker_attempt_wall_us{0};
std::atomic<u64> s_path1_retirement_wall_us{0};
std::atomic<u64> s_generated_live_contract_resolutions{0};
std::atomic<u64> s_generated_live_contract_resolution_wall_us{0};
std::atomic<u64> s_generated_live_control_cache_hits{0};
std::atomic<u64> s_generated_live_control_cache_misses{0};
std::atomic<u64> s_generated_descriptor_builds{0};
std::atomic<u64> s_generated_descriptor_build_wall_us{0};
std::atomic<u64> s_generated_descriptor_cache_wall_us{0};
std::atomic<u64> s_generated_descriptor_runtime_proof_wall_us{0};
std::atomic<u64> s_generated_descriptor_input_pack_wall_us{0};
std::atomic<u64> s_generated_descriptor_store_layout_wall_us{0};
std::atomic<u64> s_generated_descriptor_final_state_wall_us{0};
std::atomic<u64> s_generated_descriptor_transaction_wall_us{0};
std::atomic<u64> s_generated_descriptor_transaction_acquire_wall_us{0};
std::atomic<u64> s_generated_descriptor_transaction_capture_wall_us{0};
std::atomic<u64> s_generated_descriptor_transaction_configure_wall_us{0};
std::atomic<u64> s_generated_private_state_advances{0};
std::atomic<u64> s_generated_private_state_advance_wall_us{0};
std::atomic<u64> s_generated_private_same_layout_replacements{0};
std::atomic<u64> s_generated_private_covered_layout_replacements{0};
std::atomic<u64> s_generated_private_covered_owner_slots{0};
std::atomic<u64> s_generated_private_register_owner_replacements{0};
std::atomic<u64> s_generated_private_register_owner_replacement_slots{0};
std::atomic<u64> s_generated_private_register_owner_remaps{0};
std::atomic<u64> s_generated_private_bridge_calls{0};
std::atomic<u64> s_generated_private_bridge_pairs{0};
std::atomic<u64> s_generated_private_bridge_wall_us{0};
std::atomic<u64> s_generated_state_formula_calls{0};
std::atomic<u64> s_generated_state_formula_logical_pairs{0};
std::atomic<u64> s_generated_state_formula_operations{0};
std::atomic<u64> s_generated_state_formula_wall_us{0};
std::atomic<u64> s_generated_hot_execute_calls{0};
std::atomic<u64> s_generated_hot_execute_wall_us{0};
std::atomic<u64> s_generated_queue_calls{0};
std::atomic<u64> s_generated_queue_wall_us{0};
std::atomic<u64> s_generated_gather_calls{0};
std::atomic<u64> s_generated_gather_executes{0};
std::atomic<u64> s_generated_gather_wall_us{0};
std::atomic<u64> s_generated_publication_calls{0};
std::atomic<u64> s_generated_publication_draws{0};
std::atomic<u64> s_generated_publication_wall_us{0};
std::atomic<u64> s_generated_retirement_polls{0};
std::atomic<u64> s_generated_retirement_poll_wall_us{0};
std::atomic<u64> s_generated_mtvu_execute_records{0};
std::atomic<u64> s_generated_mtvu_execute_record_wall_us{0};
std::atomic<u64> s_generated_mtvu_vif_records{0};
std::atomic<u64> s_generated_mtvu_vif_record_wall_us{0};
std::atomic<u64> s_generated_mtvu_other_records{0};
std::atomic<u64> s_generated_mtvu_other_record_wall_us{0};
std::atomic<u64> s_generated_mtvu_housekeeping_calls{0};
std::atomic<u64> s_generated_mtvu_housekeeping_wall_us{0};
std::atomic<u64> s_generated_batch_commits{0};
std::atomic<u64> s_generated_batch_commit_wall_us{0};
std::atomic<u64> s_generated_batch_drain_waits{0};
std::atomic<u64> s_generated_batch_drain_wait_wall_us{0};
std::atomic<u64> s_generated_batch_drain_polls{0};
std::atomic<u64> s_mtvu_execute_queue_samples{0};
std::atomic<u64> s_mtvu_execute_queue_age_us{0};
std::atomic<u64> s_mtvu_execute_queue_age_max_us{0};
std::atomic<u64> s_mtvu_execute_outstanding_max{0};
std::atomic<u64> s_mtvu_queue_used_words_max{0};
std::atomic<u64> s_cpu0_mtvu_wait_wall_us{0};
std::atomic<u64> s_cpu0_mtvu_ring_wait_wall_us{0};
std::atomic<u64> s_cpu0_execute_budget_waits{0};
std::atomic<u64> s_cpu0_execute_budget_wait_wall_us{0};
std::atomic<u64> s_static_preflight_cache_hits{0};
std::atomic<u64> s_static_preflight_cache_misses{0};
std::atomic<u64> s_mtvu_multi_execute_gather_suppressed{0};
std::atomic<u64> s_mtvu_dispatch_cache_hits{0};
std::atomic<u64> s_mtvu_dispatch_cache_misses{0};
std::atomic<u64> s_generated_product_hot_dispatch_hits{0};
std::atomic<u64> s_generated_product_hot_dispatch_misses{0};
std::atomic<u64> s_continuation_groups{0};
std::atomic<u64> s_continuation_submissions{0};
std::atomic<u64> s_universal_provider_epochs{0};
std::atomic<u64> s_universal_provider_jobs{0};
std::atomic<u64> s_generated_provider_epochs{0};
std::atomic<u64> s_generated_provider_jobs{0};
std::atomic<u64> s_gpu_residency_samples{0};
std::atomic<u64> s_gpu_residency_wall_us{0};
std::atomic<u64> s_gpu_residency_wall_us_max{0};
std::atomic<u64> s_generated_lifecycle_reports{0};
std::atomic<bool> s_device_available{false};
std::atomic<bool> s_generated_product_admission_enabled{true};
std::atomic<bool> s_compact_provider_available{false};
std::atomic<u64> s_mtvu_path1_queue_samples{0};
std::atomic<u64> s_mtvu_path1_queue_age_us{0};
std::atomic<u64> s_mtvu_path1_queue_age_max_us{0};
std::array<std::atomic<u64>,
           static_cast<std::size_t>(UniversalGpuVuRejection::Count)>
    s_rejections{};

#if defined(__vita__)
// CPU1 may retain up to the generated transaction bound while CPU2/GXM
// consumes an asynchronous scene.  UniversalGpuVuEpoch is deliberately a
// large, alignment-sensitive ownership record; allocating one per Execute
// from newlib's fixed heap failed after 171 real BSpline GPU transactions.
// Give those records a process-lifetime BSS arena instead.  Construction and
// destruction still happen for each owner, so kernel synchronization members
// retain their normal lifetime contract.
struct alignas(UniversalGpuVuEpoch) UniversalGpuVuEpochPoolSlot final {
  std::array<std::byte, sizeof(UniversalGpuVuEpoch)> storage{};
};
static_assert(sizeof(UniversalGpuVuEpochPoolSlot) ==
              sizeof(UniversalGpuVuEpoch));
std::array<UniversalGpuVuEpochPoolSlot, UniversalGpuVuEpochPoolCapacity>
    s_universal_epoch_pool{};
std::array<std::atomic_bool, UniversalGpuVuEpochPoolCapacity>
    s_universal_epoch_pool_used{};
#endif

struct StaticPreflightCacheEntry {
  bool valid = false;
  // Issued only after the microprogram cache has compared all 16 KiB of
  // source plus entry PC and configuration. This removes a second full-source
  // scan from every Execute without weakening semantic admission.
  u64 program_identity = 0;
  u32 observer_fbrst = 0;
  UniversalGpuVuRejection rejection = UniversalGpuVuRejection::None;
  u32 pair_count = 0;
  bool runtime_path_proof = false;
  bool has_reachable_cycle = false;
  GeneratedUniversalProfile generated_profile;
  ProgramAnalysis analysis;
  bool analysis_available = false;
  u64 last_use = 0;
};

struct StaticPreflightResult {
  UniversalGpuVuRejection rejection = UniversalGpuVuRejection::None;
  u32 pair_count = 0;
  bool runtime_path_proof = false;
  bool has_reachable_cycle = false;
  GeneratedUniversalProfile generated_profile;
};

constexpr u32 StaticPreflightCacheEntries = 8;
Threading::KernelMutex s_static_preflight_cache_mutex;
std::array<StaticPreflightCacheEntry, StaticPreflightCacheEntries>
    s_static_preflight_cache{};
u64 s_static_preflight_cache_clock = 0;

bool LookupStaticPreflight(u64 program_identity, u32 fbrst,
                           StaticPreflightResult* result) {
  std::lock_guard lock(s_static_preflight_cache_mutex);
  for (StaticPreflightCacheEntry& entry : s_static_preflight_cache) {
    if (!entry.valid || entry.program_identity != program_identity ||
        entry.observer_fbrst != (fbrst & 0xc00u)) {
      continue;
    }
    entry.last_use = ++s_static_preflight_cache_clock;
    result->rejection = entry.rejection;
    result->pair_count = entry.pair_count;
    result->runtime_path_proof = entry.runtime_path_proof;
    result->has_reachable_cycle = entry.has_reachable_cycle;
    result->generated_profile = entry.generated_profile;
    s_static_preflight_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  s_static_preflight_cache_misses.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool LookupStaticPreflightAnalysis(u64 program_identity, u32 fbrst,
                                   ProgramAnalysis* analysis) {
  if (!analysis)
    return false;
  std::lock_guard lock(s_static_preflight_cache_mutex);
  for (StaticPreflightCacheEntry& entry : s_static_preflight_cache) {
    if (!entry.valid || !entry.analysis_available ||
        entry.program_identity != program_identity ||
        entry.observer_fbrst != (fbrst & 0xc00u)) {
      continue;
    }
    entry.last_use = ++s_static_preflight_cache_clock;
    *analysis = entry.analysis;
    return true;
  }
  return false;
}

void StoreStaticPreflight(u64 program_identity, u32 fbrst,
                          UniversalGpuVuRejection rejection,
                          u32 pair_count, bool runtime_path_proof,
                          bool has_reachable_cycle,
                          GeneratedUniversalProfile generated_profile = {},
                          const ProgramAnalysis* analysis = nullptr) {
  std::lock_guard lock(s_static_preflight_cache_mutex);
  StaticPreflightCacheEntry* target = nullptr;
  for (StaticPreflightCacheEntry& entry : s_static_preflight_cache) {
    if (!entry.valid) {
      target = &entry;
      break;
    }
    if (!target || entry.last_use < target->last_use)
      target = &entry;
  }
  target->valid = true;
  target->program_identity = program_identity;
  target->observer_fbrst = fbrst & 0xc00u;
  target->rejection = rejection;
  target->pair_count = pair_count;
  target->runtime_path_proof = runtime_path_proof;
  target->has_reachable_cycle = has_reachable_cycle;
  target->generated_profile = generated_profile;
  target->analysis_available = analysis != nullptr;
  target->analysis = analysis ? *analysis : ProgramAnalysis{};
  target->last_use = ++s_static_preflight_cache_clock;
}

u64 ElapsedTelemetryMicroseconds(Common::Timer::Value start) {
  if (start == 0)
    return 0;
  return static_cast<u64>(Common::Timer::ConvertValueToSeconds(
      Common::Timer::GetCurrentValue() - start) * 1000000.0);
}

class ScopedTelemetryDuration final {
public:
  explicit ScopedTelemetryDuration(std::atomic<u64>* destination)
      : m_destination(destination),
        m_start(VitaPerformanceTelemetry::IsEnabled()
                    ? Common::Timer::GetCurrentValue()
                    : 0) {}

  ~ScopedTelemetryDuration() {
    if (m_start != 0) {
      m_destination->fetch_add(
          ElapsedTelemetryMicroseconds(m_start), std::memory_order_relaxed);
    }
  }

private:
  std::atomic<u64>* m_destination;
  Common::Timer::Value m_start;
};

bool Fail(UniversalGpuVuRejection reason, UniversalGpuVuRejection* rejection,
          std::string* error, const char* message) {
  if (rejection)
    *rejection = reason;
  if (error)
    *error = message;
  s_rejections[static_cast<std::size_t>(reason)].fetch_add(
      1, std::memory_order_relaxed);
  return false;
}

UniversalGpuVuRejection PairRejection(UniversalFixedPairSupport support) {
  switch (support) {
  case UniversalFixedPairSupport::UnsupportedConfiguration:
  case UniversalFixedPairSupport::UnsupportedNumericConfiguration:
    return UniversalGpuVuRejection::UnsupportedConfiguration;
  case UniversalFixedPairSupport::InvalidMetadata:
    return UniversalGpuVuRejection::InvalidPairMetadata;
  case UniversalFixedPairSupport::UnsupportedUpperBody:
    return UniversalGpuVuRejection::UnsupportedUpper;
  case UniversalFixedPairSupport::UnsupportedLowerBody:
    return UniversalGpuVuRejection::UnsupportedLower;
  case UniversalFixedPairSupport::ApproximateQArithmetic:
    return UniversalGpuVuRejection::ApproximateQ;
  case UniversalFixedPairSupport::Supported:
  case UniversalFixedPairSupport::Count:
    break;
  }
  return UniversalGpuVuRejection::InvalidPairMetadata;
}

bool HasReachableControlCycle(const ProgramAnalysis& analysis) {
  std::vector<u32> incoming(analysis.blocks.size(), 0);
  u32 reachable_blocks = 0;
  for (const BasicBlock& block : analysis.blocks) {
    if (!block.reachable_from_entry)
      continue;
    reachable_blocks++;
    for (const ControlEdge& edge : block.successors) {
      if (edge.has_target && edge.target_block < analysis.blocks.size() &&
          analysis.blocks[edge.target_block].reachable_from_entry) {
        incoming[edge.target_block]++;
      }
    }
  }

  std::vector<u32> ready;
  ready.reserve(reachable_blocks);
  for (u32 block_index = 0; block_index < analysis.blocks.size();
       block_index++) {
    if (analysis.blocks[block_index].reachable_from_entry &&
        incoming[block_index] == 0) {
      ready.push_back(block_index);
    }
  }

  u32 retired_blocks = 0;
  while (!ready.empty()) {
    const u32 block_index = ready.back();
    ready.pop_back();
    retired_blocks++;
    for (const ControlEdge& edge : analysis.blocks[block_index].successors) {
      if (!edge.has_target || edge.target_block >= analysis.blocks.size() ||
          !analysis.blocks[edge.target_block].reachable_from_entry) {
        continue;
      }
      if (--incoming[edge.target_block] == 0)
        ready.push_back(edge.target_block);
    }
  }
  return retired_blocks != reachable_blocks;
}

u32 DynamicPairUpperBound(const ProgramAnalysis& analysis, u32 pair_count,
                          u32 requested_maximum_pairs) {
  // In an acyclic complete CFG no pair can be visited twice, so the reachable
  // source-pair count is a conservative path bound.  Any reachable cycle or
  // unresolved indirect edge may consume the command's complete runtime
  // budget and must be priced that way before submission.
  return HasReachableControlCycle(analysis) ||
                 analysis.has_unresolved_indirect_control
             ? requested_maximum_pairs
             : pair_count;
}

bool ValidateReachablePairs(const ProgramAnalysis& analysis,
                            const UniversalMicroProgram& program,
                            u32 fbrst, u32* pair_count,
                            bool* has_runtime_path_proof,
                            UniversalGpuVuRejection* rejection,
                            std::string* error) {
  std::bitset<UniversalMicroProgramPairCount> visited;
  bool mbit = false;
  bool enabled_dt = false;
  for (const BasicBlock& block : analysis.blocks) {
    if (!block.reachable_from_entry)
      continue;
    for (const ProgramPair& pair : block.pairs) {
      const u32 index = (pair.plan.pc >> 3) & 0x7ffu;
      visited.set(index);
      mbit |= pair.plan.mflag;
      enabled_dt |= (pair.plan.dflag && (fbrst & 0x400u) != 0) ||
          (pair.plan.tflag && (fbrst & 0x800u) != 0);
    }
  }
  *pair_count = static_cast<u32>(visited.count());
  if (*pair_count == 0)
    return Fail(UniversalGpuVuRejection::InvalidPairMetadata, rejection,
                error, "universal epoch has no reachable VU1 pairs");
  for (const BasicBlock& block : analysis.blocks) {
    if (!block.reachable_from_entry)
      continue;
    for (const ProgramPair& pair : block.pairs) {
      const u32 index = (pair.plan.pc >> 3) & 0x7ffu;
      const UniversalFixedPairSupport support =
          ClassifyUniversalFixedPairSupport(
              program.pairs[index], program.configuration_bits);
      if (support != UniversalFixedPairSupport::Supported) {
        return Fail(PairRejection(support), rejection, error,
                    UniversalFixedPairSupportName(support));
      }
    }
  }
  if (enabled_dt)
    return Fail(UniversalGpuVuRejection::EnabledDtObserver, rejection,
                error, "universal epoch reaches an enabled D/T observer");
  if (mbit)
    return Fail(UniversalGpuVuRejection::MbitObserver, rejection, error,
                "universal epoch reaches an M-bit observer");
  *has_runtime_path_proof = analysis.has_unresolved_indirect_control;
  return true;
}

void AccumulateReachableGeneratedProfile(
    const ProgramAnalysis& analysis, const UniversalMicroProgram& program,
    GeneratedUniversalProfile* profile) {
  std::bitset<UniversalMicroProgramPairCount> visited;
  for (const BasicBlock& block : analysis.blocks) {
    if (!block.reachable_from_entry)
      continue;
    for (const ProgramPair& pair : block.pairs) {
      const u32 index = (pair.plan.pc >> 3) & 0x7ffu;
      if (visited.test(index))
        continue;
      visited.set(index);
      AccumulateGeneratedUniversalProfile(index, program.pairs[index],
                                          profile);
    }
  }
}

}  // namespace

void* UniversalGpuVuEpoch::operator new(std::size_t size) {
#if defined(__vita__)
  if (size != sizeof(UniversalGpuVuEpoch))
    throw std::bad_alloc();
  for (u32 index = 0u; index < UniversalGpuVuEpochPoolCapacity; index++) {
    bool expected = false;
    if (s_universal_epoch_pool_used[index].compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      return s_universal_epoch_pool[index].storage.data();
    }
  }
  throw std::bad_alloc();
#else
  return ::operator new(size);
#endif
}

void UniversalGpuVuEpoch::operator delete(void* pointer) noexcept {
  if (!pointer)
    return;
#if defined(__vita__)
  const uptr base = reinterpret_cast<uptr>(s_universal_epoch_pool.data());
  const uptr address = reinterpret_cast<uptr>(pointer);
  const size_t slot_size = sizeof(UniversalGpuVuEpochPoolSlot);
  const size_t pool_size = slot_size * s_universal_epoch_pool.size();
  if (address < base || address >= base + pool_size ||
      ((address - base) % slot_size) != 0u) {
    return;
  }
  const size_t index = (address - base) / slot_size;
  s_universal_epoch_pool_used[index].store(false, std::memory_order_release);
#else
  ::operator delete(pointer);
#endif
}

const char* UniversalGpuVuRejectionName(UniversalGpuVuRejection rejection) {
  switch (rejection) {
  case UniversalGpuVuRejection::None: return "none";
  case UniversalGpuVuRejection::InvalidRequest: return "invalid-request";
  case UniversalGpuVuRejection::ProgramEncoding: return "program-encoding";
  case UniversalGpuVuRejection::IncompleteControlFlow: return "incomplete-control-flow";
  case UniversalGpuVuRejection::BranchInDelaySlot: return "branch-in-delay-slot";
  case UniversalGpuVuRejection::EnabledDtObserver: return "enabled-dt-observer";
  case UniversalGpuVuRejection::MbitObserver: return "m-bit-observer";
  case UniversalGpuVuRejection::UnsupportedConfiguration: return "unsupported-configuration";
  case UniversalGpuVuRejection::InvalidPairMetadata: return "invalid-pair-metadata";
  case UniversalGpuVuRejection::UnsupportedUpper: return "unsupported-upper";
  case UniversalGpuVuRejection::UnsupportedLower: return "unsupported-lower";
  case UniversalGpuVuRejection::ApproximateQ: return "unsupported-approximate-q";
  case UniversalGpuVuRejection::UnsupportedVifUnpack: return "unsupported-vif-unpack";
  case UniversalGpuVuRejection::CommandCapacity: return "command-capacity";
  case UniversalGpuVuRejection::PayloadGeneration: return "payload-generation";
  case UniversalGpuVuRejection::PayloadCapacity: return "payload-capacity";
  case UniversalGpuVuRejection::InputUnavailable: return "input-unavailable";
  case UniversalGpuVuRejection::StructuredBundleQuarantined:
    return "structured-bundle-quarantined";
  case UniversalGpuVuRejection::GeneratedArchitectureQuarantined:
    return "generated-architecture-quarantined";
  case UniversalGpuVuRejection::WatchdogWorkBudget: return "watchdog-work-budget";
  case UniversalGpuVuRejection::RuntimeInvalidPair: return "runtime-invalid-pair";
  case UniversalGpuVuRejection::RuntimePairBudget: return "runtime-pair-budget";
  case UniversalGpuVuRejection::RuntimeInvalidPath1: return "runtime-invalid-path1";
  case UniversalGpuVuRejection::RuntimeOutputCapacity: return "runtime-output-capacity";
  case UniversalGpuVuRejection::RuntimeTerminalXgkick: return "runtime-terminal-xgkick";
  case UniversalGpuVuRejection::RuntimeStructuredPreflight: return "runtime-structured-preflight";
  case UniversalGpuVuRejection::RuntimeStructuredAttestation: return "runtime-structured-attestation";
  case UniversalGpuVuRejection::RuntimePredecessorFailed: return "runtime-predecessor-failed";
  case UniversalGpuVuRejection::GeneratedProgramPending: return "generated-program-pending";
  case UniversalGpuVuRejection::GeneratedProgramUnavailable: return "generated-program-unavailable";
  case UniversalGpuVuRejection::GeneratedProgramUnprofitable: return "generated-program-unprofitable";
  case UniversalGpuVuRejection::SynchronousDispatchCost: return "synchronous-dispatch-cost";
  case UniversalGpuVuRejection::EpochPoolCapacity: return "epoch-pool-capacity";
  case UniversalGpuVuRejection::DeviceUnavailable: return "device-unavailable";
  case UniversalGpuVuRejection::SubmissionFailed: return "submission-failed";
  case UniversalGpuVuRejection::Count: break;
  }
  return "invalid";
}

const char* UniversalGpuVuOutputRouteName(UniversalGpuVuOutputRoute route) {
  switch (route) {
  case UniversalGpuVuOutputRoute::RawPath1: return "raw-path1";
  case UniversalGpuVuOutputRoute::TfxVertex: return "tfx-vertex";
  case UniversalGpuVuOutputRoute::DirectVuTfx: return "direct-vu-tfx";
  }
  return "invalid";
}

const char* UniversalGpuVuStructuredBoundaryComponentName(
    UniversalGpuVuStructuredBoundaryComponent component) {
  switch (component) {
  case UniversalGpuVuStructuredBoundaryComponent::None: return "none";
  case UniversalGpuVuStructuredBoundaryComponent::OuterCount:
    return "outer-count";
  case UniversalGpuVuStructuredBoundaryComponent::Vf: return "vf";
  case UniversalGpuVuStructuredBoundaryComponent::Acc: return "acc";
  case UniversalGpuVuStructuredBoundaryComponent::Vi: return "vi";
  case UniversalGpuVuStructuredBoundaryComponent::Q: return "q";
  case UniversalGpuVuStructuredBoundaryComponent::P: return "p";
  case UniversalGpuVuStructuredBoundaryComponent::I: return "i";
  }
  return "invalid";
}

UniversalGpuVuStructuredBoundaryComparison
CompareStructuredGeneratedBoundarySnapshots(
    const StructuredGeneratedBundle& bundle, const u32* snapshot_words,
    const u32* outer_state_words, const u32* vi_snapshot_words,
    const VitaVU::Vu1StructuredBoundaryTrace& cpu_trace) {
  UniversalGpuVuStructuredBoundaryComparison comparison;
  comparison.parent_entry_pc = bundle.entry_pc;
  comparison.child_entry_pc = bundle.child_entry_pc;
  comparison.cpu_parent_observations = cpu_trace.parent_observations;
  comparison.cpu_executed_pairs = cpu_trace.executed_pairs;

  if (!snapshot_words || !outer_state_words || !vi_snapshot_words ||
      cpu_trace.dropped_snapshots != 0 ||
      cpu_trace.parent_entry_pc != bundle.entry_pc ||
      cpu_trace.child_entry_pc != bundle.child_entry_pc ||
      cpu_trace.snapshots.size() >
          VitaVU::Vu1StructuredBoundaryMaximumSnapshots) {
    return comparison;
  }

  comparison.gpu_outer_iterations = outer_state_words[0];
  comparison.cpu_outer_iterations =
      static_cast<u32>(cpu_trace.snapshots.size());
  if (comparison.gpu_outer_iterations >
          VitaVU::Vu1StructuredBoundaryMaximumSnapshots ||
      comparison.gpu_outer_iterations > bundle.maximum_outer_iterations) {
    return comparison;
  }

  comparison.available = true;
  const auto mismatch = [&](u32 outer,
                            UniversalGpuVuStructuredBoundaryComponent component,
                            u32 reg, u32 lane, u32 gpu_word,
                            u32 cpu_word) {
    comparison.first_outer_iteration = outer;
    comparison.component = component;
    comparison.register_index = reg;
    comparison.lane = lane;
    comparison.gpu_word = gpu_word;
    comparison.cpu_word = cpu_word;
  };

  if (comparison.gpu_outer_iterations != comparison.cpu_outer_iterations) {
    mismatch(0, UniversalGpuVuStructuredBoundaryComponent::OuterCount, 0, 0,
             comparison.gpu_outer_iterations,
             comparison.cpu_outer_iterations);
    return comparison;
  }

  constexpr u32 snapshot_vectors_per_outer = 36;
  constexpr u32 vi_words_per_outer = 16;
  const auto capture_prior_vector = [&](u32 outer, u32 gpu_vector,
                                        u32 cpu_vector) {
    if (outer == 0)
      return;
    const u32 gpu_base =
        ((outer - 1u) * snapshot_vectors_per_outer + gpu_vector) * 4u;
    const VitaVU::Vu1StructuredBoundarySnapshot& prior_cpu =
        cpu_trace.snapshots[outer - 1u];
    for (u32 prior_lane = 0; prior_lane < 4; prior_lane++) {
      comparison.prior_gpu_vector[prior_lane] =
          snapshot_words[gpu_base + prior_lane];
      comparison.prior_cpu_vector[prior_lane] =
          prior_cpu.vf[cpu_vector * 4u + prior_lane];
    }
    comparison.prior_vector_available = true;
  };
  for (u32 outer = 0; outer < comparison.gpu_outer_iterations; outer++) {
    const VitaVU::Vu1StructuredBoundarySnapshot& cpu =
        cpu_trace.snapshots[outer];
    if (cpu.outer_iteration != outer) {
      mismatch(outer,
               UniversalGpuVuStructuredBoundaryComponent::OuterCount, 0, 0,
               outer, cpu.outer_iteration);
      return comparison;
    }

    for (u32 reg = 0; reg < bundle.child_entry_vf_lanes.size(); reg++) {
      const u8 lanes = bundle.child_entry_vf_lanes[reg];
      for (u32 lane = 0; lane < 4; lane++) {
        if ((lanes & (0x8u >> lane)) == 0)
          continue;
        const u32 gpu = snapshot_words[
            ((outer * snapshot_vectors_per_outer + reg) * 4u) + lane];
        const u32 expected = cpu.vf[reg * 4u + lane];
        if (gpu != expected) {
          mismatch(outer, UniversalGpuVuStructuredBoundaryComponent::Vf,
                   reg, lane, gpu, expected);
          capture_prior_vector(outer, reg, reg);
          return comparison;
        }
      }
    }

    for (u32 lane = 0; lane < 4; lane++) {
      if ((bundle.child_entry_acc_lanes & (0x8u >> lane)) == 0)
        continue;
      const u32 gpu = snapshot_words[
          (outer * snapshot_vectors_per_outer * 4u) + lane];
      const u32 expected = cpu.vf[32u * 4u + lane];
      if (gpu != expected) {
        mismatch(outer, UniversalGpuVuStructuredBoundaryComponent::Acc, 0,
                 lane, gpu, expected);
        capture_prior_vector(outer, 0, 32);
        return comparison;
      }
    }

    for (u32 reg = 0; reg < 16; reg++) {
      if ((bundle.child_entry_vi_mask & (1u << reg)) == 0)
        continue;
      const u32 gpu = vi_snapshot_words[outer * vi_words_per_outer + reg];
      const u32 expected = cpu.vi[reg];
      if (gpu != expected) {
        mismatch(outer, UniversalGpuVuStructuredBoundaryComponent::Vi, reg,
                 0, gpu, expected);
        return comparison;
      }
    }

    const u32 scalar_base =
        (outer * snapshot_vectors_per_outer + 32u) * 4u;
    const std::array<std::tuple<bool,
                                UniversalGpuVuStructuredBoundaryComponent,
                                u32, u32>,
                     3>
        scalars = {{
            {bundle.child_entry_q,
             UniversalGpuVuStructuredBoundaryComponent::Q,
             snapshot_words[scalar_base], cpu.q},
            {bundle.child_entry_p,
             UniversalGpuVuStructuredBoundaryComponent::P,
             snapshot_words[scalar_base + 1u], cpu.p},
            {bundle.child_entry_i,
             UniversalGpuVuStructuredBoundaryComponent::I,
             snapshot_words[scalar_base + 2u], cpu.i},
        }};
    for (const auto& [required, component, gpu, expected] : scalars) {
      if (required && gpu != expected) {
        mismatch(outer, component, 0, 0, gpu, expected);
        return comparison;
      }
    }
  }

  comparison.exact = true;
  return comparison;
}

void SetUniversalGpuVuDeviceAvailable(bool available) {
  s_device_available.store(available, std::memory_order_release);
}

bool IsUniversalGpuVuDeviceAvailable() {
  return s_device_available.load(std::memory_order_acquire);
}

void SetGeneratedLoopKernelProductAdmissionEnabled(bool enabled) {
  s_generated_product_admission_enabled.store(enabled,
                                               std::memory_order_release);
}

bool IsGeneratedLoopKernelProductAdmissionEnabled() {
#if defined(__vita__) && defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
    VITASX2_GPU_VU_DIRECT_ADMISSION && \
    defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
  // The Vita product selects only the one-root, static-flow loop-kernel JIT.
  // The fixed/serial interpreter, GeneratedNestedDirect snapshot pipeline,
  // and compiler-module job graph remain quarantined. CPU MTVU owns cold,
  // pending, unsupported, and unprofitable epochs before effects.
  return s_generated_product_admission_enabled.load(
      std::memory_order_acquire);
#else
  // Host validation enables individual builders explicitly and must not
  // silently turn this product policy on.
  return false;
#endif
}

void SetUniversalGpuVuCompactProviderAvailable(bool available) {
  s_compact_provider_available.store(available, std::memory_order_release);
}

bool IsUniversalGpuVuCompactProviderAvailable() {
  return s_compact_provider_available.load(std::memory_order_acquire);
}

UniversalGpuVuEpoch::~UniversalGpuVuEpoch() {
  for (u32 index = 0u; index < m_unpack_count; index++)
    ReleaseRawVifPayload(&m_unpacks[index].payload);
}

bool UniversalGpuVuEpoch::CanUseIndependentUnpackSubmission(
    u32 submission) const {
  for (const VifUnpackSpan& unpack : Unpacks()) {
    const u32 submissions =
        (unpack.vector_count + UniversalGpuVuUnpackVectorsPerSubmission - 1u) /
        UniversalGpuVuUnpackVectorsPerSubmission;
    if (submission < submissions)
      return IsIndependentUniversalVifUnpackSupported(unpack);
    submission -= submissions;
  }
  return false;
}

u32 UniversalGpuVuEpoch::IndependentUnpackSubmissionCount() const {
  u32 result = 0;
  for (u32 submission = 0; submission < m_unpack_submission_count;
       submission++) {
    result += CanUseIndependentUnpackSubmission(submission) ? 1u : 0u;
  }
  return result;
}

bool UniversalGpuVuEpoch::SetSubmissionNotification(
    uptr address, u32 value, u32 start_value, u32 job_base, u32 job_count) {
  if (Stage() != UniversalGpuVuEpochStage::Prepared || address == 0 ||
      value == 0 || job_count == 0) {
    return false;
  }
	m_submission_notification_start_value.store(start_value,
	                                             std::memory_order_relaxed);
	m_submission_job_base.store(job_base, std::memory_order_relaxed);
	m_submission_job_count.store(job_count, std::memory_order_relaxed);
	m_submission_notification_address.store(address, std::memory_order_relaxed);
	m_submission_notification_value.store(value, std::memory_order_relaxed);
  return true;
}

bool UniversalGpuVuEpoch::SetContinuationSubmissionNotification(
    uptr address, u32 value, u32 start_value, u32 job_base, u32 job_count) {
  if (Stage() != UniversalGpuVuEpochStage::Submitted || address == 0 ||
      value == 0 || job_count == 0) {
    return false;
  }
	m_submission_notification_start_value.store(start_value,
	                                             std::memory_order_relaxed);
	m_submission_job_base.store(job_base, std::memory_order_relaxed);
	m_submission_job_count.store(job_count, std::memory_order_relaxed);
  m_submission_notification_address.store(address, std::memory_order_relaxed);
  m_submission_notification_value.store(value, std::memory_order_release);
  m_stage_progress.NotifyOfProgress();
  return true;
}

void UniversalGpuVuEpoch::SetRejectedPath1Diagnostic(
    u32 address, const std::array<u32, 4>& tag) {
  if (Stage() != UniversalGpuVuEpochStage::Submitted)
    return;
  m_rejected_path1_address = address & 0x3ff0u;
  m_rejected_path1_tag = tag;
}

bool UniversalGpuVuEpoch::CapturePrivateStructuredResult(
    const u32* vf_words, const u32* state_words, const u8* vu_memory,
    const UniversalRawPath1Export* raw_path1,
    const u32* structured_snapshot_words,
    const u32* structured_outer_state_words,
    const u32* structured_vi_snapshot_words) {
  if (Stage() != UniversalGpuVuEpochStage::Submitted || !vf_words ||
      !state_words || !vu_memory || !raw_path1 ||
      !structured_snapshot_words || !structured_outer_state_words ||
      !structured_vi_snapshot_words ||
      raw_path1->format_version != UniversalRawPath1ExportFormatVersion ||
      raw_path1->packet_count > UniversalRawPath1ExportMaximumPackets ||
      raw_path1->data_qword_count > UniversalRawPath1ExportDataQwords) {
    return false;
  }

  auto result = std::make_unique<UniversalGpuVuPrivateStructuredResult>();
  std::copy_n(vf_words, result->vf.size(), result->vf.begin());
  std::copy_n(state_words, result->state.size(), result->state.begin());
  std::copy_n(vu_memory, result->memory.size(), result->memory.begin());
  std::copy_n(structured_snapshot_words, result->snapshots.size(),
              result->snapshots.begin());
  std::copy_n(structured_outer_state_words, result->outer_state.size(),
              result->outer_state.begin());
  std::copy_n(structured_vi_snapshot_words, result->vi_snapshots.size(),
              result->vi_snapshots.begin());
  result->path1_packets = raw_path1->packet_count;
  result->path1_words.reserve(
      static_cast<std::size_t>(raw_path1->data_qword_count) * 4u);
  for (u32 packet_index = 0; packet_index < raw_path1->packet_count;
       packet_index++) {
    const UniversalRawPath1PacketDescriptor& packet =
        raw_path1->packets[packet_index];
    if (packet.qword_count == 0 ||
        packet.output_qword_offset > raw_path1->data_qword_count ||
        packet.qword_count >
            raw_path1->data_qword_count - packet.output_qword_offset) {
      return false;
    }
    const u32* const begin =
        raw_path1->data[packet.output_qword_offset].data();
    result->path1_words.insert(
        result->path1_words.end(), begin,
        begin + static_cast<std::size_t>(packet.qword_count) * 4u);
  }
  if (result->path1_words.size() !=
      static_cast<std::size_t>(raw_path1->data_qword_count) * 4u) {
    return false;
  }
  m_private_structured_result = std::move(result);
  return true;
}

UniversalGpuVuStructuredComparison
UniversalGpuVuEpoch::ComparePrivateStructuredResult(
    const VURegs* cpu_vu, const u8* cpu_path1, u32 cpu_path1_bytes) const {
  UniversalGpuVuStructuredComparison comparison;
  if (!m_private_structured_result || !cpu_vu || !cpu_vu->Mem ||
      (cpu_path1_bytes != 0 && !cpu_path1)) {
    return comparison;
  }

  const UniversalGpuVuPrivateStructuredResult& gpu =
      *m_private_structured_result;
  comparison.available = true;
  comparison.gpu_path1_packets = gpu.path1_packets;
  comparison.gpu_path1_qwords =
      static_cast<u32>(gpu.path1_words.size() / 4u);
  comparison.cpu_path1_bytes = cpu_path1_bytes;
  comparison.gpu_executed_pairs = gpu.state[27];

  std::array<u32, 33u * 4u> cpu_vf{};
  std::memcpy(cpu_vf.data(), &cpu_vu->VF[0].UL[0],
              32u * 4u * sizeof(u32));
  std::memcpy(cpu_vf.data() + 32u * 4u, &cpu_vu->ACC.UL[0],
              4u * sizeof(u32));
  comparison.vf_exact = gpu.vf == cpu_vf;
  if (!comparison.vf_exact) {
    for (u32 word = 0; word < gpu.vf.size(); word++) {
      if (gpu.vf[word] == cpu_vf[word])
        continue;
      comparison.first_vf_word = word;
      comparison.gpu_vf_word = gpu.vf[word];
      comparison.cpu_vf_word = cpu_vf[word];
      break;
    }
  }

  const std::array<std::pair<u32, u32>, 23> state_contract = {{
      {0u, cpu_vu->VI[REG_TPC].UL << 3},
      {9u, cpu_vu->VI[1].UL & 0xffffu},
      {10u, cpu_vu->VI[2].UL & 0xffffu},
      {11u, cpu_vu->VI[3].UL & 0xffffu},
      {12u, cpu_vu->VI[4].UL & 0xffffu},
      {13u, cpu_vu->VI[5].UL & 0xffffu},
      {14u, cpu_vu->VI[6].UL & 0xffffu},
      {15u, cpu_vu->VI[7].UL & 0xffffu},
      {16u, cpu_vu->VI[8].UL & 0xffffu},
      {17u, cpu_vu->VI[9].UL & 0xffffu},
      {18u, cpu_vu->VI[10].UL & 0xffffu},
      {19u, cpu_vu->VI[11].UL & 0xffffu},
      {20u, cpu_vu->VI[12].UL & 0xffffu},
      {21u, cpu_vu->VI[13].UL & 0xffffu},
      {22u, cpu_vu->VI[14].UL & 0xffffu},
      {23u, cpu_vu->VI[15].UL & 0xffffu},
      {24u, cpu_vu->VI[REG_I].UL},
      {25u, cpu_vu->VI[REG_Q].UL},
      {26u, cpu_vu->VI[REG_P].UL},
      {42u, cpu_vu->macflag & 0xffffu},
      {43u, cpu_vu->statusflag & 0x0fffu},
      {45u, cpu_vu->clipflag & 0x00ffffffu},
      {46u, static_cast<u32>(cpu_vu->cycle)},
  }};
  comparison.state_exact = true;
  for (const auto& [word, cpu_value] : state_contract) {
    if (gpu.state[word] == cpu_value)
      continue;
    comparison.state_exact = false;
    comparison.first_state_word = word;
    comparison.gpu_state_word = gpu.state[word];
    comparison.cpu_state_word = cpu_value;
    break;
  }

  comparison.memory_exact =
      std::memcmp(gpu.memory.data(), cpu_vu->Mem, VU1_MEMSIZE) == 0;
  if (!comparison.memory_exact) {
    for (u32 word = 0; word < VU1_MEMSIZE / sizeof(u32); word++) {
      u32 gpu_word = 0;
      u32 cpu_word = 0;
      std::memcpy(&gpu_word, gpu.memory.data() + word * sizeof(u32),
                  sizeof(gpu_word));
      std::memcpy(&cpu_word, cpu_vu->Mem + word * sizeof(u32),
                  sizeof(cpu_word));
      if (gpu_word == cpu_word)
        continue;
      comparison.first_memory_word = word;
      comparison.gpu_memory_word = gpu_word;
      comparison.cpu_memory_word = cpu_word;
      break;
    }
  }

  const std::size_t cpu_path1_words = cpu_path1_bytes / sizeof(u32);
  comparison.path1_exact = (cpu_path1_bytes % 16u) == 0u &&
      cpu_path1_words == gpu.path1_words.size() &&
      (cpu_path1_words == 0 ||
       std::memcmp(gpu.path1_words.data(), cpu_path1,
                   cpu_path1_words * sizeof(u32)) == 0);
  if (!comparison.path1_exact) {
    const std::size_t common_words =
        std::min(cpu_path1_words, gpu.path1_words.size());
    for (std::size_t word = 0; word < common_words; word++) {
      u32 cpu_word = 0;
      std::memcpy(&cpu_word, cpu_path1 + word * sizeof(u32),
                  sizeof(cpu_word));
      if (gpu.path1_words[word] == cpu_word)
        continue;
      comparison.first_path1_word = static_cast<u32>(word);
      comparison.gpu_path1_word = gpu.path1_words[word];
      comparison.cpu_path1_word = cpu_word;
      break;
    }
    if (comparison.first_path1_word == std::numeric_limits<u32>::max() &&
        cpu_path1_words != gpu.path1_words.size()) {
      comparison.first_path1_word = static_cast<u32>(common_words);
      if (common_words < gpu.path1_words.size())
        comparison.gpu_path1_word = gpu.path1_words[common_words];
      if (common_words < cpu_path1_words) {
        std::memcpy(&comparison.cpu_path1_word,
                    cpu_path1 + common_words * sizeof(u32),
                    sizeof(comparison.cpu_path1_word));
      }
    }
  }
  return comparison;
}

UniversalGpuVuStructuredBoundaryComparison
UniversalGpuVuEpoch::ComparePrivateStructuredBoundaryTrace(
    const VitaVU::Vu1StructuredBoundaryTrace& cpu_trace) const {
  if (!m_private_structured_result)
    return {};
  const UniversalGpuVuPrivateStructuredResult& gpu =
      *m_private_structured_result;
  return CompareStructuredGeneratedBoundarySnapshots(
      m_structured_bundle, gpu.snapshots.data(), gpu.outer_state.data(),
      gpu.vi_snapshots.data(), cpu_trace);
}

bool UniversalGpuVuEpoch::MarkSubmitted() {
	if (SubmissionNotificationAddress() == 0 ||
		SubmissionNotificationValue() == 0) {
#if !defined(VITASX2_QEMU_VALIDATION)
    return false;
#endif
  }
  UniversalGpuVuEpochStage expected = UniversalGpuVuEpochStage::Prepared;
  if (!m_stage.compare_exchange_strong(
          expected, UniversalGpuVuEpochStage::Completing,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    return false;
  }
  m_completion.stage = UniversalGpuVuEpochStage::Submitted;
  m_stage.store(
      UniversalGpuVuEpochStage::Submitted, std::memory_order_release);
  m_stage_progress.NotifyOfProgress();
  s_submitted.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool UniversalGpuVuEpoch::MarkAccepted(
    u32 final_tpc, u32 executed_pairs, u32 output_packet_count,
    u32 output_qword_count, u32 interrupt_flags, u32 final_cycle,
    const UniversalRawPath1Export* raw_path1,
    UniversalGpuVuCommittedStateView committed_state) {
  UniversalGpuVuEpochStage expected = UniversalGpuVuEpochStage::Submitted;
  if (!m_stage.compare_exchange_strong(
          expected, UniversalGpuVuEpochStage::Completing,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    return false;
  }
  m_completion.final_tpc = final_tpc;
  m_completion.executed_pairs = executed_pairs;
  m_completion.output_packet_count = output_packet_count;
  m_completion.output_qword_count = output_qword_count;
  m_completion.interrupt_flags = interrupt_flags;
  m_completion.final_cycle = final_cycle;
  m_raw_path1_output = raw_path1;
  m_committed_state = committed_state;
  m_completion.rejection = UniversalGpuVuRejection::None;
  m_completion.stage = UniversalGpuVuEpochStage::Accepted;
  m_stage.store(
      UniversalGpuVuEpochStage::Accepted, std::memory_order_release);
  m_stage_progress.NotifyOfProgress();
  s_accepted.fetch_add(1, std::memory_order_relaxed);
  s_accepted_pairs.fetch_add(executed_pairs, std::memory_order_relaxed);
  s_accepted_path1_packets.fetch_add(output_packet_count,
                                     std::memory_order_relaxed);
  s_accepted_path1_qwords.fetch_add(output_qword_count,
                                    std::memory_order_relaxed);
  return true;
}

bool UniversalGpuVuEpoch::MarkAcceptedStateAcquired() {
  if (Stage() != UniversalGpuVuEpochStage::Accepted ||
      !m_committed_state.IsValid()) {
    return false;
  }
  bool expected = false;
  if (!m_accepted_state_acquired.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  s_async_state_acquired.fetch_add(1, std::memory_order_relaxed);
  m_stage_progress.NotifyOfProgress();
  return true;
}

bool UniversalGpuVuEpoch::MarkGpuRejected(
    UniversalGpuVuRejection rejection, u32 executed_pairs) {
  if (rejection == UniversalGpuVuRejection::None ||
      rejection == UniversalGpuVuRejection::Count) {
    return false;
  }
  UniversalGpuVuEpochStage expected = Stage();
  while (expected == UniversalGpuVuEpochStage::Prepared ||
         expected == UniversalGpuVuEpochStage::Submitted) {
    if (m_stage.compare_exchange_weak(
            expected, UniversalGpuVuEpochStage::Completing,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      m_rejection.store(rejection, std::memory_order_release);
      m_completion.stage = UniversalGpuVuEpochStage::GpuRejected;
      m_completion.rejection = rejection;
      m_completion.executed_pairs = executed_pairs;
      m_stage.store(
          UniversalGpuVuEpochStage::GpuRejected, std::memory_order_release);
      m_stage_progress.NotifyOfProgress();
      return true;
    }
  }
  return false;
}

bool UniversalGpuVuEpoch::MarkCpuFallback(
    UniversalGpuVuRejection rejection, u32 executed_pairs) {
  UniversalGpuVuEpochStage expected = Stage();
  while (expected == UniversalGpuVuEpochStage::Prepared ||
         expected == UniversalGpuVuEpochStage::GpuRejected) {
    if (m_stage.compare_exchange_weak(
            expected, UniversalGpuVuEpochStage::Completing,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      m_rejection.store(rejection, std::memory_order_release);
      m_completion.stage = UniversalGpuVuEpochStage::CpuFallback;
      m_completion.rejection = rejection;
      m_completion.executed_pairs = executed_pairs;
      m_stage.store(
          UniversalGpuVuEpochStage::CpuFallback,
          std::memory_order_release);
      m_stage_progress.NotifyOfProgress();
      s_cpu_fallbacks.fetch_add(1, std::memory_order_relaxed);
      s_rejected_pairs.fetch_add(executed_pairs, std::memory_order_relaxed);
      s_rejections[static_cast<std::size_t>(rejection)].fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

bool UniversalGpuVuEpoch::MarkRetired() {
  UniversalGpuVuEpochStage expected = Stage();
  while (expected == UniversalGpuVuEpochStage::Accepted ||
         expected == UniversalGpuVuEpochStage::CpuFallback) {
    if (expected == UniversalGpuVuEpochStage::Accepted &&
        !AcceptedStateAcquired()) {
      return false;
    }
    if (m_stage.compare_exchange_weak(
            expected, UniversalGpuVuEpochStage::Retired,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      m_stage_progress.NotifyOfProgress();
      return true;
    }
  }
  return false;
}

void UniversalGpuVuEpoch::Cancel() {
  UniversalGpuVuEpochStage expected = Stage();
  while (expected == UniversalGpuVuEpochStage::Prepared ||
         expected == UniversalGpuVuEpochStage::Submitted ||
         expected == UniversalGpuVuEpochStage::GpuRejected) {
    if (m_stage.compare_exchange_weak(
            expected, UniversalGpuVuEpochStage::Completing,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      m_completion.stage = UniversalGpuVuEpochStage::Cancelled;
      m_stage.store(
          UniversalGpuVuEpochStage::Cancelled,
          std::memory_order_release);
      m_stage_progress.NotifyOfProgress();
      return;
    }
  }
}

std::unique_ptr<UniversalGpuVuEpoch> PrepareUniversalGpuVuEpoch(
    const UniversalGpuVuEpochBuildRequest& request,
    UniversalGpuVuRejection* rejection, std::string* error,
    u32* classified_pair_count, u32* dynamic_pair_upper_bound) {
  ScopedTelemetryDuration preflight_duration(&s_preflight_wall_us);
  s_prepared.fetch_add(1, std::memory_order_relaxed);
  if (classified_pair_count)
    *classified_pair_count = 0;
  if (dynamic_pair_upper_bound)
    *dynamic_pair_upper_bound = 0;
  if (rejection)
    *rejection = UniversalGpuVuRejection::None;
  if (error)
    error->clear();
  if (!request.micro || request.micro_size != UniversalMicroProgramSourceBytes ||
      request.maximum_pairs == 0 ||
      request.maximum_pairs > UniversalCommandEpochMaximumPairsPerExecute ||
      (!request.resume &&
       ((request.start_pc & 7u) != 0 || request.start_pc > 0x3ff8u)) ||
      (request.resume &&
       ((request.current_tpc & 7u) != 0 || request.current_tpc > 0x3ff8u)) ||
      (request.configuration_bits & ~UniversalConfigurationKnownMask) != 0) {
    Fail(UniversalGpuVuRejection::InvalidRequest, rejection, error,
         "invalid universal GPU-VU epoch request");
    return nullptr;
  }

  const u32 entry_pc = request.resume ? request.current_tpc : request.start_pc;
  UniversalMicroProgramHandle program;
  {
    ScopedTelemetryDuration program_duration(&s_program_prepare_wall_us);
    program = PrepareUniversalMicroProgramForConfiguration(
        request.micro, request.micro_size, entry_pc,
        request.configuration_bits, error);
  }
  if (!program) {
    Fail(UniversalGpuVuRejection::ProgramEncoding, rejection, error,
         "universal GPU-VU PairPlan encoding failed");
    return nullptr;
  }

  ProgramAnalysis analysis;
  bool analysis_available = false;
  StaticPreflightResult static_preflight;
  bool static_cache_hit = false;
  {
    ScopedTelemetryDuration lookup_duration(
        &s_static_preflight_lookup_wall_us);
    static_cache_hit = LookupStaticPreflight(
        program.Identity(), request.fbrst, &static_preflight);
  }
  u32 preflight_pair_count = 0;
  u32 preflight_dynamic_pair_upper_bound = 0;
  bool runtime_path_proof = false;
  bool has_reachable_cycle = false;
  GeneratedUniversalProfile generated_profile;
  if (static_cache_hit) {
    preflight_pair_count = static_preflight.pair_count;
    runtime_path_proof = static_preflight.runtime_path_proof;
    has_reachable_cycle = static_preflight.has_reachable_cycle;
    generated_profile = static_preflight.generated_profile;
    if (static_preflight.rejection != UniversalGpuVuRejection::None) {
      if (classified_pair_count)
        *classified_pair_count = static_preflight.pair_count;
      Fail(static_preflight.rejection, rejection, error,
           UniversalGpuVuRejectionName(static_preflight.rejection));
      return nullptr;
    }
  } else {
    const bool assume_scheduled =
        (request.configuration_bits &
         UniversalConfigurationAssumeScheduled) != 0;
    const bool instant_qp =
        (request.configuration_bits & UniversalConfigurationInstantQp) != 0;
    bool analyzed = false;
    {
      ScopedTelemetryDuration analysis_duration(&s_control_analysis_wall_us);
      analyzed = AnalyzeGpuVu1ProgramForConfiguration(
          request.micro, request.micro_size, entry_pc, assume_scheduled,
          instant_qp, &analysis, error);
    }
    if (!analyzed) {
      StoreStaticPreflight(program.Identity(), request.fbrst,
                           UniversalGpuVuRejection::ProgramEncoding, 0,
                           false, false);
      Fail(UniversalGpuVuRejection::ProgramEncoding, rejection, error,
           "universal GPU-VU control-flow analysis failed");
      return nullptr;
    }
    analysis_available = true;
    if (!analysis.has_program_exit ||
        !analysis.every_block_can_reach_program_exit) {
      StoreStaticPreflight(program.Identity(), request.fbrst,
                           UniversalGpuVuRejection::IncompleteControlFlow,
                           0, false, false);
      Fail(UniversalGpuVuRejection::IncompleteControlFlow, rejection, error,
           "universal GPU-VU epoch has no proven complete E-bit exit");
      return nullptr;
    }
    if (analysis.has_branch_in_delay_slot) {
      StoreStaticPreflight(program.Identity(), request.fbrst,
                           UniversalGpuVuRejection::BranchInDelaySlot, 0,
                           false, false);
      Fail(UniversalGpuVuRejection::BranchInDelaySlot, rejection, error,
           "universal GPU-VU branch-in-delay semantics are not admitted");
      return nullptr;
    }
    UniversalGpuVuRejection pair_rejection =
        UniversalGpuVuRejection::None;
    std::string pair_error;
    bool pairs_valid = false;
    {
      ScopedTelemetryDuration pair_duration(&s_pair_validation_wall_us);
      pairs_valid = ValidateReachablePairs(
          analysis, *program, request.fbrst,
          &preflight_pair_count, &runtime_path_proof,
          &pair_rejection, &pair_error);
    }
    if (pairs_valid)
      AccumulateReachableGeneratedProfile(analysis, *program,
                                          &generated_profile);
    StoreStaticPreflight(program.Identity(), request.fbrst,
                         pairs_valid ? UniversalGpuVuRejection::None :
                                       pair_rejection,
                         preflight_pair_count, runtime_path_proof,
                         pairs_valid && HasReachableControlCycle(analysis),
                         generated_profile, pairs_valid ? &analysis : nullptr);
    if (!pairs_valid) {
      if (rejection)
        *rejection = pair_rejection;
      if (error)
        *error = pair_error;
      if (classified_pair_count)
        *classified_pair_count = preflight_pair_count;
      return nullptr;
    }
    has_reachable_cycle = HasReachableControlCycle(analysis);
  }
  preflight_dynamic_pair_upper_bound =
      has_reachable_cycle || runtime_path_proof ? request.maximum_pairs :
                                                 preflight_pair_count;
  const std::vector<UniversalGpuVuAdditionalExecuteRequest>* const
      additional_executes = request.additional_executes;
  static const std::vector<UniversalGpuVuAdditionalExecuteRequest>
      EmptyAdditionalExecutes;
  const auto& extra_executes = additional_executes ?
      *additional_executes : EmptyAdditionalExecutes;
  if (extra_executes.size() >= UniversalCommandEpochMaximumCommands) {
    Fail(UniversalGpuVuRejection::CommandCapacity, rejection, error,
         "universal GPU-VU epoch has too many Execute commands");
    return nullptr;
  }
  for (const UniversalGpuVuAdditionalExecuteRequest& execute :
       extra_executes) {
    if (execute.maximum_pairs == 0 ||
        execute.maximum_pairs > UniversalCommandEpochMaximumPairsPerExecute ||
        (!execute.resume &&
         ((execute.start_pc & 7u) != 0 || execute.start_pc > 0x3ff8u)) ||
        (execute.resume &&
         ((execute.current_tpc & 7u) != 0 ||
          execute.current_tpc > 0x3ff8u))) {
      Fail(UniversalGpuVuRejection::InvalidRequest, rejection, error,
           "invalid additional universal GPU-VU Execute request");
      return nullptr;
    }

    const u32 execute_entry =
        execute.resume ? execute.current_tpc : execute.start_pc;
    ProgramAnalysis execute_analysis;
    const bool assume_scheduled =
        (request.configuration_bits &
         UniversalConfigurationAssumeScheduled) != 0;
    const bool instant_qp =
        (request.configuration_bits & UniversalConfigurationInstantQp) != 0;
    {
      ScopedTelemetryDuration analysis_duration(&s_control_analysis_wall_us);
      if (!AnalyzeGpuVu1ProgramForConfiguration(
              request.micro, request.micro_size, execute_entry,
              assume_scheduled, instant_qp, &execute_analysis, error)) {
        Fail(UniversalGpuVuRejection::ProgramEncoding, rejection, error,
             "additional universal GPU-VU control-flow analysis failed");
        return nullptr;
      }
    }
    if (!execute_analysis.has_program_exit ||
        !execute_analysis.every_block_can_reach_program_exit) {
      Fail(UniversalGpuVuRejection::IncompleteControlFlow, rejection, error,
           "additional universal GPU-VU Execute has no complete E-bit exit");
      return nullptr;
    }
    if (execute_analysis.has_branch_in_delay_slot) {
      Fail(UniversalGpuVuRejection::BranchInDelaySlot, rejection, error,
           "additional universal GPU-VU Execute has branch-in-delay semantics");
      return nullptr;
    }
    u32 execute_pairs = 0;
    bool execute_runtime_path_proof = false;
    UniversalGpuVuRejection execute_rejection =
        UniversalGpuVuRejection::None;
    std::string execute_error;
    {
      ScopedTelemetryDuration pair_duration(&s_pair_validation_wall_us);
      if (!ValidateReachablePairs(
              execute_analysis, *program, execute.fbrst, &execute_pairs,
              &execute_runtime_path_proof, &execute_rejection,
              &execute_error)) {
        if (classified_pair_count)
          *classified_pair_count = preflight_pair_count + execute_pairs;
        if (rejection)
          *rejection = execute_rejection;
        if (error)
          *error = execute_error;
        return nullptr;
      }
    }
    AccumulateReachableGeneratedProfile(execute_analysis, *program,
                                        &generated_profile);
    if (preflight_pair_count >
        std::numeric_limits<u32>::max() - execute_pairs) {
      Fail(UniversalGpuVuRejection::CommandCapacity, rejection, error,
           "universal GPU-VU pair count overflows");
      return nullptr;
    }
    preflight_pair_count += execute_pairs;
    runtime_path_proof =
        runtime_path_proof || execute_runtime_path_proof;
    const u32 execute_dynamic_pair_upper_bound = DynamicPairUpperBound(
        execute_analysis, execute_pairs, execute.maximum_pairs);
    if (preflight_dynamic_pair_upper_bound >
        std::numeric_limits<u32>::max() - execute_dynamic_pair_upper_bound) {
      Fail(UniversalGpuVuRejection::CommandCapacity, rejection, error,
           "universal GPU-VU dynamic pair bound overflows");
      return nullptr;
    }
    preflight_dynamic_pair_upper_bound += execute_dynamic_pair_upper_bound;
  }
  if (classified_pair_count)
    *classified_pair_count = preflight_pair_count;
  if (dynamic_pair_upper_bound)
    *dynamic_pair_upper_bound = preflight_dynamic_pair_upper_bound;

  if (preflight_dynamic_pair_upper_bound >
      UniversalGpuVuMaximumPairsPerSubmissionBatch) {
    Fail(UniversalGpuVuRejection::WatchdogWorkBudget, rejection, error,
         "universal epoch exceeds the bounded multi-slice submission batch");
    return nullptr;
  }

#if defined(__vita__)
  if (!IsGeneratedLoopKernelProductAdmissionEnabled()) {
    Fail(UniversalGpuVuRejection::GeneratedArchitectureQuarantined,
         rejection, error,
         "legacy serial, snapshot, and module-graph GPU-VU providers are "
         "quarantined");
    return nullptr;
  }
#endif

  // The first generated transaction root deliberately supports one Execute
  // only; make the complete command-chain shape part of its generated-program
  // identity. Multi-Execute epochs remain on CPU MTVU until their generated
  // owner is complete—there is no fixed-interpreter product fallback.
  generated_profile.execute_count =
      1u + static_cast<u32>(extra_executes.size());

  std::unique_ptr<UniversalGpuVuEpoch> epoch;
  {
    ScopedTelemetryDuration allocation_duration(&s_epoch_allocation_wall_us);
    try {
      epoch.reset(new UniversalGpuVuEpoch());
    } catch (const std::bad_alloc&) {
      Fail(UniversalGpuVuRejection::EpochPoolCapacity, rejection, error,
           "universal GPU-VU epoch pool is full");
      return nullptr;
    }
  }
  epoch->m_completion.sequence =
      s_next_sequence.fetch_add(1, std::memory_order_relaxed);
  epoch->m_completion.predecessor_sequence = request.predecessor_sequence;
  epoch->m_analysis_entry_pc = entry_pc;
  epoch->m_configuration_bits = request.configuration_bits;
  epoch->m_vif_row = request.vif_row;
  epoch->m_vif_column = request.vif_column;
#if defined(__vita__)
  // Runtime compilation is opportunistic. Only one PairPlan-derived,
  // static-flow loop-kernel root is a product candidate; CPU MTVU remains the
  // pre-effect owner while its exact content key is cold, queued, rejected, or
  // unprofitable. The fixed interpreter, snapshot/precompute pipeline, and
  // module graph are deliberately absent from this selection.
  const u64 compiler_idle_before_request =
      GetGeneratedProgramCompilerIdleGeneration();
  bool generated_requested = false;
  GeneratedLoopKernelBundleState generated_loop_state =
      GeneratedLoopKernelBundleState::Missing;
  bool generated_analysis_cache_hit = false;
  bool generated_analysis_rebuilt = false;
  const auto ensure_generated_analysis = [&]() {
    if (analysis_available)
      return true;
    analysis_available = LookupStaticPreflightAnalysis(
        program.Identity(), request.fbrst, &analysis);
    generated_analysis_cache_hit = analysis_available;
    if (analysis_available)
      return true;

    const bool assume_scheduled =
        (request.configuration_bits &
         UniversalConfigurationAssumeScheduled) != 0u;
    const bool instant_qp =
        (request.configuration_bits &
         UniversalConfigurationInstantQp) != 0u;
    std::string generated_analysis_error;
    {
      ScopedTelemetryDuration analysis_duration(&s_control_analysis_wall_us);
      analysis_available = AnalyzeGpuVu1ProgramForConfiguration(
          request.micro, request.micro_size, entry_pc,
          assume_scheduled, instant_qp, &analysis,
          &generated_analysis_error);
    }
    generated_analysis_rebuilt = true;
    if (analysis_available) {
      StoreStaticPreflight(
          program.Identity(), request.fbrst,
          UniversalGpuVuRejection::None, preflight_pair_count,
          runtime_path_proof, has_reachable_cycle, generated_profile,
          &analysis);
    } else if (error && error->empty()) {
      *error = std::move(generated_analysis_error);
    }
    return analysis_available;
  };

  GeneratedLoopKernelBundle loop_kernel;
  if (extra_executes.empty()) {
    generated_loop_state = QueryGeneratedLoopKernelBundle(
        program.Identity(), entry_pc, request.configuration_bits,
        &loop_kernel);
    if (generated_loop_state ==
        GeneratedLoopKernelBundleState::Missing) {
      if (ensure_generated_analysis()) {
        generated_requested = RequestGeneratedLoopKernelBundle(
            program.Identity(), analysis, request.configuration_bits,
            &loop_kernel, &request.initial_vi, request.vif_top,
            request.vif_itop);
        generated_loop_state = QueryGeneratedLoopKernelBundle(
            program.Identity(), entry_pc, request.configuration_bits,
            &loop_kernel);
      }
    } else if (generated_loop_state ==
               GeneratedLoopKernelBundleState::Compiling) {
      // Initial construction, registration completion and variance/
      // attestation edges own background pumping. A compiling epoch stays on
      // MTVU; CPU1 must not queue or execute shader-source work per Execute.
      generated_requested = true;
    } else {
      generated_requested =
          generated_loop_state == GeneratedLoopKernelBundleState::Ready;
    }

    if (loop_kernel.HasAtomicCompilerOwnership()) {
      epoch->m_generated_program_key = loop_kernel.kernel_key;
      if (generated_loop_state ==
              GeneratedLoopKernelBundleState::Ready &&
          loop_kernel.MatchesOwner(
              program.Identity(), entry_pc, request.configuration_bits,
              loop_kernel.gif_tag)) {
        epoch->m_generated_loop_kernel = std::move(loop_kernel);
      } else if (generated_loop_state ==
                 GeneratedLoopKernelBundleState::Ready) {
        // A ready descriptor which does not name this exact source/entry/profile
        // is stale cache data and can never own the epoch.
        generated_loop_state = GeneratedLoopKernelBundleState::Failed;
        epoch->m_generated_program_key = {};
      }
    }
  } else {
    generated_loop_state = GeneratedLoopKernelBundleState::NotApplicable;
  }

  epoch->m_generated_loop_kernel_state = generated_loop_state;

  epoch->m_generated_loop_kernel_output_contract_pending =
      generated_loop_state ==
      GeneratedLoopKernelBundleState::WaitingForOutputContract;
  const bool has_generated_loop_kernel =
      epoch->m_generated_loop_kernel.HasAtomicCompilerOwnership();
  if (!has_generated_loop_kernel) {
    const u64 report = s_generated_lifecycle_reports.fetch_add(
                           1u, std::memory_order_relaxed) +
                       1u;
    if (report <= 8u || (report & (report - 1u)) == 0u) {
      Console.WriteLn(
          "GPU-VU: generated loop-kernel lifecycle report=%llu "
          "identity=%016llx entry=%04x extra_executes=%u state=%u "
          "analysis=%u analysis_cache_hit=%u analysis_rebuilt=%u "
          "requested=%u compiler_in_flight=%u cpu_owner=1.",
          static_cast<unsigned long long>(report),
          static_cast<unsigned long long>(program.Identity()), entry_pc,
          static_cast<u32>(extra_executes.size()),
          static_cast<u32>(generated_loop_state),
          analysis_available ? 1u : 0u,
          generated_analysis_cache_hit ? 1u : 0u,
          generated_analysis_rebuilt ? 1u : 0u,
          generated_requested ? 1u : 0u,
          GeneratedProgramCompilerHasInFlightWork() ? 1u : 0u);
    }
  }
  if (!has_generated_loop_kernel &&
      generated_loop_state == GeneratedLoopKernelBundleState::Compiling &&
      GeneratedProgramCompilerHasInFlightWork()) {
    epoch->m_generated_request_deferred = true;
    epoch->m_generated_request_observed_compiler_idle =
        compiler_idle_before_request;
  }
#endif
  epoch->m_program = std::move(program);
  epoch->m_preflight_pair_count = preflight_pair_count;
  epoch->m_dynamic_pair_upper_bound = preflight_dynamic_pair_upper_bound;
  epoch->m_runtime_path_proof = runtime_path_proof;

  ScopedTelemetryDuration payload_duration(&s_payload_encode_wall_us);
  const std::vector<VifUnpackSpan>* const source_unpacks = request.unpacks;
  static const std::vector<VifUnpackSpan> EmptyUnpacks;
  const std::vector<VifUnpackSpan>& unpacks =
      source_unpacks ? *source_unpacks : EmptyUnpacks;
  u32 unpack_count = static_cast<u32>(unpacks.size());
  for (const UniversalGpuVuAdditionalExecuteRequest& execute :
       extra_executes) {
    const u32 count = execute.unpacks ?
        static_cast<u32>(execute.unpacks->size()) : 0u;
    if (unpack_count > std::numeric_limits<u32>::max() - count) {
      Fail(UniversalGpuVuRejection::CommandCapacity, rejection, error,
           "universal GPU-VU UNPACK count overflows");
      return nullptr;
    }
    unpack_count += count;
  }
  if (unpack_count + static_cast<u32>(extra_executes.size()) + 2u >
      UniversalCommandEpochMaximumCommands) {
    Fail(UniversalGpuVuRejection::CommandCapacity, rejection, error,
         "universal GPU-VU epoch exceeds command capacity");
    return nullptr;
  }

  u32 payload_begin = std::numeric_limits<u32>::max();
  u32 payload_end = 0;
  RawVifPayloadRef generation;
  const auto visit_unpacks = [&](const auto& visitor) {
    for (const VifUnpackSpan& input : unpacks)
      if (!visitor(input))
        return false;
    for (const UniversalGpuVuAdditionalExecuteRequest& execute :
         extra_executes) {
      if (!execute.unpacks)
        continue;
      for (const VifUnpackSpan& input : *execute.unpacks)
        if (!visitor(input))
          return false;
    }
    return true;
  };
  if (!visit_unpacks([&](const VifUnpackSpan& input) {
    u32 required_bytes = 0;
    if (!input.payload.IsValid() ||
        !IsFixedUniversalVifUnpackSupported(input) ||
        !GetUniversalVifUnpackPayloadSize(
            input, &required_bytes, nullptr) ||
        required_bytes > input.payload.size) {
      Fail(UniversalGpuVuRejection::UnsupportedVifUnpack, rejection, error,
           "generated GPU-VU command ABI does not support an UNPACK command");
      return false;
    }
    if (!generation.IsValid())
      generation = input.payload;
    else if (input.payload.owner != generation.owner ||
             input.payload.slot != generation.slot ||
             input.payload.generation != generation.generation) {
      Fail(UniversalGpuVuRejection::PayloadGeneration, rejection, error,
           "universal GPU-VU epoch spans input-ring generations");
      return false;
    }
    if (input.payload.offset >
        std::numeric_limits<u32>::max() - required_bytes) {
      Fail(UniversalGpuVuRejection::PayloadCapacity, rejection, error,
           "universal GPU-VU payload range overflows");
      return false;
    }
    payload_begin = std::min(payload_begin, input.payload.offset);
    payload_end = std::max(payload_end, input.payload.offset + required_bytes);
    if (!ResolveRawVifPayload(input.payload)) {
      Fail(UniversalGpuVuRejection::InputUnavailable, rejection, error,
           "universal GPU-VU immutable input is unavailable");
      return false;
    }
    return true;
  }))
    return nullptr;
  if (unpack_count != 0) {
    if (payload_end < payload_begin ||
        payload_end - payload_begin > UniversalGpuVuFixedPayloadWindowBytes) {
      Fail(UniversalGpuVuRejection::PayloadCapacity, rejection, error,
           "universal GPU-VU fixed payload window is too small");
      return nullptr;
    }
    epoch->m_payload_base_offset = payload_begin;
    epoch->m_payload_size = payload_end - payload_begin;
  }

  const auto append_unpacks = [&](const std::vector<VifUnpackSpan>& inputs) {
  for (const VifUnpackSpan& input : inputs) {
    if (epoch->m_unpack_count >= epoch->m_unpacks.size()) {
      Fail(UniversalGpuVuRejection::CommandCapacity, rejection, error,
           "universal GPU-VU retained UNPACK capacity exceeded");
      return false;
    }
    VifUnpackSpan retained = input;
    u32 required_bytes = 0;
    GetUniversalVifUnpackPayloadSize(retained, &required_bytes, nullptr);
    retained.source_size = required_bytes;
    if (!RetainRawVifPayload(retained.payload)) {
      Fail(UniversalGpuVuRejection::InputUnavailable, rejection, error,
           "universal GPU-VU input retention failed");
      return false;
    }
    epoch->m_unpacks[epoch->m_unpack_count++] = retained;
    epoch->m_unpack_submission_count +=
        (retained.vector_count + UniversalGpuVuUnpackVectorsPerSubmission - 1u) /
        UniversalGpuVuUnpackVectorsPerSubmission;
    if (!EncodeUniversalVifUnpackCommand(
            retained, retained.payload.offset - epoch->m_payload_base_offset,
            &epoch->m_commands[epoch->m_command_count], error)) {
      Fail(UniversalGpuVuRejection::UnsupportedVifUnpack, rejection, error,
           "universal GPU-VU UNPACK encoding failed");
      return false;
    }
    epoch->m_command_count++;
  }
  return true;
  };
  if (!append_unpacks(unpacks))
    return nullptr;
  if (!EncodeUniversalVuExecuteCommand(
          0, request.start_pc, request.maximum_pairs, request.vif_top,
          request.vif_itop, request.fbrst, request.resume,
          &epoch->m_commands[epoch->m_command_count], error)) {
    Fail(UniversalGpuVuRejection::InvalidRequest, rejection, error,
         "universal GPU-VU Execute encoding failed");
    return nullptr;
  }
  epoch->m_command_count++;
  epoch->m_execute_count++;
  for (const UniversalGpuVuAdditionalExecuteRequest& execute :
       extra_executes) {
    if (execute.unpacks && !append_unpacks(*execute.unpacks))
      return nullptr;
    if (!EncodeUniversalVuExecuteCommand(
            0, execute.start_pc, execute.maximum_pairs, execute.vif_top,
            execute.vif_itop, execute.fbrst, execute.resume,
            &epoch->m_commands[epoch->m_command_count], error)) {
      Fail(UniversalGpuVuRejection::InvalidRequest, rejection, error,
           "additional universal GPU-VU Execute encoding failed");
      return nullptr;
    }
    epoch->m_command_count++;
    epoch->m_execute_count++;
  }
  epoch->m_commands[epoch->m_command_count++] =
      EncodeUniversalEpochEndCommand();
  s_preflight_accepted.fetch_add(1, std::memory_order_relaxed);
  return epoch;
}

UniversalGpuVuEpochStatistics GetUniversalGpuVuEpochStatistics() {
  UniversalGpuVuEpochStatistics result;
  result.prepared = s_prepared.load(std::memory_order_relaxed);
  result.preflight_accepted =
      s_preflight_accepted.load(std::memory_order_relaxed);
  result.submitted = s_submitted.load(std::memory_order_relaxed);
  result.accepted = s_accepted.load(std::memory_order_relaxed);
  result.cpu_fallbacks = s_cpu_fallbacks.load(std::memory_order_relaxed);
  result.accepted_pairs = s_accepted_pairs.load(std::memory_order_relaxed);
  result.accepted_path1_packets =
      s_accepted_path1_packets.load(std::memory_order_relaxed);
  result.accepted_path1_qwords =
      s_accepted_path1_qwords.load(std::memory_order_relaxed);
  result.rejected_pairs = s_rejected_pairs.load(std::memory_order_relaxed);
  result.cpu_vu_calls = s_cpu_vu_calls.load(std::memory_order_relaxed);
  result.async_epochs_queued =
      s_async_epochs_queued.load(std::memory_order_relaxed);
  result.async_state_acquired =
      s_async_state_acquired.load(std::memory_order_relaxed);
  result.async_pending_max =
      s_async_pending_max.load(std::memory_order_relaxed);
  result.preflight_wall_us =
      s_preflight_wall_us.load(std::memory_order_relaxed);
  result.program_prepare_wall_us =
      s_program_prepare_wall_us.load(std::memory_order_relaxed);
  result.static_preflight_lookup_wall_us =
      s_static_preflight_lookup_wall_us.load(std::memory_order_relaxed);
  result.epoch_allocation_wall_us =
      s_epoch_allocation_wall_us.load(std::memory_order_relaxed);
  result.control_analysis_wall_us =
      s_control_analysis_wall_us.load(std::memory_order_relaxed);
  result.pair_validation_wall_us =
      s_pair_validation_wall_us.load(std::memory_order_relaxed);
  result.payload_encode_wall_us =
      s_payload_encode_wall_us.load(std::memory_order_relaxed);
  result.mailbox_wait_wall_us =
      s_mailbox_wait_wall_us.load(std::memory_order_relaxed);
  result.notification_wait_wall_us =
      s_notification_wait_wall_us.load(std::memory_order_relaxed);
  result.retirement_wait_wall_us =
      s_retirement_wait_wall_us.load(std::memory_order_relaxed);
  result.cpu_fallback_wall_us =
      s_cpu_fallback_wall_us.load(std::memory_order_relaxed);
  result.cpu_materialize_wall_us =
      s_cpu_materialize_wall_us.load(std::memory_order_relaxed);
  result.cpu_unpack_replay_wall_us =
      s_cpu_unpack_replay_wall_us.load(std::memory_order_relaxed);
  result.cpu_path1_finish_wall_us =
      s_cpu_path1_finish_wall_us.load(std::memory_order_relaxed);
  result.cpu_completion_publish_wall_us =
      s_cpu_completion_publish_wall_us.load(std::memory_order_relaxed);
  result.worker_attempt_wall_us =
      s_worker_attempt_wall_us.load(std::memory_order_relaxed);
  result.path1_retirement_wall_us =
      s_path1_retirement_wall_us.load(std::memory_order_relaxed);
  result.generated_live_contract_resolutions =
      s_generated_live_contract_resolutions.load(std::memory_order_relaxed);
  result.generated_live_contract_resolution_wall_us =
      s_generated_live_contract_resolution_wall_us.load(
          std::memory_order_relaxed);
  result.generated_live_control_cache_hits =
      s_generated_live_control_cache_hits.load(std::memory_order_relaxed);
  result.generated_live_control_cache_misses =
      s_generated_live_control_cache_misses.load(std::memory_order_relaxed);
  result.generated_descriptor_builds =
      s_generated_descriptor_builds.load(std::memory_order_relaxed);
  result.generated_descriptor_build_wall_us =
      s_generated_descriptor_build_wall_us.load(std::memory_order_relaxed);
  result.generated_descriptor_cache_wall_us =
      s_generated_descriptor_cache_wall_us.load(std::memory_order_relaxed);
  result.generated_descriptor_runtime_proof_wall_us =
      s_generated_descriptor_runtime_proof_wall_us.load(
          std::memory_order_relaxed);
  result.generated_descriptor_input_pack_wall_us =
      s_generated_descriptor_input_pack_wall_us.load(
          std::memory_order_relaxed);
  result.generated_descriptor_store_layout_wall_us =
      s_generated_descriptor_store_layout_wall_us.load(
          std::memory_order_relaxed);
  result.generated_descriptor_final_state_wall_us =
      s_generated_descriptor_final_state_wall_us.load(
          std::memory_order_relaxed);
  result.generated_descriptor_transaction_wall_us =
      s_generated_descriptor_transaction_wall_us.load(
          std::memory_order_relaxed);
  result.generated_descriptor_transaction_acquire_wall_us =
      s_generated_descriptor_transaction_acquire_wall_us.load(
          std::memory_order_relaxed);
  result.generated_descriptor_transaction_capture_wall_us =
      s_generated_descriptor_transaction_capture_wall_us.load(
          std::memory_order_relaxed);
  result.generated_descriptor_transaction_configure_wall_us =
      s_generated_descriptor_transaction_configure_wall_us.load(
          std::memory_order_relaxed);
  result.generated_private_state_advances =
      s_generated_private_state_advances.load(std::memory_order_relaxed);
  result.generated_private_state_advance_wall_us =
      s_generated_private_state_advance_wall_us.load(
          std::memory_order_relaxed);
  result.generated_private_same_layout_replacements =
      s_generated_private_same_layout_replacements.load(
          std::memory_order_relaxed);
  result.generated_private_covered_layout_replacements =
      s_generated_private_covered_layout_replacements.load(
          std::memory_order_relaxed);
  result.generated_private_covered_owner_slots =
      s_generated_private_covered_owner_slots.load(
          std::memory_order_relaxed);
  result.generated_private_register_owner_replacements =
      s_generated_private_register_owner_replacements.load(
          std::memory_order_relaxed);
  result.generated_private_register_owner_replacement_slots =
      s_generated_private_register_owner_replacement_slots.load(
          std::memory_order_relaxed);
  result.generated_private_register_owner_remaps =
      s_generated_private_register_owner_remaps.load(
          std::memory_order_relaxed);
  result.generated_private_bridge_calls =
      s_generated_private_bridge_calls.load(std::memory_order_relaxed);
  result.generated_private_bridge_pairs =
      s_generated_private_bridge_pairs.load(std::memory_order_relaxed);
  result.generated_private_bridge_wall_us =
      s_generated_private_bridge_wall_us.load(std::memory_order_relaxed);
  result.generated_state_formula_calls =
      s_generated_state_formula_calls.load(std::memory_order_relaxed);
  result.generated_state_formula_logical_pairs =
      s_generated_state_formula_logical_pairs.load(std::memory_order_relaxed);
  result.generated_state_formula_operations =
      s_generated_state_formula_operations.load(std::memory_order_relaxed);
  result.generated_state_formula_wall_us =
      s_generated_state_formula_wall_us.load(std::memory_order_relaxed);
  result.generated_hot_execute_calls =
      s_generated_hot_execute_calls.load(std::memory_order_relaxed);
  result.generated_hot_execute_wall_us =
      s_generated_hot_execute_wall_us.load(std::memory_order_relaxed);
  result.generated_queue_calls =
      s_generated_queue_calls.load(std::memory_order_relaxed);
  result.generated_queue_wall_us =
      s_generated_queue_wall_us.load(std::memory_order_relaxed);
  result.generated_gather_calls =
      s_generated_gather_calls.load(std::memory_order_relaxed);
  result.generated_gather_executes =
      s_generated_gather_executes.load(std::memory_order_relaxed);
  result.generated_gather_wall_us =
      s_generated_gather_wall_us.load(std::memory_order_relaxed);
  result.generated_publication_calls =
      s_generated_publication_calls.load(std::memory_order_relaxed);
  result.generated_publication_draws =
      s_generated_publication_draws.load(std::memory_order_relaxed);
  result.generated_publication_wall_us =
      s_generated_publication_wall_us.load(std::memory_order_relaxed);
  result.generated_retirement_polls =
      s_generated_retirement_polls.load(std::memory_order_relaxed);
  result.generated_retirement_poll_wall_us =
      s_generated_retirement_poll_wall_us.load(std::memory_order_relaxed);
  result.generated_mtvu_execute_records =
      s_generated_mtvu_execute_records.load(std::memory_order_relaxed);
  result.generated_mtvu_execute_record_wall_us =
      s_generated_mtvu_execute_record_wall_us.load(std::memory_order_relaxed);
  result.generated_mtvu_vif_records =
      s_generated_mtvu_vif_records.load(std::memory_order_relaxed);
  result.generated_mtvu_vif_record_wall_us =
      s_generated_mtvu_vif_record_wall_us.load(std::memory_order_relaxed);
  result.generated_mtvu_other_records =
      s_generated_mtvu_other_records.load(std::memory_order_relaxed);
  result.generated_mtvu_other_record_wall_us =
      s_generated_mtvu_other_record_wall_us.load(std::memory_order_relaxed);
  result.generated_mtvu_housekeeping_calls =
      s_generated_mtvu_housekeeping_calls.load(std::memory_order_relaxed);
  result.generated_mtvu_housekeeping_wall_us =
      s_generated_mtvu_housekeeping_wall_us.load(std::memory_order_relaxed);
  result.generated_batch_commits =
      s_generated_batch_commits.load(std::memory_order_relaxed);
  result.generated_batch_commit_wall_us =
      s_generated_batch_commit_wall_us.load(std::memory_order_relaxed);
  result.generated_batch_drain_waits =
      s_generated_batch_drain_waits.load(std::memory_order_relaxed);
  result.generated_batch_drain_wait_wall_us =
      s_generated_batch_drain_wait_wall_us.load(std::memory_order_relaxed);
  result.generated_batch_drain_polls =
      s_generated_batch_drain_polls.load(std::memory_order_relaxed);
  result.mtvu_execute_queue_samples =
      s_mtvu_execute_queue_samples.load(std::memory_order_relaxed);
  result.mtvu_execute_queue_age_us =
      s_mtvu_execute_queue_age_us.load(std::memory_order_relaxed);
  result.mtvu_execute_queue_age_max_us =
      s_mtvu_execute_queue_age_max_us.load(std::memory_order_relaxed);
  result.mtvu_execute_outstanding_max =
      s_mtvu_execute_outstanding_max.load(std::memory_order_relaxed);
  result.mtvu_queue_used_words_max =
      s_mtvu_queue_used_words_max.load(std::memory_order_relaxed);
  result.cpu0_mtvu_wait_wall_us =
      s_cpu0_mtvu_wait_wall_us.load(std::memory_order_relaxed);
  result.cpu0_mtvu_ring_wait_wall_us =
      s_cpu0_mtvu_ring_wait_wall_us.load(std::memory_order_relaxed);
  result.cpu0_execute_budget_waits =
      s_cpu0_execute_budget_waits.load(std::memory_order_relaxed);
  result.cpu0_execute_budget_wait_wall_us =
      s_cpu0_execute_budget_wait_wall_us.load(std::memory_order_relaxed);
  result.static_preflight_cache_hits =
      s_static_preflight_cache_hits.load(std::memory_order_relaxed);
  result.static_preflight_cache_misses =
      s_static_preflight_cache_misses.load(std::memory_order_relaxed);
  result.mtvu_multi_execute_gather_suppressed =
      s_mtvu_multi_execute_gather_suppressed.load(
          std::memory_order_relaxed);
  result.mtvu_dispatch_cache_hits =
      s_mtvu_dispatch_cache_hits.load(std::memory_order_relaxed);
  result.mtvu_dispatch_cache_misses =
      s_mtvu_dispatch_cache_misses.load(std::memory_order_relaxed);
  result.generated_product_hot_dispatch_hits =
      s_generated_product_hot_dispatch_hits.load(std::memory_order_relaxed);
  result.generated_product_hot_dispatch_misses =
      s_generated_product_hot_dispatch_misses.load(std::memory_order_relaxed);
  result.continuation_groups =
      s_continuation_groups.load(std::memory_order_relaxed);
  result.continuation_submissions =
      s_continuation_submissions.load(std::memory_order_relaxed);
  result.universal_provider_epochs =
      s_universal_provider_epochs.load(std::memory_order_relaxed);
  result.universal_provider_jobs =
      s_universal_provider_jobs.load(std::memory_order_relaxed);
  result.generated_provider_epochs =
      s_generated_provider_epochs.load(std::memory_order_relaxed);
  result.generated_provider_jobs =
      s_generated_provider_jobs.load(std::memory_order_relaxed);
  result.gpu_residency_samples =
      s_gpu_residency_samples.load(std::memory_order_relaxed);
  result.gpu_residency_wall_us =
      s_gpu_residency_wall_us.load(std::memory_order_relaxed);
  result.gpu_residency_wall_us_max =
      s_gpu_residency_wall_us_max.load(std::memory_order_relaxed);
  result.mtvu_path1_queue_samples =
      s_mtvu_path1_queue_samples.load(std::memory_order_relaxed);
  result.mtvu_path1_queue_age_us =
      s_mtvu_path1_queue_age_us.load(std::memory_order_relaxed);
  result.mtvu_path1_queue_age_max_us =
      s_mtvu_path1_queue_age_max_us.load(std::memory_order_relaxed);
  for (std::size_t index = 0; index < result.rejections.size(); index++)
    result.rejections[index] =
        s_rejections[index].load(std::memory_order_relaxed);
  return result;
}

void ResetUniversalGpuVuEpochStatistics() {
  s_prepared.store(0, std::memory_order_relaxed);
  s_preflight_accepted.store(0, std::memory_order_relaxed);
  s_submitted.store(0, std::memory_order_relaxed);
  s_accepted.store(0, std::memory_order_relaxed);
  s_cpu_fallbacks.store(0, std::memory_order_relaxed);
  s_accepted_pairs.store(0, std::memory_order_relaxed);
  s_accepted_path1_packets.store(0, std::memory_order_relaxed);
  s_accepted_path1_qwords.store(0, std::memory_order_relaxed);
  s_rejected_pairs.store(0, std::memory_order_relaxed);
  s_cpu_vu_calls.store(0, std::memory_order_relaxed);
  s_async_epochs_queued.store(0, std::memory_order_relaxed);
  s_async_state_acquired.store(0, std::memory_order_relaxed);
  s_async_pending_max.store(0, std::memory_order_relaxed);
  s_preflight_wall_us.store(0, std::memory_order_relaxed);
  s_program_prepare_wall_us.store(0, std::memory_order_relaxed);
  s_static_preflight_lookup_wall_us.store(0, std::memory_order_relaxed);
  s_epoch_allocation_wall_us.store(0, std::memory_order_relaxed);
  s_control_analysis_wall_us.store(0, std::memory_order_relaxed);
  s_pair_validation_wall_us.store(0, std::memory_order_relaxed);
  s_payload_encode_wall_us.store(0, std::memory_order_relaxed);
  s_mailbox_wait_wall_us.store(0, std::memory_order_relaxed);
  s_notification_wait_wall_us.store(0, std::memory_order_relaxed);
  s_retirement_wait_wall_us.store(0, std::memory_order_relaxed);
  s_cpu_fallback_wall_us.store(0, std::memory_order_relaxed);
  s_cpu_materialize_wall_us.store(0, std::memory_order_relaxed);
  s_cpu_unpack_replay_wall_us.store(0, std::memory_order_relaxed);
  s_cpu_path1_finish_wall_us.store(0, std::memory_order_relaxed);
  s_cpu_completion_publish_wall_us.store(0, std::memory_order_relaxed);
  s_worker_attempt_wall_us.store(0, std::memory_order_relaxed);
  s_path1_retirement_wall_us.store(0, std::memory_order_relaxed);
  s_generated_live_contract_resolutions.store(0,
                                               std::memory_order_relaxed);
  s_generated_live_contract_resolution_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_live_control_cache_hits.store(0,
                                             std::memory_order_relaxed);
  s_generated_live_control_cache_misses.store(0,
                                               std::memory_order_relaxed);
  s_generated_descriptor_builds.store(0, std::memory_order_relaxed);
  s_generated_descriptor_build_wall_us.store(0, std::memory_order_relaxed);
  s_generated_descriptor_cache_wall_us.store(0,
                                              std::memory_order_relaxed);
  s_generated_descriptor_runtime_proof_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_descriptor_input_pack_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_descriptor_store_layout_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_descriptor_final_state_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_descriptor_transaction_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_descriptor_transaction_acquire_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_descriptor_transaction_capture_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_descriptor_transaction_configure_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_private_state_advances.store(0, std::memory_order_relaxed);
  s_generated_private_state_advance_wall_us.store(
      0, std::memory_order_relaxed);
  s_generated_private_same_layout_replacements.store(
      0, std::memory_order_relaxed);
  s_generated_private_covered_layout_replacements.store(
      0, std::memory_order_relaxed);
  s_generated_private_covered_owner_slots.store(
      0, std::memory_order_relaxed);
  s_generated_private_register_owner_replacements.store(
      0, std::memory_order_relaxed);
  s_generated_private_register_owner_replacement_slots.store(
      0, std::memory_order_relaxed);
  s_generated_private_register_owner_remaps.store(
      0, std::memory_order_relaxed);
  s_generated_private_bridge_calls.store(0, std::memory_order_relaxed);
  s_generated_private_bridge_pairs.store(0, std::memory_order_relaxed);
  s_generated_private_bridge_wall_us.store(0, std::memory_order_relaxed);
  s_generated_state_formula_calls.store(0, std::memory_order_relaxed);
  s_generated_state_formula_logical_pairs.store(0,
                                                std::memory_order_relaxed);
  s_generated_state_formula_operations.store(0,
                                             std::memory_order_relaxed);
  s_generated_state_formula_wall_us.store(0, std::memory_order_relaxed);
  s_generated_hot_execute_calls.store(0, std::memory_order_relaxed);
  s_generated_hot_execute_wall_us.store(0, std::memory_order_relaxed);
  s_generated_queue_calls.store(0, std::memory_order_relaxed);
  s_generated_queue_wall_us.store(0, std::memory_order_relaxed);
  s_generated_gather_calls.store(0, std::memory_order_relaxed);
  s_generated_gather_executes.store(0, std::memory_order_relaxed);
  s_generated_gather_wall_us.store(0, std::memory_order_relaxed);
  s_generated_publication_calls.store(0, std::memory_order_relaxed);
  s_generated_publication_draws.store(0, std::memory_order_relaxed);
  s_generated_publication_wall_us.store(0, std::memory_order_relaxed);
  s_generated_retirement_polls.store(0, std::memory_order_relaxed);
  s_generated_retirement_poll_wall_us.store(0, std::memory_order_relaxed);
  s_generated_mtvu_execute_records.store(0, std::memory_order_relaxed);
  s_generated_mtvu_execute_record_wall_us.store(0,
                                                 std::memory_order_relaxed);
  s_generated_mtvu_vif_records.store(0, std::memory_order_relaxed);
  s_generated_mtvu_vif_record_wall_us.store(0,
                                             std::memory_order_relaxed);
  s_generated_mtvu_other_records.store(0, std::memory_order_relaxed);
  s_generated_mtvu_other_record_wall_us.store(0,
                                               std::memory_order_relaxed);
  s_generated_mtvu_housekeeping_calls.store(0, std::memory_order_relaxed);
  s_generated_mtvu_housekeeping_wall_us.store(0,
                                               std::memory_order_relaxed);
  s_generated_batch_commits.store(0, std::memory_order_relaxed);
  s_generated_batch_commit_wall_us.store(0, std::memory_order_relaxed);
  s_generated_batch_drain_waits.store(0, std::memory_order_relaxed);
  s_generated_batch_drain_wait_wall_us.store(0, std::memory_order_relaxed);
  s_generated_batch_drain_polls.store(0, std::memory_order_relaxed);
  s_mtvu_execute_queue_samples.store(0, std::memory_order_relaxed);
  s_mtvu_execute_queue_age_us.store(0, std::memory_order_relaxed);
  s_mtvu_execute_queue_age_max_us.store(0, std::memory_order_relaxed);
  s_mtvu_execute_outstanding_max.store(0, std::memory_order_relaxed);
  s_mtvu_queue_used_words_max.store(0, std::memory_order_relaxed);
  s_cpu0_mtvu_wait_wall_us.store(0, std::memory_order_relaxed);
  s_cpu0_mtvu_ring_wait_wall_us.store(0, std::memory_order_relaxed);
  s_cpu0_execute_budget_waits.store(0, std::memory_order_relaxed);
  s_cpu0_execute_budget_wait_wall_us.store(0, std::memory_order_relaxed);
  s_static_preflight_cache_hits.store(0, std::memory_order_relaxed);
  s_static_preflight_cache_misses.store(0, std::memory_order_relaxed);
  s_mtvu_multi_execute_gather_suppressed.store(0,
                                               std::memory_order_relaxed);
  s_mtvu_dispatch_cache_hits.store(0, std::memory_order_relaxed);
  s_mtvu_dispatch_cache_misses.store(0, std::memory_order_relaxed);
  s_generated_product_hot_dispatch_hits.store(0,
                                               std::memory_order_relaxed);
  s_generated_product_hot_dispatch_misses.store(0,
                                                 std::memory_order_relaxed);
  s_continuation_groups.store(0, std::memory_order_relaxed);
  s_continuation_submissions.store(0, std::memory_order_relaxed);
  s_universal_provider_epochs.store(0, std::memory_order_relaxed);
  s_universal_provider_jobs.store(0, std::memory_order_relaxed);
  s_generated_provider_epochs.store(0, std::memory_order_relaxed);
  s_generated_provider_jobs.store(0, std::memory_order_relaxed);
  s_gpu_residency_samples.store(0, std::memory_order_relaxed);
  s_gpu_residency_wall_us.store(0, std::memory_order_relaxed);
  s_gpu_residency_wall_us_max.store(0, std::memory_order_relaxed);
  s_mtvu_path1_queue_samples.store(0, std::memory_order_relaxed);
  s_mtvu_path1_queue_age_us.store(0, std::memory_order_relaxed);
  s_mtvu_path1_queue_age_max_us.store(0, std::memory_order_relaxed);
  for (auto& value : s_rejections)
    value.store(0, std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpuFallback(u32 pairs) {
  (void)pairs;
  s_cpu_vu_calls.fetch_add(1, std::memory_order_relaxed);
}

void RecordUniversalGpuVuPreflightCpuFallback(u32 pairs) {
  s_cpu_fallbacks.fetch_add(1, std::memory_order_relaxed);
  s_rejected_pairs.fetch_add(pairs, std::memory_order_relaxed);
  RecordUniversalGpuVuCpuFallback(pairs);
}

void RecordUniversalGpuVuMailboxWait(u64 wall_us) {
  s_mailbox_wait_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordUniversalGpuVuNotificationWait(u64 wall_us) {
  s_notification_wait_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordUniversalGpuVuRetirementWait(u64 wall_us) {
  s_retirement_wait_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpuFallbackTime(u64 wall_us) {
  s_cpu_fallback_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpuMaterializeTime(u64 wall_us) {
  s_cpu_materialize_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpuUnpackReplayTime(u64 wall_us) {
  s_cpu_unpack_replay_wall_us.fetch_add(wall_us,
                                        std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpuPath1FinishTime(u64 wall_us) {
  s_cpu_path1_finish_wall_us.fetch_add(wall_us,
                                      std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpuCompletionPublishTime(u64 wall_us) {
  s_cpu_completion_publish_wall_us.fetch_add(wall_us,
                                             std::memory_order_relaxed);
}

void RecordUniversalGpuVuWorkerAttemptTime(u64 wall_us) {
  s_worker_attempt_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordUniversalGpuVuPath1RetirementTime(u64 wall_us) {
  s_path1_retirement_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelLiveContractResolution(u64 wall_us) {
  s_generated_live_contract_resolutions.fetch_add(
      1, std::memory_order_relaxed);
  s_generated_live_contract_resolution_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelLiveControlCache(bool hit) {
  (hit ? s_generated_live_control_cache_hits
       : s_generated_live_control_cache_misses)
      .fetch_add(1, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelDescriptorBuild(u64 wall_us) {
  s_generated_descriptor_builds.fetch_add(1, std::memory_order_relaxed);
  s_generated_descriptor_build_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelDescriptorStages(
    u64 cache_wall_us, u64 runtime_proof_wall_us,
    u64 input_pack_wall_us, u64 store_layout_wall_us,
    u64 final_state_wall_us, u64 transaction_wall_us,
    u64 transaction_acquire_wall_us, u64 transaction_capture_wall_us,
    u64 transaction_configure_wall_us) {
  s_generated_descriptor_cache_wall_us.fetch_add(
      cache_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_runtime_proof_wall_us.fetch_add(
      runtime_proof_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_input_pack_wall_us.fetch_add(
      input_pack_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_store_layout_wall_us.fetch_add(
      store_layout_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_final_state_wall_us.fetch_add(
      final_state_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_transaction_wall_us.fetch_add(
      transaction_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_transaction_acquire_wall_us.fetch_add(
      transaction_acquire_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_transaction_capture_wall_us.fetch_add(
      transaction_capture_wall_us, std::memory_order_relaxed);
  s_generated_descriptor_transaction_configure_wall_us.fetch_add(
      transaction_configure_wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelPrivateStateAdvance(u64 wall_us) {
  s_generated_private_state_advances.fetch_add(1,
                                               std::memory_order_relaxed);
  s_generated_private_state_advance_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelPrivateSameLayoutReplacement() {
  s_generated_private_same_layout_replacements.fetch_add(
      1, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelPrivateCoveredLayoutReplacement(
    u32 owner_slots) {
  s_generated_private_covered_layout_replacements.fetch_add(
      1, std::memory_order_relaxed);
  s_generated_private_covered_owner_slots.fetch_add(
      owner_slots, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelPrivateRegisterOwnerUpdate(
    bool replaced_owner, u32 owner_slots) {
  if (!replaced_owner) {
    s_generated_private_register_owner_remaps.fetch_add(
        1, std::memory_order_relaxed);
    return;
  }
  s_generated_private_register_owner_replacements.fetch_add(
      1, std::memory_order_relaxed);
  s_generated_private_register_owner_replacement_slots.fetch_add(
      owner_slots, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelPrivateBridge(u32 pairs, u64 wall_us) {
  s_generated_private_bridge_calls.fetch_add(1, std::memory_order_relaxed);
  s_generated_private_bridge_pairs.fetch_add(pairs,
                                             std::memory_order_relaxed);
  s_generated_private_bridge_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelStateFormula(u32 logical_pairs, u32 operations,
                                           u64 wall_us) {
  s_generated_state_formula_calls.fetch_add(1, std::memory_order_relaxed);
  s_generated_state_formula_logical_pairs.fetch_add(
      logical_pairs, std::memory_order_relaxed);
  s_generated_state_formula_operations.fetch_add(
      operations, std::memory_order_relaxed);
  s_generated_state_formula_wall_us.fetch_add(wall_us,
                                              std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelHotExecute(u64 wall_us) {
  s_generated_hot_execute_calls.fetch_add(1, std::memory_order_relaxed);
  s_generated_hot_execute_wall_us.fetch_add(wall_us,
                                             std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelQueue(u64 wall_us) {
  s_generated_queue_calls.fetch_add(1, std::memory_order_relaxed);
  s_generated_queue_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelGather(u32 executes, u64 wall_us) {
  s_generated_gather_calls.fetch_add(1, std::memory_order_relaxed);
  s_generated_gather_executes.fetch_add(executes, std::memory_order_relaxed);
  s_generated_gather_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelPublication(u32 draws, u64 wall_us) {
  s_generated_publication_calls.fetch_add(1, std::memory_order_relaxed);
  s_generated_publication_draws.fetch_add(draws, std::memory_order_relaxed);
  s_generated_publication_wall_us.fetch_add(wall_us,
                                             std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelRetirementPoll(u64 wall_us) {
  s_generated_retirement_polls.fetch_add(1, std::memory_order_relaxed);
  s_generated_retirement_poll_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelMtvuExecuteRecord(u64 wall_us) {
  s_generated_mtvu_execute_records.fetch_add(1, std::memory_order_relaxed);
  s_generated_mtvu_execute_record_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelMtvuVifRecord(u64 wall_us) {
  s_generated_mtvu_vif_records.fetch_add(1, std::memory_order_relaxed);
  s_generated_mtvu_vif_record_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelMtvuOtherRecord(u64 wall_us) {
  s_generated_mtvu_other_records.fetch_add(1, std::memory_order_relaxed);
  s_generated_mtvu_other_record_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelMtvuHousekeeping(u64 wall_us) {
  s_generated_mtvu_housekeeping_calls.fetch_add(1,
                                                 std::memory_order_relaxed);
  s_generated_mtvu_housekeeping_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelBatchCommit(u64 wall_us) {
  s_generated_batch_commits.fetch_add(1, std::memory_order_relaxed);
  s_generated_batch_commit_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelBatchDrainWait(u64 wall_us, u64 polls) {
  s_generated_batch_drain_waits.fetch_add(1, std::memory_order_relaxed);
  s_generated_batch_drain_wait_wall_us.fetch_add(
      wall_us, std::memory_order_relaxed);
  s_generated_batch_drain_polls.fetch_add(polls,
                                          std::memory_order_relaxed);
}

namespace {

void RecordMaximum(std::atomic<u64>* destination, u64 value) {
  u64 previous = destination->load(std::memory_order_relaxed);
  while (previous < value && !destination->compare_exchange_weak(
      previous, value, std::memory_order_relaxed,
      std::memory_order_relaxed)) {
  }
}

}  // namespace

void RecordUniversalGpuVuMtvuExecuteQueueAge(u64 wall_us) {
  s_mtvu_execute_queue_samples.fetch_add(1, std::memory_order_relaxed);
  s_mtvu_execute_queue_age_us.fetch_add(wall_us, std::memory_order_relaxed);
  RecordMaximum(&s_mtvu_execute_queue_age_max_us, wall_us);
}

void RecordUniversalGpuVuMtvuExecuteOutstanding(u64 count) {
  RecordMaximum(&s_mtvu_execute_outstanding_max, count);
}

void RecordUniversalGpuVuMtvuQueueUsedWords(u64 words) {
  RecordMaximum(&s_mtvu_queue_used_words_max, words);
}

void RecordUniversalGpuVuCpu0Wait(u64 wall_us) {
  s_cpu0_mtvu_wait_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpu0RingWait(u64 wall_us) {
  s_cpu0_mtvu_ring_wait_wall_us.fetch_add(wall_us,
                                          std::memory_order_relaxed);
}

void RecordUniversalGpuVuCpu0ExecuteBudgetWait(u64 wall_us) {
  s_cpu0_execute_budget_waits.fetch_add(1, std::memory_order_relaxed);
  s_cpu0_execute_budget_wait_wall_us.fetch_add(wall_us,
                                               std::memory_order_relaxed);
}

void RecordUniversalGpuVuMtvuMultiExecuteGatherSuppressed() {
  s_mtvu_multi_execute_gather_suppressed.fetch_add(
      1, std::memory_order_relaxed);
}

void RecordUniversalGpuVuMtvuDispatchCacheLookup(bool hit) {
  (hit ? s_mtvu_dispatch_cache_hits : s_mtvu_dispatch_cache_misses)
      .fetch_add(1, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelProductHotDispatch(bool hit) {
  (hit ? s_generated_product_hot_dispatch_hits :
         s_generated_product_hot_dispatch_misses)
      .fetch_add(1, std::memory_order_relaxed);
}

void RecordUniversalGpuVuCachedPreflightRejection(
    UniversalGpuVuRejection rejection) {
  const std::size_t index = static_cast<std::size_t>(rejection);
  if (rejection != UniversalGpuVuRejection::None &&
      index < s_rejections.size()) {
    s_rejections[index].fetch_add(1, std::memory_order_relaxed);
  }
}

void RecordUniversalGpuVuProductPolicyCpuFallback(
    UniversalGpuVuRejection rejection) {
  s_cpu_fallbacks.fetch_add(1, std::memory_order_relaxed);
  s_cpu_vu_calls.fetch_add(1, std::memory_order_relaxed);
  const std::size_t index = static_cast<std::size_t>(rejection);
  if (rejection != UniversalGpuVuRejection::None &&
      rejection != UniversalGpuVuRejection::Count &&
      index < s_rejections.size()) {
    s_rejections[index].fetch_add(1, std::memory_order_relaxed);
  }
}

void RecordUniversalGpuVuContinuationGroup(u32 core_submissions) {
  s_continuation_groups.fetch_add(1, std::memory_order_relaxed);
  s_continuation_submissions.fetch_add(core_submissions,
                                       std::memory_order_relaxed);
}

void RecordUniversalGpuVuProviderCompletion(bool generated, u32 jobs,
                                             u64 gpu_residency_wall_us) {
  (generated ? s_generated_provider_epochs : s_universal_provider_epochs)
      .fetch_add(1, std::memory_order_relaxed);
  (generated ? s_generated_provider_jobs : s_universal_provider_jobs)
      .fetch_add(jobs, std::memory_order_relaxed);
  if (gpu_residency_wall_us != 0) {
    s_gpu_residency_samples.fetch_add(1, std::memory_order_relaxed);
    s_gpu_residency_wall_us.fetch_add(gpu_residency_wall_us,
                                      std::memory_order_relaxed);
    RecordMaximum(&s_gpu_residency_wall_us_max, gpu_residency_wall_us);
  }
}

void RecordGeneratedGpuVuFirmwareSubmission() {
  s_generated_provider_jobs.fetch_add(1u, std::memory_order_relaxed);
}

void RecordGeneratedLoopKernelAccepted(u32 executed_pairs) {
  if (executed_pairs == 0u)
    return;
  s_accepted.fetch_add(1u, std::memory_order_relaxed);
  s_accepted_pairs.fetch_add(executed_pairs, std::memory_order_relaxed);
}

void RecordUniversalGpuVuMtvuPath1QueueAge(u64 wall_us) {
  s_mtvu_path1_queue_samples.fetch_add(1, std::memory_order_relaxed);
  s_mtvu_path1_queue_age_us.fetch_add(wall_us, std::memory_order_relaxed);
  RecordMaximum(&s_mtvu_path1_queue_age_max_us, wall_us);
}

void RecordUniversalGpuVuAsyncQueued(u32 pending_count) {
  s_async_epochs_queued.fetch_add(1, std::memory_order_relaxed);
  RecordMaximum(&s_async_pending_max, pending_count);
}

bool MaterializeUniversalGpuVuCommittedState(
    const UniversalGpuVuCommittedStateView& view, VURegs* vu) {
  if (!view.IsValid() || !vu || view.state_words[2] != 1u ||
      view.state_words[33] != 0u) {
    return false;
  }
  const u32* const state = view.state_words;
  std::memcpy(&vu->VF[0].UL[0], view.vf_words,
              32u * 4u * sizeof(u32));
  std::memcpy(&vu->ACC.UL[0], view.vf_words + 32u * 4u,
              4u * sizeof(u32));
  std::memcpy(vu->Mem, view.vu_memory, VU1_MEMSIZE);
  for (u32 reg = 1; reg < 16; reg++)
    vu->VI[reg].UL = state[8 + reg] & 0xffffu;

  vu->VI[REG_TPC].UL = (state[0] & 0x3fffu) >> 3;
  vu->VI[REG_I].UL = state[24];
  vu->VI[REG_Q].UL = state[25];
  vu->VI[REG_P].UL = state[26];
  vu->VI[REG_MAC_FLAG].UL = state[42] & 0xffffu;
  vu->VI[REG_STATUS_FLAG].UL = state[43] & 0x0fffu;
  vu->VI[REG_CLIP_FLAG].UL = state[45] & 0x00ffffffu;
  vu->q.UL = vu->VI[REG_Q].UL;
  vu->p.UL = vu->VI[REG_P].UL;
  vu->pending_q = vu->VI[REG_Q].UL;
  vu->pending_p = vu->VI[REG_P].UL;
  vu->macflag = vu->VI[REG_MAC_FLAG].UL;
  vu->statusflag = vu->VI[REG_STATUS_FLAG].UL;
  vu->clipflag = vu->VI[REG_CLIP_FLAG].UL;
  vu->cycle = state[46];

  const u32 denormalized_status =
      ((vu->statusflag >> 3) & 0x18u) |
      ((vu->statusflag << 11) & 0x1800u) |
      ((vu->statusflag << 14) & 0x03cf0000u);
  for (u32 lane = 0; lane < 4; lane++) {
    vu->micro_macflags[lane] = vu->macflag;
    vu->micro_clipflags[lane] = vu->clipflag;
    vu->micro_statusflags[lane] = denormalized_status;
  }

  vu->branch = 0;
  vu->branchpc = 0;
  vu->delaybranchpc = 0;
  vu->takedelaybranch = false;
  vu->ebit = 0;
  vu->VIBackupCycles = 0;
  vu->VIOldValue = 0;
  vu->VIRegNumber = 0;
  std::memset(vu->fmac, 0, sizeof(vu->fmac));
  vu->fmacreadpos = 0;
  vu->fmacwritepos = 0;
  vu->fmaccount = 0;
  vu->fdiv = {};
  vu->efu = {};
  std::memset(vu->ialu, 0, sizeof(vu->ialu));
  vu->ialureadpos = 0;
  vu->ialuwritepos = 0;
  vu->ialucount = 0;
  vu->xgkickaddr = 0;
  vu->xgkickdiff = 0;
  vu->xgkicksizeremaining = 0;
  vu->xgkicklastcycle = 0;
  vu->xgkickcyclecount = 0;
  vu->xgkickenable = 0;
  vu->xgkickendpacket = 0;
  return true;
}

}  // namespace VitaGpuVu
