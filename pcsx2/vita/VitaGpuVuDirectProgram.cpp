// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuDirectProgram.h"

#include "GS/GSRegs.h"
#include "VUmicro.h"
#include "common/Console.h"
#include "vita/VitaGpuVuCgGenerator.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuProgramRegistry.h"

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
  std::mutex mutex;
  // Zero until the worker observes one registered root. Afterwards all
  // candidate metadata is immutable and the 496 MHz hot descriptor path
  // avoids both the compiler-registry mutex and this preparation mutex.
  std::atomic<u32> ready_candidate_plus_one{0};
  // Set only after descriptor construction proves that the generated
  // invocation schedule cannot be covered by this program's raw VIF layout.
  // CPU0 observes it before deciding whether another immutable capture can
  // replace any ARM work.
  std::atomic<bool> raw_input_layout_rejected{false};
  u32 start_pc = 0;
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

std::mutex s_cache_mutex;
std::array<CacheSlot, MaximumPreparedPrograms> s_cache;
u64 s_cache_clock = 0;
std::atomic<u64> s_cache_epoch{1};

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

// The returned pointer remains valid until this thread performs another
// successful lookup or exits. This gives the hot descriptor path a lock-free,
// reference-count-free cache hit while an eviction on another thread can only
// retire the cache's ownership, never the caller's thread-local pin.
PreparedProgram *LookupPinnedProgram(DirectProgramToken token) {
  struct LocalLookup {
    DirectProgramToken token{};
    PreparedProgramReference program;
    u64 epoch = 0;
  };
  thread_local LocalLookup local;
  const u64 epoch = s_cache_epoch.load(std::memory_order_acquire);
  if (local.epoch == epoch && local.token == token && local.program)
    return local.program.Get();

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
  local.token = token;
  local.program = std::move(replacement);
  local.epoch = epoch;
  return local.program.Get();
}

