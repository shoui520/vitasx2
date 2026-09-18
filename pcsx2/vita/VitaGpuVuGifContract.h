// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuLoopKernel.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace VitaGpuVu {

struct PackedIntegerExpression {
  u32 expression = 0;
  u32 mask = 0;
  u8 right_shift = 0;
};

// One O(1) runtime attestation retained by a source/configuration cache entry.
// Both sides are evaluated from the exact VI snapshot which already belongs
// to the immutable epoch input. Qword equalities compare modulo the 1,024-
// qword VU1 memory window.
struct DirectTfxViEquality {
  AffineViValue left;
  AffineViValue right;
  bool qword_wrapped = true;
};

// One expanded vertex evaluates geometry at source_vertex and flat RGBA at
// provoking_vertex. The latter is the final vertex of the emitted primitive
// (GS User's Manual 3.2.8), including after ADC skips.
struct DirectTfxFlatVertex {
  u16 source_vertex = 0;
  u16 provoking_vertex = 0;

  bool operator==(const DirectTfxFlatVertex&) const = default;
};

// Cold, immutable topology construction. Each ADC byte is exactly zero or one;
// the input domain is the original GIF vertex order, before GSState's optional
// buffer compaction. Only line/triangle strips are accepted. Empty output is
// valid here (e.g. every drawing kick skipped), not a drawable transaction.
// Failure leaves the destination unchanged.
bool BuildDirectTfxStripIndices(u32 primitive, std::span<const u8> adc,
                               std::vector<u16>* indices,
                               std::string* error = nullptr);
bool BuildDirectTfxFlatVertexMap(u32 primitive, u32 vertex_count,
                                std::span<const u16> exact_indices,
                                std::vector<DirectTfxFlatVertex>* vertices,
                                std::string* error = nullptr);
// The nested flat-line GXP has two shader endpoints for every possible line,
// before ADC filtering. Zero means that this domain cannot fit U16 INDEX.
inline u32 DirectTfxFlatLineIndexDomain(u32 vertex_count) {
  return vertex_count >= 2u && vertex_count <= 32769u
      ? 2u * (vertex_count - 1u) : 0u;
}
// Convert GSState::VertexKick's ordered source pairs to the generated flat
// line shader's endpoint domain. This is a cold index transform, never CPU
// vertex reconstruction. Failure leaves the destination unchanged.
bool BuildDirectTfxFlatLineIndices(u32 vertex_count,
                                 std::span<const u16> exact_indices,
                                 std::vector<u16>* expanded_indices,
                                 std::string* error = nullptr);

// PairPlan proof for replacing a terminal ADC-patched PATH1 packet with one
// indexed direct-TFX draw. `exact_indices` are PCSX2 GSState::VertexKick's
// emitted indices, not a guessed primitive restart convention. Final VI state
// is still private until the complete generated transaction commits.
struct DirectTfxPostLoopProof {
  std::vector<u16> exact_indices;
  std::vector<u16> expanded_line_indices;
  std::vector<DirectTfxFlatVertex> flat_vertices;
  // Exact qword destinations of the terminal ISW.W ADC patches. They remain
  // affine until invocation construction resolves the immutable VI/VIF entry
  // state; canonical commit writes 0x00008000 after the GPU loop journal.
  std::vector<AffineViValue> adc_patch_addresses;
  std::vector<DirectTfxViEquality> runtime_vi_equalities;
  std::array<AffineViValue, 16> final_vi_values{};
  u32 primitive_count = 0;
  u32 exact_pair_count = 0;
  u32 post_loop_pair_count = 0;
  u32 unique_resume_pc = 0;
  u16 final_vi_write_mask = 0;
  u16 adc_vertex_count = 0;
  u8 indices_per_primitive = 0;
  bool terminal_xgkick_proven = false;
  bool exact_adc_topology_proven = false;
};

struct DirectTfxContract {
  u32 vertex_count = 0;
  u32 primitive = 0;
  u8 output_base_vi = 0;
  s32 output_base_qword = 0;
  std::array<u32, 2> st{};
  std::array<PackedIntegerExpression, 4> color{};
  u32 q = 0;
  std::array<PackedIntegerExpression, 2> position{};
  PackedIntegerExpression depth;
  std::array<PackedIntegerExpression, 2> uv{};
  PackedIntegerExpression fog;
  u32 adc_expression = 0;
  // Original child-entry address identities survive resolved contract
  // construction. They let the post-loop proof compare a later symbolic ISW
  // or XGKICK address without putting runtime VI values in the shader key.
  AffineQwordAddress output_source_address;
  AffineQwordAddress position_source_address;
  u8 position_record_qword = 0;
  bool gouraud = false;
  bool textured = false;
  bool fog_enabled = false;
  bool fixed_texture_coordinates = false;
  bool adc_always_clear = false;
  bool has_color = false;
  bool has_stq = false;
  bool has_uv = false;
  bool has_position = false;
};

