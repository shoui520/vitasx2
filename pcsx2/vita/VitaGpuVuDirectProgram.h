// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuInvocationPlan.h"

#include <memory>
#include <vector>

namespace VitaGpuVu {
struct ShaderKey;
struct VifUnpackSpan;
}

namespace VitaGpuVu {

// Generation-tagged handle to one exact VU1 micro-memory image and external
// entry point. It is safe to copy through the MTVU ring: eviction makes an old
// token fail closed instead of aliasing a newer program.
struct DirectProgramToken {
  u32 value = 0;

  bool IsValid() const { return value != 0; }
  bool operator==(const DirectProgramToken &other) const {
    return value == other.value;
  }
};

enum class DirectInputState : u8 {
  Unavailable,
  Pending,
  Ready,
  LayoutRejected,
};

struct DirectProgramInfo {
  u32 start_pc = 0;
  u32 basic_blocks = 0;
  u32 natural_loops = 0;
  u32 parallel_candidates = 0;
  u32 unique_resume_pc = 0;
  u32 resume_pc_count = 0;
  bool complete_cfg = false;
};

struct DirectProgramStatistics {
  u64 prepared_programs = 0;
  u64 preparation_cache_hits = 0;
  u64 preparation_evictions = 0;
  u64 analysis_failures = 0;
  u64 programs_without_parallel_candidate = 0;
  u64 parallel_candidates = 0;
  u64 prime_attempts = 0;
  u64 prime_cache_hits = 0;
  u64 stale_tokens = 0;
  u64 gif_address_failures = 0;
  u64 gif_contract_rejections = 0;
  u64 generated_roots = 0;
  u64 compiler_requests = 0;
  u64 compiler_request_retries = 0;
  u64 shared_continuation_builds = 0;
  u64 general_continuation_builds = 0;
};

// Immutable entry-state snapshot for a semantically paired MSCAL/MSCNT
// command chain. The generated entry root recomputes the skipped acyclic
// prologue on SGX; only descriptor-scale initial state and one-time constant
// qwords are retained here. No VU arithmetic or per-vertex values are
// evaluated on ARM.
struct DirectContinuationSeed {
  DirectProgramToken entry_program;
  DirectProgramToken resume_program;
  std::array<u16, 16> initial_vi{};
  std::array<u32, 32 * 4> initial_vf{};
  std::array<u32, 4> initial_acc{};
  std::array<std::array<u32, 4>, 32> constant_values{};
  GpuVuUniformBlockRef uniform_block;
  u32 constant_mask = 0;
  u32 initial_q = 0;
  u32 initial_p = 0;
  u32 initial_i = 0;

  bool IsValid() const {
    return entry_program.IsValid() && resume_program.IsValid();
  }
};

// CPU0 cold seam. Decodes the exact uploaded micro-memory image once, builds
// semantic loop slices and invocation provenance, and returns an immutable
// token for subsequent MTVU jobs. A lack of a direct candidate is not an
// unsupported-program decision; lower GPU output paths own that case.
DirectProgramToken PrepareDirectProgram(const u8 *micro, u32 micro_size,
                                        u32 start_pc);

// MTVU worker cold-miss seam. Resolves only descriptor-scale provenance
// against the initial architectural state, proves the static GIF contract,
// generates Cg, and submits it to the asynchronous compiler. It never changes
// VU state and never waits for compilation.
bool PrimeDirectProgram(DirectProgramToken token,
                        const InvocationEvaluationContext &context);

// Lock-free producer predicate after the first token lookup on a thread. Raw
// VIF ownership is activated only after the GS owner has registered the exact
// generated root; cold compilation therefore retains the ordinary MTVU
// copy/unpack path instead of paying capture plus CPU replay.
bool IsDirectProgramReadyForInput(DirectProgramToken token);

// Returns the complete producer state in one lookup. LayoutRejected is sticky
// for the exact generation-tagged program: the current direct lowering has
// proven that its invocation-indexed raw inputs cannot be covered by the
// uploaded VIF spans, so subsequent epochs must retain the ordinary VIF/VU
// fallback until microcode invalidation or a broader GPU lowering replaces it.
DirectInputState GetDirectProgramInputState(DirectProgramToken token);

// GS-thread completion handoff. Registration is rare, so it may scan the
// bounded semantic cache and publish ready candidates to their producer and
// MTVU descriptor paths.
void PublishDirectProgramRegistration(const ShaderKey &key, bool succeeded);

// MTVU descriptor seam. Resolves only descriptor-scale provenance and retains
// the immutable raw VIF spans needed by the ready generated root. It performs
// no VU pair execution, per-vertex unpack, shader compilation, or GS work.
std::unique_ptr<GpuVuDraw> BuildDirectGpuVuDraw(
    DirectProgramToken token, const InvocationEvaluationContext &context,
    const std::vector<VifUnpackSpan> &spans);

// Proves that two exact-image entries reach the same parallel loop and that
// replaying the explicit entry slice can supply the resume root's live state.
// This is semantic CFG/resource matching, never program identity recognition.
bool IsDirectContinuationPair(DirectProgramToken entry,
                              DirectProgramToken resume);

// Captures descriptor-scale state from one accepted explicit-entry draw.
// Subsequent MSCNT draws use the same generated entry root, with current raw
// VIF streams and the resume entry's exact VI-exit formulas.
bool CaptureDirectContinuationSeed(
    DirectProgramToken entry, DirectProgramToken resume,
    const InvocationEvaluationContext &entry_context,
    const GpuVuDraw &entry_draw, DirectContinuationSeed *seed);
std::unique_ptr<GpuVuDraw> BuildDirectGpuVuContinuationDraw(
    const DirectContinuationSeed &seed,
    const InvocationEvaluationContext &resume_context,
    const std::vector<VifUnpackSpan> &spans);

bool GetDirectProgramInfo(DirectProgramToken token, DirectProgramInfo *info);
DirectProgramStatistics GetDirectProgramStatistics();

// Drops cold semantic preparation storage. Generated GXP registry ownership
// remains independent on the GS thread.
void ClearDirectPrograms();

} // namespace VitaGpuVu
