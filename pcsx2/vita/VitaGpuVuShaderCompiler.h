// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Threading.h"

#include <array>
#include <atomic>
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
  u64 pending_requests = 0;
  u64 completed_results = 0;
  u64 in_flight_requests = 0;
  u64 active_compiles = 0;
  u64 invalid_outputs = 0;
  u64 truncated_diagnostics = 0;
  u64 private_arena_capacity = 0;
  u64 private_arena_peak = 0;
  u64 private_arena_current = 0;
  u64 private_arena_guard_failures = 0;
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
  void Stop();

  // Copies source into a bounded queue. This never waits for ShaccCg.
  bool Submit(const ShaderKey &key, std::string source);

  // Called by the GS worker. GXP registration is intentionally outside
  // this class so the owning GXM thread remains the only patcher caller.
  bool Poll(CompileResult *result);

  State GetState() const { return m_state.load(std::memory_order_acquire); }

  std::string GetCompilerVersion() const;
  ShaderCompilerStatistics GetStatistics() const;

private:
  static constexpr size_t MaxCompilerSlots = 8;
  static constexpr size_t MaxGeneratedSourceBytes = 512 * 1024;
  static constexpr size_t MaxGeneratedProgramBytes = 512 * 1024;
  static constexpr size_t MaxCompileDiagnostics = 64;
  static constexpr size_t MaxDiagnosticMessageBytes = 256;
  static constexpr size_t PrivateArenaBytes = 8 * 1024 * 1024;

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
    u8 *gxp = nullptr;
    u32 gxp_size = 0;
    std::array<FixedDiagnostic, MaxCompileDiagnostics> diagnostics{};
    u32 diagnostic_count = 0;
    u32 total_diagnostic_count = 0;
    u64 compile_us = 0;
    bool succeeded = false;
    bool invalid_output = false;
    bool diagnostics_truncated = false;
  };

  void WorkerMain();
  bool LoadCompilerModule();
  void UnloadCompilerModule();
  void Compile(CompileSlot *slot);
  bool InitializePrivateArena();
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

  mutable std::mutex m_mutex;
  std::array<CompileSlot, MaxCompilerSlots> m_slots{};
  std::array<u8, MaxCompilerSlots> m_request_queue{};
  std::array<u8, MaxCompilerSlots> m_result_queue{};
  u8 m_request_read = 0;
  u8 m_request_write = 0;
  u8 m_request_count = 0;
  u8 m_result_read = 0;
  u8 m_result_write = 0;
  u8 m_result_count = 0;
  std::array<char, 96> m_compiler_version{};
  void *m_private_arena_backing = nullptr;
  void *m_private_mspace = nullptr;
  Threading::WorkSema m_work_sema;
  Threading::Thread m_thread;
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
  std::atomic<u64> m_truncated_diagnostics{0};
  std::atomic<u64> m_private_arena_peak{0};
  std::atomic<u64> m_private_arena_current{0};
  std::atomic<u64> m_private_arena_guard_failures{0};
  s32 m_module_id = -1;
  bool m_system_module = false;
  bool m_extensions_enabled = false;
};
} // namespace VitaGpuVu
