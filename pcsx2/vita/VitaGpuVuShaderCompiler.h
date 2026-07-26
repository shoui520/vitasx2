// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Threading.h"

#include <atomic>
#include <deque>
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

private:
  struct Request {
    ShaderKey key;
    std::string source;
  };

  static constexpr size_t MaxPendingRequests = 8;
  static constexpr size_t MaxCompletedResults = 8;
  static constexpr size_t MaxGeneratedSourceBytes = 512 * 1024;

  void WorkerMain();
  bool LoadCompilerModule();
  void UnloadCompilerModule();
  CompileResult Compile(const Request &request);
  bool HasKeyLocked(const ShaderKey &key) const;

  mutable std::mutex m_mutex;
  std::deque<Request> m_requests;
  std::deque<CompileResult> m_results;
  std::vector<ShaderKey> m_in_flight;
  std::string m_compiler_version;
  Threading::WorkSema m_work_sema;
  Threading::Thread m_thread;
  std::atomic<State> m_state{State::Stopped};
  std::atomic<bool> m_shutdown{false};
  s32 m_module_id = -1;
  bool m_system_module = false;
  bool m_extensions_enabled = false;
};
} // namespace VitaGpuVu
