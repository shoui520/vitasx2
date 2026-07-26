// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuShaderCompiler.h"

#include "common/Console.h"
#include "common/Timer.h"
#include "vita/VitaGsMailbox.h"

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/shacccg.h>
#include <psp2/sysmodule.h>
#include <shacccg_ext.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace VitaGpuVu {
namespace {
// SceShaccCg's callback ABI has no user pointer. Only the one serialized
// compiler thread touches this source descriptor.
SceShaccCgSourceFile s_current_source{};
constexpr char s_source_name[] = "<vitasx2-generated-vu1-tfx>";
constexpr char s_open_error[] =
    "generated VU1 shaders do not permit external includes";

SceShaccCgSourceFile *OpenSource(const char *file_name,
                                 const SceShaccCgSourceLocation *,
                                 const SceShaccCgCompileOptions *,
                                 const char **error) {
  if (file_name && std::strcmp(file_name, s_source_name) == 0)
    return &s_current_source;
  if (error)
    *error = s_open_error;
  return nullptr;
}

void *ShaccAllocate(unsigned int size) { return std::malloc(size); }

void ShaccFree(void *pointer) { std::free(pointer); }

constexpr const char *ExternalCompilerPaths[] = {
    "ur0:data/external/libshacccg.suprx",
    "ur0:data/external/libshaccCg.suprx",
    "ur0:data/libshacccg.suprx",
    "ur0:data/libshaccCg.suprx",
};
} // namespace

ShaderCompiler::~ShaderCompiler() { Stop(); }

bool ShaderCompiler::Start() {
  State expected = State::Stopped;
  if (!m_state.compare_exchange_strong(expected, State::Starting,
                                       std::memory_order_acq_rel)) {
    return expected == State::Starting || expected == State::Ready;
  }

  m_shutdown.store(false, std::memory_order_release);
  m_work_sema.Reset();
  m_thread.SetStackSize(512 * 1024);
  if (!m_thread.Start([this]() { WorkerMain(); })) {
    m_state.store(State::Unavailable, std::memory_order_release);
    Console.Warning(
        "GPU-VU: low-priority ShaccCg compiler thread could not start.");
    return false;
  }
  return true;
}

void ShaderCompiler::Stop() {
  const State state = m_state.load(std::memory_order_acquire);
  if (state == State::Stopped)
    return;

  m_state.store(State::Stopping, std::memory_order_release);
  m_shutdown.store(true, std::memory_order_release);
  m_work_sema.NotifyOfWork();
  if (m_thread.Joinable())
    m_thread.Join();

  std::lock_guard lock(m_mutex);
  m_requests.clear();
  m_results.clear();
  m_in_flight.clear();
  m_compiler_version.clear();
  m_state.store(State::Stopped, std::memory_order_release);
}

