// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuGxpResource.h"

#include <limits>

namespace VitaGpuVu {
namespace {

constexpr std::size_t GxpResourcePrefixBytes = 0x5c;
constexpr u32 GxpMagic = 0x00505847u; // "GXP\0" in little-endian storage.
constexpr u8 SupportedMajorVersion = 1;
constexpr u8 SupportedMinorVersion = 5;
constexpr std::size_t UsseInstructionBytes = 8;
constexpr u32 GxpProgramFlagPerInstanceMode = 1u << 1u;

// SGX543 USSE instructions are two little-endian words.  These masks are the
// stable Series5 encoding described by the PowerVR USE instruction reference:
// OPCAT=31 selects SPECIAL, SPECIAL_OPCAT=0 plus EXTRA=0 selects flow control,
// and FLOWCTRL_OP2 0/1 are BA/BR.  Keep this tiny original decoder local: the
// runtime cannot invoke Sony's desktop-only psp2shaderperf disassembler.
constexpr u32 UsseOpcodeShift = 27u;
constexpr u32 UsseSpecialOpcode = 31u;
constexpr u32 UsseSpecialCategoryShift = 20u;
constexpr u32 UsseSpecialCategoryMask = 0x3u;
constexpr u32 UsseFlowControlCategory = 0u;
constexpr u32 UsseSpecialExtraBit = 0x00400000u;
constexpr u32 UsseFlowControlOpcodeShift = 6u;
constexpr u32 UsseFlowControlOpcodeMask = 0x7u;
constexpr u32 UsseBranchAbsoluteOpcode = 0u;
constexpr u32 UsseBranchRelativeOpcode = 1u;

u16 ReadLe16(const u8* bytes, std::size_t offset) {
  return static_cast<u16>(bytes[offset]) |
         (static_cast<u16>(bytes[offset + 1]) << 8u);
}

u32 ReadLe32(const u8* bytes, std::size_t offset) {
  return static_cast<u32>(bytes[offset]) |
         (static_cast<u32>(bytes[offset + 1]) << 8u) |
         (static_cast<u32>(bytes[offset + 2]) << 16u) |
         (static_cast<u32>(bytes[offset + 3]) << 24u);
}

bool RelativeProgramRangeIsValid(std::size_t relative_field_offset,
                                 u32 relative_offset, u32 instruction_count,
                                 std::size_t declared_size,
                                 std::size_t* start = nullptr,
                                 std::size_t* end = nullptr) {
  if (relative_offset >
      std::numeric_limits<std::size_t>::max() - relative_field_offset) {
    return false;
  }
  const std::size_t found_start =
      relative_field_offset + static_cast<std::size_t>(relative_offset);
  if (found_start > declared_size ||
      instruction_count >
          (declared_size - found_start) / UsseInstructionBytes) {
    return false;
  }
  const std::size_t found_end =
      found_start +
      static_cast<std::size_t>(instruction_count) * UsseInstructionBytes;
  if (start)
    *start = found_start;
  if (end)
    *end = found_end;
  return true;
}

u32 CountBranchInstructions(const u8* bytes, std::size_t start,
                            u32 instruction_count) {
  u32 branches = 0;
  for (u32 index = 0; index < instruction_count; index++) {
    const std::size_t word1_offset =
        start + static_cast<std::size_t>(index) * UsseInstructionBytes + 4u;
    const u32 word1 = ReadLe32(bytes, word1_offset);
    const u32 opcode = word1 >> UsseOpcodeShift;
    const u32 special_category =
        (word1 >> UsseSpecialCategoryShift) & UsseSpecialCategoryMask;
    const u32 flow_opcode =
        (word1 >> UsseFlowControlOpcodeShift) &
        UsseFlowControlOpcodeMask;
    const bool flow_control = opcode == UsseSpecialOpcode &&
        special_category == UsseFlowControlCategory &&
        (word1 & UsseSpecialExtraBit) == 0u;
    branches += flow_control &&
        (flow_opcode == UsseBranchAbsoluteOpcode ||
         flow_opcode == UsseBranchRelativeOpcode);
  }
  return branches;
}

} // namespace

const char* GeneratedGxpResourceAttestationName(
    GeneratedGxpResourceAttestation result) {
  switch (result) {
    case GeneratedGxpResourceAttestation::Accepted:
      return "accepted";
    case GeneratedGxpResourceAttestation::MissingInput:
      return "missing-input";
    case GeneratedGxpResourceAttestation::TruncatedHeader:
      return "truncated-header";
    case GeneratedGxpResourceAttestation::InvalidMagic:
      return "invalid-magic";
    case GeneratedGxpResourceAttestation::UnsupportedFormat:
      return "unsupported-format";
    case GeneratedGxpResourceAttestation::InvalidDeclaredSize:
      return "invalid-declared-size";
    case GeneratedGxpResourceAttestation::InvalidPrimaryProgramRange:
      return "invalid-primary-program-range";
    case GeneratedGxpResourceAttestation::InvalidSecondaryProgramRange:
      return "invalid-secondary-program-range";
    case GeneratedGxpResourceAttestation::ScratchSpill:
      return "scratch-spill";
    case GeneratedGxpResourceAttestation::PerThreadBacking:
      return "per-thread-backing";
    case GeneratedGxpResourceAttestation::PerInstanceExecution:
      return "per-instance-execution";
    case GeneratedGxpResourceAttestation::DynamicFlowControl:
      return "dynamic-flow-control";
  }
  return "unknown";
}

GeneratedGxpResourceAttestation AttestRuntimeGeneratedGxpResources(
    const void* data, std::size_t size, GeneratedGxpResourceUsage* usage,
    GeneratedGxpExecutionRequirement execution_requirement) {
  if (usage)
    *usage = {};
  if (!data)
    return GeneratedGxpResourceAttestation::MissingInput;
  if (size < GxpResourcePrefixBytes)
    return GeneratedGxpResourceAttestation::TruncatedHeader;

  const u8* const bytes = static_cast<const u8*>(data);
  if (ReadLe32(bytes, 0x00) != GxpMagic)
    return GeneratedGxpResourceAttestation::InvalidMagic;

  GeneratedGxpResourceUsage parsed;
  parsed.major_version = bytes[0x04];
  parsed.minor_version = bytes[0x05];
  parsed.sdk_version = ReadLe16(bytes, 0x06);
  if (parsed.major_version != SupportedMajorVersion ||
      parsed.minor_version != SupportedMinorVersion) {
    return GeneratedGxpResourceAttestation::UnsupportedFormat;
  }

  parsed.declared_size = ReadLe32(bytes, 0x08);
  if (parsed.declared_size < GxpResourcePrefixBytes ||
      parsed.declared_size > size) {
    return GeneratedGxpResourceAttestation::InvalidDeclaredSize;
  }

  parsed.program_flags = ReadLe32(bytes, 0x14);
  parsed.primary_register_count = ReadLe16(bytes, 0x30);
  parsed.secondary_register_count = ReadLe16(bytes, 0x32);
  parsed.temporary_register_count = ReadLe32(bytes, 0x34);
  parsed.programmable_blending_temporary_register_count =
      ReadLe16(bytes, 0x38);
  parsed.primary_phase_count = ReadLe16(bytes, 0x3a);
  parsed.primary_instruction_count = ReadLe32(bytes, 0x3c);
  parsed.secondary_instruction_count = ReadLe32(bytes, 0x44);
  parsed.scratch_buffer_size = ReadLe32(bytes, 0x50);
  parsed.thread_buffer_size = ReadLe32(bytes, 0x54);
  parsed.literal_buffer_size = ReadLe32(bytes, 0x58);

  std::size_t primary_start = 0;
  if (parsed.primary_phase_count == 0 ||
      parsed.primary_instruction_count == 0 ||
      !RelativeProgramRangeIsValid(
          0x40, ReadLe32(bytes, 0x40),
          parsed.primary_instruction_count, parsed.declared_size,
          &primary_start)) {
    return GeneratedGxpResourceAttestation::InvalidPrimaryProgramRange;
  }
  parsed.primary_branch_instruction_count = CountBranchInstructions(
      bytes, primary_start, parsed.primary_instruction_count);

  if (parsed.secondary_instruction_count != 0) {
    std::size_t secondary_start = 0;
    std::size_t secondary_instruction_end = 0;
    if (!RelativeProgramRangeIsValid(
            0x48, ReadLe32(bytes, 0x48),
            parsed.secondary_instruction_count, parsed.declared_size,
            &secondary_start, &secondary_instruction_end)) {
      return GeneratedGxpResourceAttestation::InvalidSecondaryProgramRange;
    }
    const u32 relative_end = ReadLe32(bytes, 0x4c);
    if (relative_end >
        std::numeric_limits<std::size_t>::max() - std::size_t{0x4c}) {
      return GeneratedGxpResourceAttestation::InvalidSecondaryProgramRange;
    }
    const std::size_t secondary_end =
        std::size_t{0x4c} + static_cast<std::size_t>(relative_end);
    if (secondary_start > secondary_end ||
        secondary_instruction_end > secondary_end ||
        secondary_end > parsed.declared_size) {
      return GeneratedGxpResourceAttestation::InvalidSecondaryProgramRange;
    }
    parsed.secondary_branch_instruction_count = CountBranchInstructions(
        bytes, secondary_start, parsed.secondary_instruction_count);
  }

  if (usage)
    *usage = parsed;
  if (parsed.scratch_buffer_size != 0)
    return GeneratedGxpResourceAttestation::ScratchSpill;
  if (parsed.thread_buffer_size != 0)
    return GeneratedGxpResourceAttestation::PerThreadBacking;
  if (execution_requirement ==
          GeneratedGxpExecutionRequirement::ParallelStaticFlow &&
      (parsed.program_flags & GxpProgramFlagPerInstanceMode) != 0u) {
    return GeneratedGxpResourceAttestation::PerInstanceExecution;
  }
  if (execution_requirement ==
          GeneratedGxpExecutionRequirement::ParallelStaticFlow &&
      (parsed.primary_branch_instruction_count != 0u ||
       parsed.secondary_branch_instruction_count != 0u)) {
    return GeneratedGxpResourceAttestation::DynamicFlowControl;
  }
  return GeneratedGxpResourceAttestation::Accepted;
}

bool IsBoundedParallelStaticValidationScratchSpill(
    const GeneratedGxpResourceUsage& usage) {
  return usage.scratch_buffer_size != 0u &&
         usage.scratch_buffer_size <=
             GeneratedGxpValidationScratchMaximumBytes &&
         usage.thread_buffer_size == 0u &&
         !usage.UsesPerInstanceExecution() &&
         usage.primary_branch_instruction_count == 0u &&
         usage.secondary_branch_instruction_count == 0u;
}

} // namespace VitaGpuVu
