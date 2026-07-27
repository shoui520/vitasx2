// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuShaderCompiler.h"

#include "common/Console.h"
#include "common/Timer.h"
#include "vita/VitaGsMailbox.h"

#include <psp2/kernel/clib.h>
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
// SceShaccCg's callback ABI has no user pointer. VitaSX2 owns one serialized
// compiler service, so its independently managed arena can be published here
// for the duration of that service.
std::atomic<void *> s_private_mspace{nullptr};
SceShaccCgSourceFile s_current_source{};
constexpr char s_source_name[] = "<vitasx2-generated-vu1-tfx>";
constexpr char s_open_error[] =
    "generated VU1 shaders do not permit external includes";
constexpr u8 InvalidSlot = 0xff;
constexpr size_t PrivateArenaGuardBytes = 64;
constexpr u8 PrivateArenaPrefixByte = 0xa5;
constexpr u8 PrivateArenaSuffixByte = 0x5a;
constexpr s32 MaximumCredibleDiagnosticCount = 4096;

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

// openFile lends ShaccCg the descriptor and immutable slot storage. The
// matching release callback deliberately does not release either object.
void ReleaseSource(const SceShaccCgSourceFile *,
                   const SceShaccCgCompileOptions *) {}

void *ShaccAllocate(unsigned int size) {
  void *const mspace = s_private_mspace.load(std::memory_order_acquire);
  return mspace ? sceClibMspaceMalloc(mspace, size) : nullptr;
}

void ShaccFree(void *pointer) {
  if (!pointer)
    return;
  void *const mspace = s_private_mspace.load(std::memory_order_acquire);
  if (mspace)
    sceClibMspaceFree(mspace, pointer);
}

size_t BoundedStringLength(const char *text, size_t capacity) {
  if (!text)
    return 0;
  size_t length = 0;
  while (length < capacity && text[length] != '\0')
    length++;
  return length;
}

constexpr const char *ExternalCompilerPaths[] = {
    "ur0:data/external/libshacccg.suprx",
    "ur0:data/external/libshaccCg.suprx",
    "ur0:data/libshacccg.suprx",
    "ur0:data/libshaccCg.suprx",
};
} // namespace

ShaderCompiler::~ShaderCompiler() { Stop(); }

bool ShaderCompiler::InitializePrivateArena() {
  const size_t allocation_size =
      PrivateArenaBytes + 2 * PrivateArenaGuardBytes;
  void *const backing = std::malloc(allocation_size);
  if (!backing)
    return false;

  u8 *const bytes = static_cast<u8 *>(backing);
  std::memset(bytes, PrivateArenaPrefixByte, PrivateArenaGuardBytes);
  std::memset(bytes + PrivateArenaGuardBytes + PrivateArenaBytes,
              PrivateArenaSuffixByte, PrivateArenaGuardBytes);
  void *const arena_base = bytes + PrivateArenaGuardBytes;
  void *const mspace = sceClibMspaceCreate(arena_base, PrivateArenaBytes);
  if (!mspace) {
    std::free(backing);
    return false;
  }

  void *expected = nullptr;
  if (!s_private_mspace.compare_exchange_strong(
          expected, mspace, std::memory_order_release,
          std::memory_order_relaxed)) {
    sceClibMspaceDestroy(mspace);
    std::free(backing);
    return false;
  }

  m_private_arena_backing = backing;
  m_private_mspace = mspace;
  m_private_arena_peak.store(0, std::memory_order_relaxed);
  m_private_arena_current.store(0, std::memory_order_relaxed);
  return true;
}

bool ShaderCompiler::PrivateArenaGuardsHold() const {
  if (!m_private_arena_backing)
    return true;
  const u8 *const bytes =
      static_cast<const u8 *>(m_private_arena_backing);
  for (size_t index = 0; index < PrivateArenaGuardBytes; index++) {
    if (bytes[index] != PrivateArenaPrefixByte ||
        bytes[PrivateArenaGuardBytes + PrivateArenaBytes + index] !=
            PrivateArenaSuffixByte) {
      return false;
    }
  }
  return true;
}

