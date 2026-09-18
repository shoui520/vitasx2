// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuShaderCompiler.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Threading.h"
#include "common/Timer.h"
#include "vita/VitaGsMailbox.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/shacccg.h>
#include <psp2/sysmodule.h>
#include <shacccg_ext.h>
#include <taihen.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace VitaGpuVu {
namespace {
// SceShaccCg's callback ABI has no user pointer. VitaSX2 owns one serialized
// compiler service, so its independently managed arena can be published here
// for the duration of that service.
std::atomic<void *> s_private_mspace{nullptr};
// Shacc's allocator has no context pointer. Only the serialized generation
// writes these counters; atomics also make asynchronous telemetry unambiguous.
std::atomic<u32> s_allocation_failures{0};
std::atomic<u32> s_first_failed_allocation_bytes{0};
SceShaccCgSourceFile s_current_source{};
constexpr char s_source_name[] = "<vitasx2-generated-vu1-tfx>";
constexpr char s_open_error[] =
    "generated VU1 shaders do not permit external includes";
constexpr u8 InvalidSlot = 0xff;
constexpr size_t PrivateArenaGuardBytes = 64;
constexpr u8 PrivateArenaPrefixByte = 0xa5;
constexpr u8 PrivateArenaSuffixByte = 0x5a;
constexpr s32 MaximumCredibleDiagnosticCount = 4096;
constexpr char DynamicPath1OptimizationMarker[] =
    "VitaSX2 generated dynamic PATH1 optimization ceiling O1";
constexpr char LoopKernelOptimizationMarker[] =
    "VitaSX2 generated loop-kernel optimization ceiling O1";
constexpr char LoopKernelExactOptimizationMarker[] =
    "VitaSX2 generated loop-kernel exact optimization ceiling O0";
constexpr char ControlFlowRegionOptimizationMarker[] =
    "VitaSX2 generated CFG region optimization ceiling O1";
constexpr char ControlFlowRegionO3OptimizationMarker[] =
    "VitaSX2 generated CFG region optimization ceiling O3";
constexpr char StructuredStateOptimizationMarker[] =
    "VitaSX2 generated structured state ceiling O1";
constexpr char StructuredNativeStateOptimizationMarker[] =
    "VitaSX2 generated structured native state ceiling O3";
constexpr char StructuredExactMarker[] =
    "VitaSX2 generated structured exact ceiling O0";
constexpr char StructuredParallelOptimizationMarker[] =
    "VitaSX2 generated structured parallel ceiling O1";
constexpr char StructuredNativeParallelOptimizationMarker[] =
    "VitaSX2 generated structured native parallel ceiling O3";
constexpr char PrivateInPlaceMemoryO2OptimizationMarker[] =
    "VitaSX2 generated private in-place memory ceiling O2";
#ifndef VITASX2_GPU_VU_COMPILER_CACHE_DIRECTORY
#define VITASX2_GPU_VU_COMPILER_CACHE_DIRECTORY "ux0:data/vitasx2/cache/gpu-vu"
#endif
constexpr char PersistentCacheDirectory[] = VITASX2_GPU_VU_COMPILER_CACHE_DIRECTORY;
// One structured program may legally contain 256 independently compiled
// title-neutral partitions.  A 32-entry cache guaranteed that a complete
// bundle evicted its own early dependencies before the next launch, turning
// every cold attestation into repeated Shacc work.  Keep room for one maximum
// structured bundle plus ordinary generated programs while retaining a hard
// on-disk bound.
constexpr u32 PersistentCacheSlotCount = 512;
constexpr u32 PersistentCacheMagic = 0x47585056u; // "VXPG"
constexpr u32 PersistentCacheFormat = 1;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
// The executable cache deliberately excludes spilling, per-instance, and
// otherwise unprofitable compiler outputs. Keep those exact Shacc artifacts
// available to the post-run psp2shaderperf audit without allowing them to
// displace a usable program. The compiler is serialized and a validation run
// which emits more than this bounded number of roots is split by the harness.
#ifndef VITASX2_GPU_VU_COMPILER_AUDIT_DIRECTORY
#define VITASX2_GPU_VU_COMPILER_AUDIT_DIRECTORY "ux0:data/vitasx2/cache/gpu-vu-audit"
#endif
constexpr char AuditArtifactDirectory[] = VITASX2_GPU_VU_COMPILER_AUDIT_DIRECTORY;
constexpr u32 AuditArtifactSlotCount = 64;
#endif

struct PersistentCacheHeader {
  u32 magic = PersistentCacheMagic;
  u32 format = PersistentCacheFormat;
  u64 key_low = 0;
  u64 key_high = 0;
  u64 compiler_identity = 0;
  u64 gxp_hash = 0;
  u32 gxp_size = 0;
  u32 reserved = 0;
};
static_assert(sizeof(PersistentCacheHeader) == 48);

u64 HashBytes(const void* bytes, size_t size) {
  constexpr u64 Offset = 14695981039346656037ull;
  constexpr u64 Prime = 1099511628211ull;
  u64 hash = Offset;
  const u8* const input = static_cast<const u8*>(bytes);
  for (size_t index = 0; index < size; index++) {
    hash ^= input[index];
    hash *= Prime;
  }
  return hash;
}

u32 PersistentCacheHomeSlot(const ShaderKey& key) {
  return static_cast<u32>((key.low ^ key.high) % PersistentCacheSlotCount);
}

std::array<char, 96> PersistentCachePath(u32 slot) {
  std::array<char, 96> path{};
  std::snprintf(path.data(), path.size(), "%s/slot-%02u.bin",
                PersistentCacheDirectory, slot % PersistentCacheSlotCount);
  return path;
}

enum class PersistentCacheProbe : u8 {
  Missing,
  Invalid,
  Header,
};

// Runtime Shacc work starts when the Vita process has the least spare
// general-heap headroom.  FileSystem::ReadBinaryFile() materializes every
// probed cache file in a throwing std::vector; r199 consequently terminated
// the compiler worker with std::bad_alloc while an already-attested product
// was running.  Probe only the fixed header and copy a matching payload
// directly into the CompileSlot allocation.
PersistentCacheProbe ReadPersistentCacheHeader(
    const char* path, PersistentCacheHeader* header) {
  if (!path || !header)
    return PersistentCacheProbe::Invalid;
  std::FILE* const file = std::fopen(path, "rb");
  if (!file)
    return PersistentCacheProbe::Missing;
  const bool read =
      std::fread(header, sizeof(*header), 1, file) == 1;
  const bool closed = std::fclose(file) == 0;
  return read && closed ? PersistentCacheProbe::Header :
                          PersistentCacheProbe::Invalid;
}

bool ReadPersistentCachePayload(const char* path,
                                const PersistentCacheHeader& expected,
                                u8* payload) {
  if (!path || !payload || expected.gxp_size == 0u)
    return false;
  std::FILE* const file = std::fopen(path, "rb");
  if (!file)
    return false;
  PersistentCacheHeader actual;
  const bool header_read =
      std::fread(&actual, sizeof(actual), 1, file) == 1;
  const bool header_matches =
      header_read && std::memcmp(&actual, &expected, sizeof(actual)) == 0;
  const bool payload_read =
      header_matches &&
      std::fread(payload, 1, expected.gxp_size, file) == expected.gxp_size;
  const bool exact_end = payload_read && std::fgetc(file) == EOF;
  const bool closed = std::fclose(file) == 0;
  return exact_end && closed;
}

bool WritePersistentCachePayload(const char* path,
                                 const PersistentCacheHeader& header,
                                 const u8* payload) {
  if (!path || !payload || header.gxp_size == 0u)
    return false;
  std::FILE* const file = std::fopen(path, "wb");
  if (!file)
    return false;
  const bool header_written =
      std::fwrite(&header, sizeof(header), 1, file) == 1;
  const bool payload_written =
      header_written &&
      std::fwrite(payload, 1, header.gxp_size, file) == header.gxp_size;
  const bool closed = std::fclose(file) == 0;
  return payload_written && closed;
}

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
std::array<char, 112> AuditArtifactPath(u32 slot) {
  std::array<char, 112> path{};
  std::snprintf(path.data(), path.size(), "%s/slot-%02u.bin",
                AuditArtifactDirectory, slot % AuditArtifactSlotCount);
  return path;
}

std::array<char, 112> AuditSourcePath(const ShaderKey& key) {
  std::array<char, 112> path{};
  const u32 slot = static_cast<u32>((key.low ^ key.high) %
                                    AuditArtifactSlotCount);
  std::snprintf(path.data(), path.size(), "%s/source-%02u.cg",
                AuditArtifactDirectory, slot);
  return path;
}
#endif

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
  void *const pointer = mspace ? sceClibMspaceMalloc(mspace, size) : nullptr;
  if (!pointer) {
    if (s_allocation_failures.fetch_add(1, std::memory_order_relaxed) == 0)
      s_first_failed_allocation_bytes.store(size, std::memory_order_relaxed);
  }
  return pointer;
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

constexpr char ExternalCompilerPath[] = "ur0:data/libshacccg.suprx";
} // namespace