std::unique_ptr<PreparedProgram> BuildPreparedProgram(const u8 *micro,
                                                      u32 start_pc) {
  auto prepared = std::make_unique<PreparedProgram>();
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
                InvocationEvaluationWorkspace *workspace,
                std::array<u32, 4> *tag) {
  if (!workspace || !tag || !context.read_memory_u32)
    return false;

  u32 address = 0;
  if (!EvaluateInvocationValue(invocation,
                               invocation.gif_tag_qword_address, context,
                               workspace, &address)) {
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
  if (!draw)
    return nullptr;
  for (ConstantUniform &uniform : draw->constant_uniforms) {
    if (uniform.input_index == input_index)
      return &uniform;
  }
  return nullptr;
}

const ConstantUniform *FindConstantUniform(const GpuVuDraw &draw,
                                           u32 input_index) {
  for (const ConstantUniform &uniform : draw.constant_uniforms) {
    if (uniform.input_index == input_index)
      return &uniform;
  }
  return nullptr;
}

bool ConfigureGeometry(const DirectCandidate &candidate, GpuVuDraw *draw) {
  if (!draw)
    return false;
  const u32 vertices = candidate.contract.vertex_count;
  if (vertices == 0)
    return false;

  u32 primitive_count = 0;
  u32 native_indices = vertices;
  switch (candidate.contract.primitive) {
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

  if (candidate.generated.uses_flat_instance_inputs) {
    if (candidate.generated.flat_vertices_per_primitive == 0)
      return false;
    draw->primitive_boundary = PrimitiveBoundary::InstanceIndexed;
    native_indices =
        primitive_count * candidate.generated.flat_vertices_per_primitive;
  } else {
    draw->primitive_boundary = PrimitiveBoundary::Native;
  }

  draw->invocation_count = vertices;
  draw->vertex_count = vertices;
  draw->primitive_count = primitive_count;
  draw->index_count = native_indices;
  return primitive_count != 0 && native_indices != 0;
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

  std::unique_ptr<PreparedProgram> prepared =
      BuildPreparedProgram(micro, start_pc);
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
      "direct candidates).",
      published_start_pc, published_blocks, published_loops,
      published_candidates);
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
        "GPU-VU: prime entry %04x candidate %u tag @%04x = %08x %08x %08x "
        "%08x -> requested %u, accepted %u%s%s.",
        prepared->start_pc, index, address, tag[0], tag[1], tag[2], tag[3],
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
  if (prepared->raw_input_layout_rejected.load(std::memory_order_acquire))
    return DirectInputState::LayoutRejected;
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
  // One MTVU thread owns direct descriptor construction. Preserve the dense
  // evaluator storage between invocations and share its cache across every
  // descriptor-scale formula for this immutable context.
  thread_local InvocationEvaluationWorkspace evaluation_workspace;
  evaluation_workspace.Begin(candidate.invocation.values.size());

  std::array<u32, 4> tag{};
  if (!ReadGifTag(candidate.invocation, context, &evaluation_workspace, &tag) ||
      tag != candidate.gif_tag) {
    RecordDirectAdmissionFailure(AdmissionFailure::TagMismatch);
    return {};
  }
  if (!ClampStableSeedsHold(candidate.generated, context)) {
    RecordDirectAdmissionFailure(AdmissionFailure::SeedUnstable);
    return {};
  }

  auto draw = std::make_unique<GpuVuDraw>();
  draw->program = candidate.key;
  draw->direct_tfx = candidate.contract;
  draw->gif_tag = tag;
  draw->lowering = OutputLowering::DirectTfx;
  draw->execution = ExecutionKind::GeneratedParallel;
  if (!ConfigureGeometry(candidate, draw.get())) {
    RecordDirectAdmissionFailure(AdmissionFailure::GeometryFailed);
    return {};
  }

  bool failed = false;
  for (const CgMemoryInput &input : candidate.generated.memory_inputs) {
    u32 base_qword = 0;
    if (input.address.base_vi >= candidate.invocation.loop_entry_vi.size() ||
        !EvaluateInvocationValue(
            candidate.invocation,
            candidate.invocation.loop_entry_vi[input.address.base_vi], context,
            &evaluation_workspace, &base_qword)) {
      failed = true;
      break;
    }
    const u16 first_qword =
        static_cast<u16>((base_qword + input.address.qword_offset) & 0x3ffu);
    RawQwordBinding raw{};
    const VifUnpackSpan *span =
        FindInputSpan(spans, first_qword, input.address.invocation_coefficient,
                      draw->invocation_count, &raw);
    if (!span) {
      bool expected = false;
      if (prepared->raw_input_layout_rejected.compare_exchange_strong(
              expected, true, std::memory_order_release,
              std::memory_order_relaxed)) {
        Console.WriteLn(
            "GPU-VU: entry %04x raw input layout cannot cover the generated "
            "invocation schedule; immutable capture is suspended until "
            "program invalidation.",
            prepared->start_pc);
      }
      failed = true;
      break;
    }

    u16 span_index = 0;
    bool retained = false;
    const auto& retained_spans = draw->InputSpans();
    for (u32 index = 0; index < retained_spans.size(); index++) {
      if (SamePayload(retained_spans[index].payload, span->payload)) {
        span_index = static_cast<u16>(index);
        retained = true;
        break;
      }
    }
    if (!retained) {
      if (retained_spans.size() >= std::numeric_limits<u16>::max() ||
          !draw->AddInputSpan(*span)) {
        failed = true;
        break;
      }
      span_index = static_cast<u16>(draw->InputSpans().size() - 1);
    }
    draw->streams.push_back({raw.payload_byte_offset, raw.byte_stride,
                             span_index, static_cast<u8>(input.attribute_index),
                             0});
  }
  if (failed) {
    RecordDirectAdmissionFailure(AdmissionFailure::InputResolveFailed);
    return {};
  }

  for (const CgConstantInput &input : candidate.generated.constant_inputs) {
    const ContinuationConstantOverride *const override =
        s_continuation_constant_override;
    if (override && override->entry_program == token && override->seed &&
        override->resume_generated &&
        !HasConstantAddress(*override->resume_generated, input.address)) {
      if (input.uniform_index >= override->seed->constant_values.size() ||
          (override->seed->constant_mask & (1u << input.uniform_index)) == 0) {
        failed = true;
        break;
      }
      ConstantUniform uniform{};
      uniform.input_index = static_cast<u8>(input.uniform_index);
      uniform.bits = override->seed->constant_values[input.uniform_index];
      draw->constant_uniforms.push_back(std::move(uniform));
      continue;
    }

    u32 base_qword = 0;
    if (input.address.base_vi >= candidate.invocation.loop_entry_vi.size() ||
        !EvaluateInvocationValue(
            candidate.invocation,
            candidate.invocation.loop_entry_vi[input.address.base_vi], context,
            &evaluation_workspace, &base_qword)) {
      failed = true;
      break;
    }
    ConstantUniform uniform{};
    uniform.input_index = static_cast<u8>(input.uniform_index);
    const u16 address =
        static_cast<u16>((base_qword + input.address.qword_offset) & 0x3ffu);
    for (u32 lane = 0; lane < uniform.bits.size(); lane++) {
      if (!context.read_memory_u32(context.memory_user, address,
                                   static_cast<u8>(lane),
                                   &uniform.bits[lane])) {
        failed = true;
        break;
      }
    }
    if (failed)
      break;
    draw->constant_uniforms.push_back(uniform);
  }
  if (failed) {
    RecordDirectAdmissionFailure(AdmissionFailure::InputResolveFailed);
    return {};
  }

  for (u32 reg = 1; reg < 32; reg++) {
    if ((candidate.generated.vf_uniform_mask & (1u << reg)) == 0)
      continue;
    VectorUniform uniform{};
    uniform.register_index = static_cast<u8>(reg);
    std::memcpy(uniform.bits.data(), context.initial_vf_words + reg * 4,
                sizeof(uniform.bits));
    draw->vf_uniforms.push_back(uniform);
  }
  if (candidate.generated.uses_acc_uniform) {
    if (!context.initial_acc_words)
      return {};
    std::memcpy(draw->acc_uniform.data(), context.initial_acc_words,
                sizeof(draw->acc_uniform));
  }
  if (candidate.generated.uses_q_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformQ;
    draw->scalar_uniforms.q = context.initial_q;
  }
  if (candidate.generated.uses_p_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformP;
    draw->scalar_uniforms.p = context.initial_p;
  }
  if (candidate.generated.uses_i_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformI;
    draw->scalar_uniforms.i = context.initial_i;
  }
  if (candidate.generated.uses_gif_q_uniform) {
    draw->scalar_uniforms.present |= ScalarUniformGifQ;
    // GSState::Transfer resets packed-tag Q to 1.0 before the first register.
    draw->scalar_uniforms.gif_q = 0x3f800000u;
  }
  if (!EvaluateFinalViState(candidate.invocation, context,
                            &evaluation_workspace,
                            &draw->final_vi_values,
                            &draw->final_vi_write_mask)) {
    RecordDirectAdmissionFailure(AdmissionFailure::BuildFailed);
    return {};
  }
  return draw;
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
      entry_draw.execution != ExecutionKind::GeneratedParallel) {
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
  std::memcpy(captured.initial_vf.data(), entry_context.initial_vf_words,
              sizeof(captured.initial_vf));
  if (entry_candidate->generated.uses_acc_uniform) {
    if (!entry_context.initial_acc_words)
      return false;
    std::memcpy(captured.initial_acc.data(),
                entry_context.initial_acc_words,
                sizeof(captured.initial_acc));
  }
  captured.initial_q = entry_context.initial_q;
  captured.initial_p = entry_context.initial_p;
  captured.initial_i = entry_context.initial_i;

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

  InvocationEvaluationContext entry_context = resume_context;
  entry_context.initial_vi = seed.initial_vi.data();
  entry_context.initial_vf_words = seed.initial_vf.data();
  entry_context.initial_acc_words = seed.initial_acc.data();
  entry_context.initial_q = seed.initial_q;
  entry_context.initial_p = seed.initial_p;
  entry_context.initial_i = seed.initial_i;
  ContinuationConstantOverride constant_override{
      seed.entry_program, &seed, &resume_candidate->generated};
  std::unique_ptr<GpuVuDraw> draw;
  {
    ScopedContinuationConstantOverride override_scope(&constant_override);
    draw = BuildDirectGpuVuDraw(seed.entry_program, entry_context, spans);
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

bool GetDirectProgramInfo(DirectProgramToken token, DirectProgramInfo *info) {
  if (!info)
    return false;
  PreparedProgram *const prepared = LookupPinnedProgram(token);
  if (!prepared)
    return false;
  info->start_pc = prepared->start_pc;
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