void ShaderCompiler::DestroyPrivateArena() {
  if (!m_private_arena_backing)
    return;

  if (!PrivateArenaGuardsHold()) {
    m_private_arena_guard_failures.fetch_add(1,
                                             std::memory_order_relaxed);
    Console.Error("GPU-VU: private ShaccCg arena boundary was overwritten.");
  }

  void *expected = m_private_mspace;
  (void)s_private_mspace.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel,
      std::memory_order_relaxed);
  if (m_private_mspace)
    sceClibMspaceDestroy(m_private_mspace);
  std::free(m_private_arena_backing);
  m_private_mspace = nullptr;
  m_private_arena_backing = nullptr;
}

bool ShaderCompiler::Start() {
  State expected = State::Stopped;
  if (!m_state.compare_exchange_strong(expected, State::Starting,
                                       std::memory_order_acq_rel)) {
    return expected == State::Starting || expected == State::Ready;
  }

  if (!InitializePrivateArena()) {
    m_state.store(State::Unavailable, std::memory_order_release);
    Console.Warning(
        "GPU-VU: private runtime-compiler arena could not be created; "
        "Maximum Cortex-A9 remains active.");
    return false;
  }

  m_shutdown.store(false, std::memory_order_release);
  m_service_state_pending.store(false, std::memory_order_relaxed);
  m_service_state_reported.store(false, std::memory_order_relaxed);
  m_startup_system_result.store(0, std::memory_order_relaxed);
  m_startup_external_result.store(0, std::memory_order_relaxed);
  m_startup_extension_result.store(0, std::memory_order_relaxed);
  m_startup_allocator_result.store(0, std::memory_order_relaxed);
  m_work_sema.Reset();
  m_thread.SetStackSize(512 * 1024);
  if (!m_thread.Start([this]() { WorkerMain(); })) {
    DestroyPrivateArena();
    m_state.store(State::Unavailable, std::memory_order_release);
    Console.Warning(
        "GPU-VU: low-priority ShaccCg compiler thread could not start.");
    return false;
  }
  return true;
}

void ShaderCompiler::ResetSlotLocked(CompileSlot *slot) {
  if (!slot)
    return;
  if (slot->source && m_private_mspace)
    sceClibMspaceFree(m_private_mspace, slot->source);
  if (slot->gxp && m_private_mspace)
    sceClibMspaceFree(m_private_mspace, slot->gxp);
  slot->state = SlotState::Free;
  slot->key = {};
  slot->source = nullptr;
  slot->source_size = 0;
  slot->gxp = nullptr;
  slot->gxp_size = 0;
  slot->diagnostic_count = 0;
  slot->total_diagnostic_count = 0;
  slot->compile_us = 0;
  slot->succeeded = false;
  slot->invalid_output = false;
  slot->diagnostics_truncated = false;
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

  {
    std::lock_guard lock(m_mutex);
    for (CompileSlot &slot : m_slots)
      ResetSlotLocked(&slot);
    m_request_read = 0;
    m_request_write = 0;
    m_request_count = 0;
    m_result_read = 0;
    m_result_write = 0;
    m_result_count = 0;
    m_compiler_version.fill('\0');
  }
  DestroyPrivateArena();
  m_state.store(State::Stopped, std::memory_order_release);
}

u8 ShaderCompiler::FindFreeSlotLocked() const {
  for (u8 index = 0; index < m_slots.size(); index++) {
    if (m_slots[index].state == SlotState::Free)
      return index;
  }
  return InvalidSlot;
}

void ShaderCompiler::PushRequestLocked(u8 slot) {
  m_request_queue[m_request_write] = slot;
  m_request_write =
      static_cast<u8>((m_request_write + 1) % MaxCompilerSlots);
  m_request_count++;
}

u8 ShaderCompiler::PopRequestLocked() {
  if (m_request_count == 0)
    return InvalidSlot;
  const u8 slot = m_request_queue[m_request_read];
  m_request_read =
      static_cast<u8>((m_request_read + 1) % MaxCompilerSlots);
  m_request_count--;
  return slot;
}