ShaderCompiler::~ShaderCompiler() { Stop(); }

bool ShaderCompiler::InitializePrivateArena() {
  static_assert(PrivateArenaBackingBytes ==
                PrivateArenaBytes + 2 * PrivateArenaGuardBytes);
  const SceUID uid = sceKernelAllocMemBlock(
      "VitaSX2 ShaccCg arena",
      SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW,
      PrivateArenaBackingBytes, nullptr);
  if (uid < 0)
    return false;

  void *backing = nullptr;
  const int base_result = sceKernelGetMemBlockBase(uid, &backing);
  if (base_result < 0 || !backing) {
    sceKernelFreeMemBlock(uid);
    return false;
  }

  u8 *const bytes = static_cast<u8 *>(backing);
  std::memset(bytes, PrivateArenaPrefixByte, PrivateArenaGuardBytes);
  std::memset(bytes + PrivateArenaGuardBytes + PrivateArenaBytes,
              PrivateArenaSuffixByte, PrivateArenaGuardBytes);
  void *const arena_base = bytes + PrivateArenaGuardBytes;
  void *const mspace = sceClibMspaceCreate(arena_base, PrivateArenaBytes);
  if (!mspace) {
    sceKernelFreeMemBlock(uid);
    return false;
  }

  void *expected = nullptr;
  if (!s_private_mspace.compare_exchange_strong(
          expected, mspace, std::memory_order_release,
          std::memory_order_relaxed)) {
    sceClibMspaceDestroy(mspace);
    sceKernelFreeMemBlock(uid);
    return false;
  }

  m_private_arena_backing = backing;
  m_private_mspace = mspace;
  m_private_arena_uid = uid;
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

bool ShaderCompiler::ResetPrivateArena() {
  if (!m_private_arena_backing || !m_private_mspace)
    return false;

  const bool guards_hold = PrivateArenaGuardsHold();
  if (!guards_hold) {
    m_private_arena_guard_failures.fetch_add(1,
                                             std::memory_order_relaxed);
    Console.Error("GPU-VU: private ShaccCg arena boundary was overwritten.");
    return false;
  }

  void *expected = m_private_mspace;
  if (!s_private_mspace.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return false;
  }

  sceClibMspaceDestroy(m_private_mspace);
  m_private_mspace = nullptr;

  u8 *const bytes = static_cast<u8 *>(m_private_arena_backing);
  std::memset(bytes, PrivateArenaPrefixByte, PrivateArenaGuardBytes);
  std::memset(bytes + PrivateArenaGuardBytes + PrivateArenaBytes,
              PrivateArenaSuffixByte, PrivateArenaGuardBytes);
  void *const arena_base = bytes + PrivateArenaGuardBytes;
  void *const mspace = sceClibMspaceCreate(arena_base, PrivateArenaBytes);
  if (!mspace)
    return false;

  expected = nullptr;
  if (!s_private_mspace.compare_exchange_strong(
          expected, mspace, std::memory_order_release,
          std::memory_order_relaxed)) {
    sceClibMspaceDestroy(mspace);
    return false;
  }

  m_private_mspace = mspace;
  m_private_arena_current.store(0, std::memory_order_relaxed);
  m_private_arena_resets.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void ShaderCompiler::DestroyPrivateArena() {
  if (!m_private_arena_backing)
    return;
  // A failed unload may leave callbacks and compiler graph pointers live.
  // Retain this one bounded arena until process exit rather than freeing
  // storage still owned by the module. Its global callback blocks a new owner.
  if (m_module_id >= 0) {
    Console.Error("GPU-VU: compiler module not retired; retaining its arena.");
    return;
  }

  if (!PrivateArenaGuardsHold()) {
    m_private_arena_guard_failures.fetch_add(1,
                                             std::memory_order_relaxed);
    Console.Error("GPU-VU: private ShaccCg arena boundary was overwritten.");
  }

  void *expected = m_private_mspace;
  (void)s_private_mspace.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel,
      std::memory_order_relaxed);
  if (m_private_mspace && PrivateArenaGuardsHold())
    sceClibMspaceDestroy(m_private_mspace);
  if (m_private_arena_uid >= 0)
    sceKernelFreeMemBlock(m_private_arena_uid);
  m_private_mspace = nullptr;
  m_private_arena_backing = nullptr;
  m_private_arena_uid = -1;
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
  while (m_startup_sema.TryWait()) {
  }
  m_work_sema.Reset();
  m_audit_artifact_sequence = 0;
  m_thread.SetStackSize(64 * 1024);
  bool started = false;
  try {
    started = m_thread.Start([this]() { SupervisorMain(); });
  } catch (const std::bad_alloc&) {
    started = false;
  }
  if (!started) {
    DestroyPrivateArena();
    m_state.store(State::Unavailable, std::memory_order_release);
    Console.Warning(
        "GPU-VU: low-priority ShaccCg compiler thread could not start.");
    return false;
  }
  return true;
}

bool ShaderCompiler::StartAndWaitUntilReady() {
  if (!Start())
    return false;
  while (m_state.load(std::memory_order_acquire) == State::Starting)
    m_startup_sema.Wait();
  return m_state.load(std::memory_order_acquire) == State::Ready;
}

void ShaderCompiler::ResetSlotLocked(CompileSlot *slot) {
  if (!slot)
    return;
  std::free(slot->source);
  std::free(slot->gxp);
  slot->state = SlotState::Free;
  slot->key = {};
  slot->source = nullptr;
  slot->source_size = 0;
  slot->optimization_level = 3;
  slot->gxp = nullptr;
  slot->gxp_size = 0;
  slot->diagnostic_count = 0;
  slot->total_diagnostic_count = 0;
  slot->compile_us = 0;
  slot->succeeded = false;
  slot->persistent_cache_hit = false;
  slot->invalid_output = false;
  slot->resource_rejected_output = false;
  slot->diagnostics_truncated = false;
  slot->execution_requirement = GeneratedGxpExecutionRequirement::Any;
  slot->gxp_resources = {};
  slot->gxp_resource_attestation =
      GeneratedGxpResourceAttestation::MissingInput;
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

  // No compiler or planner is still running. Cancel outside m_mutex: owners
  // may acquire registry locks which are above the submission queue lock.
  FailPendingWork();
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
    m_active_planning_identity = 0u;
    m_compiler_version.fill('\0');
  }
  DestroyPrivateArena();
  m_state.store(m_module_id < 0 ? State::Stopped : State::Unavailable,
                 std::memory_order_release);
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

bool ShaderCompiler::Submit(
    const ShaderKey &key, std::string source,
    GeneratedGxpExecutionRequirement execution_requirement) {
  m_submission_attempts.fetch_add(1, std::memory_order_relaxed);
  if (source.empty() || source.size() > MaxGeneratedSourceBytes) {
    m_rejected_source.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Most retail windows never request a generated GPU-VU shader. Keep the
  // compiler's bounded private arena and worker stack out of their permanent
  // LPDDR/heap footprint. RequestGeneratedProgram() is serialized by the
  // registry mutex, and Start() publishes the arena before it returns, so the
  // first cold miss can queue immediately while module loading continues on
  // the low-priority worker.
  State state = m_state.load(std::memory_order_acquire);
  if (state == State::Stopped) {
    if (!Start()) {
      m_rejected_state.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    state = m_state.load(std::memory_order_acquire);
  }
  if (state != State::Starting && state != State::Ready) {
    m_rejected_state.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  {
    std::lock_guard lock(m_mutex);
    state = m_state.load(std::memory_order_acquire);
    if (state != State::Starting && state != State::Ready) {
      m_rejected_state.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (m_request_count >= MaxCompilerSlots || HasKeyLocked(key)) {
      (HasKeyLocked(key) ? m_rejected_duplicate : m_rejected_capacity)
          .fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const u8 slot_index = FindFreeSlotLocked();
    if (slot_index == InvalidSlot) {
      m_rejected_capacity.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    char *const source_copy =
        static_cast<char *>(std::malloc(source.size() + 1));
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
    const bool explicit_o3 =
        std::strstr(source_copy, StructuredNativeStateOptimizationMarker) ||
        std::strstr(source_copy, StructuredNativeParallelOptimizationMarker) ||
        std::strstr(source_copy, ControlFlowRegionO3OptimizationMarker);
    const bool explicit_o2 = std::strstr(
        source_copy, PrivateInPlaceMemoryO2OptimizationMarker);
    const bool explicit_o0 =
        std::strstr(source_copy, StructuredExactMarker) ||
        std::strstr(source_copy, LoopKernelExactOptimizationMarker);
    slot.optimization_level = explicit_o0 ? 0u
                              : explicit_o3 ? 3u
                              : explicit_o2 ? 2u
                              : (std::strstr(
                                     source_copy,
                                     StructuredParallelOptimizationMarker) ||
                                 std::strstr(source_copy,
                                             StructuredStateOptimizationMarker) ||
                                 std::strstr(source_copy,
                                             LoopKernelOptimizationMarker) ||
                                 std::strstr(source_copy,
                                             DynamicPath1OptimizationMarker) ||
                                 std::strstr(source_copy,
                                             ControlFlowRegionOptimizationMarker))
                                  ? 1u
                                  : 3u;
    slot.gxp = nullptr;
    slot.gxp_size = 0;
    slot.diagnostic_count = 0;
    slot.total_diagnostic_count = 0;
    slot.compile_us = 0;
    slot.succeeded = false;
    slot.invalid_output = false;
    slot.resource_rejected_output = false;
    slot.diagnostics_truncated = false;
    slot.execution_requirement = execution_requirement;
    slot.gxp_resources = {};
    slot.gxp_resource_attestation =
        GeneratedGxpResourceAttestation::MissingInput;
    PushRequestLocked(slot_index);
  }
  m_accepted_submissions.fetch_add(1, std::memory_order_relaxed);
  m_work_sema.NotifyOfWork();
  return true;
}

bool ShaderCompiler::SubmitPlanning(u64 identity,
                                    std::function<void()> work,
                                    std::function<void()> cancel) {
  m_planning_submission_attempts.fetch_add(1, std::memory_order_relaxed);
  if (identity == 0u || !work) {
    m_rejected_planning_state.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  State state = m_state.load(std::memory_order_acquire);
  if (state == State::Stopped) {
    if (!Start()) {
      m_rejected_planning_state.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    state = m_state.load(std::memory_order_acquire);
  }
  if (state != State::Starting && state != State::Ready) {
    m_rejected_planning_state.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  {
    std::lock_guard lock(m_mutex);
    state = m_state.load(std::memory_order_acquire);
    if (state != State::Starting && state != State::Ready) {
      m_rejected_planning_state.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (HasPlanningLocked(identity)) {
      m_coalesced_planning_tasks.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    if (m_planning_tasks.size() >= MaxPlanningTasks) {
      m_rejected_planning_capacity.fetch_add(1,
                                             std::memory_order_relaxed);
      return false;
    }
    try {
      m_planning_tasks.push_back(
          {identity, std::move(work), std::move(cancel)});
    } catch (const std::bad_alloc&) {
      m_rejected_planning_capacity.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  m_accepted_planning_tasks.fetch_add(1, std::memory_order_relaxed);
  m_work_sema.NotifyOfWork();
  return true;
}

bool ShaderCompiler::HasPlanning(u64 identity) const {
  if (identity == 0u)
    return false;
  std::lock_guard lock(m_mutex);
  return HasPlanningLocked(identity);
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
  u32 optimization_level = 3;
  u32 total_diagnostics = 0;
  bool invalid_output = false;
  bool diagnostics_truncated = false;
  bool persistent_cache_hit = false;
  u64 arena_peak = 0;
  u64 arena_current = 0;
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
    published.gxp_resources = slot.gxp_resources;
    published.gxp_resource_attestation = slot.gxp_resource_attestation;
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
    optimization_level = slot.optimization_level;
    total_diagnostics = slot.total_diagnostic_count;
    invalid_output = slot.invalid_output;
    diagnostics_truncated = slot.diagnostics_truncated;
    persistent_cache_hit = slot.persistent_cache_hit;
    arena_peak = m_private_arena_peak.load(std::memory_order_relaxed);
    arena_current = m_private_arena_current.load(std::memory_order_relaxed);
    ResetSlotLocked(&slot);
  }

  m_polled_results.fetch_add(1, std::memory_order_relaxed);
  Console.WriteLn(
      "GPU-VU: generated GXP %s for %016llx%016llx in %llu us "
      "(%u source bytes, %u GXP bytes, O%u, %u diagnostics%s%s, "
      "resource=%s format=%u.%u sdk=%03x primary_instructions=%u "
      "secondary_instructions=%u registers=%u/%u/%u scratch=%u "
      "thread=%u literal=%u mode=%s branches=%u/%u "
      "arena_peak=%llu arena_current=%llu).",
      persistent_cache_hit ? "persistent-cache-hit" :
          (published.succeeded ? "compiled" : "compile-failed"),
      static_cast<unsigned long long>(published.key.high),
      static_cast<unsigned long long>(published.key.low),
      static_cast<unsigned long long>(compile_us), source_size,
      static_cast<u32>(published.gxp.size()), optimization_level,
      total_diagnostics,
      invalid_output ? ", invalid output" : "",
      diagnostics_truncated ? ", diagnostics truncated" : "",
      GeneratedGxpResourceAttestationName(
          published.gxp_resource_attestation),
      published.gxp_resources.major_version,
      published.gxp_resources.minor_version,
      published.gxp_resources.sdk_version,
      published.gxp_resources.primary_instruction_count,
      published.gxp_resources.secondary_instruction_count,
      published.gxp_resources.primary_register_count,
      published.gxp_resources.temporary_register_count,
      published.gxp_resources.secondary_register_count,
      published.gxp_resources.scratch_buffer_size,
      published.gxp_resources.thread_buffer_size,
      published.gxp_resources.literal_buffer_size,
      published.gxp_resources.UsesPerInstanceExecution()
          ? "per-instance"
          : "parallel",
      published.gxp_resources.primary_branch_instruction_count,
      published.gxp_resources.secondary_branch_instruction_count,
      static_cast<unsigned long long>(arena_peak),
      static_cast<unsigned long long>(arena_current));
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
  stats.resource_rejected_outputs =
      m_resource_rejected_outputs.load(std::memory_order_relaxed);
  stats.truncated_diagnostics =
      m_truncated_diagnostics.load(std::memory_order_relaxed);
  stats.private_arena_peak =
      m_private_arena_peak.load(std::memory_order_relaxed);
  stats.private_arena_current =
      m_private_arena_current.load(std::memory_order_relaxed);
  stats.private_arena_resets =
      m_private_arena_resets.load(std::memory_order_relaxed);
  stats.private_arena_guard_failures =
      m_private_arena_guard_failures.load(std::memory_order_relaxed);
  stats.allocator_failures = m_allocator_failures.load(std::memory_order_relaxed);
  stats.last_failed_allocation_bytes =
      m_last_failed_allocation_bytes.load(std::memory_order_relaxed);
  stats.worker_generations = m_worker_generations.load(std::memory_order_relaxed);
  stats.recovery_attempts = m_recovery_attempts.load(std::memory_order_relaxed);
  stats.recovery_successes = m_recovery_successes.load(std::memory_order_relaxed);
  stats.module_unload_failures =
      m_module_unload_failures.load(std::memory_order_relaxed);
  stats.cancelled_requests = m_cancelled_requests.load(std::memory_order_relaxed);
  stats.cancelled_planning_tasks =
      m_cancelled_planning_tasks.load(std::memory_order_relaxed);
  stats.planning_submission_attempts =
      m_planning_submission_attempts.load(std::memory_order_relaxed);
  stats.accepted_planning_tasks =
      m_accepted_planning_tasks.load(std::memory_order_relaxed);
  stats.rejected_planning_state =
      m_rejected_planning_state.load(std::memory_order_relaxed);
  stats.rejected_planning_capacity =
      m_rejected_planning_capacity.load(std::memory_order_relaxed);
  stats.coalesced_planning_tasks =
      m_coalesced_planning_tasks.load(std::memory_order_relaxed);
  stats.planning_starts =
      m_planning_starts.load(std::memory_order_relaxed);
  stats.planning_completions =
      m_planning_completions.load(std::memory_order_relaxed);
  stats.total_planning_us =
      m_total_planning_us.load(std::memory_order_relaxed);
  stats.longest_planning_us =
      m_longest_planning_us.load(std::memory_order_relaxed);
  stats.active_planning_tasks =
      m_active_planning_tasks.load(std::memory_order_relaxed);
  stats.persistent_cache_hits =
      m_persistent_cache_hits.load(std::memory_order_relaxed);
  stats.persistent_cache_misses =
      m_persistent_cache_misses.load(std::memory_order_relaxed);
  stats.persistent_cache_writes =
      m_persistent_cache_writes.load(std::memory_order_relaxed);
  stats.persistent_cache_write_failures =
      m_persistent_cache_write_failures.load(std::memory_order_relaxed);
  stats.persistent_cache_invalid =
      m_persistent_cache_invalid.load(std::memory_order_relaxed);
  stats.worker_priority_before =
      m_worker_priority_before.load(std::memory_order_relaxed);
  stats.worker_priority_after =
      m_worker_priority_after.load(std::memory_order_relaxed);
  stats.worker_priority_result =
      m_worker_priority_result.load(std::memory_order_relaxed);
  stats.worker_affinity_result =
      m_worker_affinity_result.load(std::memory_order_relaxed);
  std::lock_guard lock(m_mutex);
  stats.worker_cpu_us = m_retired_thread_cpu_us +
      m_live_worker.GetCPUTime() + m_live_supervisor.GetCPUTime();
  stats.private_arena_capacity =
      m_private_arena_backing ? PrivateArenaBytes : 0;
  stats.pending_requests = m_request_count;
  stats.completed_results = m_result_count;
  stats.pending_planning_tasks = m_planning_tasks.size();
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

bool ShaderCompiler::HasPlanningLocked(u64 identity) const {
  if (identity == 0u)
    return false;
  if (m_active_planning_identity == identity)
    return true;
  return std::any_of(
      m_planning_tasks.begin(), m_planning_tasks.end(),
      [identity](const PlanningTask& task) {
        return task.identity == identity;
      });
}

bool ShaderCompiler::LoadCompilerModule() {
  s_allocation_failures.store(0, std::memory_order_relaxed);
  s_first_failed_allocation_bytes.store(0, std::memory_order_relaxed);
  const int system_result = sceSysmoduleLoadModule(SCE_SYSMODULE_SHACCCG);
  m_startup_system_result.store(system_result, std::memory_order_relaxed);
  if (system_result >= 0) {
    m_system_module = true;
    m_module_id = 0;
    tai_module_info_t info{};
    info.size = sizeof(info);
    if (taiGetModuleInfo("SceShaccCg", &info) < 0 || info.modid <= 0) {
      UnloadCompilerModule();
      return false;
    }
    m_module_id = info.modid;
  } else {
    const SceUID module = sceKernelLoadStartModule(
        ExternalCompilerPath, 0, nullptr, 0, nullptr, nullptr);
    m_startup_external_result.store(module,
                                    std::memory_order_relaxed);
    if (module < 0)
      return false;
    m_module_id = module;
    m_system_module = false;
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
  size_t version_length = 0;
  {
    std::lock_guard lock(m_mutex);
    version_length =
        BoundedStringLength(version, m_compiler_version.size() - 1);
    if (version_length > 0)
      std::memcpy(m_compiler_version.data(), version, version_length);
    m_compiler_version[version_length] = '\0';
  }
  m_compiler_identity = HashBytes(version, version_length);
  m_persistent_cache_ready = FileSystem::EnsureDirectoryExists(
      PersistentCacheDirectory, true, nullptr);
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
  m_audit_artifact_archive_ready = FileSystem::EnsureDirectoryExists(
      AuditArtifactDirectory, true, nullptr);
#endif
  Console.WriteLn(
      "GPU-VU: generated GXP persistent cache %s slots=%u "
      "compiler_identity=%016llx.",
      m_persistent_cache_ready ? "ready" : "unavailable",
      PersistentCacheSlotCount,
      static_cast<unsigned long long>(m_compiler_identity));
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
  Console.WriteLn(
      "GPU-VU: generated GXP audit archive %s slots=%u; every fresh bounded "
      "Shacc output is retained for post-run psp2shaderperf.",
      m_audit_artifact_archive_ready ? "ready" : "unavailable",
      AuditArtifactSlotCount);
#endif
  return true;
}

bool ShaderCompiler::UnloadCompilerModule() {
  m_persistent_cache_ready = false;
  m_audit_artifact_archive_ready = false;
  m_compiler_identity = 0;
  if (m_extensions_enabled) {
    sceShaccCgExtDisableExtensions();
    m_extensions_enabled = false;
  }
  if (m_module_id < 0)
    return true;

  const int result = m_system_module
      ? sceSysmoduleUnloadModule(SCE_SYSMODULE_SHACCCG)
      : sceKernelStopUnloadModule(m_module_id, 0, nullptr, 0, nullptr, nullptr);
  SceKernelModuleInfo info{};
  info.size = sizeof(info);
  const int lookup = m_module_id > 0
      ? sceKernelGetModuleInfo(m_module_id, &info)
      : sceSysmoduleIsLoaded(SCE_SYSMODULE_SHACCCG);
  const bool absent = m_module_id > 0
      ? static_cast<u32>(lookup) == SCE_KERNEL_ERROR_INVALID_UID
      : static_cast<u32>(lookup) == SCE_SYSMODULE_ERROR_UNLOADED;
  Console.WriteLn(
      "GPU-VU: ShaccCg retirement module=%d system=%u result=%08x lookup=%08x.",
      m_module_id, m_system_module ? 1u : 0u,
      static_cast<u32>(result), static_cast<u32>(lookup));
  if (result < 0 || !absent) {
    m_module_unload_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  m_module_id = -1;
  m_system_module = false;
  return true;
}

bool ShaderCompiler::LoadPersistentCache(CompileSlot* slot) {
  if (!slot || !m_persistent_cache_ready)
    return false;
  const u32 home = PersistentCacheHomeSlot(slot->key);
  for (u32 probe = 0; probe < PersistentCacheSlotCount; probe++) {
    const u32 cache_slot = (home + probe) % PersistentCacheSlotCount;
    const std::array<char, 96> path = PersistentCachePath(cache_slot);
    PersistentCacheHeader header;
    const PersistentCacheProbe cache_probe =
        ReadPersistentCacheHeader(path.data(), &header);
    // StorePersistentCache() uses the first vacant slot in this probe chain,
    // so a vacant path terminates a normal lookup.  If a user manually removes
    // a file from the cache, an entry beyond the hole can merely miss and be
    // regenerated; cache layout never determines semantic support.
    if (cache_probe == PersistentCacheProbe::Missing)
      break;
    if (cache_probe != PersistentCacheProbe::Header) {
      m_persistent_cache_invalid.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (header.magic != PersistentCacheMagic ||
        header.format != PersistentCacheFormat ||
        header.compiler_identity != m_compiler_identity) {
      m_persistent_cache_invalid.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (header.key_low != slot->key.low ||
        header.key_high != slot->key.high) {
      continue;
    }
    if (header.gxp_size == 0 ||
        header.gxp_size > MaxGeneratedProgramBytes) {
      m_persistent_cache_invalid.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    slot->gxp = static_cast<u8*>(std::malloc(header.gxp_size));
    if (!slot->gxp)
      break;
    if (!ReadPersistentCachePayload(path.data(), header, slot->gxp) ||
        HashBytes(slot->gxp, header.gxp_size) != header.gxp_hash) {
      std::free(slot->gxp);
      slot->gxp = nullptr;
      m_persistent_cache_invalid.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    slot->gxp_size = header.gxp_size;
    slot->succeeded = true;
    slot->persistent_cache_hit = true;
    m_persistent_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  m_persistent_cache_misses.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void ShaderCompiler::StorePersistentCache(const CompileSlot& slot) {
  // Cache the compiler artifact independently from the current execution
  // policy.  A structurally valid GXP which spills, uses per-thread backing,
  // or otherwise misses today's performance gate is still expensive to
  // reproduce and may be useful to a bounded validation profile.  Every load
  // is parsed and re-attested before registration, so retaining the bytes
  // cannot turn a policy-rejected program into an accepted provider.
  const bool structurally_valid_artifact =
      slot.gxp_resource_attestation ==
          GeneratedGxpResourceAttestation::Accepted ||
      slot.gxp_resource_attestation ==
          GeneratedGxpResourceAttestation::ScratchSpill ||
      slot.gxp_resource_attestation ==
          GeneratedGxpResourceAttestation::PerThreadBacking ||
      slot.gxp_resource_attestation ==
          GeneratedGxpResourceAttestation::PerInstanceExecution ||
      slot.gxp_resource_attestation ==
          GeneratedGxpResourceAttestation::DynamicFlowControl;
  if (!m_persistent_cache_ready || !slot.gxp || slot.gxp_size == 0 ||
      slot.invalid_output || !structurally_valid_artifact)
    return;
  PersistentCacheHeader header;
  header.key_low = slot.key.low;
  header.key_high = slot.key.high;
  header.compiler_identity = m_compiler_identity;
  header.gxp_hash = HashBytes(slot.gxp, slot.gxp_size);
  header.gxp_size = slot.gxp_size;
  // Diagnostic hint only. LoadPersistentCache() never trusts this prior
  // decision and always parses and re-attests the GXP for the current use.
  header.reserved = static_cast<u32>(slot.gxp_resource_attestation);
  const u32 home = PersistentCacheHomeSlot(slot.key);
  u32 destination = home;
  u32 first_reusable = PersistentCacheSlotCount;
  bool found_exact = false;
  for (u32 probe = 0; probe < PersistentCacheSlotCount; probe++) {
    const u32 cache_slot = (home + probe) % PersistentCacheSlotCount;
    const std::array<char, 96> candidate_path =
        PersistentCachePath(cache_slot);
    PersistentCacheHeader candidate_header;
    const PersistentCacheProbe cache_probe =
        ReadPersistentCacheHeader(candidate_path.data(), &candidate_header);
    if (cache_probe == PersistentCacheProbe::Missing) {
      first_reusable = cache_slot;
      break;
    }
    if (cache_probe != PersistentCacheProbe::Header) {
      if (first_reusable == PersistentCacheSlotCount)
        first_reusable = cache_slot;
      continue;
    }
    if (candidate_header.magic != PersistentCacheMagic ||
        candidate_header.format != PersistentCacheFormat ||
        candidate_header.compiler_identity != m_compiler_identity) {
      if (first_reusable == PersistentCacheSlotCount)
        first_reusable = cache_slot;
      continue;
    }
    if (candidate_header.key_low == slot.key.low &&
        candidate_header.key_high == slot.key.high) {
      destination = cache_slot;
      found_exact = true;
      break;
    }
  }
  // All bounded slots are valid and occupied by other keys. Evict only the
  // deterministic home slot; lookup still probes the complete bounded set.
  if (!found_exact && first_reusable != PersistentCacheSlotCount)
    destination = first_reusable;
  const std::array<char, 96> path = PersistentCachePath(destination);
  if (WritePersistentCachePayload(path.data(), header, slot.gxp))
    m_persistent_cache_writes.fetch_add(1, std::memory_order_relaxed);
  else
    m_persistent_cache_write_failures.fetch_add(1,
                                                std::memory_order_relaxed);
}

void ShaderCompiler::StoreAuditSource(const CompileSlot& slot) {
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
  if (!m_audit_artifact_archive_ready || !slot.source ||
      slot.source_size == 0u || slot.source_size > MaxGeneratedSourceBytes)
    return;

  // The executable cache is keyed by this exact source/configuration digest.
  // Keep one bounded collision-replacing source corpus beside the GXP archive
  // so the host Vita3K-Shacc runner can reproduce libshaccCg code generation,
  // optimization failures and resource usage without another Vita launch.
  const std::array<char, 112> path = AuditSourcePath(slot.key);
  const bool stored = FileSystem::WriteBinaryFile(
      path.data(), slot.source, slot.source_size);
  Console.WriteLn(
      "GPU-VU: generated Cg audit source %s key=%016llx%016llx "
      "bytes=%u path=%s.",
      stored ? "stored" : "write-failed",
      static_cast<unsigned long long>(slot.key.high),
      static_cast<unsigned long long>(slot.key.low), slot.source_size,
      path.data());
#else
  (void)slot;
#endif
}

void ShaderCompiler::StoreAuditArtifact(const CompileSlot& slot) {
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
  if (!m_audit_artifact_archive_ready || slot.persistent_cache_hit ||
      slot.invalid_output || !slot.gxp || slot.gxp_size == 0)
    return;

  const u32 artifact_slot =
      m_audit_artifact_sequence++ % AuditArtifactSlotCount;
  PersistentCacheHeader header;
  header.key_low = slot.key.low;
  header.key_high = slot.key.high;
  header.compiler_identity = m_compiler_identity;
  header.gxp_hash = HashBytes(slot.gxp, slot.gxp_size);
  header.gxp_size = slot.gxp_size;
  // The persistent-cache ABI reserves this word as zero. The isolated audit
  // archive records the resource decision so the host can prove that a
  // rejected GXP was inspected without mistaking it for executable cache.
  header.reserved = static_cast<u32>(slot.gxp_resource_attestation);
  const std::array<char, 112> path = AuditArtifactPath(artifact_slot);
  const bool stored =
      WritePersistentCachePayload(path.data(), header, slot.gxp);
  Console.WriteLn(
      "GPU-VU: generated GXP audit artifact %s key=%016llx%016llx "
      "slot=%u bytes=%u fnv1a64=%016llx resource=%s.",
      stored ? "stored" : "write-failed",
      static_cast<unsigned long long>(slot.key.high),
      static_cast<unsigned long long>(slot.key.low), artifact_slot,
      slot.gxp_size, static_cast<unsigned long long>(header.gxp_hash),
      GeneratedGxpResourceAttestationName(
          slot.gxp_resource_attestation));
#else
  (void)slot;
#endif
}

void ShaderCompiler::Compile(CompileSlot *slot) {
  if (!slot)
    return;

  slot->gxp = nullptr;
  slot->gxp_size = 0;
  slot->diagnostic_count = 0;
  slot->total_diagnostic_count = 0;
  slot->succeeded = false;
  slot->persistent_cache_hit = false;
  slot->invalid_output = false;
  slot->resource_rejected_output = false;
  slot->diagnostics_truncated = false;
  slot->gxp_resources = {};
  slot->gxp_resource_attestation =
      GeneratedGxpResourceAttestation::MissingInput;

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
  options.optimizationLevel = slot->optimization_level;
  options.useFastmath = 0;
  options.useFastprecision = 0;
  options.useFastint = 0;
  options.warningLevel = 1;
  options.performanceWarnings = 1;

  const SceShaccCgCompileOutput *const output =
      sceShaccCgCompileProgram(&options, &callbacks, 0);
  // Physical OOM controls prove that ordinary output/release cleanup can
  // abort. Do not trust or retain any output from that failed transaction;
  // retirement happens at the module boundary on this worker instead.
  if (s_allocation_failures.load(std::memory_order_relaxed) != 0 ||
      !PrivateArenaGuardsHold()) {
    append_synthetic_diagnostic(
        403, "ShaccCg allocator failed; retiring compiler without failed-state cleanup");
    s_current_source = {};
    return;
  }
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
      slot->gxp = static_cast<u8 *>(std::malloc(output->programSize));
      if (slot->gxp) {
        std::memcpy(slot->gxp, output->programData,
                    output->programSize);
        slot->gxp_size = output->programSize;
        slot->succeeded = true;
      } else {
        append_synthetic_diagnostic(
            0, "bounded generated-GXP retention allocation failed");
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

void ShaderCompiler::RecordThreadStart(bool supervisor) {
  std::lock_guard lock(m_mutex);
  (supervisor ? m_live_supervisor : m_live_worker) =
      Threading::ThreadHandle::GetForCallingThread();
}

void ShaderCompiler::RecordThreadEnd(bool supervisor) {
  std::lock_guard lock(m_mutex);
  Threading::ThreadHandle& live = supervisor ? m_live_supervisor : m_live_worker;
  m_retired_thread_cpu_us += live.GetCPUTime();
  live = Threading::ThreadHandle{};
}

void ShaderCompiler::CancelPlanningTask(PlanningTask& task) {
  m_cancelled_planning_tasks.fetch_add(1, std::memory_order_relaxed);
  try {
    if (task.cancel)
      task.cancel();
  } catch (...) {
    Console.Error("GPU-VU: planning cancellation callback threw.");
  }
}

void ShaderCompiler::FailPendingWork() {
  // The terminal state is published before taking the queue lock. Submit()
  // rechecks under that lock, so no accepted request can miss this drain.
  for (;;) {
    PlanningTask task;
    {
      std::lock_guard lock(m_mutex);
      for (u8 index = PopRequestLocked(); index != InvalidSlot;
           index = PopRequestLocked()) {
        CompileSlot& slot = m_slots[index];
        std::free(slot.source);
        slot.source = nullptr;
        slot.succeeded = false;
        slot.diagnostic_count = slot.total_diagnostic_count = 1;
        FixedDiagnostic& diagnostic = slot.diagnostics[0];
        diagnostic = {};
        diagnostic.level = SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR;
        std::snprintf(diagnostic.message.data(), diagnostic.message.size(),
                      "Compiler service stopped before compilation");
        slot.state = SlotState::Completed;
        PushResultLocked(index);
        m_cancelled_requests.fetch_add(1, std::memory_order_relaxed);
      }
      if (m_planning_tasks.empty())
        break;
      task = std::move(m_planning_tasks.front());
      m_planning_tasks.pop_front();
    }
    CancelPlanningTask(task);
  }
  VitaGS::NotifyGpuVuCompilerResult();
}

void ShaderCompiler::SupervisorMain() {
  RecordThreadStart(true);
  const auto self = Threading::ThreadHandle::GetForCallingThread();
  // This 64 KiB supervisor never enters Shacc, plans a VU program or waits on
  // the GS owner. Joining a failed generation only blocks this background
  // thread. Healthy compiles keep their existing worker (no per-root churn).
  bool restart = self.SetAffinity(1u << 0) &&
      sceKernelChangeThreadPriority(sceKernelGetThreadId(), 127) >= 0;
  bool recovering = false;
  while (restart && !m_shutdown.load(std::memory_order_acquire)) {
    Threading::Thread worker;
    worker.SetStackSize(512 * 1024);
    restart = false;
    // A prior generation may have drained WorkSema's pending bit before OOM.
    // Wake the next one for surviving queued work; never reset concurrent
    // producers' notifications during recovery.
    m_work_sema.NotifyOfWork();
    bool started = false;
    try {
      started = worker.Start([this, &restart, recovering]() {
        RecordThreadStart(false);
        m_worker_generations.fetch_add(1, std::memory_order_relaxed);
        // Recovery success means the fresh service reached Ready, not that
        // the shader which exhausted the arena has become compilable.
        restart = WorkerMain(recovering);
        RecordThreadEnd(false);
      });
    } catch (const std::bad_alloc&) {
      started = false;
    }
    if (!started)
      break;
    worker.Join();
    recovering = restart;
  }
  if (!m_shutdown.load(std::memory_order_acquire)) {
    State expected = m_state.load(std::memory_order_acquire);
    while (expected != State::Stopping && expected != State::Stopped &&
           !m_state.compare_exchange_weak(expected, State::Unavailable,
                                          std::memory_order_acq_rel)) {
    }
  }
  m_service_state_reported.store(false, std::memory_order_relaxed);
  m_service_state_pending.store(true, std::memory_order_release);
  m_startup_sema.Post();
  FailPendingWork();
  RecordThreadEnd(true);
}

bool ShaderCompiler::WorkerMain(bool recovering) {
  const SceUID thread_id = sceKernelGetThreadId();
  const int current_priority = sceKernelGetThreadCurrentPriority();
  m_worker_priority_before.store(current_priority, std::memory_order_relaxed);
  const Threading::ThreadHandle self =
      Threading::ThreadHandle::GetForCallingThread();
  // ThreadProc enters through Sony's common USER_ALL queue. Pin first so
  // SetAffinity can translate the documented default 160 priority to the
  // corresponding USER_0 individual-queue priority 96. Adding 0x20 while it
  // was still common requested invalid priority 192 and disabled ShaccCg on a
  // normal retail launch.
  const int affinity_result = self.SetAffinity(1u << 0) ? 0 : -1;
  const int pinned_priority = sceKernelGetThreadCurrentPriority();
  int priority_result = -1;
  if (affinity_result >= 0 && pinned_priority >= 64 &&
      pinned_priority <= 127) {
    // Vita priorities increase toward lower scheduling priority. Keep
    // runtime compilation behind emulation. Pin it to USER_0 as well: EE owns
    // that core at a higher priority, while USER_1 and USER_2 remain cleanly
    // attributable to MTVU and MTGS. A USER_ALL compiler made an idle MTGS
    // core look saturated even though the GS worker itself was not busy.
    priority_result = sceKernelChangeThreadPriority(
        thread_id, std::min(pinned_priority + 0x20, 127));
  }
  const int effective_priority = sceKernelGetThreadCurrentPriority();
  m_worker_priority_result.store(priority_result, std::memory_order_relaxed);
  m_worker_priority_after.store(effective_priority,
                                std::memory_order_relaxed);
  m_worker_affinity_result.store(affinity_result, std::memory_order_relaxed);
  Console.WriteLn(
      "GPU-VU: ShaccCg worker scheduling priority_before=%08x "
      "priority_pinned=%08x priority_after=%08x priority_result=%08x "
      "affinity=user0 "
      "affinity_result=%08x.",
      static_cast<u32>(current_priority), static_cast<u32>(pinned_priority),
      static_cast<u32>(effective_priority), static_cast<u32>(priority_result),
      static_cast<u32>(affinity_result));
  if (priority_result < 0 || affinity_result < 0) {
    return false;
  }

  if (!LoadCompilerModule()) {
    return false;
  }
  if (m_shutdown.load(std::memory_order_acquire)) {
    if (s_allocation_failures.load(std::memory_order_relaxed) == 0 &&
        PrivateArenaGuardsHold())
      sceShaccCgReleaseCompiler();
    UnloadCompilerModule();
    return false;
  }
  State starting = State::Starting;
  const bool ready = m_state.compare_exchange_strong(
      starting, State::Ready, std::memory_order_acq_rel);
  if (ready && recovering)
    m_recovery_successes.fetch_add(1, std::memory_order_relaxed);
  m_service_state_reported.store(false, std::memory_order_relaxed);
  m_service_state_pending.store(true, std::memory_order_release);
  m_startup_sema.Post();
  VitaGS::NotifyGpuVuCompilerResult();

  bool retire_generation = false;
  bool retry_generation = false;
  bool skip_release = false;
  while (!retire_generation && !m_shutdown.load(std::memory_order_acquire)) {
    m_work_sema.WaitForWork();
    while (!m_shutdown.load(std::memory_order_acquire)) {
      u8 slot_index = InvalidSlot;
      PlanningTask planning;
      {
        std::lock_guard lock(m_mutex);
        slot_index = PopRequestLocked();
        if (slot_index != InvalidSlot) {
          m_slots[slot_index].state = SlotState::Compiling;
        } else if (!m_planning_tasks.empty()) {
          planning = std::move(m_planning_tasks.front());
          m_planning_tasks.pop_front();
          m_active_planning_identity = planning.identity;
        } else {
          break;
        }
      }

      if (planning.identity != 0u) {
        m_planning_starts.fetch_add(1, std::memory_order_relaxed);
        m_active_planning_tasks.fetch_add(1, std::memory_order_relaxed);
        Console.WriteLn(
            "GPU-VU: asynchronous structured planner starting for "
            "%016llx on the low-priority compiler worker.",
            static_cast<unsigned long long>(planning.identity));
        const Common::Timer::Value planning_start =
            Common::Timer::GetCurrentValue();
        try {
          planning.work();
        } catch (...) {
          Console.Error(
              "GPU-VU: asynchronous structured planner threw for "
              "%016llx; fixed/MTVU ownership remains authoritative.",
              static_cast<unsigned long long>(planning.identity));
        }
        const Common::Timer::Value planning_end =
            Common::Timer::GetCurrentValue();
        const u64 planning_us = static_cast<u64>(
            Common::Timer::ConvertValueToSeconds(
                planning_end - planning_start) *
            1000000.0);
        {
          std::lock_guard lock(m_mutex);
          m_active_planning_identity = 0u;
        }
        m_active_planning_tasks.fetch_sub(1, std::memory_order_relaxed);
        m_planning_completions.fetch_add(1, std::memory_order_relaxed);
        m_total_planning_us.fetch_add(planning_us,
                                      std::memory_order_relaxed);
        u64 longest_planning_us =
            m_longest_planning_us.load(std::memory_order_relaxed);
        while (planning_us > longest_planning_us &&
               !m_longest_planning_us.compare_exchange_weak(
                   longest_planning_us, planning_us,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        Console.WriteLn(
            "GPU-VU: asynchronous structured planner completed for "
            "%016llx in %llu us.",
            static_cast<unsigned long long>(planning.identity),
            static_cast<unsigned long long>(planning_us));
        VitaGS::NotifyGpuVuCompilerResult();
        continue;
      }

      CompileSlot &slot = m_slots[slot_index];
      m_dequeued_requests.fetch_add(1, std::memory_order_relaxed);
      const Common::Timer::Value compile_start =
          Common::Timer::GetCurrentValue();
      StoreAuditSource(slot);
      const bool persistent_cache_hit = LoadPersistentCache(&slot);
      if (!persistent_cache_hit) {
        m_compile_starts.fetch_add(1, std::memory_order_relaxed);
        m_active_compiles.fetch_add(1, std::memory_order_relaxed);
        Console.WriteLn(
            "GPU-VU: ShaccCg compile starting for %016llx%016llx "
            "(%u source bytes, O%u; persistent-cache-miss).",
            static_cast<unsigned long long>(slot.key.high),
            static_cast<unsigned long long>(slot.key.low), slot.source_size,
            slot.optimization_level);
        Compile(&slot);
      }

      if (slot.succeeded) {
        slot.gxp_resource_attestation =
            AttestRuntimeGeneratedGxpResources(
                slot.gxp, slot.gxp_size, &slot.gxp_resources,
                slot.execution_requirement);
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
    VITASX2_GPU_VU_UNIVERSAL_VALIDATION
        const bool validation_scratch_spill =
            slot.gxp_resource_attestation ==
                GeneratedGxpResourceAttestation::ScratchSpill &&
            slot.execution_requirement ==
                GeneratedGxpExecutionRequirement::ParallelStaticFlow &&
            IsBoundedParallelStaticValidationScratchSpill(
                slot.gxp_resources);
#else
        constexpr bool validation_scratch_spill = false;
#endif
        if (slot.gxp_resource_attestation !=
                GeneratedGxpResourceAttestation::Accepted &&
            !validation_scratch_spill) {
          slot.succeeded = false;
          slot.resource_rejected_output = true;
        }
        if (validation_scratch_spill) {
          Console.Warning(
              "GPU-VU: validation-only bounded scratch-spill root retained "
              "key=%016llx%016llx scratch=%u; product admission remains "
              "closed and MTVU is authoritative until the one-canary gate.",
              static_cast<unsigned long long>(slot.key.high),
              static_cast<unsigned long long>(slot.key.low),
              slot.gxp_resources.scratch_buffer_size);
        }
      }
      const Common::Timer::Value compile_end =
          Common::Timer::GetCurrentValue();
      slot.compile_us = static_cast<u64>(
          Common::Timer::ConvertValueToSeconds(compile_end - compile_start) *
          1000000.0);
      std::free(slot.source);
      slot.source = nullptr;

      const bool guards_hold = PrivateArenaGuardsHold();
      if (!guards_hold) {
        slot.succeeded = false;
        slot.invalid_output = true;
        m_private_arena_guard_failures.fetch_add(
            1, std::memory_order_relaxed);
      }
      if (s_allocation_failures.load(std::memory_order_relaxed) != 0 ||
          !guards_hold) {
        slot.succeeded = false;
        slot.invalid_output = true;
        std::free(slot.gxp);
        slot.gxp = nullptr;
        slot.gxp_size = 0;
      }
      SceClibMspaceStats arena_stats{};
      if (guards_hold)
        sceClibMspaceMallocStats(m_private_mspace, &arena_stats);
      u64 arena_peak = m_private_arena_peak.load(std::memory_order_relaxed);
      while (arena_stats.peak_in_use > arena_peak &&
             !m_private_arena_peak.compare_exchange_weak(
                 arena_peak, arena_stats.peak_in_use,
                 std::memory_order_relaxed,
                 std::memory_order_relaxed)) {
      }
      m_private_arena_current.store(arena_stats.current_in_use,
                                    std::memory_order_relaxed);

      // ShaccCg 3.0 retains internal allocations even after a successful
      // output is destroyed.  Generated bundles deliberately queue unrelated
      // roots, and carrying one root's compiler graph into the next produced
      // a data abort inside SceShaccCg before its bounded allocator could
      // report failure. Source and copied GXP results live outside this arena,
      // so make every compile an isolated transaction, matching psp2cgc's
      // process-per-root behavior.
      if (!slot.persistent_cache_hit) {
        if (s_allocation_failures.load(std::memory_order_relaxed) == 0 &&
            guards_hold)
          sceShaccCgReleaseCompiler();
        const u32 failures = s_allocation_failures.load(std::memory_order_relaxed);
        if (failures != 0 || !guards_hold) {
          skip_release = retire_generation = true;
          retry_generation = guards_hold;
          m_allocator_failures.fetch_add(failures, std::memory_order_relaxed);
          m_last_failed_allocation_bytes.store(
              s_first_failed_allocation_bytes.load(std::memory_order_relaxed),
              std::memory_order_relaxed);
          if (retry_generation)
            m_recovery_attempts.fetch_add(1, std::memory_order_relaxed);
          State expected = State::Ready;
          m_state.compare_exchange_strong(expected, State::Starting,
                                          std::memory_order_acq_rel);
          Console.Warning(
              "GPU-VU: retiring failed ShaccCg worker key=%016llx%016llx "
              "allocation_failures=%u first_failed_bytes=%u guards=%u retry=%u.",
              static_cast<unsigned long long>(slot.key.high),
              static_cast<unsigned long long>(slot.key.low), failures,
              s_first_failed_allocation_bytes.load(std::memory_order_relaxed),
              guards_hold ? 1u : 0u, retry_generation ? 1u : 0u);
        } else {
          const bool arena_reset = ResetPrivateArena();
          const int allocator_result = arena_reset
              ? sceShaccCgSetDefaultAllocator(ShaccAllocate, ShaccFree) : -1;
          if (!arena_reset || allocator_result < 0 ||
              s_allocation_failures.load(std::memory_order_relaxed) != 0) {
            m_startup_allocator_result.store(allocator_result,
                                             std::memory_order_relaxed);
            // Release already ran. An unsuccessful reset may have destroyed
            // the mspace; only unload is now allowed to retire the module.
            skip_release = retire_generation = true;
          }
        }
      }

      // Include failures reported during output destruction/release. No
      // failed transaction can publish or cache a seemingly valid GXP.
      if (s_allocation_failures.load(std::memory_order_relaxed) != 0) {
        slot.succeeded = false;
        slot.invalid_output = true;
        std::free(slot.gxp);
        slot.gxp = nullptr;
        slot.gxp_size = 0;
      }
      StoreAuditArtifact(slot);
      // Cache structurally valid policy-rejected artifacts too; admission is
      // repeated on load. Failure recovery never changes that resource gate.
      if (!persistent_cache_hit)
        StorePersistentCache(slot);

      if (!persistent_cache_hit) {
        m_active_compiles.fetch_sub(1, std::memory_order_relaxed);
        m_compile_completions.fetch_add(1, std::memory_order_relaxed);
        (slot.succeeded ? m_compile_successes : m_compile_failures)
            .fetch_add(1, std::memory_order_relaxed);
      }
      if (slot.invalid_output)
        m_invalid_outputs.fetch_add(1, std::memory_order_relaxed);
      if (slot.resource_rejected_output)
        m_resource_rejected_outputs.fetch_add(1,
                                              std::memory_order_relaxed);
      if (slot.diagnostics_truncated)
        m_truncated_diagnostics.fetch_add(1,
                                          std::memory_order_relaxed);
      if (!persistent_cache_hit) {
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
      if (retire_generation)
        break;
    }
  }

  if (!skip_release && s_allocation_failures.load(std::memory_order_relaxed) == 0 &&
      PrivateArenaGuardsHold())
    sceShaccCgReleaseCompiler();
  const bool unloaded = UnloadCompilerModule();
  // OOM recovery requires BOTH module retirement and a new thread. The arena
  // is reset only after absence is confirmed; the supervisor joins this
  // worker before creating the next generation. Never replay the failed key.
  return retry_generation && unloaded &&
      !m_shutdown.load(std::memory_order_acquire) && ResetPrivateArena();
}
} // namespace VitaGpuVu
