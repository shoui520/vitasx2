// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuDirectProgram.h"

#include "VUmicro.h"
#include "common/Console.h"
#include "vita/VitaGpuVuCgGenerator.h"
#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuProgramRegistry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace VitaGpuVu {
namespace {

constexpr u32 MaximumPreparedPrograms = 32;
constexpr u32 TokenSlotBits = 8;
constexpr u32 TokenSlotMask = (1u << TokenSlotBits) - 1u;
constexpr u32 TokenGenerationMask = 0x00ffffffu;

struct DirectCandidate {
  ParallelLoopKernel kernel;
  ParallelInvocationPlan invocation;
  std::array<u32, 4> gif_tag{};
  DirectTfxContract contract;
  GeneratedCgProgram generated;
  ShaderKey key;
  bool has_generated_root = false;
  bool request_accepted = false;
};

struct PreparedProgram {
  std::array<u8, VU1_PROGSIZE> micro{};
  ProgramAnalysis analysis;
  std::vector<DirectCandidate> candidates;
  std::mutex mutex;
  u32 start_pc = 0;
  bool primed = false;
};

struct CacheSlot {
  std::shared_ptr<PreparedProgram> program;
  u64 last_use = 0;
  u32 generation = 0;
};

std::mutex s_cache_mutex;
std::array<CacheSlot, MaximumPreparedPrograms> s_cache;
u64 s_cache_clock = 0;

std::atomic<u64> s_prepared_programs{0};
std::atomic<u64> s_preparation_cache_hits{0};
std::atomic<u64> s_preparation_evictions{0};
std::atomic<u64> s_analysis_failures{0};
std::atomic<u64> s_programs_without_parallel_candidate{0};
std::atomic<u64> s_parallel_candidates{0};
std::atomic<u64> s_prime_attempts{0};
std::atomic<u64> s_prime_cache_hits{0};
std::atomic<u64> s_stale_tokens{0};
std::atomic<u64> s_gif_address_failures{0};
std::atomic<u64> s_gif_contract_rejections{0};
std::atomic<u64> s_generated_roots{0};
std::atomic<u64> s_compiler_requests{0};
std::atomic<u64> s_compiler_request_retries{0};

DirectProgramToken MakeToken(u32 slot, u32 generation) {
  return {((generation & TokenGenerationMask) << TokenSlotBits) |
          ((slot + 1u) & TokenSlotMask)};
}

bool DecodeToken(DirectProgramToken token, u32 *slot, u32 *generation) {
  if (!slot || !generation || !token.IsValid())
    return false;
  const u32 encoded_slot = token.value & TokenSlotMask;
  if (encoded_slot == 0 || encoded_slot > MaximumPreparedPrograms)
    return false;
  *slot = encoded_slot - 1u;
  *generation = (token.value >> TokenSlotBits) & TokenGenerationMask;
  return *generation != 0;
}

bool Matches(const PreparedProgram &program, const u8 *micro, u32 start_pc) {
  return program.start_pc == start_pc &&
         std::memcmp(program.micro.data(), micro, VU1_PROGSIZE) == 0;
}

std::shared_ptr<PreparedProgram> LookupProgram(DirectProgramToken token) {
  u32 slot = 0;
  u32 generation = 0;
  if (!DecodeToken(token, &slot, &generation)) {
    s_stale_tokens.fetch_add(1, std::memory_order_relaxed);
    return {};
  }

  std::lock_guard lock(s_cache_mutex);
  CacheSlot &entry = s_cache[slot];
  if (!entry.program || entry.generation != generation) {
    s_stale_tokens.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  entry.last_use = ++s_cache_clock;
  return entry.program;
}

std::shared_ptr<PreparedProgram> BuildPreparedProgram(const u8 *micro,
                                                      u32 start_pc) {
  auto prepared = std::make_shared<PreparedProgram>();
  prepared->start_pc = start_pc;
  std::memcpy(prepared->micro.data(), micro, prepared->micro.size());

  std::string error;
  if (!AnalyzeGpuVu1Program(prepared->micro.data(), prepared->micro.size(),
                            start_pc, &prepared->analysis, &error)) {
    s_analysis_failures.fetch_add(1, std::memory_order_relaxed);
    return {};
  }

  prepared->candidates.reserve(prepared->analysis.natural_loops.size());
  for (u32 loop_index = 0;
       loop_index < prepared->analysis.natural_loops.size(); loop_index++) {
    DirectCandidate candidate;
    if (!BuildParallelLoopKernel(prepared->analysis, loop_index,
                                 &candidate.kernel, &error) ||
        !candidate.kernel.independent_store_values ||
        !BuildParallelInvocationPlan(prepared->analysis, candidate.kernel,
                                     &candidate.invocation, &error) ||
        !InlineAcyclicEntrySlice(prepared->analysis, loop_index,
                                &candidate.kernel, &error) ||
        !candidate.kernel.acyclic_entry_inlined ||
        candidate.kernel.requires_dynamic_entry_state ||
        !candidate.invocation.has_static_gif_source) {
      continue;
    }
    prepared->candidates.push_back(std::move(candidate));
  }

  if (prepared->candidates.empty()) {
    s_programs_without_parallel_candidate.fetch_add(1,
                                                    std::memory_order_relaxed);
  } else {
    s_parallel_candidates.fetch_add(prepared->candidates.size(),
                                    std::memory_order_relaxed);
  }
  return prepared;
}

bool ReadGifTag(const ParallelInvocationPlan &invocation,
                const InvocationEvaluationContext &context,
                std::array<u32, 4> *tag) {
  if (!tag || !context.read_memory_u32)
    return false;

  u32 address = 0;
  if (!EvaluateInvocationValue(invocation,
                               invocation.gif_tag_qword_address, context,
                               &address)) {
    return false;
  }
  for (u32 lane = 0; lane < tag->size(); lane++) {
    if (!context.read_memory_u32(context.memory_user,
                                 static_cast<u16>(address & 0x3ffu),
                                 static_cast<u8>(lane), &(*tag)[lane])) {
      return false;
    }
  }
  return true;
}

bool RequestCandidate(DirectCandidate *candidate,
                      const std::array<u32, 4> &tag) {
  if (!candidate)
    return false;

  if (!candidate->has_generated_root || candidate->gif_tag != tag) {
    DirectTfxContract contract;
    GeneratedCgProgram generated;
    std::string error;
    if (!BuildDirectTfxContract(candidate->kernel, tag.data(), &contract,
                                &error) ||
        !GenerateParallelTfxCg(candidate->kernel, contract, &generated,
                               &error) ||
        generated.requires_dynamic_entry_state) {
      s_gif_contract_rejections.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    candidate->gif_tag = tag;
    candidate->contract = contract;
    candidate->key = MakeGeneratedProgramKey(generated);
    candidate->generated = std::move(generated);
    candidate->has_generated_root =
        candidate->key.low != 0 || candidate->key.high != 0;
    candidate->request_accepted = false;
    if (!candidate->has_generated_root)
      return false;
    s_generated_roots.fetch_add(1, std::memory_order_relaxed);
  }

  if (candidate->request_accepted)
    return true;

#if defined(__vita__)
  ShaderKey submitted_key;
  if (!RequestGeneratedProgram(candidate->generated, &submitted_key)) {
    // A failed content-keyed registry entry is terminal until the registry is
    // explicitly cleared. Do not regenerate the same source or probe the
    // compiler once per VU invocation while the exact CPU fallback remains
    // active.
    if (submitted_key == candidate->key &&
        QueryGeneratedProgram(candidate->key) ==
            GeneratedProgramState::Failed) {
      return true;
    }
    s_compiler_request_retries.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!(submitted_key == candidate->key)) {
    s_compiler_request_retries.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  candidate->request_accepted = true;
  s_compiler_requests.fetch_add(1, std::memory_order_relaxed);
#else
  // Host-neutral Cortex-A9 validation proves semantic preparation and source
  // generation. Only the PSP2 build owns ShaccCg and GXP registration.
  candidate->request_accepted = true;
#endif

  Console.WriteLn(
      "GPU-VU: requested generated direct VU1+TFX root "
      "%016llx%016llx (%u vertices, primitive %u, %u streams, "
      "%u constants, VF mask %08x, %u expressions).",
      static_cast<unsigned long long>(candidate->key.high),
      static_cast<unsigned long long>(candidate->key.low),
      candidate->contract.vertex_count, candidate->contract.primitive,
      static_cast<u32>(candidate->generated.memory_inputs.size()),
      static_cast<u32>(candidate->generated.constant_inputs.size()),
      candidate->generated.vf_uniform_mask,
      candidate->generated.emitted_expression_count);
  return true;
}

} // namespace

DirectProgramToken PrepareDirectProgram(const u8 *micro, u32 micro_size,
                                        u32 start_pc) {
  if (!micro || micro_size != VU1_PROGSIZE || (start_pc & 7u) != 0)
    return {};
  start_pc &= VU1_PROGMASK;

  {
    std::lock_guard lock(s_cache_mutex);
    for (u32 slot = 0; slot < s_cache.size(); slot++) {
      CacheSlot &entry = s_cache[slot];
      if (!entry.program || !Matches(*entry.program, micro, start_pc))
        continue;
      entry.last_use = ++s_cache_clock;
      s_preparation_cache_hits.fetch_add(1, std::memory_order_relaxed);
      return MakeToken(slot, entry.generation);
    }
  }

  std::shared_ptr<PreparedProgram> prepared =
      BuildPreparedProgram(micro, start_pc);
  if (!prepared)
    return {};

  std::lock_guard lock(s_cache_mutex);
  // CPU0 is the ordinary preparation owner, but retain exact behavior if a
  // diagnostic preparation raced it.
  for (u32 slot = 0; slot < s_cache.size(); slot++) {
    CacheSlot &entry = s_cache[slot];
    if (!entry.program || !Matches(*entry.program, micro, start_pc))
      continue;
    entry.last_use = ++s_cache_clock;
    s_preparation_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return MakeToken(slot, entry.generation);
  }

  u32 replacement = 0;
  for (u32 slot = 1; slot < s_cache.size(); slot++) {
    if (!s_cache[slot].program ||
        (s_cache[replacement].program &&
         s_cache[slot].last_use < s_cache[replacement].last_use)) {
      replacement = slot;
    }
  }

  CacheSlot &entry = s_cache[replacement];
  if (entry.program)
    s_preparation_evictions.fetch_add(1, std::memory_order_relaxed);
  entry.generation = (entry.generation + 1u) & TokenGenerationMask;
  if (entry.generation == 0)
    entry.generation = 1;
  entry.program = std::move(prepared);
  entry.last_use = ++s_cache_clock;
  s_prepared_programs.fetch_add(1, std::memory_order_relaxed);

  Console.WriteLn(
      "GPU-VU: prepared VU1 entry %04x (%u blocks, %u loops, %u parallel "
      "direct candidates).",
      entry.program->start_pc,
      static_cast<u32>(entry.program->analysis.blocks.size()),
      static_cast<u32>(entry.program->analysis.natural_loops.size()),
      static_cast<u32>(entry.program->candidates.size()));
  return MakeToken(replacement, entry.generation);
}

bool PrimeDirectProgram(DirectProgramToken token,
                        const InvocationEvaluationContext &context) {
  s_prime_attempts.fetch_add(1, std::memory_order_relaxed);
  std::shared_ptr<PreparedProgram> prepared = LookupProgram(token);
  if (!prepared)
    return false;

  std::lock_guard lock(prepared->mutex);
  if (prepared->primed) {
    s_prime_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Priming is a cold, bounded attempt. A semantic/source rejection or a
  // temporarily unavailable compiler must never turn the VU execution stream
  // into a retry loop on the 496 MHz MTVU core. Compiler-service retry and a
  // lower GPU output selection are separate descriptor-level events.
  prepared->primed = true;
  bool resolved_tag = false;
  for (DirectCandidate &candidate : prepared->candidates) {
    std::array<u32, 4> tag{};
    if (!ReadGifTag(candidate.invocation, context, &tag))
      continue;
    resolved_tag = true;
    if (RequestCandidate(&candidate, tag))
      return true;
  }

  if (!resolved_tag)
    s_gif_address_failures.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool GetDirectProgramInfo(DirectProgramToken token, DirectProgramInfo *info) {
  if (!info)
    return false;
  std::shared_ptr<PreparedProgram> prepared = LookupProgram(token);
  if (!prepared)
    return false;
  info->start_pc = prepared->start_pc;
  info->basic_blocks = static_cast<u32>(prepared->analysis.blocks.size());
  info->natural_loops =
      static_cast<u32>(prepared->analysis.natural_loops.size());
  info->parallel_candidates =
      static_cast<u32>(prepared->candidates.size());
  info->complete_cfg = prepared->analysis.complete_cfg;
  return true;
}

DirectProgramStatistics GetDirectProgramStatistics() {
  DirectProgramStatistics stats;
  stats.prepared_programs =
      s_prepared_programs.load(std::memory_order_relaxed);
  stats.preparation_cache_hits =
      s_preparation_cache_hits.load(std::memory_order_relaxed);
  stats.preparation_evictions =
      s_preparation_evictions.load(std::memory_order_relaxed);
  stats.analysis_failures = s_analysis_failures.load(std::memory_order_relaxed);
  stats.programs_without_parallel_candidate =
      s_programs_without_parallel_candidate.load(std::memory_order_relaxed);
  stats.parallel_candidates =
      s_parallel_candidates.load(std::memory_order_relaxed);
  stats.prime_attempts = s_prime_attempts.load(std::memory_order_relaxed);
  stats.prime_cache_hits = s_prime_cache_hits.load(std::memory_order_relaxed);
  stats.stale_tokens = s_stale_tokens.load(std::memory_order_relaxed);
  stats.gif_address_failures =
      s_gif_address_failures.load(std::memory_order_relaxed);
  stats.gif_contract_rejections =
      s_gif_contract_rejections.load(std::memory_order_relaxed);
  stats.generated_roots = s_generated_roots.load(std::memory_order_relaxed);
  stats.compiler_requests =
      s_compiler_requests.load(std::memory_order_relaxed);
  stats.compiler_request_retries =
      s_compiler_request_retries.load(std::memory_order_relaxed);
  return stats;
}

void ClearDirectPrograms() {
  std::lock_guard lock(s_cache_mutex);
  for (CacheSlot &entry : s_cache) {
    entry.program.reset();
    entry.last_use = 0;
    entry.generation = (entry.generation + 1u) & TokenGenerationMask;
    if (entry.generation == 0)
      entry.generation = 1;
  }
}

} // namespace VitaGpuVu