void ShaderCompiler::PushResultLocked(u8 slot) {
  m_result_queue[m_result_write] = slot;
  m_result_write =
      static_cast<u8>((m_result_write + 1) % MaxCompilerSlots);
  m_result_count++;
}

u8 ShaderCompiler::PopResultLocked() {
  if (m_result_count == 0)
    return InvalidSlot;
  const u8 slot = m_result_queue[m_result_read];
  m_result_read =
      static_cast<u8>((m_result_read + 1) % MaxCompilerSlots);
  m_result_count--;
  return slot;
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
    if (m_request_count >= MaxCompilerSlots || HasKeyLocked(key)) {
      (HasKeyLocked(key) ? m_rejected_duplicate : m_rejected_capacity)
          .fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const u8 slot_index = FindFreeSlotLocked();
    if (slot_index == InvalidSlot || !m_private_mspace) {
      m_rejected_capacity.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    char *const source_copy = static_cast<char *>(sceClibMspaceMalloc(
        m_private_mspace, source.size() + 1));
    if (!source_copy) {
      m_rejected_capacity.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    std::memcpy(source_copy, source.data(), source.size());
    source_copy[source.size()] = '\0';

    CompileSlot &slot = m_slots[slot_index];
    slot.state = SlotState::Pending;
    slot.key = key;
    slot.source = source_copy;
    slot.source_size = static_cast<u32>(source.size());
    slot.gxp = nullptr;
    slot.gxp_size = 0;
    slot.diagnostic_count = 0;
    slot.total_diagnostic_count = 0;
    slot.compile_us = 0;
    slot.succeeded = false;
    slot.invalid_output = false;
    slot.diagnostics_truncated = false;
    PushRequestLocked(slot_index);
  }
  m_accepted_submissions.fetch_add(1, std::memory_order_relaxed);
  m_work_sema.NotifyOfWork();
  return true;
}

void ShaderCompiler::ReportServiceState() {
  if (!m_service_state_pending.exchange(false, std::memory_order_acq_rel) ||
      m_service_state_reported.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  const State state = m_state.load(std::memory_order_acquire);
  if (state == State::Ready) {
    const std::string version = GetCompilerVersion();
    Console.WriteLn(
        "GPU-VU: asynchronous ShaccCg compiler ready (%s, private %u KiB "
        "arena).",
        version.empty() ? "unknown" : version.c_str(),
        static_cast<u32>(PrivateArenaBytes / 1024));
    return;
  }

  if (state == State::Unavailable) {
    Console.Warning(
        "GPU-VU: ShaccCg unavailable "
        "(sysmodule=%08x external=%08x extensions=%08x allocator=%08x); "
        "Maximum Cortex-A9 remains active.",
        static_cast<u32>(
            m_startup_system_result.load(std::memory_order_relaxed)),
        static_cast<u32>(
            m_startup_external_result.load(std::memory_order_relaxed)),
        static_cast<u32>(
            m_startup_extension_result.load(std::memory_order_relaxed)),
        static_cast<u32>(
            m_startup_allocator_result.load(std::memory_order_relaxed)));
  }
}

bool ShaderCompiler::Poll(CompileResult *result) {
  ReportServiceState();
  if (!result)
    return false;

  CompileResult published;
  u64 compile_us = 0;
  u32 source_size = 0;
  u32 total_diagnostics = 0;
  bool invalid_output = false;
  bool diagnostics_truncated = false;
  {
    std::lock_guard lock(m_mutex);
    const u8 slot_index = PopResultLocked();
    if (slot_index == InvalidSlot)
      return false;
    CompileSlot &slot = m_slots[slot_index];
    if (slot.state != SlotState::Completed) {
      m_dropped_results.fetch_add(1, std::memory_order_relaxed);
      ResetSlotLocked(&slot);
      return false;
    }

    published.key = slot.key;
    published.succeeded = slot.succeeded;
    if (slot.gxp && slot.gxp_size > 0)
      published.gxp.assign(slot.gxp, slot.gxp + slot.gxp_size);
    published.diagnostics.reserve(slot.diagnostic_count);
    for (u32 index = 0; index < slot.diagnostic_count; index++) {
      const FixedDiagnostic &fixed = slot.diagnostics[index];
      CompileDiagnostic diagnostic{};
      diagnostic.level = fixed.level;
      diagnostic.code = fixed.code;
      diagnostic.line = fixed.line;
      diagnostic.column = fixed.column;
      diagnostic.message.assign(fixed.message.data());
      published.diagnostics.push_back(std::move(diagnostic));
    }
    compile_us = slot.compile_us;
    source_size = slot.source_size;
    total_diagnostics = slot.total_diagnostic_count;
    invalid_output = slot.invalid_output;
    diagnostics_truncated = slot.diagnostics_truncated;
    ResetSlotLocked(&slot);
  }

  m_polled_results.fetch_add(1, std::memory_order_relaxed);
  Console.WriteLn(
      "GPU-VU: ShaccCg compile %s for %016llx%016llx in %llu us "
      "(%u source bytes, %u GXP bytes, %u diagnostics%s%s).",
      published.succeeded ? "completed" : "failed",
      static_cast<unsigned long long>(published.key.high),
      static_cast<unsigned long long>(published.key.low),
      static_cast<unsigned long long>(compile_us), source_size,
      static_cast<u32>(published.gxp.size()), total_diagnostics,
      invalid_output ? ", invalid output" : "",
      diagnostics_truncated ? ", diagnostics truncated" : "");
  *result = std::move(published);
  return true;
}

std::string ShaderCompiler::GetCompilerVersion() const {
  std::lock_guard lock(m_mutex);
  return std::string(m_compiler_version.data());
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
  stats.invalid_outputs = m_invalid_outputs.load(std::memory_order_relaxed);
  stats.truncated_diagnostics =
      m_truncated_diagnostics.load(std::memory_order_relaxed);
  stats.private_arena_capacity = PrivateArenaBytes;
  stats.private_arena_peak =
      m_private_arena_peak.load(std::memory_order_relaxed);
  stats.private_arena_current =
      m_private_arena_current.load(std::memory_order_relaxed);
  stats.private_arena_guard_failures =
      m_private_arena_guard_failures.load(std::memory_order_relaxed);
  std::lock_guard lock(m_mutex);
  stats.pending_requests = m_request_count;
  stats.completed_results = m_result_count;
  for (const CompileSlot &slot : m_slots)
    stats.in_flight_requests += slot.state != SlotState::Free;
  return stats;
}

bool ShaderCompiler::HasKeyLocked(const ShaderKey &key) const {
  for (const CompileSlot &slot : m_slots) {
    if (slot.state != SlotState::Free && slot.key == key)
      return true;
  }
  return false;
}

bool ShaderCompiler::LoadCompilerModule() {
  const int system_result = sceSysmoduleLoadModule(SCE_SYSMODULE_SHACCCG);
  m_startup_system_result.store(system_result, std::memory_order_relaxed);
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
        last_result = module;
        break;
      }
      last_result = module;
    }
    m_startup_external_result.store(last_result,
                                    std::memory_order_relaxed);
    if (m_module_id < 0)
      return false;
  }

  const int extension_result = sceShaccCgExtEnableExtensions();
  m_startup_extension_result.store(extension_result,
                                   std::memory_order_relaxed);
  if (extension_result < 0) {
    UnloadCompilerModule();
    return false;
  }
  m_extensions_enabled = true;

  const int allocator_result =
      sceShaccCgSetDefaultAllocator(ShaccAllocate, ShaccFree);
  m_startup_allocator_result.store(allocator_result,
                                   std::memory_order_relaxed);
  if (allocator_result < 0) {
    UnloadCompilerModule();
    return false;
  }

  const char *const version = sceShaccCgGetVersionString();
  {
    std::lock_guard lock(m_mutex);
    const size_t length =
        BoundedStringLength(version, m_compiler_version.size() - 1);
    if (length > 0)
      std::memcpy(m_compiler_version.data(), version, length);
    m_compiler_version[length] = '\0';
  }
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

void ShaderCompiler::Compile(CompileSlot *slot) {
  if (!slot)
    return;

  slot->gxp = nullptr;
  slot->gxp_size = 0;
  slot->diagnostic_count = 0;
  slot->total_diagnostic_count = 0;
  slot->succeeded = false;
  slot->invalid_output = false;
  slot->diagnostics_truncated = false;

  const auto append_synthetic_diagnostic =
      [slot](u32 code, const char *message) {
        slot->total_diagnostic_count++;
        if (slot->diagnostic_count >= MaxCompileDiagnostics) {
          slot->diagnostics_truncated = true;
          return;
        }
        FixedDiagnostic &diagnostic =
            slot->diagnostics[slot->diagnostic_count++];
        diagnostic = {};
        diagnostic.level = SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR;
        diagnostic.code = code;
        const size_t length =
            BoundedStringLength(message, diagnostic.message.size() - 1);
        if (length > 0)
          std::memcpy(diagnostic.message.data(), message, length);
        diagnostic.message[length] = '\0';
      };

  s_current_source.fileName = s_source_name;
  s_current_source.text = slot->source;
  s_current_source.size = slot->source_size;

  SceShaccCgCallbackList callbacks{};
  sceShaccCgInitializeCallbackList(&callbacks, SCE_SHACCCG_TRIVIAL);
  callbacks.openFile = OpenSource;
  callbacks.releaseFile = ReleaseSource;

  SceShaccCgCompileOptions options{};
  if (sceShaccCgInitializeCompileOptions(&options) < 0) {
    append_synthetic_diagnostic(
        0, "ShaccCg rejected generated compile options");
    s_current_source = {};
    return;
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

  const SceShaccCgCompileOutput *const output =
      sceShaccCgCompileProgram(&options, &callbacks, 0);
  if (!output) {
    append_synthetic_diagnostic(0, "ShaccCg returned no compile output");
    s_current_source = {};
    return;
  }

  const bool diagnostic_shape_valid =
      output->diagnosticCount >= 0 &&
      output->diagnosticCount <= MaximumCredibleDiagnosticCount &&
      (output->diagnosticCount == 0 || output->diagnostics);
  const bool program_shape_valid =
      output->programSize <= MaxGeneratedProgramBytes &&
      (output->programSize == 0 || output->programData);
  if (!diagnostic_shape_valid || !program_shape_valid) {
    slot->invalid_output = true;
    append_synthetic_diagnostic(
        0, "ShaccCg returned an invalid bounded output descriptor");
  } else {
    slot->total_diagnostic_count =
        static_cast<u32>(output->diagnosticCount);
    if (output->programSize > 0) {
      slot->gxp = static_cast<u8 *>(sceClibMspaceMalloc(
          m_private_mspace, output->programSize));
      if (slot->gxp) {
        std::memcpy(slot->gxp, output->programData,
                    output->programSize);
        slot->gxp_size = output->programSize;
        slot->succeeded = true;
      } else {
        append_synthetic_diagnostic(
            0, "private ShaccCg arena could not retain generated GXP");
      }
    }

    // Successful compiler warnings are telemetry only. Copy bounded text only
    // for a failed root, where the GS owner will report it.
    if (!slot->succeeded && output->diagnosticCount > 0) {
      const u32 copy_count = std::min<u32>(
          static_cast<u32>(output->diagnosticCount),
          MaxCompileDiagnostics);
      slot->diagnostic_count = copy_count;
      slot->diagnostics_truncated =
          static_cast<u32>(output->diagnosticCount) > copy_count;
      for (u32 index = 0; index < copy_count; index++) {
        const SceShaccCgDiagnosticMessage &source =
            output->diagnostics[index];
        FixedDiagnostic &diagnostic = slot->diagnostics[index];
        diagnostic = {};
        diagnostic.level = static_cast<u32>(source.level);
        diagnostic.code = source.code;
        if (source.location) {
          diagnostic.line = source.location->lineNumber;
          diagnostic.column = source.location->columnNumber;
        }
        const size_t length = BoundedStringLength(
            source.message, diagnostic.message.size() - 1);
        if (length > 0)
          std::memcpy(diagnostic.message.data(), source.message, length);
        diagnostic.message[length] = '\0';
      }
    }
  }

  sceShaccCgDestroyCompileOutput(output);
  s_current_source = {};
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
    if (!m_shutdown.load(std::memory_order_acquire))
      m_state.store(State::Unavailable, std::memory_order_release);
    m_service_state_pending.store(true, std::memory_order_release);
    VitaGS::NotifyGpuVuCompilerResult();
    return;
  }
  if (m_shutdown.load(std::memory_order_acquire)) {
    sceShaccCgReleaseCompiler();
    UnloadCompilerModule();
    return;
  }
  m_state.store(State::Ready, std::memory_order_release);
  m_service_state_pending.store(true, std::memory_order_release);
  VitaGS::NotifyGpuVuCompilerResult();

  while (!m_shutdown.load(std::memory_order_acquire)) {
    m_work_sema.WaitForWork();
    while (!m_shutdown.load(std::memory_order_acquire)) {
      u8 slot_index = InvalidSlot;
      {
        std::lock_guard lock(m_mutex);
        slot_index = PopRequestLocked();
        if (slot_index == InvalidSlot)
          break;
        m_slots[slot_index].state = SlotState::Compiling;
      }

      CompileSlot &slot = m_slots[slot_index];
      m_dequeued_requests.fetch_add(1, std::memory_order_relaxed);
      m_compile_starts.fetch_add(1, std::memory_order_relaxed);
      m_active_compiles.fetch_add(1, std::memory_order_relaxed);
      const Common::Timer::Value compile_start =
          Common::Timer::GetCurrentValue();
      Compile(&slot);
      const Common::Timer::Value compile_end =
          Common::Timer::GetCurrentValue();
      slot.compile_us = static_cast<u64>(
          Common::Timer::ConvertValueToSeconds(compile_end - compile_start) *
          1000000.0);
      if (slot.source) {
        sceClibMspaceFree(m_private_mspace, slot.source);
        slot.source = nullptr;
      }

      if (!PrivateArenaGuardsHold()) {
        slot.succeeded = false;
        slot.invalid_output = true;
        m_private_arena_guard_failures.fetch_add(
            1, std::memory_order_relaxed);
      }
      SceClibMspaceStats arena_stats{};
      sceClibMspaceMallocStats(m_private_mspace, &arena_stats);
      m_private_arena_peak.store(arena_stats.peak_in_use,
                                 std::memory_order_relaxed);
      m_private_arena_current.store(arena_stats.current_in_use,
                                    std::memory_order_relaxed);

      m_active_compiles.fetch_sub(1, std::memory_order_relaxed);
      m_compile_completions.fetch_add(1, std::memory_order_relaxed);
      (slot.succeeded ? m_compile_successes : m_compile_failures)
          .fetch_add(1, std::memory_order_relaxed);
      if (slot.invalid_output)
        m_invalid_outputs.fetch_add(1, std::memory_order_relaxed);
      if (slot.diagnostics_truncated)
        m_truncated_diagnostics.fetch_add(1,
                                          std::memory_order_relaxed);
      m_total_compile_us.fetch_add(slot.compile_us,
                                   std::memory_order_relaxed);
      u64 longest_compile_us =
          m_longest_compile_us.load(std::memory_order_relaxed);
      while (slot.compile_us > longest_compile_us &&
             !m_longest_compile_us.compare_exchange_weak(
                 longest_compile_us, slot.compile_us,
                 std::memory_order_relaxed,
                 std::memory_order_relaxed)) {
      }

      {
        std::lock_guard lock(m_mutex);
        slot.state = SlotState::Completed;
        PushResultLocked(slot_index);
      }
      // The GS mailbox may otherwise sleep indefinitely after a cold compile.
      // Publish first, then issue a one-way CPU work notification; the GS
      // owner remains the only thread which registers and patches the GXP.
      VitaGS::NotifyGpuVuCompilerResult();
    }
  }

  sceShaccCgReleaseCompiler();
  UnloadCompilerModule();
}
} // namespace VitaGpuVu
