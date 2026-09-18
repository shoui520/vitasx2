// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <cstddef>

namespace VitaGpuVu {

// Resource fields carried by Sony's GXP 1.5 container.  libGXM remains the
// authority for executable-program validation through sceGxmProgramCheck();
// this host-neutral view keeps unsuitable generated hot-tier programs from
// entering the shader patcher. Policy-rejected but structurally valid compiler
// artifacts may still be retained in the persistent cache to avoid repeating
// minutes of ShaccCg work; every load is re-attested before registration.
struct GeneratedGxpResourceUsage {
  u8 major_version = 0;
  u8 minor_version = 0;
  u16 sdk_version = 0;
  u32 declared_size = 0;
  u32 program_flags = 0;
  u16 primary_register_count = 0;
  u16 secondary_register_count = 0;
  u32 temporary_register_count = 0;
  u16 programmable_blending_temporary_register_count = 0;
  u16 primary_phase_count = 0;
  u32 primary_instruction_count = 0;
  u32 secondary_instruction_count = 0;
  u32 scratch_buffer_size = 0;
  u32 thread_buffer_size = 0;
  u32 literal_buffer_size = 0;
  u32 primary_branch_instruction_count = 0;
  u32 secondary_branch_instruction_count = 0;

  bool UsesPerInstanceExecution() const {
    return (program_flags & (1u << 1u)) != 0u;
  }
};

enum class GeneratedGxpExecutionRequirement : u8 {
  Any,
  // The SGX543MP4+ executes statically structured vertex work across its
  // parallel lanes.  A generated loop kernel must never silently regress to
  // the serial/per-instance execution shape or retain a compiled USE branch.
  ParallelStaticFlow,
};

enum class GeneratedGxpResourceAttestation : u8 {
  Accepted,
  MissingInput,
  TruncatedHeader,
  InvalidMagic,
  UnsupportedFormat,
  InvalidDeclaredSize,
  InvalidPrimaryProgramRange,
  InvalidSecondaryProgramRange,
  ScratchSpill,
  PerThreadBacking,
  PerInstanceExecution,
  DynamicFlowControl,
};

const char* GeneratedGxpResourceAttestationName(
    GeneratedGxpResourceAttestation result);

// Parses only the stable resource prefix of a GXP 1.5 container.  The input
// is never modified and no unaligned host loads are performed.  A successful
// structural parse can still return ScratchSpill or PerThreadBacking: those
// programs are valid GXPs, but Sony documents both storage classes as memory
// backed and unsuitable for VitaSX2's generated performance tier.
GeneratedGxpResourceAttestation AttestRuntimeGeneratedGxpResources(
    const void* data, std::size_t size,
    GeneratedGxpResourceUsage* usage = nullptr,
    GeneratedGxpExecutionRequirement execution_requirement =
        GeneratedGxpExecutionRequirement::Any);

// A deliberately narrow physical-validation escape hatch. GXM supports
// compiler scratch through its vertex ring, but a spilling root cannot meet
// the generated hot-tier performance contract. This predicate permits one
// bounded canary to distinguish semantic/output correctness from that known
// performance failure without admitting per-instance or dynamic-flow code.
constexpr u32 GeneratedGxpValidationScratchMaximumBytes = 4u * 1024u;
bool IsBoundedParallelStaticValidationScratchSpill(
    const GeneratedGxpResourceUsage& usage);

} // namespace VitaGpuVu
