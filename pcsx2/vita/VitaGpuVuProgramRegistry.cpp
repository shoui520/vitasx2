// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuProgramRegistry.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace VitaGpuVu {
namespace {

struct ShaderKeyLess {
  bool operator()(const ShaderKey &left, const ShaderKey &right) const {
    return left.high < right.high ||
           (left.high == right.high && left.low < right.low);
  }
};

struct RegistryEntry {
  GeneratedProgramState state = GeneratedProgramState::Missing;
  GeneratedCgProgram metadata;
};

std::mutex s_registry_mutex;
std::map<ShaderKey, RegistryEntry, ShaderKeyLess> s_registry;
ShaderCompiler *s_compiler = nullptr;
std::atomic<u64> s_requests{0};
std::atomic<u64> s_unavailable_requests{0};
std::atomic<u64> s_cache_hits{0};
std::atomic<u64> s_cache_misses{0};
std::atomic<u64> s_compiler_queue_retries{0};
std::atomic<u64> s_compile_successes{0};
std::atomic<u64> s_compile_failures{0};
std::atomic<u64> s_ready_programs{0};
std::atomic<u64> s_failed_programs{0};

constexpr u64 FnvPrime = 1099511628211ull;
constexpr u64 LowOffset = 14695981039346656037ull;
constexpr u64 HighOffset = 7809847782465536322ull;

void HashByte(u64 *hash, u8 value) {
  *hash ^= value;
  *hash *= FnvPrime;
}

ShaderKey HashSource(std::string_view source) {
  ShaderKey key{LowOffset, HighOffset};
  for (u32 shift = 0; shift < 32; shift += 8) {
    const u8 byte = static_cast<u8>(GeneratedProgramAbiVersion >> shift);
    HashByte(&key.low, byte);
    HashByte(&key.high, static_cast<u8>(byte ^ 0xa5u));
  }
  for (const char character : source) {
    const u8 byte = static_cast<u8>(character);
    HashByte(&key.low, byte);
    HashByte(&key.high, static_cast<u8>((byte << 1) | (byte >> 7)));
  }
  // Distinguish concatenations from the ABI prefix and make the empty-source
  // value an explicit invalid request rather than a useful cache entry.
  for (u32 shift = 0; shift < 64; shift += 8) {
    const u8 byte = static_cast<u8>(source.size() >> shift);
    HashByte(&key.low, byte);
    HashByte(&key.high, static_cast<u8>(byte ^ 0x5au));
  }
  return key;
}

} // namespace

ShaderKey MakeGeneratedProgramKey(const GeneratedCgProgram &program) {
  return program.source.empty() ? ShaderKey{} : HashSource(program.source);
}

bool AttachGeneratedProgramCompiler(ShaderCompiler *compiler) {
  if (!compiler)
    return false;
  std::lock_guard lock(s_registry_mutex);
  if (s_compiler && s_compiler != compiler)
    return false;
  s_compiler = compiler;
  return true;
}

void DetachGeneratedProgramCompiler(ShaderCompiler *compiler) {
  std::lock_guard lock(s_registry_mutex);
  if (s_compiler == compiler)
    s_compiler = nullptr;
}

