// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuDirectProgram.h"

#include "GS/GSRegs.h"
#include "VUmicro.h"
#include "common/Console.h"
#include "common/Threading.h"
#include "vita/VitaGpuVuCgGenerator.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuMicroProgram.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "common/Timer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
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
  // Vita's libstdc++ reports __gthread_active_p() false, so shared_ptr's
  // _Sp_counted_base uses plain non-atomic reference-count updates even though
  // this object crosses CPU0, MTVU and GS threads. Keep lifetime ownership in
  // an explicitly atomic intrusive count instead. The cache and each thread's
  // lookup pin own one reference.
  std::atomic<u32> references{1};
  std::array<u8, VU1_PROGSIZE> micro{};
  ProgramAnalysis analysis;
  std::vector<DirectCandidate> candidates;
  Threading::KernelMutex mutex;
  // Zero until the worker observes one registered root. Afterwards all
  // candidate metadata is immutable and the 496 MHz hot descriptor path
  // avoids both the compiler-registry mutex and this preparation mutex.
  std::atomic<u32> ready_candidate_plus_one{0};
  u64 source_hash = 0;
  u32 start_pc = 0;
  u32 configuration_bits = 0;
  u32 semantic_profile_key = 0;
  u32 analysis_abi_version = GeneratedLoopAnalysisAbiVersion;
  // Publication is monotonic for one exact generation. The worker's hot
  // MSCAL/MSCNT stream may revisit a registered entry thousands of times, so
  // reject those hits before taking the cold preparation mutex.
  std::atomic<bool> primed{false};
};

void RetainPreparedProgram(PreparedProgram *program) {
  if (program)
    program->references.fetch_add(1, std::memory_order_relaxed);
}

void ReleasePreparedProgram(PreparedProgram *program) {
  if (program &&
      program->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete program;
  }
}

class PreparedProgramReference {
public:
  PreparedProgramReference() = default;

  static PreparedProgramReference Adopt(PreparedProgram *program) {
    PreparedProgramReference reference;
    reference.m_program = program;
    return reference;
  }

  explicit PreparedProgramReference(PreparedProgram *program)
      : m_program(program) {
    RetainPreparedProgram(m_program);
  }

  PreparedProgramReference(const PreparedProgramReference &) = delete;
  PreparedProgramReference &
  operator=(const PreparedProgramReference &) = delete;

  PreparedProgramReference(PreparedProgramReference &&other) noexcept
      : m_program(std::exchange(other.m_program, nullptr)) {}

  PreparedProgramReference &
  operator=(PreparedProgramReference &&other) noexcept {
    if (this != &other) {
      PreparedProgram *const previous = m_program;
      m_program = std::exchange(other.m_program, nullptr);
      ReleasePreparedProgram(previous);
    }
    return *this;
  }

  ~PreparedProgramReference() { ReleasePreparedProgram(m_program); }

  PreparedProgram *Get() const { return m_program; }
  explicit operator bool() const { return m_program != nullptr; }
  PreparedProgram &operator*() const { return *m_program; }
  PreparedProgram *operator->() const { return m_program; }

private:
  PreparedProgram *m_program = nullptr;
};

struct CacheSlot {
  PreparedProgramReference program;
  u64 last_use = 0;
  u32 generation = 0;
};

Threading::KernelMutex s_cache_mutex;
std::array<CacheSlot, MaximumPreparedPrograms> s_cache;
u64 s_cache_clock = 0;
std::atomic<u64> s_cache_epoch{1};

std::atomic<u64> s_preparation_requests{0};
std::atomic<u64> s_prepared_programs{0};
std::atomic<u64> s_preparation_cache_hits{0};
std::atomic<u64> s_preparation_evictions{0};
std::atomic<u64> s_preparation_wall_us{0};
std::atomic<u64> s_preparation_wall_us_max{0};
std::atomic<u64> s_source_hash_bytes{0};
std::atomic<u64> s_source_compare_bytes{0};
std::atomic<u64> s_source_copy_bytes{0};
std::atomic<u64> s_analysis_builds{0};
std::atomic<u64> s_analysis_wall_us{0};
std::atomic<u64> s_analysis_wall_us_max{0};
std::atomic<u64> s_candidate_proof_wall_us{0};
std::atomic<u64> s_candidate_proof_wall_us_max{0};
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
std::atomic<u64> s_shared_continuation_builds{0};
std::atomic<u64> s_general_continuation_builds{0};
std::atomic<u64> s_hybrid_input_attempts{0};
std::atomic<u64> s_hybrid_input_hits{0};
std::atomic<u64> s_hybrid_input_fallbacks{0};
std::atomic<u64> s_hybrid_raw_bytes_retained{0};
std::atomic<u64> s_hybrid_derived_bytes{0};
std::atomic<u64> s_hybrid_copy_bytes_avoided{0};
std::atomic<u64> s_canonical_input_bindings{0};
std::atomic<u64> s_canonical_input_bytes{0};
std::atomic<u64> s_persistent_raw_input_bindings{0};
std::atomic<u64> s_persistent_raw_input_bytes{0};

u64 ElapsedMicroseconds(Common::Timer::Value started) {
  return static_cast<u64>(Common::Timer::ConvertValueToSeconds(
      Common::Timer::GetCurrentValue() - started) * 1000000.0);
}