bool ShaderCompiler::Submit(const ShaderKey &key, std::string source) {
  m_submission_attempts.fetch_add(1, std::memory_order_relaxed);
  const State state = m_state.load(std::memory_order_acquire);
  if (state != State::Starting && state != State::Ready) {
    m_rejected_state.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (source.empty() || source.size() > MaxGeneratedSourceBytes) {
    m_rejected_source.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  {
    std::lock_guard lock(m_mutex);
    if (m_requests.size() >= MaxPendingRequests ||
        m_in_flight.size() >= MaxPendingRequests + MaxCompletedResults) {
      m_rejected_capacity.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (HasKeyLocked(key)) {
      m_rejected_duplicate.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    m_in_flight.push_back(key);
    m_requests.push_back({key, std::move(source)});
  }
  m_accepted_submissions.fetch_add(1, std::memory_order_relaxed);
  m_work_sema.NotifyOfWork();
  return true;
}

bool ShaderCompiler::Poll(CompileResult *result) {
  if (!result)
    return false;

  std::lock_guard lock(m_mutex);
  if (m_results.empty())
    return false;
  *result = std::move(m_results.front());
  m_results.pop_front();
  const auto it =
      std::find(m_in_flight.begin(), m_in_flight.end(), result->key);
  if (it != m_in_flight.end())
    m_in_flight.erase(it);
  m_polled_results.fetch_add(1, std::memory_order_relaxed);
  return true;
}

std::string ShaderCompiler::GetCompilerVersion() const {
  std::lock_guard lock(m_mutex);
  return m_compiler_version;
}

ShaderCompilerStatistics ShaderCompiler::GetStatistics() const {
  ShaderCompilerStatistics stats;
  stats.submission_attempts =
      m_submission_attempts.load(std::memory_order_relaxed);
  stats.accepted_submissions =
      m_accepted_submissions.load(std::memory_order_relaxed);
  stats.rejected_state = m_rejected_state.load(std::memory_order_relaxed);
  stats.rejected_source = m_rejected_source.load(std::memory_order_relaxed);
  stats.rejected_capacity =
      m_rejected_capacity.load(std::memory_order_relaxed);
  stats.rejected_duplicate =
      m_rejected_duplicate.load(std::memory_order_relaxed);
  stats.dequeued_requests =
      m_dequeued_requests.load(std::memory_order_relaxed);
  stats.compile_starts = m_compile_starts.load(std::memory_order_relaxed);
  stats.compile_completions =
      m_compile_completions.load(std::memory_order_relaxed);
  stats.compile_successes =
      m_compile_successes.load(std::memory_order_relaxed);
  stats.compile_failures = m_compile_failures.load(std::memory_order_relaxed);
  stats.polled_results = m_polled_results.load(std::memory_order_relaxed);
  stats.dropped_results = m_dropped_results.load(std::memory_order_relaxed);
  stats.total_compile_us =
      m_total_compile_us.load(std::memory_order_relaxed);
  stats.longest_compile_us =
      m_longest_compile_us.load(std::memory_order_relaxed);
  stats.active_compiles = m_active_compiles.load(std::memory_order_relaxed);
  std::lock_guard lock(m_mutex);
  stats.pending_requests = m_requests.size();
  stats.completed_results = m_results.size();
  stats.in_flight_requests = m_in_flight.size();
  return stats;
}

bool ShaderCompiler::HasKeyLocked(const ShaderKey &key) const {
  return std::find(m_in_flight.begin(), m_in_flight.end(), key) !=
         m_in_flight.end();
}

bool ShaderCompiler::LoadCompilerModule() {
  const int system_result = sceSysmoduleLoadModule(SCE_SYSMODULE_SHACCCG);
  if (system_result >= 0) {
    m_system_module = true;
    m_module_id = 0;
  } else {
    int last_result = system_result;
    for (const char *path : ExternalCompilerPaths) {
      const SceUID module =
          sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
      if (module >= 0) {
        m_module_id = module;
        m_system_module = false;
        break;
      }
      last_result = module;
    }
    if (m_module_id < 0) {
      Console.Warning(
          "GPU-VU: ShaccCg unavailable (sysmodule=%08x external=%08x); "
          "Maximum Cortex-A9 remains active.",
          static_cast<u32>(system_result), static_cast<u32>(last_result));
      return false;
    }
  }

  if (sceShaccCgExtEnableExtensions() < 0) {
    Console.Warning("GPU-VU: SceShaccCgExt rejected the installed compiler; "
                    "Maximum Cortex-A9 remains active.");
    UnloadCompilerModule();
    return false;
  }
  m_extensions_enabled = true;

  if (sceShaccCgSetDefaultAllocator(ShaccAllocate, ShaccFree) < 0) {
    Console.Warning("GPU-VU: ShaccCg allocator initialization failed; "
                    "Maximum Cortex-A9 remains active.");
    UnloadCompilerModule();
    return false;
  }

  const char *version = sceShaccCgGetVersionString();
  {
    std::lock_guard lock(m_mutex);
    m_compiler_version = version ? version : "unknown";
  }
  Console.WriteLn("GPU-VU: asynchronous ShaccCg compiler ready (%s).",
                  version ? version : "unknown");
  return true;
}

void ShaderCompiler::UnloadCompilerModule() {
  if (m_extensions_enabled) {
    sceShaccCgExtDisableExtensions();
    m_extensions_enabled = false;
  }
  if (m_module_id < 0)
    return;

  if (m_system_module)
    (void)sceSysmoduleUnloadModule(SCE_SYSMODULE_SHACCCG);
  else
    (void)sceKernelStopUnloadModule(m_module_id, 0, nullptr, 0, nullptr,
                                    nullptr);
  m_module_id = -1;
  m_system_module = false;
}

CompileResult ShaderCompiler::Compile(const Request &request) {
  CompileResult result{};
  result.key = request.key;

  s_current_source.fileName = s_source_name;
  s_current_source.text = request.source.c_str();
  s_current_source.size = static_cast<SceUInt32>(request.source.size());

  SceShaccCgCallbackList callbacks{};
  sceShaccCgInitializeCallbackList(&callbacks, SCE_SHACCCG_TRIVIAL);
  callbacks.openFile = OpenSource;

  SceShaccCgCompileOptions options{};
  if (sceShaccCgInitializeCompileOptions(&options) < 0) {
    s_current_source = {};
    return result;
  }
  options.mainSourceFile = s_source_name;
  options.targetProfile = SCE_SHACCCG_PROFILE_VP;
  options.entryFunctionName = "main";
  options.useFx = 1;
  options.locale = SCE_SHACCCG_ENGLISH;
  options.optimizationLevel = 3;
  options.useFastmath = 0;
  options.useFastprecision = 0;
  options.useFastint = 0;
  options.warningLevel = 1;
  options.performanceWarnings = 1;

  const SceShaccCgCompileOutput *output =
      sceShaccCgCompileProgram(&options, &callbacks, 0);
  if (!output) {
    s_current_source = {};
    return result;
  }

  if (output->diagnosticCount > 0 && output->diagnostics) {
    result.diagnostics.reserve(output->diagnosticCount);
    for (s32 i = 0; i < output->diagnosticCount; i++) {
      const SceShaccCgDiagnosticMessage &source = output->diagnostics[i];
      CompileDiagnostic diagnostic{};
      diagnostic.level = static_cast<u32>(source.level);
      diagnostic.code = source.code;
      if (source.location) {
        diagnostic.line = source.location->lineNumber;
        diagnostic.column = source.location->columnNumber;
      }
      if (source.message)
        diagnostic.message = source.message;
      result.diagnostics.push_back(std::move(diagnostic));
    }
  }

  if (output->programData && output->programSize > 0) {
    result.gxp.assign(output->programData,
                      output->programData + output->programSize);
    result.succeeded = true;
  }
  sceShaccCgDestroyCompileOutput(output);
  s_current_source = {};
  return result;
}

void ShaderCompiler::WorkerMain() {
  const int current_priority = sceKernelGetThreadCurrentPriority();
  if (current_priority >= 0) {
    // Vita priorities increase toward lower scheduling priority. Keep
    // runtime compilation behind emulation and the GS worker.
    (void)sceKernelChangeThreadPriority(sceKernelGetThreadId(),
                                        current_priority + 0x20);
  }

  if (!LoadCompilerModule()) {
    m_state.store(State::Unavailable, std::memory_order_release);
    return;
  }
  m_state.store(State::Ready, std::memory_order_release);

  while (!m_shutdown.load(std::memory_order_acquire)) {
    m_work_sema.WaitForWork();
    while (!m_shutdown.load(std::memory_order_acquire)) {
      Request request;
      {
        std::lock_guard lock(m_mutex);
        if (m_requests.empty())
          break;
        request = std::move(m_requests.front());
        m_requests.pop_front();
      }

      m_dequeued_requests.fetch_add(1, std::memory_order_relaxed);
      m_compile_starts.fetch_add(1, std::memory_order_relaxed);
      m_active_compiles.fetch_add(1, std::memory_order_relaxed);
      const Common::Timer::Value compile_start =
          Common::Timer::GetCurrentValue();
      Console.WriteLn(
          "GPU-VU: ShaccCg compile started for %016llx%016llx (%u source "
          "bytes).",
          static_cast<unsigned long long>(request.key.high),
          static_cast<unsigned long long>(request.key.low),
          static_cast<u32>(request.source.size()));
      CompileResult result = Compile(request);
      const Common::Timer::Value compile_end = Common::Timer::GetCurrentValue();
      const u64 compile_us = static_cast<u64>(
          Common::Timer::ConvertValueToSeconds(compile_end - compile_start) *
          1000000.0);
      m_active_compiles.fetch_sub(1, std::memory_order_relaxed);
      m_compile_completions.fetch_add(1, std::memory_order_relaxed);
      (result.succeeded ? m_compile_successes : m_compile_failures)
          .fetch_add(1, std::memory_order_relaxed);
      m_total_compile_us.fetch_add(compile_us, std::memory_order_relaxed);
      u64 longest_compile_us =
          m_longest_compile_us.load(std::memory_order_relaxed);
      while (compile_us > longest_compile_us &&
             !m_longest_compile_us.compare_exchange_weak(
                 longest_compile_us, compile_us, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {
      }
      Console.WriteLn(
          "GPU-VU: ShaccCg compile %s for %016llx%016llx in %llu us "
          "(%u GXP bytes, %u diagnostics).",
          result.succeeded ? "completed" : "failed",
          static_cast<unsigned long long>(request.key.high),
          static_cast<unsigned long long>(request.key.low),
          static_cast<unsigned long long>(compile_us),
          static_cast<u32>(result.gxp.size()),
          static_cast<u32>(result.diagnostics.size()));
      bool result_published = false;
      {
        std::lock_guard lock(m_mutex);
        if (m_results.size() < MaxCompletedResults) {
          m_results.push_back(std::move(result));
          result_published = true;
        } else {
          const auto it =
              std::find(m_in_flight.begin(), m_in_flight.end(), request.key);
          if (it != m_in_flight.end())
            m_in_flight.erase(it);
          m_dropped_results.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // The GS mailbox may otherwise sleep indefinitely after a cold compile.
      // Publish first, then issue a one-way CPU work notification; the GS
      // owner remains the only thread which registers and patches the GXP.
      if (result_published)
        VitaGS::NotifyGpuVuCompilerResult();
    }
  }

  sceShaccCgReleaseCompiler();
  UnloadCompilerModule();
}
} // namespace VitaGpuVu
