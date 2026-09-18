// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Threading.h"
#include "vita/VitaGpuVuGxpResource.h"

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace VitaGpuVu {
struct ShaderKey {
  u64 low = 0;
  u64 high = 0;

  bool operator==(const ShaderKey &other) const {
    return low == other.low && high == other.high;
  }
};

struct CompileDiagnostic {
  u32 level = 0;
  u32 code = 0;
  u32 line = 0;
  u32 column = 0;
  std::string message;
};

struct CompileResult {
  ShaderKey key;
  bool succeeded = false;
  std::vector<u8> gxp;
  std::vector<CompileDiagnostic> diagnostics;
  GeneratedGxpResourceUsage gxp_resources;
  GeneratedGxpResourceAttestation gxp_resource_attestation =
      GeneratedGxpResourceAttestation::MissingInput;
};

struct ShaderCompilerStatistics {
  u64 submission_attempts = 0;
  u64 accepted_submissions = 0;
  u64 rejected_state = 0;
  u64 rejected_source = 0;
  u64 rejected_capacity = 0;
  u64 rejected_duplicate = 0;
  u64 dequeued_requests = 0;
  u64 compile_starts = 0;
  u64 compile_completions = 0;
  u64 compile_successes = 0;
  u64 compile_failures = 0;
  u64 polled_results = 0;
  u64 dropped_results = 0;
  u64 total_compile_us = 0;
  u64 longest_compile_us = 0;
  u64 worker_cpu_us = 0;
  u64 pending_requests = 0;
  u64 completed_results = 0;
  u64 in_flight_requests = 0;
  u64 active_compiles = 0;
  u64 invalid_outputs = 0;
  u64 resource_rejected_outputs = 0;
  u64 truncated_diagnostics = 0;
  u64 private_arena_capacity = 0;
  u64 private_arena_peak = 0;
  u64 private_arena_current = 0;
  u64 private_arena_resets = 0;
  u64 private_arena_guard_failures = 0;
  u64 allocator_failures = 0;
  u64 last_failed_allocation_bytes = 0;
  u64 worker_generations = 0;
  u64 recovery_attempts = 0;
  u64 recovery_successes = 0;
  u64 module_unload_failures = 0;
  u64 cancelled_requests = 0;
  u64 cancelled_planning_tasks = 0;
  u64 planning_submission_attempts = 0;
  u64 accepted_planning_tasks = 0;
  u64 rejected_planning_state = 0;
  u64 rejected_planning_capacity = 0;
  u64 coalesced_planning_tasks = 0;
  u64 planning_starts = 0;
  u64 planning_completions = 0;
  u64 total_planning_us = 0;
  u64 longest_planning_us = 0;
  u64 pending_planning_tasks = 0;
  u64 active_planning_tasks = 0;
  u64 persistent_cache_hits = 0;
  u64 persistent_cache_misses = 0;
  u64 persistent_cache_writes = 0;
  u64 persistent_cache_write_failures = 0;
  u64 persistent_cache_invalid = 0;
  s32 worker_priority_before = -1;
  s32 worker_priority_after = -1;
  s32 worker_priority_result = -1;
  s32 worker_affinity_result = -1;
};

// One bounded, serialized compiler for generated VU1+TFX vertex programs.
// ShaccCg is deliberately never called by the GS worker. Completed GXP is
// copied out of Shacc-owned storage and only registered/patched by the GS
// worker which polls this service.
class ShaderCompiler final {
public:
  enum class State : u8 {
    Stopped,
    Starting,
    Ready,
    Unavailable,
    Stopping,
  };

  ShaderCompiler() = default;
  ShaderCompiler(const ShaderCompiler &) = delete;
  ShaderCompiler &operator=(const ShaderCompiler &) = delete;
  ~ShaderCompiler();

  bool Start();
  // Product startup reserves the installed compiler module before optional
  // mapped GPU owners consume the last free physical pages. This waits only
  // for module/extension initialization; shader compilation remains entirely
  // asynchronous and never delays a guest epoch.
  bool StartAndWaitUntilReady();
  void Stop();

  // Copies source into a bounded queue. This never waits for ShaccCg.
  bool Submit(
      const ShaderKey &key, std::string source,
      GeneratedGxpExecutionRequirement execution_requirement =
          GeneratedGxpExecutionRequirement::Any);

  // Queues title-neutral PairPlan/source partitioning on the same
  // low-priority USER_0 worker as ShaccCg. Planning and compilation are
  // serialized so neither can transiently double the generated provider's
  // memory/CPU footprint, and an emulation thread never executes the task.
  // Accepted work is either run or cancelled exactly once. Cancellation runs
  // outside the queue lock, including Stop(); rejection calls neither closure.
  // Duplicate identities coalesce with the already-owned task.
  bool SubmitPlanning(u64 identity, std::function<void()> work,
                      std::function<void()> cancel = {});
  bool HasPlanning(u64 identity) const;

  // Called by the GS worker. GXP registration is intentionally outside
  // this class so the owning GXM thread remains the only patcher caller.
  bool Poll(CompileResult *result);

  State GetState() const { return m_state.load(std::memory_order_acquire); }

  std::string GetCompilerVersion() const;
  ShaderCompilerStatistics GetStatistics() const;

private:
  static constexpr size_t MaxCompilerSlots = 8;
  static constexpr size_t MaxPlanningTasks = 4;
  static constexpr size_t MaxGeneratedSourceBytes = 512 * 1024;
  static constexpr size_t MaxGeneratedProgramBytes = 512 * 1024;
  static constexpr size_t MaxCompileDiagnostics = 64;
  static constexpr size_t MaxDiagnosticMessageBytes = 256;
  // Runtime compilation owns generated per-program roots; the complete
  // scheduled interpreter remains an offline sidecar. Sony documents a
  // 16 MiB maximum LPDDR memblock, so one dedicated cached physically
  // contiguous block owns the compiler instead of taking a 24 MiB bite out
  // of newlib's general heap. Sony's Memory Management tutorial specifies
  // that physically contiguous LPDDR is otherwise equivalent to ordinary
  // LPDDR. This permanent startup arena is a valid use of that separate
  // 26 MiB pool and leaves cached USER_RW pages for the immutable VIF ring.
  // Physical BSpline roots exhausted 12-14 MiB arenas before producing GXP;
  // leave only the two guard lines outside the mspace payload.
  static constexpr size_t PrivateArenaBackingBytes = 16 * 1024 * 1024;
  static constexpr size_t PrivateArenaBytes =
      PrivateArenaBackingBytes - 2 * 64;

  enum class SlotState : u8 {
    Free,
    Pending,
    Compiling,
    Completed,
  };

  struct FixedDiagnostic {
    u32 level = 0;
    u32 code = 0;
    u32 line = 0;
    u32 column = 0;
    std::array<char, MaxDiagnosticMessageBytes> message{};
  };

  struct CompileSlot {
    SlotState state = SlotState::Free;
    ShaderKey key;
    char *source = nullptr;
    u32 source_size = 0;
    u32 optimization_level = 3;
    u8 *gxp = nullptr;
    u32 gxp_size = 0;
    std::array<FixedDiagnostic, MaxCompileDiagnostics> diagnostics{};
    u32 diagnostic_count = 0;
    u32 total_diagnostic_count = 0;
    u64 compile_us = 0;
    bool succeeded = false;
    bool persistent_cache_hit = false;
    bool invalid_output = false;
    bool resource_rejected_output = false;
    bool diagnostics_truncated = false;
    GeneratedGxpExecutionRequirement execution_requirement =
        GeneratedGxpExecutionRequirement::Any;
    GeneratedGxpResourceUsage gxp_resources;
    GeneratedGxpResourceAttestation gxp_resource_attestation =
        GeneratedGxpResourceAttestation::MissingInput;
  };

  struct PlanningTask {
    u64 identity = 0;
    std::function<void()> work;
    std::function<void()> cancel;
  };

  void SupervisorMain();
  bool WorkerMain(bool recovering);
  void FailPendingWork();
  void CancelPlanningTask(PlanningTask& task);
  void RecordThreadStart(bool supervisor);
  void RecordThreadEnd(bool supervisor);
  bool LoadCompilerModule();
  bool UnloadCompilerModule();
  void Compile(CompileSlot *slot);
  bool LoadPersistentCache(CompileSlot *slot);
  void StorePersistentCache(const CompileSlot& slot);
  void StoreAuditSource(const CompileSlot& slot);
  void StoreAuditArtifact(const CompileSlot& slot);
  bool InitializePrivateArena();
  bool ResetPrivateArena();
  void DestroyPrivateArena();
  bool PrivateArenaGuardsHold() const;
  void ResetSlotLocked(CompileSlot *slot);
  u8 FindFreeSlotLocked() const;
  u8 PopRequestLocked();
  u8 PopResultLocked();
  void PushRequestLocked(u8 slot);
  void PushResultLocked(u8 slot);
  void ReportServiceState();
  bool HasKeyLocked(const ShaderKey &key) const;
  bool HasPlanningLocked(u64 identity) const;

  mutable Threading::KernelMutex m_mutex;
  std::array<CompileSlot, MaxCompilerSlots> m_slots{};
  std::array<u8, MaxCompilerSlots> m_request_queue{};
  std::array<u8, MaxCompilerSlots> m_result_queue{};
  u8 m_request_read = 0;
  u8 m_request_write = 0;
  u8 m_request_count = 0;
  u8 m_result_read = 0;
  u8 m_result_write = 0;
  u8 m_result_count = 0;
  std::deque<PlanningTask> m_planning_tasks;
  u64 m_active_planning_identity = 0;
  std::array<char, 96> m_compiler_version{};
  void *m_private_arena_backing = nullptr;
  void *m_private_mspace = nullptr;
  s32 m_private_arena_uid = -1;
  Threading::WorkSema m_work_sema;
  Threading::KernelSemaphore m_startup_sema;
  Threading::Thread m_thread;
  // Protected by m_mutex. Retire handles before joining/deleting their threads
  // so telemetry never queries a recycled SceUID, or loses recovery CPU time.
  Threading::ThreadHandle m_live_worker;
  Threading::ThreadHandle m_live_supervisor;
  u64 m_retired_thread_cpu_us = 0;
  std::atomic<State> m_state{State::Stopped};
  std::atomic<bool> m_shutdown{false};
  std::atomic<bool> m_service_state_pending{false};
  std::atomic<bool> m_service_state_reported{false};
  std::atomic<s32> m_startup_system_result{0};
  std::atomic<s32> m_startup_external_result{0};
  std::atomic<s32> m_startup_extension_result{0};
  std::atomic<s32> m_startup_allocator_result{0};
  std::atomic<u64> m_submission_attempts{0};
  std::atomic<u64> m_accepted_submissions{0};
  std::atomic<u64> m_rejected_state{0};
  std::atomic<u64> m_rejected_source{0};
  std::atomic<u64> m_rejected_capacity{0};
  std::atomic<u64> m_rejected_duplicate{0};
  std::atomic<u64> m_dequeued_requests{0};
  std::atomic<u64> m_compile_starts{0};
  std::atomic<u64> m_compile_completions{0};
  std::atomic<u64> m_compile_successes{0};
  std::atomic<u64> m_compile_failures{0};
  std::atomic<u64> m_polled_results{0};
  std::atomic<u64> m_dropped_results{0};
  std::atomic<u64> m_total_compile_us{0};
  std::atomic<u64> m_longest_compile_us{0};
  std::atomic<u64> m_active_compiles{0};
  std::atomic<u64> m_invalid_outputs{0};
  std::atomic<u64> m_resource_rejected_outputs{0};
  std::atomic<u64> m_truncated_diagnostics{0};
  std::atomic<u64> m_private_arena_peak{0};
  std::atomic<u64> m_private_arena_current{0};
  std::atomic<u64> m_private_arena_resets{0};
  std::atomic<u64> m_private_arena_guard_failures{0};
  std::atomic<u64> m_allocator_failures{0};
  std::atomic<u64> m_last_failed_allocation_bytes{0};
  std::atomic<u64> m_worker_generations{0};
  std::atomic<u64> m_recovery_attempts{0};
  std::atomic<u64> m_recovery_successes{0};
  std::atomic<u64> m_module_unload_failures{0};
  std::atomic<u64> m_cancelled_requests{0};
  std::atomic<u64> m_cancelled_planning_tasks{0};
  std::atomic<u64> m_planning_submission_attempts{0};
  std::atomic<u64> m_accepted_planning_tasks{0};
  std::atomic<u64> m_rejected_planning_state{0};
  std::atomic<u64> m_rejected_planning_capacity{0};
  std::atomic<u64> m_coalesced_planning_tasks{0};
  std::atomic<u64> m_planning_starts{0};
  std::atomic<u64> m_planning_completions{0};
  std::atomic<u64> m_total_planning_us{0};
  std::atomic<u64> m_longest_planning_us{0};
  std::atomic<u64> m_active_planning_tasks{0};
  std::atomic<u64> m_persistent_cache_hits{0};
  std::atomic<u64> m_persistent_cache_misses{0};
  std::atomic<u64> m_persistent_cache_writes{0};
  std::atomic<u64> m_persistent_cache_write_failures{0};
  std::atomic<u64> m_persistent_cache_invalid{0};
  std::atomic<s32> m_worker_priority_before{-1};
  std::atomic<s32> m_worker_priority_after{-1};
  std::atomic<s32> m_worker_priority_result{-1};
  std::atomic<s32> m_worker_affinity_result{-1};
  u64 m_compiler_identity = 0;
  bool m_persistent_cache_ready = false;
  bool m_audit_artifact_archive_ready = false;
  u32 m_audit_artifact_sequence = 0;
  s32 m_module_id = -1;
  bool m_system_module = false;
  bool m_extensions_enabled = false;
};
} // namespace VitaGpuVu