// Runtime-resolved ownership proof for a generated direct-TFX root. Every
// PairPlan store lane is unique across the complete invocation grid, and no
// reachable immutable VU-memory input aliases any of those lanes. This is a
// prerequisite for treating the packet store range as a distinct output
// lifetime; it does not by itself permit approximate words to become canonical
// VU memory or prove anything about a future epoch.
struct DirectTfxStoreReadDisjointProof {
  u32 invocation_count = 0;
  u32 reachable_expression_count = 0;
  u32 store_qword_count = 0;
  u32 store_lane_count = 0;
  u32 expression_memory_read_lane_count = 0;
  u32 compact_memory_read_lane_count = 0;
  bool complete = false;
};

// Matches one static packed GIF tag to the loop's affine qword stores. The
// proof uses only decoded tag semantics and expression/address relationships;
// it has no program, title, address, or instruction-sequence recognition.
bool BuildDirectTfxContract(const ParallelLoopKernel &kernel,
                            const void *gif_tag_qword,
                            DirectTfxContract *contract,
                            std::string *error);

// Resolves distinct affine store-base registers through one attested child
// entry VI snapshot before applying the same GIF contract. This covers
// software-pipelined VU programs which keep RGBAQ/ST/XYZ in separate VI
// pointers while incrementing every pointer by the same record stride. The
// caller owns the pre-effect snapshot proof; source identity is irrelevant.
bool BuildResolvedDirectTfxContract(
    const ParallelLoopKernel& kernel, const void* gif_tag_qword,
    const std::array<u16, 16>& child_entry_vi,
    DirectTfxContract* contract, std::string* error);
bool BuildResolvedDirectTfxContract(
    const ParallelLoopKernel& kernel, const void* gif_tag_qword,
    const std::array<u16, 16>& child_entry_vi, u16 vif_top, u16 vif_itop,
    DirectTfxContract* contract, std::string* error);

// Resolves every store and reachable memory leaf over the exact bounded
// outer/child domain. Same-invocation aliases, cross-invocation aliases,
// wrapped aliases, duplicate output destinations, malformed compact tables,
// and invalid affine bases all reject before shader generation. Failure is
// non-mutating so this result can safely participate in pre-effect admission.
bool ProveDirectTfxStoreReadDisjoint(
    const ParallelLoopKernel& kernel, const DirectTfxContract& contract,
    const std::array<u16, 16>& child_entry_vi, u16 vif_top, u16 vif_itop,
    u32 outer_iteration_count, u32 child_iteration_count,
    DirectTfxStoreReadDisjointProof* proof, std::string* error = nullptr);

// Extends a closed-form nested value kernel through a canonical post-loop
// ADC patch, terminal XGKICK, and E-bit. Supports a single-block affine
// post-loop and line/triangle strips, retaining a separate provoking-vertex
// map for flat shading. This proves topology, not a supported shader mapping
// or rasterization profile. Every decision is derived from PairPlans/CFG/GIF
// semantics. Failure is non-mutating; CPU MTVU remains the pre-effect owner.
bool BuildClosedFormDirectTfxPostLoopProof(
    const ProgramAnalysis& program, const ParallelLoopKernel& kernel,
    const ClosedFormNestedLoopProof& closed_form,
    const DirectTfxContract& contract, DirectTfxPostLoopProof* proof,
    std::string* error = nullptr);

// Evaluates only the proof's bounded affine equalities and final VI formulas.
// It performs no VU instruction body, memory access, or output publication.
bool EvaluateDirectTfxPostLoopProof(
    const DirectTfxPostLoopProof& proof,
    const std::array<u16, 16>& initial_vi,
    std::array<u16, 16>* final_vi, std::string* error = nullptr);
bool EvaluateDirectTfxPostLoopProof(
    const DirectTfxPostLoopProof& proof,
    const std::array<u16, 16>& initial_vi, u16 vif_top, u16 vif_itop,
    std::array<u16, 16>* final_vi, std::string* error = nullptr);

} // namespace VitaGpuVu