void RecordMaximum(std::atomic<u64>* maximum, u64 value) {
  u64 observed = maximum->load(std::memory_order_relaxed);
  while (value > observed &&
         !maximum->compare_exchange_weak(
             observed, value, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

class ScopedPreparationTimer final {
public:
  ScopedPreparationTimer()
      : m_started(Common::Timer::GetCurrentValue()) {
    s_preparation_requests.fetch_add(1, std::memory_order_relaxed);
  }

  ~ScopedPreparationTimer() {
    const u64 elapsed = ElapsedMicroseconds(m_started);
    s_preparation_wall_us.fetch_add(elapsed, std::memory_order_relaxed);
    RecordMaximum(&s_preparation_wall_us_max, elapsed);
  }

private:
  Common::Timer::Value m_started;
};

u64 HashExactSource(const u8* micro, u32 micro_size) {
  // FNV-1a is a cheap cache-directory key, not an admission proof. Matches()
  // still compares all exact source bytes before reuse.
  u64 hash = 1469598103934665603ull;
  for (u32 index = 0; index < micro_size; index++) {
    hash ^= micro[index];
    hash *= 1099511628211ull;
  }
  s_source_hash_bytes.fetch_add(micro_size, std::memory_order_relaxed);
  return hash;
}

u32 SemanticProfileKey(u32 configuration_bits) {
  // The named Exact/OracleNearest/Playable product profiles resolve into
  // distinct immutable configuration bit sets. Preserve that complete set in
  // the analysis key so custom diagnostic profiles are isolated as well.
  return configuration_bits & UniversalConfigurationKnownMask;
}

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

bool Matches(const PreparedProgram &program, const u8 *micro, u64 source_hash,
             u32 start_pc, u32 configuration_bits,
             u32 semantic_profile_key) {
  if (program.source_hash != source_hash || program.start_pc != start_pc ||
      program.configuration_bits != configuration_bits ||
      program.semantic_profile_key != semantic_profile_key ||
      program.analysis_abi_version != GeneratedLoopAnalysisAbiVersion) {
    return false;
  }
  s_source_compare_bytes.fetch_add(VU1_PROGSIZE,
                                   std::memory_order_relaxed);
  return std::memcmp(program.micro.data(), micro, VU1_PROGSIZE) == 0;
}

// The returned pointer remains pinned in this thread's two-entry working set
// until a cache publication invalidates the epoch or a third token replaces
// it. IGA alternates explicit MSCAL and resumed MSCNT entries, so one pin would
// take a cross-core kernel mutex and churn two intrusive references on every
// dispatch. Two pins keep that hot pair lock- and reference-count-free while
// an eviction on another thread can only retire the cache's ownership.
PreparedProgram *LookupPinnedProgram(DirectProgramToken token) {
  struct LocalEntry {
    DirectProgramToken token{};
    PreparedProgramReference program;
  };
  struct LocalLookups {
    std::array<LocalEntry, 2> entries;
    u64 epoch = 0;
    u8 replacement = 0;
  };
  thread_local LocalLookups local;
  const u64 epoch = s_cache_epoch.load(std::memory_order_acquire);
  if (local.epoch == epoch) {
    for (const LocalEntry &entry : local.entries) {
      if (entry.token == token && entry.program)
        return entry.program.Get();
    }
  } else {
    // Drop stale pins before taking the shared cache mutex. Their potentially
    // large analyses are destroyed outside every cache critical section.
    for (LocalEntry &entry : local.entries) {
      entry.token = {};
      entry.program = {};
    }
    local.epoch = epoch;
    local.replacement = 0;
  }

  u32 slot = 0;
  u32 generation = 0;
  if (!DecodeToken(token, &slot, &generation)) {
    s_stale_tokens.fetch_add(1, std::memory_order_relaxed);
    return {};
  }

  PreparedProgramReference replacement;
  {
    std::lock_guard lock(s_cache_mutex);
    CacheSlot &entry = s_cache[slot];
    if (!entry.program || entry.generation != generation) {
      s_stale_tokens.fetch_add(1, std::memory_order_relaxed);
      return {};
    }
    entry.last_use = ++s_cache_clock;
    replacement = PreparedProgramReference(entry.program.Get());
  }
  LocalEntry &pinned = local.entries[local.replacement];
  pinned.token = token;
  pinned.program = std::move(replacement);
  local.replacement =
      static_cast<u8>((local.replacement + 1u) % local.entries.size());
  return pinned.program.Get();
}

std::unique_ptr<PreparedProgram> BuildPreparedProgram(
    const u8* micro, u64 source_hash, u32 start_pc, u32 configuration_bits,
    u32 semantic_profile_key) {
  auto prepared = std::make_unique<PreparedProgram>();
  prepared->source_hash = source_hash;
  prepared->start_pc = start_pc;
  prepared->configuration_bits = configuration_bits;
  prepared->semantic_profile_key = semantic_profile_key;
  std::memcpy(prepared->micro.data(), micro, prepared->micro.size());
  s_source_copy_bytes.fetch_add(prepared->micro.size(),
                                std::memory_order_relaxed);

  std::string error;
  s_analysis_builds.fetch_add(1, std::memory_order_relaxed);
  const Common::Timer::Value analysis_started =
      Common::Timer::GetCurrentValue();
  const bool analyzed = AnalyzeGpuVu1ProgramForConfiguration(
      prepared->micro.data(), prepared->micro.size(), start_pc,
      (configuration_bits & UniversalConfigurationAssumeScheduled) != 0,
      (configuration_bits & UniversalConfigurationInstantQp) != 0,
      &prepared->analysis, &error);
  const u64 analysis_elapsed = ElapsedMicroseconds(analysis_started);
  s_analysis_wall_us.fetch_add(analysis_elapsed, std::memory_order_relaxed);
  RecordMaximum(&s_analysis_wall_us_max, analysis_elapsed);
  if (!analyzed) {
    s_analysis_failures.fetch_add(1, std::memory_order_relaxed);
    return {};
  }

  const Common::Timer::Value proof_started = Common::Timer::GetCurrentValue();
  prepared->candidates.reserve(prepared->analysis.natural_loops.size());
  for (u32 loop_index = 0;
       loop_index < prepared->analysis.natural_loops.size(); loop_index++) {
    DirectCandidate candidate;
    if (!BuildParallelLoopKernelForConfiguration(
            prepared->analysis, loop_index, configuration_bits,
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
  const u64 proof_elapsed = ElapsedMicroseconds(proof_started);
  s_candidate_proof_wall_us.fetch_add(proof_elapsed,
                                      std::memory_order_relaxed);
  RecordMaximum(&s_candidate_proof_wall_us_max, proof_elapsed);

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
                InvocationEvaluationWorkspace *workspace,
                std::array<u32, 4> *tag) {
  if (!workspace || !tag)
    return false;
  for (u32 lane = 0; lane < tag->size(); lane++) {
    if (!EvaluateInvocationValue(invocation, invocation.gif_tag_words[lane],
                                 context, workspace, &(*tag)[lane])) {
      return false;
    }
  }
  return true;
}

// Names the exact entry state the generated root demands but cannot treat as a
// stable uniform. Every lane reported here is written by a pair this entry can
// execute, so reusing the CPU snapshot across invocations would be a stale
// seed. Unreplaced Invariant* leaves mean the acyclic entry slice could not
// resolve the value at all.
std::string DescribeDynamicEntryState(const ParallelLoopKernel &kernel,
                                      const GeneratedCgProgram &generated) {
  std::string reason = "dynamic entry state:";
  for (u32 reg = 0; reg < 32; reg++) {
    const u8 unstable = static_cast<u8>(generated.stable_initial_vf_lanes[reg] &
                                        ~kernel.stable_initial_vf_lanes[reg]);
    if (unstable == 0)
      continue;
    reason += " VF" + std::to_string(reg) + ".";
    for (u32 lane = 0; lane < 4; lane++) {
      if ((unstable & (0x8u >> lane)) != 0)
        reason += "xyzw"[lane];
    }
  }
  if ((generated.stable_initial_acc_lanes & ~kernel.stable_initial_acc_lanes) !=
      0) {
    reason += " ACC";
  }
  if (generated.uses_q_uniform && !kernel.stable_initial_q)
    reason += " Q";
  if (generated.uses_p_uniform && !kernel.stable_initial_p)
    reason += " P";
  if (generated.uses_i_uniform && !kernel.stable_initial_i)
    reason += " I";

  u32 unresolved_invariants = 0;
  for (const ExpressionNode &node : kernel.expressions) {
    switch (node.kind) {
    case ExpressionKind::InvariantVf:
    case ExpressionKind::InvariantAcc:
    case ExpressionKind::InvariantQ:
    case ExpressionKind::InvariantP:
    case ExpressionKind::InvariantI:
      unresolved_invariants++;
      break;
    default:
      break;
    }
  }
  if (unresolved_invariants != 0)
    reason += " unresolved-invariants=" + std::to_string(unresolved_invariants);
  return reason;
}

bool RequestCandidate(DirectCandidate *candidate,
                      const std::array<u32, 4> &tag,
                      std::string *rejection = nullptr) {
  if (!candidate)
    return false;

  if (!candidate->has_generated_root || candidate->gif_tag != tag) {
    DirectTfxContract contract;
    GeneratedCgProgram generated;
    std::string error;
    if (!BuildDirectTfxContract(candidate->kernel, tag.data(), &contract,
                                &error)) {
      s_gif_contract_rejections.fetch_add(1, std::memory_order_relaxed);
      if (rejection)
        *rejection = "contract: " + error;
      return false;
    }
    if (!GenerateParallelTfxCg(candidate->kernel, contract, &generated,
                               &error)) {
      s_gif_contract_rejections.fetch_add(1, std::memory_order_relaxed);
      if (rejection)
        *rejection = "cg: " + error;
      return false;
    }
    if (generated.requires_dynamic_entry_state) {
      s_gif_contract_rejections.fetch_add(1, std::memory_order_relaxed);
      if (rejection)
        *rejection = DescribeDynamicEntryState(candidate->kernel, generated);
      return false;
    }

    candidate->gif_tag = tag;
    candidate->contract = contract;
    candidate->key = MakeGeneratedProgramKey(generated);
    candidate->generated = std::move(generated);
    candidate->has_generated_root =
        candidate->key.low != 0 || candidate->key.high != 0;
    candidate->request_accepted = false;
    if (!candidate->has_generated_root) {
      if (rejection)
        *rejection = "empty generated program key";
      return false;
    }
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
      if (rejection)
        *rejection = "registry reports compilation failure";
      return true;
    }
    s_compiler_request_retries.fetch_add(1, std::memory_order_relaxed);
    if (rejection)
      *rejection = "registry rejected the request";
    return false;
  }
  if (!(submitted_key == candidate->key)) {
    s_compiler_request_retries.fetch_add(1, std::memory_order_relaxed);
    if (rejection)
      *rejection = "registry returned a different content key";
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

// A clamp-stable lane is only genuinely stable when this draw's seed already
// satisfies the clamp; the region is then a no-op on that lane and the CPU
// snapshot stays correct behind every accepted draw. NaN never satisfies
// either comparison, so it is rejected by construction.
bool ClampStableSeedsHold(const GeneratedCgProgram &generated,
                          const InvocationEvaluationContext &context) {
  if (generated.clamp_stable_lanes.empty())
    return true;
  if (!context.initial_vf_words)
    return false;
  for (const ClampStableLane &clamp : generated.clamp_stable_lanes) {
    if (!context.InitialVfLaneAvailable(clamp.reg, clamp.lane))
      return false;
    float seed = 0.0f;
    float bound = 0.0f;
    std::memcpy(&seed,
                context.initial_vf_words + clamp.reg * 4u + clamp.lane,
                sizeof(seed));
    std::memcpy(&bound, &clamp.bound_bits, sizeof(bound));
    if (clamp.minimum ? !(seed <= bound) : !(seed >= bound))
      return false;
  }
  return true;
}

bool SamePayload(const RawVifPayloadRef &left,
                 const RawVifPayloadRef &right) {
  return left.owner == right.owner && left.slot == right.slot &&
         left.generation == right.generation && left.offset == right.offset &&
         left.size == right.size;
}

const VifUnpackSpan *FindInputSpan(
    const std::vector<VifUnpackSpan> &spans, u16 first_qword,
    s32 invocation_coefficient, u32 invocation_count,
    RawQwordBinding *binding) {
  for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
    if (BindAffineRawQwords(*it, first_qword, invocation_coefficient,
                           invocation_count, binding)) {
      return &*it;
    }
  }
  return nullptr;
}

const VifUnpackSpan* FindGridInputSpan(
    const std::vector<VifUnpackSpan>& spans, u16 first_qword,
    s32 outer_invocation_coefficient, u32 outer_invocation_count,
    s32 child_invocation_coefficient, u32 child_invocation_count,
    RawQwordBinding* binding) {
  for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
    if (BindGridRawQwords(
            *it, first_qword, outer_invocation_coefficient,
            outer_invocation_count, child_invocation_coefficient,
            child_invocation_count, binding)) {
      return &*it;
    }
  }
  return nullptr;
}

const DirectCandidate *ReadyCandidate(const PreparedProgram *program) {
  if (!program)
    return nullptr;
  const u32 ready =
      program->ready_candidate_plus_one.load(std::memory_order_acquire);
  if (ready == 0 || ready > program->candidates.size())
    return nullptr;
  return &program->candidates[ready - 1];
}

bool SameMemoryInputs(const GeneratedCgProgram &left,
                      const GeneratedCgProgram &right) {
  if (left.memory_inputs.size() != right.memory_inputs.size())
    return false;
  for (u32 index = 0; index < left.memory_inputs.size(); index++) {
    const CgMemoryInput &a = left.memory_inputs[index];
    const CgMemoryInput &b = right.memory_inputs[index];
    if (!(a.address == b.address) ||
        a.attribute_index != b.attribute_index ||
        a.flat_attribute_vertex_mask != b.flat_attribute_vertex_mask) {
      return false;
    }
  }
  return true;
}

bool SameStaticOutputContract(const DirectCandidate &left,
                              const DirectCandidate &right) {
  const DirectTfxContract &a = left.contract;
  const DirectTfxContract &b = right.contract;
  return left.gif_tag == right.gif_tag &&
         a.vertex_count == b.vertex_count &&
         a.primitive == b.primitive &&
         a.output_base_vi == b.output_base_vi &&
         a.output_base_qword == b.output_base_qword &&
         a.gouraud == b.gouraud &&
         a.textured == b.textured &&
         a.fog_enabled == b.fog_enabled &&
         a.fixed_texture_coordinates == b.fixed_texture_coordinates &&
         a.adc_always_clear == b.adc_always_clear &&
         a.has_color == b.has_color &&
         a.has_stq == b.has_stq &&
         a.has_uv == b.has_uv &&
         a.has_position == b.has_position &&
         left.generated.uses_flat_instance_inputs ==
             right.generated.uses_flat_instance_inputs &&
         left.generated.flat_vertices_per_primitive ==
             right.generated.flat_vertices_per_primitive &&
         left.generated.flat_instance_vertex_step ==
             right.generated.flat_instance_vertex_step &&
         left.generated.flat_strip_winding ==
             right.generated.flat_strip_winding;
}

bool InvocationNodeDependsOnInitialVf(
    const ParallelInvocationPlan &plan, u32 id,
    std::vector<u8> *dependency_state) {
  if (!dependency_state || id == 0 || id >= plan.values.size() ||
      dependency_state->size() < plan.values.size()) {
    return true;
  }
  u8 &state = (*dependency_state)[id];
  if (state == 2)
    return false;
  if (state == 3 || state == 1)
    return true;
  state = 1;

  const InvocationValueNode &node = plan.values[id];
  const auto operand = [&](u32 index) {
    return InvocationNodeDependsOnInitialVf(
        plan, node.operands[index], dependency_state);
  };
  bool depends = false;
  switch (node.kind) {
  case InvocationValueKind::InitialVfWord:
    depends = true;
    break;
  case InvocationValueKind::Constant:
  case InvocationValueKind::InitialVi:
  case InvocationValueKind::VifTop:
  case InvocationValueKind::VifItop:
    break;
  case InvocationValueKind::AddConstantU16:
  case InvocationValueKind::SignExtendU16:
  case InvocationValueKind::MemoryU16:
  case InvocationValueKind::MemoryU32:
  case InvocationValueKind::LessThanZeroS16:
  case InvocationValueKind::GreaterThanZeroS16:
  case InvocationValueKind::LessEqualZeroS16:
  case InvocationValueKind::GreaterEqualZeroS16:
  case InvocationValueKind::BooleanNot:
  case InvocationValueKind::CountUntilZeroU16:
    depends = operand(0);
    break;
  case InvocationValueKind::AddU16:
  case InvocationValueKind::SubtractU16:
  case InvocationValueKind::AndU16:
  case InvocationValueKind::OrU16:
  case InvocationValueKind::EqualU16:
  case InvocationValueKind::NotEqualU16:
  case InvocationValueKind::BooleanAnd:
  case InvocationValueKind::ScaleAddU16:
    depends = operand(0) || operand(1);
    break;
  }
  state = depends ? 3 : 2;
  return depends;
}

bool FinalViDependsOnInitialVf(const ParallelInvocationPlan &plan) {
  if (!plan.has_final_vi_state || plan.final_vi_alternatives.empty())
    return true;
  std::vector<u8> dependency_state(plan.values.size(), 0);
  for (const FinalViAlternative &alternative :
       plan.final_vi_alternatives) {
    if (InvocationNodeDependsOnInitialVf(
            plan, alternative.predicate, &dependency_state)) {
      return true;
    }
    for (u32 reg = 1; reg < alternative.values.size(); reg++) {
      if ((plan.final_vi_write_mask & (1u << reg)) != 0 &&
          InvocationNodeDependsOnInitialVf(
              plan, alternative.values[reg], &dependency_state)) {
        return true;
      }
    }
  }
  return false;
}

bool ResolveContinuationPair(
    DirectProgramToken entry_token, DirectProgramToken resume_token,
    const DirectCandidate **entry_candidate,
    const DirectCandidate **resume_candidate) {
  if (!entry_candidate || !resume_candidate || !entry_token.IsValid() ||
      !resume_token.IsValid() || entry_token == resume_token) {
    return false;
  }

  struct LocalContinuationPair {
    DirectProgramToken entry_token{};
    DirectProgramToken resume_token{};
    PreparedProgramReference entry;
    PreparedProgramReference resume;
    const DirectCandidate *entry_candidate = nullptr;
    const DirectCandidate *resume_candidate = nullptr;
    u64 epoch = 0;
    bool resolved = false;
  };
  thread_local LocalContinuationPair local;
  const u64 epoch = s_cache_epoch.load(std::memory_order_acquire);
  if (local.epoch == epoch && local.entry_token == entry_token &&
      local.resume_token == resume_token) {
    *entry_candidate = local.entry_candidate;
    *resume_candidate = local.resume_candidate;
    return local.resolved;
  }

  local.entry_token = entry_token;
  local.resume_token = resume_token;
  local.entry = {};
  local.resume = {};
  local.entry_candidate = nullptr;
  local.resume_candidate = nullptr;
  local.epoch = epoch;
  local.resolved = false;

  PreparedProgram *const entry = LookupPinnedProgram(entry_token);
  if (!entry)
    return false;
  local.entry = PreparedProgramReference(entry);
  PreparedProgram *const resume = LookupPinnedProgram(resume_token);
  if (!resume)
    return false;
  local.resume = PreparedProgramReference(resume);

  const DirectCandidate *const entry_ready = ReadyCandidate(entry);
  const DirectCandidate *const resume_ready = ReadyCandidate(resume);
  if (!entry_ready || !resume_ready ||
      std::memcmp(entry->micro.data(), resume->micro.data(),
                  entry->micro.size()) != 0 ||
      entry->configuration_bits != resume->configuration_bits ||
      entry->semantic_profile_key != resume->semantic_profile_key ||
      entry->analysis_abi_version != resume->analysis_abi_version ||
      entry->analysis.resume_pcs.size() != 1 ||
      entry->analysis.resume_pcs.front() != resume->start_pc ||
      resume->analysis.resume_pcs.size() != 1 ||
      resume->analysis.resume_pcs.front() != resume->start_pc ||
      entry_ready->kernel.header_pc != resume_ready->kernel.header_pc ||
      entry_ready->kernel.latch_pc != resume_ready->kernel.latch_pc ||
      !SameMemoryInputs(entry_ready->generated, resume_ready->generated) ||
      !SameStaticOutputContract(*entry_ready, *resume_ready) ||
      entry_ready->generated.clamp_stable_lanes !=
          resume_ready->generated.clamp_stable_lanes ||
      FinalViDependsOnInitialVf(resume_ready->invocation)) {
    return false;
  }

  local.entry_candidate = entry_ready;
  local.resume_candidate = resume_ready;
  local.resolved = true;
  *entry_candidate = entry_ready;
  *resume_candidate = resume_ready;
  return true;
}

bool HasConstantAddress(const GeneratedCgProgram &program,
                        const AffineQwordAddress &address) {
  return std::any_of(
      program.constant_inputs.begin(), program.constant_inputs.end(),
      [&address](const CgConstantInput &input) {
        return input.address == address;
      });
}

struct ContinuationConstantOverride {
  DirectProgramToken entry_program;
  const DirectContinuationSeed *seed = nullptr;
  const GeneratedCgProgram *resume_generated = nullptr;
};

thread_local const ContinuationConstantOverride
    *s_continuation_constant_override = nullptr;

class ScopedContinuationConstantOverride {
public:
  explicit ScopedContinuationConstantOverride(
      const ContinuationConstantOverride *replacement)
      : m_previous(s_continuation_constant_override) {
    s_continuation_constant_override = replacement;
  }

  ~ScopedContinuationConstantOverride() {
    s_continuation_constant_override = m_previous;
  }

private:
  const ContinuationConstantOverride *m_previous = nullptr;
};

ConstantUniform *FindConstantUniform(GpuVuDraw *draw, u32 input_index) {
  if (!draw || !draw->UniformBlock())
    return nullptr;
  for (ConstantUniform &uniform :
       draw->UniformBlock().Get()->constant_uniforms) {
    if (uniform.input_index == input_index)
      return &uniform;
  }
  return nullptr;
}

const ConstantUniform *FindConstantUniform(const GpuVuDraw &draw,
                                           u32 input_index) {
  for (const ConstantUniform &uniform : draw.ConstantUniforms()) {
    if (uniform.input_index == input_index)
      return &uniform;
  }
  return nullptr;
}

bool ConfigureGeometry(const DirectTfxContract& contract,
                       const GeneratedCgProgram& generated,
                       GpuVuDraw* draw,
                       u32 active_nested_outer_iterations = 0u) {
  if (!draw)
    return false;
  const u32 vertices = contract.vertex_count;
  if (vertices == 0)
    return false;
  const u32 nested_outer_iterations = active_nested_outer_iterations != 0u
      ? active_nested_outer_iterations
      : generated.nested_outer_iterations;
  const u64 nested_invocations =
      static_cast<u64>(nested_outer_iterations) *
      generated.nested_child_iterations;
  const bool nested_flat_line_product = generated.uses_nested_iteration_grid &&
      generated.uses_flat_index_inputs && !generated.uses_flat_instance_inputs &&
      !generated.uses_loop_kernel_private_store_output &&
      contract.primitive == GS_LINESTRIP && !contract.gouraud &&
      (generated.loop_kernel_source_abi == GeneratedLoopKernelNestedFlatProductCgAbiVersion ||
       generated.loop_kernel_source_abi == GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion) &&
      DirectTfxFlatLineIndexDomain(vertices) != 0u;
  if (generated.uses_nested_iteration_grid) {
    if (nested_invocations != vertices ||
        nested_outer_iterations > generated.nested_outer_iterations ||
        generated.nested_outer_iterations == 0u ||
        generated.nested_child_iterations == 0u ||
        !generated.uses_buffered_batch_inputs ||
        generated.uses_flat_instance_inputs ||
        (generated.uses_flat_index_inputs && !nested_flat_line_product)) {
      return false;
    }
  } else if (generated.nested_outer_iterations != 0u ||
             generated.nested_child_iterations != 0u) {
    return false;
  }

  u32 primitive_count = 0;
  u32 native_indices = vertices;
  switch (contract.primitive) {
  case GS_POINTLIST:
    primitive_count = vertices;
    break;
  case GS_LINELIST:
    if ((vertices & 1u) != 0)
      return false;
    primitive_count = vertices / 2;
    break;
  case GS_LINESTRIP:
    if (vertices < 2)
      return false;
    primitive_count = vertices - 1;
    break;
  case GS_TRIANGLELIST:
    if ((vertices % 3u) != 0)
      return false;
    primitive_count = vertices / 3;
    break;
  case GS_TRIANGLESTRIP:
  case GS_TRIANGLEFAN:
    if (vertices < 3)
      return false;
    primitive_count = vertices - 2;
    break;
  default:
    return false;
  }

  if (generated.uses_flat_instance_inputs ||
      generated.uses_flat_index_inputs) {
    if (generated.uses_flat_instance_inputs &&
        generated.uses_flat_index_inputs)
      return false;
    if (generated.flat_vertices_per_primitive == 0)
      return false;
    draw->primitive_boundary = generated.uses_flat_index_inputs
        ? PrimitiveBoundary::ExpandedIndexed
        : PrimitiveBoundary::InstanceIndexed;
    native_indices =
        primitive_count * generated.flat_vertices_per_primitive;
  } else {
    draw->primitive_boundary = PrimitiveBoundary::Native;
  }

  draw->invocation_count = vertices;
  draw->vertex_count = vertices;
  draw->primitive_count = primitive_count;
  draw->index_count = native_indices;
  if (generated.uses_loop_kernel_state_canary) {
    // A private canary must execute the complete compiled grid. Sparse output
    // and variable-capacity prefixes cannot attest the omitted VU stores.
    return HasGeneratedLoopKernelStateCanaryContract(generated) &&
        nested_outer_iterations == generated.nested_outer_iterations &&
        draw->ConfigurePrivateStatePoints();
  }
  return primitive_count != 0 && native_indices != 0;
}

std::unique_ptr<GpuVuDraw> BuildGeneratedAffineGpuVuDrawInternal(
    const ShaderKey& key, const DirectTfxContract& contract,
    const std::array<u32, 4>& expected_gif_tag,
    const ParallelInvocationPlan& invocation,
    const GeneratedCgProgram& generated,
    const InvocationEvaluationContext& context,
    const std::vector<VifUnpackSpan>& spans,
    DirectProgramToken continuation_override_token, std::string* error,
    const std::array<u16, 16>* resolved_entry_vi = nullptr,
    const std::array<u16, 16>* final_vi_override = nullptr,
    u32 final_vi_override_mask = 0u,
    bool gif_tag_pre_attested = false,
    bool allow_compact_raw_inputs = false,
    u32 active_nested_outer_iterations = 0u) {
  const auto fail = [error](AdmissionFailure reason, const char* detail) {
    RecordDirectAdmissionFailure(reason);
    if (error)
      *error = detail;
    return std::unique_ptr<GpuVuDraw>{};
  };
  if (error)
    error->clear();
  if ((key.low == 0u && key.high == 0u) ||
      !context.initial_vi || !context.initial_vf_words ||
      !context.read_memory_u32) {
    return fail(AdmissionFailure::NoInputSpans,
                "generated affine draw lacks immutable inputs");
  }

  const auto read_memory_qwords = [&context](u16 first_qword,
                                              u32 qword_count,
                                              u32* values) {
    if (!values || qword_count == 0u)
      return false;
    if (context.read_memory_qwords) {
      return context.read_memory_qwords(context.memory_user, first_qword,
                                        qword_count, values);
    }
    if (!context.read_memory_u32)
      return false;
    for (u32 qword = 0u; qword < qword_count; qword++) {
      for (u32 lane = 0u; lane < 4u; lane++) {
        if (!context.read_memory_u32(
                context.memory_user,
                static_cast<u16>((first_qword + qword) & 0x3ffu),
                static_cast<u8>(lane), &values[qword * 4u + lane])) {
          return false;
        }
      }
    }
    return true;
  };

  // One MTVU thread owns descriptor construction. Preserve the dense formula
  // workspace between invocations; this executes address/control formulas,
  // never VU arithmetic or a per-vertex transform.
  thread_local InvocationEvaluationWorkspace evaluation_workspace;
  evaluation_workspace.Begin(invocation.values.size());

  std::array<u32, 4> tag{};
  if (gif_tag_pre_attested) {
    tag = expected_gif_tag;
  } else if (!ReadGifTag(invocation, context, &evaluation_workspace, &tag) ||
             tag != expected_gif_tag) {
    return fail(AdmissionFailure::TagMismatch,
                "generated affine GIF tag changed before effects");
  }
  if (!ClampStableSeedsHold(generated, context)) {
    return fail(AdmissionFailure::SeedUnstable,
                "generated affine seed is outside its clamp proof");
  }

  auto draw = std::make_unique<GpuVuDraw>();
  draw->SetUniformBlock(
      GpuVuUniformBlockRef::Adopt(new GpuVuUniformBlock()));
  GpuVuUniformBlock* const uniforms = draw->UniformBlock().Get();
  draw->program = key;
  draw->direct_tfx = contract;
  draw->gif_tag = tag;
  draw->lowering = OutputLowering::DirectTfx;
  draw->execution = ExecutionKind::GeneratedParallel;
  if (!ConfigureGeometry(contract, generated, draw.get(),
                         active_nested_outer_iterations)) {
    return fail(AdmissionFailure::GeometryFailed,
                "generated affine geometry is not representable");
  }
  // A capacity-compatible generated root may execute a strict prefix of its
  // compiled outer grid.  The INDEX domain makes rows outside this count
  // unreachable.  Resolve and copy only the active architectural sources;
  // packed compact tables retain their compiler-visible capacity stride below
  // so every emitted table offset remains unchanged.
  const u32 active_outer_iterations = generated.uses_nested_iteration_grid
      ? draw->invocation_count / generated.nested_child_iterations
      : 1u;
  if (generated.uses_loop_kernel_private_store_output) {
    if (!draw->ConfigurePrivateStoreJournal(
            generated.loop_kernel_private_store_count)) {
      return fail(AdmissionFailure::GeometryFailed,
                  "generated loop-kernel private store ABI is invalid");
    }
  } else if (generated.loop_kernel_private_store_count != 0u) {
    return fail(AdmissionFailure::GeometryFailed,
                "generated private store metadata lacks buffer ownership");
  }

  bool failed = false;
  // Input binding is a pre-effect transaction.  Keep a compact stage code so
  // physical failures identify the ownership transition which rejected the
  // descriptor instead of collapsing raw-ring, canonical-memory and compact
  // sidecar failures into one unhelpful message.
  u32 input_failure_stage = 0u;
  u32 input_failure_index = std::numeric_limits<u32>::max();
  const auto resolve_entry_base = [&](u8 base_vi, u32* value) {
    if (!value)
      return false;
    if (!resolved_entry_vi) {
      return base_vi < invocation.loop_entry_vi.size() &&
             EvaluateInvocationValue(
                 invocation, invocation.loop_entry_vi[base_vi], context,
                 &evaluation_workspace, value);
    }
    if (base_vi < resolved_entry_vi->size()) {
      *value = (*resolved_entry_vi)[base_vi];
      return true;
    }
    if (base_vi == AffineViBaseVifTop) {
      *value = context.vif_top;
      return true;
    }
    if (base_vi == AffineViBaseVifItop) {
      *value = context.vif_itop;
      return true;
    }
    return false;
  };
  constexpr size_t MaximumDescriptorMemoryInputs = 16u;
  struct ResolvedRawInput {
    RawVifPayloadRef payload;
    RawQwordBinding binding;
    bool available = false;
    bool persistent = false;
  };
  struct ResolvedCanonicalInput {
    StreamBinding binding;
    bool available = false;
  };
  std::array<ResolvedRawInput, MaximumDescriptorMemoryInputs>
      resolved_raw_inputs{};
  std::array<ResolvedCanonicalInput, MaximumDescriptorMemoryInputs>
      resolved_canonical_inputs{};
  if (generated.memory_inputs.empty() ||
      generated.memory_inputs.size() > resolved_raw_inputs.size()) {
    failed = true;
    input_failure_stage = 1u;
  }
  u32 raw_input_count = 0u;
  u32 canonical_input_count = 0u;
  for (u32 input_index = 0u;
       !failed && input_index < generated.memory_inputs.size();
       input_index++) {
    const CgMemoryInput& input = generated.memory_inputs[input_index];
    // CompactOuterInput tables are transaction-private qwords assembled from
    // several architectural sources.  Their synthetic address is only a
    // BUFFER0 binding marker; it must never be mistaken for VU-memory qword
    // zero even when a live UNPACK happens to cover that address.
    if (input.compact_outer_table ==
        CgMemoryInput::PackedCompactOuterTables) {
      continue;
    }
    u32 base_qword = 0;
    if (!resolve_entry_base(input.address.base_vi, &base_qword)) {
      failed = true;
      input_failure_stage = 2u;
      input_failure_index = input_index;
      break;
    }
    const u16 first_qword = static_cast<u16>(
        (base_qword + input.address.qword_offset) & 0x3ffu);
    RawQwordBinding raw{};
    const VifUnpackSpan* const span = generated.uses_nested_iteration_grid
        ? FindGridInputSpan(
              spans, first_qword,
              input.address.outer_invocation_coefficient,
              active_outer_iterations,
              input.address.invocation_coefficient,
              generated.nested_child_iterations, &raw)
        : FindInputSpan(
              spans, first_qword, input.address.invocation_coefficient,
              draw->invocation_count, &raw);
    if (span) {
      resolved_raw_inputs[input_index] = {span->payload, raw, true, false};
      raw_input_count++;
      continue;
    }
    if (context.bind_memory_qwords_to_raw_payload) {
      RawVifPayloadRef persistent_payload;
      const bool persistent = context.bind_memory_qwords_to_raw_payload(
          context.memory_user, first_qword,
          generated.uses_nested_iteration_grid
              ? input.address.outer_invocation_coefficient
              : 0,
          generated.uses_nested_iteration_grid
              ? active_outer_iterations
              : 1u,
          input.address.invocation_coefficient,
          generated.uses_nested_iteration_grid
              ? generated.nested_child_iterations
              : draw->invocation_count,
          &persistent_payload, &raw);
      if (persistent) {
        resolved_raw_inputs[input_index] = {
            persistent_payload, raw, true, true};
        raw_input_count++;
        continue;
      }
    }

    // A qword which has not been superseded by a deferred UNPACK, a private
    // generated store, or a CPU bridge still belongs to the canonical VU1
    // generation. Bind that mapped owner directly instead of reconstructing
    // and copying it into another ring on CPU1. Require one non-wrapping range
    // so GXM can rebase the existing BUFFER0 input directly onto the canonical
    // 16 KiB VU1-memory image. The generated root must not carry a second
    // memory space or a per-load owner selection.
    if (!generated.uses_buffered_batch_inputs ||
        !context.memory_qwords_have_canonical_owner ||
        !input.address.valid ||
        input.address.invocation_coefficient < 0 ||
        (generated.uses_nested_iteration_grid &&
         input.address.outer_invocation_coefficient < 0)) {
      continue;
    }
    const u32 outer_count = generated.uses_nested_iteration_grid
        ? active_outer_iterations
        : 1u;
    const u32 child_count = generated.uses_nested_iteration_grid
        ? generated.nested_child_iterations
        : draw->invocation_count;
    const u64 outer_coefficient = generated.uses_nested_iteration_grid
        ? static_cast<u32>(input.address.outer_invocation_coefficient)
        : 0u;
    const u64 child_coefficient =
        static_cast<u32>(input.address.invocation_coefficient);
    if (outer_count == 0u || child_count == 0u)
      continue;
    const u64 extent_qwords =
        static_cast<u64>(outer_count - 1u) * outer_coefficient +
        static_cast<u64>(child_count - 1u) * child_coefficient + 1u;
    if (extent_qwords == 0u || extent_qwords > 1024u ||
        static_cast<u64>(first_qword) + extent_qwords > 1024u ||
        !context.memory_qwords_have_canonical_owner(
            context.memory_user, first_qword,
            static_cast<u32>(extent_qwords))) {
      continue;
    }
    StreamBinding binding{};
    binding.payload_byte_offset = static_cast<u32>(first_qword) * 16u;
    binding.byte_stride = static_cast<u32>(child_coefficient * 16u);
    binding.input_span = std::numeric_limits<u16>::max();
    binding.attribute_index = static_cast<u8>(input.attribute_index);
    binding.owner = StreamInputOwner::CanonicalVuMemory;
    binding.outer_byte_stride =
        static_cast<u32>(outer_coefficient * 16u);
    binding.payload_byte_extent = static_cast<u32>(extent_qwords * 16u);
    resolved_canonical_inputs[input_index] = {binding, true};
    canonical_input_count++;
  }

  constexpr u64 MaximumCompactQwords =
      static_cast<u64>(std::numeric_limits<s16>::max()) + 1u;
  const auto pack_compact_input =
      [&](const CgMemoryInput& input, std::vector<u32>* compact_words,
          StreamBinding* stream) {
    if (!compact_words || !stream)
      return false;
    *stream = {};
    if (input.compact_outer_table ==
        CgMemoryInput::PackedCompactOuterTables) {
      const u64 first_compact_qword = compact_words->size() / 4u;
      u64 packed_qwords = 0u;
      if (!generated.uses_nested_iteration_grid ||
          generated.compact_outer_inputs.empty() ||
          !input.address.valid || input.address.base_vi != 0u ||
          input.address.qword_offset != 0 ||
          input.address.invocation_coefficient != 0 ||
          input.address.outer_invocation_coefficient != 0) {
        return false;
      }
      for (const CompactOuterInputTable& table :
           generated.compact_outer_inputs) {
        if (table.sources.size() != generated.nested_outer_iterations ||
            table.sources.size() > MaximumCompactQwords - packed_qwords) {
          return false;
        }
        packed_qwords += table.sources.size();
      }
      if (packed_qwords == 0u ||
          first_compact_qword > MaximumCompactQwords ||
          packed_qwords > MaximumCompactQwords - first_compact_qword) {
        return false;
      }

      compact_words->resize(
          static_cast<size_t>(first_compact_qword + packed_qwords) * 4u);
      u32* table_destination = compact_words->data() +
          static_cast<size_t>(first_compact_qword) * 4u;
      for (const CompactOuterInputTable& table :
           generated.compact_outer_inputs) {
        // The Cg root computes each table's base from its capacity-sized
        // predecessor. Keep that layout, but do not demand sources for outer
        // rows which the active INDEX draw cannot execute. resize() has
        // value-initialized those inactive qwords, providing deterministic
        // mapped padding for speculative cache-line fetches without granting
        // them architectural provenance.
        for (u32 outer = 0u; outer < active_outer_iterations; outer++) {
          const CompactQwordSource& source = table.sources[outer];
          u32* const destination = table_destination + outer * 4u;
          if (source.kind == CompactQwordSourceKind::InitialVf) {
            if (source.reg == 0u || source.reg >= 32u ||
                !context.InitialVfRegisterAvailable(source.reg)) {
              return false;
            }
            std::memcpy(destination,
                        context.initial_vf_words + source.reg * 4u,
                        4u * sizeof(u32));
          } else if (source.kind == CompactQwordSourceKind::Memory) {
            const AffineQwordAddress& address = source.memory_address;
            u32 base_qword = 0u;
            if (!address.valid ||
                address.invocation_coefficient != 0 ||
                address.outer_invocation_coefficient != 0 ||
                !resolve_entry_base(address.base_vi, &base_qword)) {
              return false;
            }
            const u16 source_qword = static_cast<u16>(
                (base_qword + address.qword_offset) & 0x3ffu);
            if (!read_memory_qwords(source_qword, 1u, destination))
              return false;
          } else {
            return false;
          }
        }
        table_destination += table.sources.size() * 4u;
      }

      stream->payload_byte_offset =
          static_cast<u32>(first_compact_qword * 16u);
      stream->byte_stride = 0u;
      stream->input_span = std::numeric_limits<u16>::max();
      stream->attribute_index = static_cast<u8>(input.attribute_index);
      stream->outer_byte_stride = 0u;
      stream->payload_byte_extent = static_cast<u32>(packed_qwords * 16u);
      return true;
    }
    if (input.compact_outer_table != CgMemoryInput::OrdinaryVuMemory)
      return false;

    u32 base_qword = 0u;
    if (!resolve_entry_base(input.address.base_vi, &base_qword) ||
        input.address.invocation_coefficient < 0 ||
        (generated.uses_nested_iteration_grid &&
         input.address.outer_invocation_coefficient < 0)) {
      return false;
    }
    const u32 outer_count = generated.uses_nested_iteration_grid
        ? active_outer_iterations
        : 1u;
    const u32 child_count = generated.uses_nested_iteration_grid
        ? generated.nested_child_iterations
        : draw->invocation_count;
    const u64 outer_coefficient = generated.uses_nested_iteration_grid
        ? static_cast<u32>(input.address.outer_invocation_coefficient)
        : 0u;
    const u64 child_coefficient =
        static_cast<u32>(input.address.invocation_coefficient);
    if (outer_count == 0u || child_count == 0u)
      return false;
    const u64 extent_qwords =
        static_cast<u64>(outer_count - 1u) * outer_coefficient +
        static_cast<u64>(child_count - 1u) * child_coefficient + 1u;
    const u64 first_compact_qword = compact_words->size() / 4u;
    if (extent_qwords == 0u ||
        first_compact_qword > MaximumCompactQwords ||
        extent_qwords > MaximumCompactQwords - first_compact_qword) {
      return false;
    }
    compact_words->resize(
        static_cast<size_t>(first_compact_qword + extent_qwords) * 4u);
    const u16 first_source_qword = static_cast<u16>(
        (base_qword + input.address.qword_offset) & 0x3ffu);
    bool copied = true;
    if (child_coefficient == 0u && outer_coefficient == 0u) {
      copied = read_memory_qwords(
          first_source_qword, 1u,
          compact_words->data() + first_compact_qword * 4u);
    } else {
      for (u32 outer = 0u; outer < outer_count && copied; outer++) {
        const u64 outer_relative =
            static_cast<u64>(outer) * outer_coefficient;
        if (child_coefficient == 0u) {
          const u16 source_qword = static_cast<u16>(
              (static_cast<u64>(first_source_qword) + outer_relative) &
              0x3ffu);
          u32* const destination = compact_words->data() +
              static_cast<size_t>(first_compact_qword + outer_relative) * 4u;
          copied = read_memory_qwords(source_qword, 1u, destination);
          continue;
        }
        if (child_coefficient == 1u) {
          const u16 source_qword = static_cast<u16>(
              (static_cast<u64>(first_source_qword) + outer_relative) &
              0x3ffu);
          u32* const destination = compact_words->data() +
              static_cast<size_t>(first_compact_qword + outer_relative) * 4u;
          copied = read_memory_qwords(
              source_qword, child_count, destination);
          continue;
        }
        for (u32 child = 0u; child < child_count && copied; child++) {
          const u64 relative_qword =
              outer_relative + static_cast<u64>(child) * child_coefficient;
          const u16 source_qword = static_cast<u16>(
              (static_cast<u64>(first_source_qword) + relative_qword) &
              0x3ffu);
          u32* const destination = compact_words->data() +
              static_cast<size_t>(first_compact_qword + relative_qword) * 4u;
          copied = read_memory_qwords(source_qword, 1u, destination);
        }
      }
    }
    if (!copied)
      return false;

    stream->payload_byte_offset =
        static_cast<u32>(first_compact_qword * 16u);
    stream->byte_stride = static_cast<u32>(child_coefficient * 16u);
    stream->input_span = std::numeric_limits<u16>::max();
    stream->attribute_index = static_cast<u8>(input.attribute_index);
    stream->outer_byte_stride =
        static_cast<u32>(outer_coefficient * 16u);
    stream->payload_byte_extent = static_cast<u32>(extent_qwords * 16u);
    return true;
  };

  std::array<RawVifPayloadRef, MaximumDescriptorMemoryInputs + 1u>
      planned_payloads{};
  std::array<StreamBinding, MaximumDescriptorMemoryInputs> planned_streams{};
  size_t planned_payload_count = 0u;
  size_t planned_stream_count = 0u;
  bool descriptor_inputs_mutated = false;
  const auto reset_input_plan = [&]() {
    planned_payload_count = 0u;
    planned_stream_count = 0u;
  };
  const auto add_planned_payload = [&](const RawVifPayloadRef& payload,
                                       u16* span_index) {
    if (!span_index || !payload.IsValid())
      return false;
    for (size_t index = 0u; index < planned_payload_count; index++) {
      if (SamePayload(planned_payloads[index], payload)) {
        *span_index = static_cast<u16>(index);
        return true;
      }
    }
    if (planned_payload_count >= planned_payloads.size())
      return false;
    *span_index = static_cast<u16>(planned_payload_count);
    planned_payloads[planned_payload_count++] = payload;
    return true;
  };
  const auto plan_raw_input = [&](u32 input_index) {
    if (input_index >= generated.memory_inputs.size() ||
        input_index >= resolved_raw_inputs.size() ||
        !resolved_raw_inputs[input_index].available ||
        planned_stream_count >= planned_streams.size()) {
      return false;
    }
    const ResolvedRawInput& resolved = resolved_raw_inputs[input_index];
    u16 span_index = 0u;
    if (!add_planned_payload(resolved.payload, &span_index))
      return false;
    const CgMemoryInput& input = generated.memory_inputs[input_index];
    StreamBinding& stream = planned_streams[planned_stream_count++];
    stream.payload_byte_offset = resolved.binding.payload_byte_offset;
    stream.byte_stride = resolved.binding.byte_stride;
    stream.input_span = span_index;
    stream.attribute_index = static_cast<u8>(input.attribute_index);
    stream.owner = StreamInputOwner::RawInput;
    stream.outer_byte_stride = resolved.binding.outer_byte_stride;
    stream.payload_byte_extent = resolved.binding.payload_byte_extent;
    return true;
  };
  const auto plan_canonical_input = [&](u32 input_index) {
    if (input_index >= generated.memory_inputs.size() ||
        input_index >= resolved_canonical_inputs.size() ||
        !resolved_canonical_inputs[input_index].available ||
        planned_stream_count >= planned_streams.size()) {
      return false;
    }
    planned_streams[planned_stream_count++] =
        resolved_canonical_inputs[input_index].binding;
    return true;
  };
  const auto commit_input_plan = [&]() {
    const bool installed = draw->SetGeneratedInputPlan(
        planned_payloads.data(), planned_payload_count,
        planned_streams.data(), planned_stream_count,
        generated.uses_buffered_batch_inputs);
    descriptor_inputs_mutated = installed;
    return installed;
  };

#if defined(__vita__) && !defined(VITASX2_QEMU_VALIDATION)
  // MTVU reuses one descriptor workspace and publishes only the finished
  // sparse sidecar into the mapped input ring. No vector is retained by the
  // draw after construction.
  static thread_local std::vector<u32> compact_words_workspace;
  std::vector<u32>& compact_words = compact_words_workspace;
#else
  std::vector<u32> compact_words;
#endif

  const u32 direct_input_count = raw_input_count + canonical_input_count;
  const bool all_inputs_are_raw =
      !failed && raw_input_count == generated.memory_inputs.size();
  const bool all_inputs_are_canonical =
      !failed && canonical_input_count == generated.memory_inputs.size();
  const bool all_inputs_have_one_direct_owner =
      all_inputs_are_raw || all_inputs_are_canonical;
  bool inputs_bound = false;
  if (all_inputs_have_one_direct_owner) {
    reset_input_plan();
    for (u32 input_index = 0u;
         !failed && input_index < generated.memory_inputs.size();
         input_index++) {
      failed = all_inputs_are_raw ? !plan_raw_input(input_index)
                                  : !plan_canonical_input(input_index);
      if (failed)
      {
        input_failure_stage = 3u;
        input_failure_index = input_index;
      }
    }
    if (!failed) {
      // SetGeneratedInputPlan() is the one construction-time preflight.  It
      // retains every payload transactionally, proves this exact payload/stream
      // set with ResolveGeneratedInputWindow(), and publishes neither
      // collection on failure.  Queue sealing deliberately repeats the proof
      // after sequence assignment; walking it once more immediately before
      // installation added no independent ownership boundary.
      inputs_bound = commit_input_plan();
      failed = !inputs_bound;
      if (failed)
        input_failure_stage = 5u;
    }
  }

#if defined(__vita__) && !defined(VITASX2_QEMU_VALIDATION)
  if (!failed && !inputs_bound && allow_compact_raw_inputs &&
      DerivedGpuVuPayloadSharesRawInputArena() &&
      canonical_input_count == 0u &&
      direct_input_count != 0u &&
      direct_input_count < generated.memory_inputs.size()) {
    // Mixed raw/derived transactions used to copy every input into a new table
    // merely because one CompactOuterInput or inherited qword lacked a raw
    // owner. Preserve raw VIF bytes in place and copy only that sparse subset.
    reset_input_plan();
    compact_words.clear();
    s_hybrid_input_attempts.fetch_add(1u, std::memory_order_relaxed);
    u64 hybrid_raw_bytes = 0u;
    for (u32 input_index = 0u;
         !failed && input_index < generated.memory_inputs.size();
         input_index++) {
      if (resolved_raw_inputs[input_index].available) {
        hybrid_raw_bytes +=
            resolved_raw_inputs[input_index].binding.payload_byte_extent;
        failed = !plan_raw_input(input_index);
        continue;
      }
      if (planned_stream_count >= planned_streams.size() ||
          !pack_compact_input(generated.memory_inputs[input_index],
                              &compact_words,
                              &planned_streams[planned_stream_count])) {
        failed = true;
        input_failure_stage = 6u;
        input_failure_index = input_index;
        break;
      }
      planned_stream_count++;
    }
    RawVifPayloadRef sparse_payload;
    if (!failed) {
      const u64 compact_bytes =
          static_cast<u64>(compact_words.size()) * sizeof(u32);
      if (compact_bytes == 0u ||
          compact_bytes > std::numeric_limits<u32>::max() ||
          !CaptureDerivedGpuVuPayload(
              compact_words.data(), static_cast<u32>(compact_bytes),
              &sparse_payload)) {
        failed = true;
        input_failure_stage = 7u;
      } else {
        u16 sparse_span = 0u;
        if (!add_planned_payload(sparse_payload, &sparse_span)) {
          failed = true;
          input_failure_stage = 8u;
        } else {
          for (size_t index = 0u; index < planned_stream_count; index++) {
            if (planned_streams[index].input_span ==
                    std::numeric_limits<u16>::max() &&
                planned_streams[index].owner ==
                    StreamInputOwner::RawInput) {
              planned_streams[index].input_span = sparse_span;
            }
          }
        }
      }
    }
    if (!failed) {
      inputs_bound = commit_input_plan();
      failed = !inputs_bound;
      if (failed)
        input_failure_stage = 10u;
    }
    if (inputs_bound) {
      const u64 hybrid_derived_bytes =
          static_cast<u64>(compact_words.size()) * sizeof(u32);
      s_hybrid_input_hits.fetch_add(1u, std::memory_order_relaxed);
      s_hybrid_raw_bytes_retained.fetch_add(
          hybrid_raw_bytes, std::memory_order_relaxed);
      s_hybrid_derived_bytes.fetch_add(
          hybrid_derived_bytes, std::memory_order_relaxed);
      s_hybrid_copy_bytes_avoided.fetch_add(
          hybrid_raw_bytes, std::memory_order_relaxed);
    } else {
      s_hybrid_input_fallbacks.fetch_add(1u, std::memory_order_relaxed);
    }
    ReleaseRawVifPayload(&sparse_payload);
  }

  // A failed mixed-window attempt must not turn into partial GPU ownership.
  // Rebuild the old all-compact input table only when the failure happened
  // before descriptor publication; otherwise the draw owns the complete plan.
  if (failed && !inputs_bound && !descriptor_inputs_mutated &&
      allow_compact_raw_inputs) {
    failed = false;
  }
#endif

  if (!failed && !inputs_bound) {
    // Conservative fallback for inputs which cannot share BUFFER0 directly.
    // Unlike the mixed fast path above this retains the old all-compact ABI.
    if (!allow_compact_raw_inputs) {
      failed = true;
      input_failure_stage = 11u;
    } else {
      reset_input_plan();
      compact_words.clear();
      for (u32 input_index = 0u;
           !failed && input_index < generated.memory_inputs.size();
           input_index++) {
        if (planned_stream_count >= planned_streams.size() ||
            !pack_compact_input(generated.memory_inputs[input_index],
                                &compact_words,
                                &planned_streams[planned_stream_count])) {
          failed = true;
          input_failure_stage = 12u;
          input_failure_index = input_index;
          break;
        }
        planned_stream_count++;
      }
      if (!failed) {
#if defined(__vita__) && !defined(VITASX2_QEMU_VALIDATION)
        const u64 compact_bytes =
            static_cast<u64>(compact_words.size()) * sizeof(u32);
        RawVifPayloadRef compact_payload;
        if (compact_bytes == 0u ||
            compact_bytes > std::numeric_limits<u32>::max() ||
            !CaptureDerivedGpuVuPayload(
                compact_words.data(), static_cast<u32>(compact_bytes),
                &compact_payload)) {
          failed = true;
          input_failure_stage = 13u;
        } else {
          u16 compact_span = 0u;
          if (!add_planned_payload(compact_payload, &compact_span)) {
            failed = true;
            input_failure_stage = 14u;
          } else {
            for (size_t index = 0u; index < planned_stream_count; index++) {
              planned_streams[index].input_span = compact_span;
              planned_streams[index].owner = StreamInputOwner::RawInput;
            }
          }
          if (!failed) {
            inputs_bound = commit_input_plan();
            failed = !inputs_bound;
            if (failed)
              input_failure_stage = 16u;
          }
          ReleaseRawVifPayload(&compact_payload);
        }
#else
        for (size_t index = 0u; index < planned_stream_count; index++) {
          planned_streams[index].input_span =
              std::numeric_limits<u16>::max();
          draw->streams.push_back(planned_streams[index]);
        }
        inputs_bound = draw->SetCompactRawInputWords(std::move(compact_words));
        failed = !inputs_bound;
#endif
      }
    }
  }
  if (failed ||
      (generated.uses_buffered_batch_inputs &&
       !draw->HasCompactRawInputs() &&
       !draw->GeneratedInputWindowProof())) {
    if (!failed)
      input_failure_stage = 17u;
    static std::atomic<u64> input_failure_reports{0u};
    const u64 report = input_failure_reports.fetch_add(
                           1u, std::memory_order_relaxed) +
                       1u;
    if (report <= 8u || (report & (report - 1u)) == 0u) {
      Console.Warning(
          "GPU-VU: immutable input binder rejected report=%llu stage=%u "
          "input=%u "
          "inputs=%u raw=%u canonical=%u direct=%u payloads=%u streams=%u "
          "compact_words=%u compact_allowed=%u shared_arena=%u "
          "descriptor_mutated=%u buffered=%u pre_effect=1.",
          static_cast<unsigned long long>(report), input_failure_stage,
          input_failure_index,
          static_cast<u32>(generated.memory_inputs.size()), raw_input_count,
          canonical_input_count, direct_input_count,
          static_cast<u32>(planned_payload_count),
          static_cast<u32>(planned_stream_count),
          static_cast<u32>(compact_words.size()),
          allow_compact_raw_inputs ? 1u : 0u,
          DerivedGpuVuPayloadSharesRawInputArena() ? 1u : 0u,
          descriptor_inputs_mutated ? 1u : 0u,
          generated.uses_buffered_batch_inputs ? 1u : 0u);
    }
    return fail(AdmissionFailure::InputResolveFailed,
                "generated affine stream is outside immutable input spans");
  }

  for (const CgConstantInput& input : generated.constant_inputs) {
    const ContinuationConstantOverride* const override =
        s_continuation_constant_override;
    if (continuation_override_token.IsValid() && override &&
        override->entry_program == continuation_override_token &&
        override->seed && override->resume_generated &&
        !HasConstantAddress(*override->resume_generated, input.address)) {
      if (input.uniform_index >= override->seed->constant_values.size() ||
          (override->seed->constant_mask &
           (1u << input.uniform_index)) == 0u) {
        failed = true;
        break;
      }
      ConstantUniform uniform{};
      uniform.input_index = static_cast<u8>(input.uniform_index);
      uniform.bits = override->seed->constant_values[input.uniform_index];
      uniforms->constant_uniforms.push_back(std::move(uniform));
      continue;
    }

    u32 base_qword = 0;
    if (!resolve_entry_base(input.address.base_vi, &base_qword)) {
      failed = true;
      break;
    }
    ConstantUniform uniform{};
    uniform.input_index = static_cast<u8>(input.uniform_index);
    const u16 address = static_cast<u16>(
        (base_qword + input.address.qword_offset) & 0x3ffu);
    // Constant records are complete qwords.  Resolve them through the same
    // immutable bulk workspace as compact BUFFER0 input instead of performing
    // four reverse searches through the deferred-UNPACK journal.  On Vita the
    // first bulk request builds one qword-owner table for the whole descriptor;
    // all later constants and final-state leaves are then O(1) pointer copies.
    if (!read_memory_qwords(address, 1u, uniform.bits.data()))
      failed = true;
    if (failed)
      break;
    uniforms->constant_uniforms.push_back(uniform);
  }
  if (failed) {
    return fail(AdmissionFailure::InputResolveFailed,
                "generated affine constant input is unavailable");
  }

  for (u32 reg = 1; reg < 32; reg++) {
    if ((generated.vf_uniform_mask & (1u << reg)) == 0u)
      continue;
    if (!context.InitialVfRegisterAvailable(reg)) {
      return fail(AdmissionFailure::BuildFailed,
                  "generated affine VF input is deferred");
    }
    VectorUniform uniform{};
    uniform.register_index = static_cast<u8>(reg);
    std::memcpy(uniform.bits.data(), context.initial_vf_words + reg * 4u,
                sizeof(uniform.bits));
    uniforms->vf_uniforms.push_back(uniform);
  }
  if (generated.uses_acc_uniform) {
    if (!context.initial_acc_words ||
        (context.unavailable_initial_acc_lanes & 0x0fu) != 0u)
      return fail(AdmissionFailure::BuildFailed,
                  "generated affine ACC input is unavailable");
    std::memcpy(draw->acc_uniform.data(), context.initial_acc_words,
                sizeof(draw->acc_uniform));
  }
  if (generated.uses_q_uniform) {
    if (!context.initial_q_available)
      return fail(AdmissionFailure::BuildFailed,
                  "generated affine Q input is deferred");
    draw->scalar_uniforms.present |= ScalarUniformQ;
    draw->scalar_uniforms.q = context.initial_q;
  }
  if (generated.uses_p_uniform) {
    if (!context.initial_p_available)
      return fail(AdmissionFailure::BuildFailed,
                  "generated affine P input is deferred");
    draw->scalar_uniforms.present |= ScalarUniformP;
    draw->scalar_uniforms.p = context.initial_p;
  }
  if (generated.uses_i_uniform) {
    if (!context.initial_i_available)
      return fail(AdmissionFailure::BuildFailed,
                  "generated affine I input is deferred");
    draw->scalar_uniforms.present |= ScalarUniformI;
    draw->scalar_uniforms.i = context.initial_i;
  }
  if (generated.uses_gif_q_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformGifQ;
    draw->scalar_uniforms.gif_q = 0x3f800000u;
  }
  if (final_vi_override) {
    draw->final_vi_values = *final_vi_override;
    draw->final_vi_write_mask = final_vi_override_mask;
  } else if (!EvaluateFinalViState(
                 invocation, context, &evaluation_workspace,
                 &draw->final_vi_values, &draw->final_vi_write_mask)) {
    return fail(AdmissionFailure::BuildFailed,
                "generated affine final-VI formula rejected");
  }
  u64 canonical_bytes = 0u;
  u64 canonical_bindings = 0u;
  for (const StreamBinding& stream : draw->streams) {
    if (stream.owner != StreamInputOwner::CanonicalVuMemory)
      continue;
    canonical_bindings++;
    canonical_bytes += stream.payload_byte_extent;
  }
  s_canonical_input_bindings.fetch_add(
      canonical_bindings, std::memory_order_relaxed);
  s_canonical_input_bytes.fetch_add(
      canonical_bytes, std::memory_order_relaxed);
  u64 persistent_raw_bytes = 0u;
  u64 persistent_raw_bindings = 0u;
  for (const ResolvedRawInput& resolved : resolved_raw_inputs) {
    if (!resolved.available || !resolved.persistent)
      continue;
    const bool retained = std::any_of(
        draw->InputPayloads().begin(), draw->InputPayloads().end(),
        [&resolved](const RawVifPayloadRef& payload) {
          return SamePayload(payload, resolved.payload);
        });
    if (!retained)
      continue;
    persistent_raw_bindings++;
    persistent_raw_bytes += resolved.binding.payload_byte_extent;
  }
  s_persistent_raw_input_bindings.fetch_add(
      persistent_raw_bindings, std::memory_order_relaxed);
  s_persistent_raw_input_bytes.fetch_add(
      persistent_raw_bytes, std::memory_order_relaxed);
  return draw;
}

} // namespace

std::unique_ptr<GpuVuDraw> BuildGeneratedAffineGpuVuDraw(
    const ShaderKey& key, const DirectTfxContract& contract,
    const std::array<u32, 4>& gif_tag,
    const ParallelInvocationPlan& invocation,
    const GeneratedCgProgram& generated,
    const InvocationEvaluationContext& context,
    const std::vector<VifUnpackSpan>& spans, std::string* error) {
  return BuildGeneratedAffineGpuVuDrawInternal(
      key, contract, gif_tag, invocation, generated, context, spans, {},
      error);
}

std::unique_ptr<GpuVuDraw> BuildGeneratedResolvedGridGpuVuDraw(
    const ShaderKey& key, const DirectTfxContract& contract,
    const std::array<u32, 4>& attested_gif_tag,
    const GeneratedCgProgram& generated,
    const std::array<u16, 16>& resolved_entry_vi,
    const std::array<u16, 16>& final_vi, u32 final_vi_write_mask,
    u32 active_nested_outer_iterations,
    const InvocationEvaluationContext& context,
    const std::vector<VifUnpackSpan>& spans, std::string* error) {
  // The closed-form root has no legacy per-pair InvocationValue program: its
  // complete entry and exit VI states were already reduced from the canonical
  // PairPlan CFG.  Pass an empty legacy plan only as an unused ABI argument;
  // all address bases, the GIF tag, and final VI values are supplied by the
  // resolved proof below.
  const ParallelInvocationPlan unused_invocation;
  return BuildGeneratedAffineGpuVuDrawInternal(
      key, contract, attested_gif_tag, unused_invocation, generated, context,
      spans, {}, error, &resolved_entry_vi, &final_vi,
      final_vi_write_mask, true, true, active_nested_outer_iterations);
}

DirectProgramToken PrepareDirectProgram(const u8 *micro, u32 micro_size,
                                        u32 start_pc) {
  return PrepareDirectProgramForConfiguration(
      micro, micro_size, start_pc,
      GetCurrentUniversalMicroProgramConfigurationBits());
}

DirectProgramToken PrepareDirectProgramForConfiguration(
    const u8* micro, u32 micro_size, u32 start_pc,
    u32 configuration_bits) {
  ScopedPreparationTimer preparation_timer;
  if (!micro || micro_size != VU1_PROGSIZE || (start_pc & 7u) != 0 ||
      (configuration_bits & ~UniversalConfigurationKnownMask) != 0) {
    return {};
  }
  start_pc &= VU1_PROGMASK;
  const u64 source_hash = HashExactSource(micro, micro_size);
  const u32 semantic_profile_key = SemanticProfileKey(configuration_bits);

  {
    std::lock_guard lock(s_cache_mutex);
    for (u32 slot = 0; slot < s_cache.size(); slot++) {
      CacheSlot &entry = s_cache[slot];
      if (!entry.program ||
          !Matches(*entry.program, micro, source_hash, start_pc,
                   configuration_bits, semantic_profile_key)) {
        continue;
      }
      entry.last_use = ++s_cache_clock;
      s_preparation_cache_hits.fetch_add(1, std::memory_order_relaxed);
      return MakeToken(slot, entry.generation);
    }
  }

  std::unique_ptr<PreparedProgram> prepared =
      BuildPreparedProgram(micro, source_hash, start_pc, configuration_bits,
                           semantic_profile_key);
  if (!prepared)
    return {};

  PreparedProgramReference retired;
  DirectProgramToken token;
  u32 published_start_pc = 0;
  u32 published_blocks = 0;
  u32 published_loops = 0;
  u32 published_candidates = 0;
  {
    std::lock_guard lock(s_cache_mutex);
    // CPU0 is the ordinary preparation owner, but retain exact behavior if a
    // diagnostic preparation raced it.
    for (u32 slot = 0; slot < s_cache.size(); slot++) {
      CacheSlot &entry = s_cache[slot];
      if (!entry.program ||
          !Matches(*entry.program, micro, source_hash, start_pc,
                   configuration_bits, semantic_profile_key)) {
        continue;
      }
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
    retired = std::move(entry.program);
    entry.generation = (entry.generation + 1u) & TokenGenerationMask;
    if (entry.generation == 0)
      entry.generation = 1;
    entry.program = PreparedProgramReference::Adopt(prepared.release());
    entry.last_use = ++s_cache_clock;
    published_start_pc = entry.program->start_pc;
    published_blocks =
        static_cast<u32>(entry.program->analysis.blocks.size());
    published_loops =
        static_cast<u32>(entry.program->analysis.natural_loops.size());
    published_candidates =
        static_cast<u32>(entry.program->candidates.size());
    token = MakeToken(replacement, entry.generation);
    s_cache_epoch.fetch_add(1, std::memory_order_release);
    s_prepared_programs.fetch_add(1, std::memory_order_relaxed);
  }

  // Retiring a large analysis can release many nested allocations. Do that
  // after dropping the cache mutex so other preparation/lookups never inherit
  // allocator latency.
  retired = {};
  Console.WriteLn(
      "GPU-VU: prepared VU1 entry %04x (%u blocks, %u loops, %u parallel "
      "direct candidates, config=%08x profile=%08x analysis_abi=%u).",
      published_start_pc, published_blocks, published_loops,
      published_candidates, configuration_bits, semantic_profile_key,
      GeneratedLoopAnalysisAbiVersion);
  return token;
}

bool PrimeDirectProgram(DirectProgramToken token,
                        const InvocationEvaluationContext &context) {
  s_prime_attempts.fetch_add(1, std::memory_order_relaxed);
  PreparedProgram *const prepared = LookupPinnedProgram(token);
  if (!prepared)
    return false;

  if (prepared->primed.load(std::memory_order_acquire)) {
    s_prime_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  std::lock_guard lock(prepared->mutex);
  if (prepared->primed.load(std::memory_order_relaxed)) {
    s_prime_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Priming is a cold, bounded attempt. A semantic/source rejection or a
  // temporarily unavailable compiler must never turn the VU execution stream
  // into a retry loop on the 496 MHz MTVU core. Compiler-service retry and a
  // lower GPU output selection are separate descriptor-level events.
  prepared->primed.store(true, std::memory_order_release);
  Console.WriteLn("GPU-VU: cold prime of entry %04x (%u candidates).",
                  prepared->start_pc,
                  static_cast<u32>(prepared->candidates.size()));
  bool resolved_tag = false;
  for (u32 index = 0; index < prepared->candidates.size(); index++) {
    DirectCandidate &candidate = prepared->candidates[index];
    InvocationEvaluationWorkspace workspace;
    workspace.Begin(candidate.invocation.values.size());
    // Bounded: the cold prime of one prepared program runs exactly once.
    u32 address = 0;
    const bool address_resolved = EvaluateInvocationValue(
        candidate.invocation, candidate.invocation.gif_tag_qword_address,
        context, &workspace, &address);
    std::array<u32, 4> tag{};
    if (!ReadGifTag(candidate.invocation, context, &workspace, &tag)) {
      Console.WriteLn(
          "GPU-VU: prime entry %04x candidate %u could not read the packed "
          "GIF tag (address resolved %u, address %04x).",
          prepared->start_pc, index, address_resolved ? 1u : 0u, address);
      continue;
    }
    resolved_tag = true;
    std::string rejection;
    const bool requested = RequestCandidate(&candidate, tag, &rejection);
    Console.WriteLn(
        "GPU-VU: prime entry %04x candidate %u tag %s%04x = %08x %08x %08x "
        "%08x -> requested %u, accepted %u%s%s.",
        prepared->start_pc, index,
        address_resolved ? "@" : "from-vf ", address,
        tag[0], tag[1], tag[2], tag[3],
        requested ? 1u : 0u, candidate.request_accepted ? 1u : 0u,
        rejection.empty() ? "" : ", rejected: ", rejection.c_str());
    if (requested) {
#if defined(__vita__)
      if (candidate.request_accepted &&
          QueryGeneratedProgram(candidate.key) ==
              GeneratedProgramState::Ready) {
        prepared->ready_candidate_plus_one.store(index + 1,
                                                 std::memory_order_release);
      }
#else
      if (candidate.request_accepted) {
        prepared->ready_candidate_plus_one.store(index + 1,
                                                 std::memory_order_release);
      }
#endif
      return true;
    }
  }

  if (!resolved_tag)
    s_gif_address_failures.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool IsDirectProgramReadyForInput(DirectProgramToken token) {
  return GetDirectProgramInputState(token) == DirectInputState::Ready;
}

DirectInputState GetDirectProgramInputState(DirectProgramToken token) {
  PreparedProgram *const prepared = LookupPinnedProgram(token);
  if (!prepared)
    return DirectInputState::Unavailable;
  return prepared->ready_candidate_plus_one.load(std::memory_order_acquire) != 0
             ? DirectInputState::Ready
             : DirectInputState::Pending;
}

void PublishDirectProgramRegistration(const ShaderKey &key, bool succeeded) {
  if (!succeeded)
    return;

  std::lock_guard cache_lock(s_cache_mutex);
  for (CacheSlot &slot : s_cache) {
    PreparedProgram *const prepared = slot.program.Get();
    if (!prepared ||
        prepared->ready_candidate_plus_one.load(
            std::memory_order_relaxed) != 0) {
      continue;
    }

    std::lock_guard program_lock(prepared->mutex);
    for (u32 index = 0; index < prepared->candidates.size(); index++) {
      const DirectCandidate &candidate = prepared->candidates[index];
      if (candidate.has_generated_root && candidate.request_accepted &&
          !candidate.generated.requires_dynamic_entry_state &&
          candidate.key == key) {
        prepared->ready_candidate_plus_one.store(index + 1,
                                                 std::memory_order_release);
        break;
      }
    }
  }
}

std::unique_ptr<GpuVuDraw>
BuildDirectGpuVuDraw(DirectProgramToken token,
                     const InvocationEvaluationContext &context,
                     const std::vector<VifUnpackSpan> &spans) {
  PreparedProgram *const prepared = LookupPinnedProgram(token);
  if (!prepared || spans.empty() || !context.initial_vi ||
      !context.initial_vf_words ||
      !context.read_memory_u32) {
    RecordDirectAdmissionFailure(AdmissionFailure::NoInputSpans);
    return {};
  }

  const u32 ready_candidate =
      prepared->ready_candidate_plus_one.load(std::memory_order_acquire);
  if (ready_candidate == 0 || ready_candidate > prepared->candidates.size()) {
    RecordDirectAdmissionFailure(AdmissionFailure::NoReadyCandidate);
    return {};
  }

  const DirectCandidate &candidate = prepared->candidates[ready_candidate - 1];
  return BuildGeneratedAffineGpuVuDrawInternal(
      candidate.key, candidate.contract, candidate.gif_tag,
      candidate.invocation, candidate.generated, context, spans, token,
      nullptr);
}

bool IsDirectContinuationPair(DirectProgramToken entry,
                              DirectProgramToken resume) {
  const DirectCandidate *entry_candidate = nullptr;
  const DirectCandidate *resume_candidate = nullptr;
  return ResolveContinuationPair(
      entry, resume, &entry_candidate, &resume_candidate);
}

bool CaptureDirectContinuationSeed(
    DirectProgramToken entry, DirectProgramToken resume,
    const InvocationEvaluationContext &entry_context,
    const GpuVuDraw &entry_draw, DirectContinuationSeed *seed) {
  if (!seed || !entry_context.initial_vi ||
      !entry_context.initial_vf_words ||
      entry_draw.lowering != OutputLowering::DirectTfx ||
      entry_draw.execution != ExecutionKind::GeneratedParallel ||
      !entry_draw.UniformBlock()) {
    return false;
  }

  const DirectCandidate *entry_candidate = nullptr;
  const DirectCandidate *resume_candidate = nullptr;
  if (!ResolveContinuationPair(
          entry, resume, &entry_candidate, &resume_candidate) ||
      entry_draw.program != entry_candidate->key ||
      entry_draw.gif_tag != entry_candidate->gif_tag) {
    return false;
  }

  DirectContinuationSeed captured;
  captured.entry_program = entry;
  captured.resume_program = resume;
  std::memcpy(captured.initial_vi.data(), entry_context.initial_vi,
              sizeof(captured.initial_vi));
  for (u32 reg = 0u; reg < 32u; reg++) {
    if (!entry_context.InitialVfRegisterAvailable(reg))
      return false;
  }
  std::memcpy(captured.initial_vf.data(), entry_context.initial_vf_words,
              sizeof(captured.initial_vf));
  if (entry_candidate->generated.uses_acc_uniform) {
    if (!entry_context.initial_acc_words ||
        (entry_context.unavailable_initial_acc_lanes & 0x0fu) != 0u)
      return false;
    std::memcpy(captured.initial_acc.data(),
                entry_context.initial_acc_words,
                sizeof(captured.initial_acc));
  }
  if ((entry_candidate->generated.uses_q_uniform &&
       !entry_context.initial_q_available) ||
      (entry_candidate->generated.uses_p_uniform &&
       !entry_context.initial_p_available) ||
      (entry_candidate->generated.uses_i_uniform &&
       !entry_context.initial_i_available)) {
    return false;
  }
  captured.initial_q = entry_context.initial_q;
  captured.initial_p = entry_context.initial_p;
  captured.initial_i = entry_context.initial_i;
  captured.uniform_block = entry_draw.UniformBlock();

  for (const CgConstantInput &input :
       entry_candidate->generated.constant_inputs) {
    if (input.uniform_index >= captured.constant_values.size())
      return false;
    const ConstantUniform *const uniform =
        FindConstantUniform(entry_draw, input.uniform_index);
    if (!uniform)
      return false;
    captured.constant_values[input.uniform_index] = uniform->bits;
    captured.constant_mask |= 1u << input.uniform_index;
  }
  *seed = std::move(captured);
  return true;
}

std::unique_ptr<GpuVuDraw> BuildDirectGpuVuContinuationDraw(
    const DirectContinuationSeed &seed,
    const InvocationEvaluationContext &resume_context,
    const std::vector<VifUnpackSpan> &spans) {
  if (!seed.IsValid() || !resume_context.initial_vi ||
      !resume_context.initial_vf_words || !resume_context.read_memory_u32) {
    return {};
  }

  const DirectCandidate *entry_candidate = nullptr;
  const DirectCandidate *resume_candidate = nullptr;
  if (!ResolveContinuationPair(
          seed.entry_program, seed.resume_program,
          &entry_candidate, &resume_candidate)) {
    return {};
  }

  const bool has_dynamic_constants = std::any_of(
      entry_candidate->generated.constant_inputs.begin(),
      entry_candidate->generated.constant_inputs.end(),
      [resume_candidate](const CgConstantInput &input) {
        return HasConstantAddress(resume_candidate->generated,
                                  input.address);
      });
  // A current-memory constant can legitimately differ at every MSCNT. Retain
  // the general descriptor builder for that semantic shape; only chains whose
  // complete uniform block is invariant use the compact shared path below.
  if (!seed.uniform_block || has_dynamic_constants) {
    s_general_continuation_builds.fetch_add(1,
                                            std::memory_order_relaxed);
    InvocationEvaluationContext entry_context = resume_context;
    entry_context.initial_vi = seed.initial_vi.data();
    entry_context.initial_vf_words = seed.initial_vf.data();
    entry_context.initial_acc_words = seed.initial_acc.data();
    entry_context.unavailable_initial_vf_lanes = nullptr;
    entry_context.unavailable_initial_acc_lanes = 0u;
    entry_context.initial_q = seed.initial_q;
    entry_context.initial_p = seed.initial_p;
    entry_context.initial_i = seed.initial_i;
    entry_context.initial_q_available = true;
    entry_context.initial_p_available = true;
    entry_context.initial_i_available = true;
    ContinuationConstantOverride constant_override{
        seed.entry_program, &seed, &resume_candidate->generated};
    std::unique_ptr<GpuVuDraw> draw;
    {
      ScopedContinuationConstantOverride override_scope(&constant_override);
      draw =
          BuildDirectGpuVuDraw(seed.entry_program, entry_context, spans);
    }
    if (!draw || draw->program != entry_candidate->key)
      return {};

    // Constants which only belong to the skipped MSCAL prologue are immutable
    // chain inputs. A constant also demanded by the resume entry remains a
    // current per-dispatch value and is deliberately left as built above.
    for (const CgConstantInput &input :
         entry_candidate->generated.constant_inputs) {
      if (HasConstantAddress(resume_candidate->generated, input.address))
        continue;
      if (input.uniform_index >= seed.constant_values.size() ||
          (seed.constant_mask & (1u << input.uniform_index)) == 0) {
        return {};
      }
      ConstantUniform *const uniform =
          FindConstantUniform(draw.get(), input.uniform_index);
      if (!uniform)
        return {};
      uniform->bits = seed.constant_values[input.uniform_index];
    }

    thread_local InvocationEvaluationWorkspace resume_workspace;
    resume_workspace.Begin(resume_candidate->invocation.values.size());
    std::array<u32, 4> resume_tag{};
    if (!ReadGifTag(resume_candidate->invocation, resume_context,
                    &resume_workspace, &resume_tag) ||
        resume_tag != resume_candidate->gif_tag ||
        resume_tag != draw->gif_tag ||
        !EvaluateFinalViState(resume_candidate->invocation, resume_context,
                              &resume_workspace, &draw->final_vi_values,
                              &draw->final_vi_write_mask)) {
      return {};
    }
    return draw;
  }

  if (spans.empty()) {
    return {};
  }

  thread_local InvocationEvaluationWorkspace resume_workspace;
  resume_workspace.Begin(resume_candidate->invocation.values.size());
  std::array<u32, 4> resume_tag{};
  std::array<u16, 16> final_vi_values{};
  u32 final_vi_write_mask = 0;
  if (!ReadGifTag(resume_candidate->invocation, resume_context,
                  &resume_workspace, &resume_tag) ||
      resume_tag != resume_candidate->gif_tag ||
      resume_tag != entry_candidate->gif_tag ||
      !EvaluateFinalViState(resume_candidate->invocation, resume_context,
                            &resume_workspace, &final_vi_values,
                            &final_vi_write_mask)) {
    return {};
  }

  auto draw = std::make_unique<GpuVuDraw>();
  draw->SetUniformBlock(seed.uniform_block);
  draw->program = entry_candidate->key;
  draw->direct_tfx = entry_candidate->contract;
  draw->gif_tag = entry_candidate->gif_tag;
  draw->lowering = OutputLowering::DirectTfx;
  draw->execution = ExecutionKind::GeneratedParallel;
  if (!ConfigureGeometry(entry_candidate->contract,
                         entry_candidate->generated, draw.get()))
    return {};

  constexpr size_t MaximumContinuationMemoryInputs = 16u;
  std::array<RawVifPayloadRef, MaximumContinuationMemoryInputs>
      planned_payloads{};
  std::array<StreamBinding, MaximumContinuationMemoryInputs> planned_streams{};
  size_t planned_payload_count = 0u;
  size_t planned_stream_count = 0u;
  for (u32 index = 0;
       index < entry_candidate->generated.memory_inputs.size(); index++) {
    const CgMemoryInput &input =
        entry_candidate->generated.memory_inputs[index];
    // MSCNT resumes through the entry at 0x0460, whose PairPlan-derived
    // acyclic slice executes XTOP and rebuilds the affine input pointers for
    // the current VIF double-buffer. The generated explicit-entry root has
    // the same memory-input ABI, but its first dispatch's qword addresses are
    // not stable chain state. Evaluate the resume plan against this command's
    // VIF/VI snapshot so alternating TOP buffers bind their current payload
    // instead of stale geometry retained from the initial MSCAL.
    u32 base_qword = 0;
    if (input.address.base_vi >=
            resume_candidate->invocation.loop_entry_vi.size() ||
        !EvaluateInvocationValue(
            resume_candidate->invocation,
            resume_candidate->invocation.loop_entry_vi[
                input.address.base_vi],
            resume_context, &resume_workspace, &base_qword)) {
      return {};
    }
    const u16 first_qword = static_cast<u16>(
        (base_qword + input.address.qword_offset) & 0x3ffu);
    RawQwordBinding raw{};
    const VifUnpackSpan *const span = FindInputSpan(
        spans, first_qword,
        input.address.invocation_coefficient, draw->invocation_count, &raw);
    if (!span) {
      RecordDirectAdmissionFailure(AdmissionFailure::InputResolveFailed);
      return {};
    }

    u16 span_index = 0;
    bool retained = false;
    for (u32 retained_index = 0;
         retained_index < planned_payload_count; retained_index++) {
      if (SamePayload(planned_payloads[retained_index], span->payload)) {
        span_index = static_cast<u16>(retained_index);
        retained = true;
        break;
      }
    }
    if (!retained) {
      if (planned_payload_count >= planned_payloads.size()) {
        return {};
      }
      span_index = static_cast<u16>(planned_payload_count);
      planned_payloads[planned_payload_count++] = span->payload;
    }
    if (planned_stream_count >= planned_streams.size())
      return {};
    StreamBinding stream;
    stream.payload_byte_offset = raw.payload_byte_offset;
    stream.byte_stride = raw.byte_stride;
    stream.input_span = span_index;
    stream.attribute_index = static_cast<u8>(input.attribute_index);
    stream.outer_byte_stride = raw.outer_byte_stride;
    stream.payload_byte_extent = raw.payload_byte_extent;
    planned_streams[planned_stream_count++] = stream;
  }
  if (entry_candidate->generated.uses_buffered_batch_inputs &&
      !HasSingleAddressableRawInputWindow(
          planned_payloads.data(), planned_payload_count,
          planned_streams.data(), planned_stream_count)) {
    RecordDirectAdmissionFailure(AdmissionFailure::InputResolveFailed);
    return {};
  }
  if (!draw->SetGeneratedInputPlan(
          planned_payloads.data(), planned_payload_count,
          planned_streams.data(), planned_stream_count,
          entry_candidate->generated.uses_buffered_batch_inputs)) {
    RecordDirectAdmissionFailure(AdmissionFailure::InputResolveFailed);
    return {};
  }

  if (entry_candidate->generated.uses_acc_uniform)
    draw->acc_uniform = seed.initial_acc;
  if (entry_candidate->generated.uses_q_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformQ;
    draw->scalar_uniforms.q = seed.initial_q;
  }
  if (entry_candidate->generated.uses_p_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformP;
    draw->scalar_uniforms.p = seed.initial_p;
  }
  if (entry_candidate->generated.uses_i_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformI;
    draw->scalar_uniforms.i = seed.initial_i;
  }
  if (entry_candidate->generated.uses_gif_q_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformGifQ;
    draw->scalar_uniforms.gif_q = 0x3f800000u;
  }
  draw->final_vi_values = final_vi_values;
  draw->final_vi_write_mask = final_vi_write_mask;
  s_shared_continuation_builds.fetch_add(1,
                                         std::memory_order_relaxed);
  return draw;
}

bool GetDirectProgramInfo(DirectProgramToken token, DirectProgramInfo *info) {
  if (!info)
    return false;
  PreparedProgram *const prepared = LookupPinnedProgram(token);
  if (!prepared)
    return false;
  info->start_pc = prepared->start_pc;
  info->configuration_bits = prepared->configuration_bits;
  info->semantic_profile_key = prepared->semantic_profile_key;
  info->analysis_abi_version = prepared->analysis_abi_version;
  info->basic_blocks = static_cast<u32>(prepared->analysis.blocks.size());
  info->natural_loops =
      static_cast<u32>(prepared->analysis.natural_loops.size());
  info->parallel_candidates =
      static_cast<u32>(prepared->candidates.size());
  info->resume_pc_count =
      static_cast<u32>(prepared->analysis.resume_pcs.size());
  info->unique_resume_pc = info->resume_pc_count == 1 ?
      prepared->analysis.resume_pcs.front() : 0;
  info->complete_cfg = prepared->analysis.complete_cfg;
  return true;
}

DirectProgramStatistics GetDirectProgramStatistics() {
  DirectProgramStatistics stats;
  stats.preparation_requests =
      s_preparation_requests.load(std::memory_order_relaxed);
  stats.prepared_programs =
      s_prepared_programs.load(std::memory_order_relaxed);
  stats.preparation_cache_hits =
      s_preparation_cache_hits.load(std::memory_order_relaxed);
  stats.preparation_evictions =
      s_preparation_evictions.load(std::memory_order_relaxed);
  stats.preparation_wall_us =
      s_preparation_wall_us.load(std::memory_order_relaxed);
  stats.preparation_wall_us_max =
      s_preparation_wall_us_max.load(std::memory_order_relaxed);
  stats.source_hash_bytes =
      s_source_hash_bytes.load(std::memory_order_relaxed);
  stats.source_compare_bytes =
      s_source_compare_bytes.load(std::memory_order_relaxed);
  stats.source_copy_bytes =
      s_source_copy_bytes.load(std::memory_order_relaxed);
  stats.analysis_builds = s_analysis_builds.load(std::memory_order_relaxed);
  stats.analysis_wall_us =
      s_analysis_wall_us.load(std::memory_order_relaxed);
  stats.analysis_wall_us_max =
      s_analysis_wall_us_max.load(std::memory_order_relaxed);
  stats.candidate_proof_wall_us =
      s_candidate_proof_wall_us.load(std::memory_order_relaxed);
  stats.candidate_proof_wall_us_max =
      s_candidate_proof_wall_us_max.load(std::memory_order_relaxed);
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
  stats.shared_continuation_builds =
      s_shared_continuation_builds.load(std::memory_order_relaxed);
  stats.general_continuation_builds =
      s_general_continuation_builds.load(std::memory_order_relaxed);
  stats.hybrid_input_attempts =
      s_hybrid_input_attempts.load(std::memory_order_relaxed);
  stats.hybrid_input_hits =
      s_hybrid_input_hits.load(std::memory_order_relaxed);
  stats.hybrid_input_fallbacks =
      s_hybrid_input_fallbacks.load(std::memory_order_relaxed);
  stats.hybrid_raw_bytes_retained =
      s_hybrid_raw_bytes_retained.load(std::memory_order_relaxed);
  stats.hybrid_derived_bytes =
      s_hybrid_derived_bytes.load(std::memory_order_relaxed);
  stats.hybrid_copy_bytes_avoided =
      s_hybrid_copy_bytes_avoided.load(std::memory_order_relaxed);
  stats.canonical_input_bindings =
      s_canonical_input_bindings.load(std::memory_order_relaxed);
  stats.canonical_input_bytes =
      s_canonical_input_bytes.load(std::memory_order_relaxed);
  stats.persistent_raw_input_bindings =
      s_persistent_raw_input_bindings.load(std::memory_order_relaxed);
  stats.persistent_raw_input_bytes =
      s_persistent_raw_input_bytes.load(std::memory_order_relaxed);
  return stats;
}

void ClearDirectPrograms() {
  std::array<PreparedProgramReference, MaximumPreparedPrograms> retired;
  {
    std::lock_guard lock(s_cache_mutex);
    for (u32 index = 0; index < s_cache.size(); index++) {
      CacheSlot &entry = s_cache[index];
      retired[index] = std::move(entry.program);
      entry.last_use = 0;
      entry.generation = (entry.generation + 1u) & TokenGenerationMask;
      if (entry.generation == 0)
        entry.generation = 1;
    }
    s_cache_epoch.fetch_add(1, std::memory_order_release);
  }
}

} // namespace VitaGpuVu
