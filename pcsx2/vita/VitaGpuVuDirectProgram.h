// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "vita/VitaGpuVuInvocationPlan.h"

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

struct DirectProgramInfo {
  u32 start_pc = 0;
  u32 basic_blocks = 0;
  u32 natural_loops = 0;
  u32 parallel_candidates = 0;
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

bool GetDirectProgramInfo(DirectProgramToken token, DirectProgramInfo *info);
DirectProgramStatistics GetDirectProgramStatistics();

// Drops cold semantic preparation storage. Generated GXP registry ownership
// remains independent on the GS thread.
void ClearDirectPrograms();

} // namespace VitaGpuVu
