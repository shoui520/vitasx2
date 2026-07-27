// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuLoopKernel.h"

#include <string>
#include <vector>

namespace VitaGpuVu {

struct CgMemoryInput {
  AffineQwordAddress address;
  u32 attribute_index = 0;
  // Bit N means the flat instance root declares VertexN for this logical
  // stream. Geometry dependencies require every primitive lane; a
  // color-only dependency requires only the final/provoking lane.
  u8 flat_attribute_vertex_mask = 0;
};

// One descriptor-scale VU-memory qword which is invariant across every
// invocation in the draw. It is uploaded once through the vertex default
// uniform buffer, never repeated as a per-vertex stream.
struct CgConstantInput {
  AffineQwordAddress address;
  u32 uniform_index = 0;
};

struct GeneratedCgProgram {
  static constexpr u32 RawInputBufferQwords = (2 * 1024 * 1024) / 16;
  static constexpr u32 MaximumBatchDraws = 4096;
  // Sony's gxm/memory.h explicitly permits a dynamically indexed user
  // uniform buffer to access beyond its shader-declared size at runtime. Keep
  // the declaration at the skinning sample's proven 64-vector scale: declaring
  // the complete 2 MiB ring caused runtime ShaccCg to exhaust its work heap and
  // return a fatal internal error even though offline psp2cgc accepted it.
  static constexpr u32 DeclaredBufferVectors = 64;

  std::string source;
  std::vector<CgMemoryInput> memory_inputs;
  std::vector<CgConstantInput> constant_inputs;
  u32 vf_uniform_mask = 0;
  std::array<u8, 32> stable_initial_vf_lanes{};
  // Demanded entry lanes which are stable only because the region's writes to
  // them are idempotent self-clamps. The descriptor path must verify each
  // runtime seed against its bound before accepting a draw.
  std::vector<ClampStableLane> clamp_stable_lanes;
  u8 stable_initial_acc_lanes = 0;
  u32 emitted_expression_count = 0;
  u8 flat_vertices_per_primitive = 0;
  u8 flat_instance_vertex_step = 0;
  u16 batch_primitives_per_draw = 0;
  bool stable_initial_q = false;
  bool stable_initial_p = false;
  bool stable_initial_i = false;
  bool requires_dynamic_entry_state = false;
  bool uses_acc_uniform = false;
  bool uses_q_uniform = false;
  bool uses_p_uniform = false;
  bool uses_i_uniform = false;
  bool uses_gif_q_uniform = false;
  bool uses_tfx_uniforms = false;
  bool uses_tfx_point_size = false;
  // The direct GIF contract proves TME=1/FST=1/FGE=0, so the generated root
  // and its linked fragment variants exchange packed UV only. This omits the
  // dead STQ/fog TEXCOORD which the generic runtime-selector ABI must retain.
  bool uses_tfx_uv_no_fog_interface = false;
  bool uses_flat_instance_inputs = false;
  bool uses_buffered_batch_inputs = false;
  bool flat_strip_winding = false;

  u32 BatchBindingVectorCount() const {
    return (static_cast<u32>(memory_inputs.size()) + 3u) / 4u;
  }
  u32 BatchUniformVectorCount() const {
    u32 vectors = static_cast<u32>(constant_inputs.size());
    for (u32 reg = 1; reg < 32; reg++)
      vectors += (vf_uniform_mask & (1u << reg)) != 0;
    vectors += uses_acc_uniform ? 1u : 0u;
    vectors += (uses_q_uniform || uses_p_uniform || uses_i_uniform ||
                uses_gif_q_uniform)
                   ? 1u
                   : 0u;
    return vectors;
  }
  u32 BatchRecordVectorCount() const {
    return BatchBindingVectorCount() + BatchUniformVectorCount();
  }
};

// Emits a vertex program which evaluates the proven parallel semantic slice
// and exposes each VU qword store as a varying. This is the debug/oracle root
// used to inspect ShaccCg allocation and compare generated values. The final
// fused root reuses the same expression emitter and replaces these store
// varyings with the existing TFX tail.
bool GenerateParallelStoreValidationCg(const ParallelLoopKernel &kernel,
                                       GeneratedCgProgram *program,
                                       std::string *error);

// Emits the primary playable root: the VU semantic slice feeds the existing
// TFX vertex tail directly, with no VU output buffer or CPU vertex expansion.
bool GenerateParallelTfxCg(const ParallelLoopKernel &kernel,
                           const DirectTfxContract &contract,
                           GeneratedCgProgram *program,
                           std::string *error);

} // namespace VitaGpuVu