bool RequestGeneratedProgram(GeneratedCgProgram program, ShaderKey *key) {
  if (!key || program.source.empty())
    return false;
  s_requests.fetch_add(1, std::memory_order_relaxed);
  *key = MakeGeneratedProgramKey(program);
  if (key->low == 0 && key->high == 0)
    return false;

  // Keep compiler ownership stable through the non-blocking Submit(). Detach()
  // uses the same mutex, so shutdown cannot invalidate the borrowed pointer
  // between lookup and queue insertion.
  std::lock_guard lock(s_registry_mutex);
  ShaderCompiler *const compiler = s_compiler;
  if (!compiler) {
    s_unavailable_requests.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const auto existing = s_registry.find(*key);
  if (existing != s_registry.end()) {
    s_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return existing->second.state != GeneratedProgramState::Failed;
  }

  RegistryEntry entry;
  entry.state = GeneratedProgramState::Queued;
  entry.metadata = program;
  entry.metadata.source.clear();
  s_registry.emplace(*key, std::move(entry));
  s_cache_misses.fetch_add(1, std::memory_order_relaxed);

  if (compiler->Submit(*key, std::move(program.source)))
    return true;

  // A full bounded queue is retryable. Remove only the entry installed by
  // this request; an independently completed result cannot exist because
  // Submit() rejected it.
  const auto it = s_registry.find(*key);
  if (it != s_registry.end() &&
      it->second.state == GeneratedProgramState::Queued) {
    s_registry.erase(it);
  }
  s_compiler_queue_retries.fetch_add(1, std::memory_order_relaxed);
  return false;
}

GeneratedProgramState QueryGeneratedProgram(const ShaderKey &key) {
  std::lock_guard lock(s_registry_mutex);
  if (!s_compiler)
    return GeneratedProgramState::Unavailable;
  const ShaderCompiler::State compiler_state = s_compiler->GetState();
  if (compiler_state == ShaderCompiler::State::Stopped ||
      compiler_state == ShaderCompiler::State::Stopping ||
      compiler_state == ShaderCompiler::State::Unavailable) {
    return GeneratedProgramState::Unavailable;
  }
  const auto it = s_registry.find(key);
  return it == s_registry.end() ? GeneratedProgramState::Missing
                                : it->second.state;
}

bool PollGeneratedProgramCompile(CompileResult *result,
                                 GeneratedCgProgram *metadata) {
  if (!result || !metadata)
    return false;

  ShaderCompiler *compiler = nullptr;
  {
    std::lock_guard lock(s_registry_mutex);
    compiler = s_compiler;
  }
  if (!compiler || !compiler->Poll(result))
    return false;

  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(result->key);
  if (it == s_registry.end())
    return false;
  it->second.state = GeneratedProgramState::Compiled;
  (result->succeeded ? s_compile_successes : s_compile_failures)
      .fetch_add(1, std::memory_order_relaxed);
  *metadata = it->second.metadata;
  return true;
}

void CompleteGeneratedProgramRegistration(const ShaderKey &key,
                                          bool succeeded) {
  std::lock_guard lock(s_registry_mutex);
  const auto it = s_registry.find(key);
  if (it == s_registry.end())
    return;
  it->second.state =
      succeeded ? GeneratedProgramState::Ready : GeneratedProgramState::Failed;
  (succeeded ? s_ready_programs : s_failed_programs)
      .fetch_add(1, std::memory_order_relaxed);
}

ProgramRegistryStatistics GetGeneratedProgramRegistryStatistics() {
  ProgramRegistryStatistics stats;
  stats.requests = s_requests.load(std::memory_order_relaxed);
  stats.unavailable_requests =
      s_unavailable_requests.load(std::memory_order_relaxed);
  stats.cache_hits = s_cache_hits.load(std::memory_order_relaxed);
  stats.cache_misses = s_cache_misses.load(std::memory_order_relaxed);
  stats.compiler_queue_retries =
      s_compiler_queue_retries.load(std::memory_order_relaxed);
  stats.compile_successes = s_compile_successes.load(std::memory_order_relaxed);
  stats.compile_failures = s_compile_failures.load(std::memory_order_relaxed);
  stats.ready_programs = s_ready_programs.load(std::memory_order_relaxed);
  stats.failed_programs = s_failed_programs.load(std::memory_order_relaxed);
  std::lock_guard lock(s_registry_mutex);
  if (s_compiler)
    stats.compiler = s_compiler->GetStatistics();
  return stats;
}

void ClearGeneratedProgramRegistry() {
  std::lock_guard lock(s_registry_mutex);
  s_registry.clear();
}

} // namespace VitaGpuVu
