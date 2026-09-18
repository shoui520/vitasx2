// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuCommandEpoch.h"

#include "Config.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "vita/VitaGpuVuMicroProgram.h"
#include "vita/VitaGpuVuVifInput.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace VitaGpuVu {
namespace {

constexpr u32 CommandMask = 0xffu;
constexpr u32 CycleClShift = 8;
constexpr u32 CycleWlShift = 16;
constexpr u32 ModeShift = 24;
constexpr u32 UnsignedBit = 1u << 26;
constexpr u32 StartAlignmentShift = 27;
constexpr u32 StartAlignmentMask = 0x7u;
constexpr u32 VifControlMask = 0x3fffffffu;

constexpr u32 ExecuteResumeBit = 1u << 0;
constexpr u32 ExecuteControlMask = ExecuteResumeBit;

constexpr u32 Vu1MemoryBytes = 16u * 1024u;
constexpr u32 Vu1MemoryQwords = Vu1MemoryBytes / 16u;

constexpr std::array<u8, 16> SourceVectorBytes = {
    4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};

bool Fail(std::string* error, const char* message) {
  if (error)
    *error = message;
  return false;
}

bool FailPath1(UniversalRawPath1Export* output,
               UniversalRawPath1ExportError code, std::string* error,
               const char* message) {
  if (output)
    output->error = code;
  return Fail(error, message);
}

u16 LowHalf(u32 value) {
  return static_cast<u16>(value);
}

u16 HighHalf(u32 value) {
  return static_cast<u16>(value >> 16);
}

u32 PackHalves(u16 low, u16 high) {
  return static_cast<u32>(low) | (static_cast<u32>(high) << 16);
}

u16 LoadU16(const u8* source) {
  u16 value = 0;
  std::memcpy(&value, source, sizeof(value));
  return value;
}

u32 LoadU32(const u8* source) {
  u32 value = 0;
  std::memcpy(&value, source, sizeof(value));
  return value;
}

u32 ExtendU8(u8 value, bool unsigned_data) {
  if (unsigned_data)
    return value;
  return static_cast<u32>(static_cast<s32>(static_cast<s8>(value)));
}

u32 ExtendU16(u16 value, bool unsigned_data) {
  if (unsigned_data)
    return value;
  return static_cast<u32>(static_cast<s32>(static_cast<s16>(value)));
}

bool IsSupportedFormat(u32 format) {
  return format < SourceVectorBytes.size() && SourceVectorBytes[format] != 0;
}

u32 VectorReadBytes(u32 format) {
  // PCSX2's generated V3 forms retain the V4 load width. The extra W element
  // is indeterminate/zeroed according to packet alignment after being loaded.
  switch (format) {
  case 0x08:
    return 16;
  case 0x09:
    return 8;
  case 0x0a:
    return 4;
  default:
    return SourceVectorBytes[format];
  }
}

struct DecodedVifCommand {
  u8 command = 0;
  u8 cycle_cl = 0;
  u8 cycle_wl = 0;
  u8 mode = 0;
  bool unsigned_data = false;
  u8 start_alignment = 0;
  u32 payload_offset = 0;
  u32 source_size = 0;
  u32 mask = 0;
  u16 destination_qword = 0;
  u16 vector_count = 0;
  u16 vif_top = 0;
  u16 vif_itop = 0;
};

bool DecodeVifCommand(const UniversalEpochMicroOp& command,
                      DecodedVifCommand* decoded) {
  if (!decoded ||
      command.kind != static_cast<u32>(UniversalEpochCommandKind::VifUnpack) ||
      (command.control & ~VifControlMask) != 0 || command.arguments[5] != 0) {
    return false;
  }

  decoded->command = static_cast<u8>(command.control & CommandMask);
  decoded->cycle_cl = static_cast<u8>(command.control >> CycleClShift);
  decoded->cycle_wl = static_cast<u8>(command.control >> CycleWlShift);
  decoded->mode = static_cast<u8>((command.control >> ModeShift) & 0x3u);
  decoded->unsigned_data = (command.control & UnsignedBit) != 0;
  decoded->start_alignment = static_cast<u8>(
      (command.control >> StartAlignmentShift) & StartAlignmentMask);
  decoded->payload_offset = command.arguments[0];
  decoded->source_size = command.arguments[1];
  decoded->mask = command.arguments[2];
  decoded->destination_qword = LowHalf(command.arguments[3]);
  decoded->vector_count = HighHalf(command.arguments[3]);
  decoded->vif_top = LowHalf(command.arguments[4]);
  decoded->vif_itop = HighHalf(command.arguments[4]);
  return true;
}

bool RequiredPayloadBytes(const DecodedVifCommand& command,
                          u32* required_bytes) {
  if (!required_bytes || command.vector_count == 0 ||
      command.destination_qword >= 1024 || command.mode > 3 ||
      (command.command & 0x80u) != 0 ||
      (command.command & 0x60u) != 0x60u) {
    return false;
  }

  const u32 format = command.command & 0x0fu;
  if (!IsSupportedFormat(format))
    return false;

  const u32 vector_bytes = SourceVectorBytes[format];
  const u32 read_bytes = VectorReadBytes(format);
  // PCSX2 Vif_Unpack.cpp::_nVifUnpack() expands WL=0 to 256 while retaining
  // CL=0. The latter is a legal fill form which repeatedly writes one source
  // vector for the complete WL cycle.
  const u32 cycle_cl = command.cycle_cl;
  const u32 cycle_wl = command.cycle_wl ? command.cycle_wl : 256u;
  const bool fill = cycle_cl < cycle_wl;
  u32 source_offset = 0;
  u32 cycle = 0;
  u32 maximum_end = 0;
  for (u32 index = 0; index < command.vector_count; index++) {
    if (source_offset > std::numeric_limits<u32>::max() - read_bytes)
      return false;
    maximum_end = std::max(maximum_end, source_offset + read_bytes);
    cycle++;
    if (fill) {
      if (cycle <= cycle_cl)
        source_offset += vector_bytes;
      else if (cycle == cycle_wl)
        cycle = 0;
    } else {
      source_offset += vector_bytes;
      if (cycle >= cycle_wl)
        cycle = 0;
    }
  }
  *required_bytes = maximum_end;
  return true;
}

void LoadVector(const DecodedVifCommand& command, const u8* source,
                u32 generated_iteration, u32 generated_alignment,
                std::array<u32, 4>* output) {
  const u32 format = command.command & 0x0fu;
  const bool usn = command.unsigned_data;
  auto& out = *output;
  switch (format) {
  case 0x00:
    out.fill(LoadU32(source));
    break;
  case 0x01:
    out.fill(ExtendU16(LoadU16(source), usn));
    break;
  case 0x02:
    out.fill(ExtendU8(*source, usn));
    break;
  case 0x04:
    out = {LoadU32(source), LoadU32(source + 4),
           LoadU32(source), LoadU32(source + 4)};
    break;
  case 0x05: {
    const u32 x = ExtendU16(LoadU16(source), usn);
    const u32 y = ExtendU16(LoadU16(source + 2), usn);
    out = {x, y, x, y};
    break;
  }
  case 0x06: {
    const u32 x = ExtendU8(source[0], usn);
    const u32 y = ExtendU8(source[1], usn);
    out = {x, y, x, y};
    break;
  }
  case 0x08:
    out = {LoadU32(source), LoadU32(source + 4), LoadU32(source + 8),
           generated_iteration == generated_alignment ?
               LoadU32(source + 12) : 0u};
    break;
  case 0x09:
    out = {ExtendU16(LoadU16(source), usn),
           ExtendU16(LoadU16(source + 2), usn),
           ExtendU16(LoadU16(source + 4), usn),
           ExtendU16(LoadU16(source + 6), usn)};
    if ((generated_iteration & 1u) == 0 &&
        (((generated_iteration / 4u) + 1u +
          (4u - generated_alignment)) & 3u) == 0) {
      out[3] = 0;
    }
    break;
  case 0x0a:
    out = {ExtendU8(source[0], usn), ExtendU8(source[1], usn),
           ExtendU8(source[2], usn),
           generated_iteration == generated_alignment ?
               ExtendU8(source[3], usn) : 0u};
    break;
  case 0x0c:
    out = {LoadU32(source), LoadU32(source + 4), LoadU32(source + 8),
           LoadU32(source + 12)};
    break;
  case 0x0d:
    out = {ExtendU16(LoadU16(source), usn),
           ExtendU16(LoadU16(source + 2), usn),
           ExtendU16(LoadU16(source + 4), usn),
           ExtendU16(LoadU16(source + 6), usn)};
    break;
  case 0x0e:
    out = {ExtendU8(source[0], usn), ExtendU8(source[1], usn),
           ExtendU8(source[2], usn), ExtendU8(source[3], usn)};
    break;
  case 0x0f: {
    const u32 packed = LoadU16(source);
    out = {(packed & 0x001fu) << 3, (packed & 0x03e0u) >> 2,
           (packed & 0x7c00u) >> 7, (packed & 0x8000u) >> 8};
    break;
  }
  default:
    out = {};
    break;
  }
}

void StoreVector(const DecodedVifCommand& command,
                 UniversalCommandEpochState* state, u32 cycle,
                 const std::array<u32, 4>& input, u8* destination) {
  u32 output[4];
  std::memcpy(output, destination, sizeof(output));
  const bool masked = (command.command & 0x10u) != 0;
  const u32 mask_cycle = std::min(cycle, 3u);
  const u32 cycle_mask =
      masked ? (command.mask >> (mask_cycle * 8u)) & 0xffu : 0u;
  const u32 mode = (command.command & 0x0fu) == 0x0fu ? 0u : command.mode;
  for (u32 lane = 0; lane < 4; lane++) {
    switch ((cycle_mask >> (lane * 2u)) & 3u) {
    case 0:
      switch (mode) {
      case 1:
        output[lane] = input[lane] + state->row[lane];
        break;
      case 2:
        state->row[lane] += input[lane];
        output[lane] = state->row[lane];
        break;
      case 3:
        state->row[lane] = input[lane];
        output[lane] = input[lane];
        break;
      default:
        output[lane] = input[lane];
        break;
      }
      break;
    case 1:
      output[lane] = state->row[lane];
      break;
    case 2:
      output[lane] = state->column[mask_cycle];
      break;
    default:
      break;
    }
  }
  std::memcpy(destination, output, sizeof(output));
}

bool ExecuteVifUnpack(const DecodedVifCommand& command, const u8* payload,
                      u32 payload_size, VURegs* vu,
                      UniversalCommandEpochState* state) {
  u32 required_bytes = 0;
  if (!RequiredPayloadBytes(command, &required_bytes) ||
      command.source_size < required_bytes ||
      command.payload_offset > payload_size ||
      command.source_size > payload_size - command.payload_offset ||
      !payload || !vu || !vu->Mem || !state) {
    return false;
  }

  const u32 format = command.command & 0x0fu;
  const u32 vector_bytes = SourceVectorBytes[format];
  const u32 cycle_cl = command.cycle_cl;
  const u32 cycle_wl = command.cycle_wl ? command.cycle_wl : 256u;
  const bool fill = cycle_cl < cycle_wl;
  const u32 generated_alignment = format == 0x09u ?
      command.start_alignment : (command.start_alignment & 1u);
  const u8* source = payload + command.payload_offset;
  u32 source_offset = 0;
  u32 destination_qword = command.destination_qword;
  u32 cycle = 0;
  u32 generated_iteration = 0;

  for (u32 index = 0; index < command.vector_count; index++) {
    u32 vector_iteration = generated_iteration;
    if (format == 0x09u || format == 0x0au)
      vector_iteration = ++generated_iteration;
    std::array<u32, 4> vector{};
    LoadVector(command, source + source_offset, vector_iteration,
               generated_alignment, &vector);
    StoreVector(command, state, cycle, vector,
                vu->Mem + ((destination_qword & 0x3ffu) * 16u));
    if (format == 0x08u)
      generated_iteration = (generated_iteration + 1u) & 1u;

    destination_qword++;
    cycle++;
    if (fill) {
      if (cycle <= cycle_cl)
        source_offset += vector_bytes;
      else if (cycle == cycle_wl)
        cycle = 0;
    } else {
      source_offset += vector_bytes;
      if (cycle >= cycle_wl) {
        destination_qword += cycle_cl - cycle_wl;
        cycle = 0;
      }
    }
  }

  state->vif_top = command.vif_top;
  state->vif_itop = command.vif_itop;
  return true;
}

UniversalEpochReferenceStop MapVuStop(UniversalReferenceStepResult stop) {
  switch (stop) {
  case UniversalReferenceStepResult::ProgramFinished:
    return UniversalEpochReferenceStop::Completed;
  case UniversalReferenceStepResult::DbitObserver:
  case UniversalReferenceStepResult::TbitObserver:
    return UniversalEpochReferenceStop::VuObserver;
  case UniversalReferenceStepResult::PairLimitReached:
    return UniversalEpochReferenceStop::VuPairLimitReached;
  case UniversalReferenceStepResult::InvalidEncoding:
    return UniversalEpochReferenceStop::VuInvalidEncoding;
  case UniversalReferenceStepResult::PairCompleted:
    break;
  }
  return UniversalEpochReferenceStop::VuInvalidEncoding;
}

}  // namespace

bool AppendUniversalRawPath1Packet(
    const u8* vu_memory, u32 source_byte_address,
    UniversalRawPath1Export* output, std::string* error) {
  if (error)
    error->clear();
  if (!vu_memory || !output ||
      output->format_version != UniversalRawPath1ExportFormatVersion ||
      output->packet_count > UniversalRawPath1ExportMaximumPackets ||
      output->data_qword_count > UniversalRawPath1ExportDataQwords) {
    return FailPath1(output, UniversalRawPath1ExportError::InvalidState,
                     error, "invalid universal PATH1 export state");
  }
  if ((source_byte_address & 0x0fu) != 0 ||
      source_byte_address >= Vu1MemoryBytes) {
    return FailPath1(output, UniversalRawPath1ExportError::InvalidAddress,
                     error, "unaligned universal PATH1 source address");
  }

  // Gif_Unit.h::GetGSPacketSize() rejects a PATH1 packet as soon as its
  // inclusive tag+payload size reaches the complete 16 KiB VU memory image.
  // Scan first so a missing EOP, oversized tag, or output-capacity failure can
  // never publish a partial packet.
  u32 packet_qwords = 0;
  u32 tag_count = 0;
  bool found_eop = false;
  while (packet_qwords < Vu1MemoryQwords) {
    const u32 tag_qword =
        ((source_byte_address >> 4) + packet_qwords) & 0x3ffu;
    std::array<u32, 4> tag{};
    std::memcpy(tag.data(), vu_memory + tag_qword * 16u, sizeof(tag));
    const u32 nloop = tag[0] & 0x7fffu;
    const bool eop = (tag[0] & 0x8000u) != 0;
    const u32 flg = (tag[1] >> 26) & 0x3u;
    u32 nreg = (tag[1] >> 28) & 0x0fu;
    if (nreg == 0)
      nreg = 16;

    u64 payload_qwords = 0;
    if (flg == 0)
      payload_qwords = static_cast<u64>(nloop) * nreg;
    else if (flg == 1)
      payload_qwords =
          (static_cast<u64>(nloop) * nreg + 1u) >> 1;
    else
      payload_qwords = nloop;
    const u64 next_qwords = static_cast<u64>(packet_qwords) + 1u +
                            payload_qwords;
    if (next_qwords >= Vu1MemoryQwords) {
      return FailPath1(
          output, UniversalRawPath1ExportError::PacketExceedsVuMemory,
          error, "universal PATH1 packet reaches the VU-memory size limit");
    }
    packet_qwords = static_cast<u32>(next_qwords);
    tag_count++;
    if (eop) {
      found_eop = true;
      break;
    }
  }
  if (!found_eop) {
    return FailPath1(
        output, UniversalRawPath1ExportError::PacketExceedsVuMemory, error,
        "universal PATH1 packet has no bounded EOP tag");
  }
  if (output->packet_count == UniversalRawPath1ExportMaximumPackets) {
    return FailPath1(output,
                     UniversalRawPath1ExportError::DescriptorCapacity,
                     error, "universal PATH1 descriptor buffer is full");
  }
  if (packet_qwords > UniversalRawPath1ExportDataQwords -
                          output->data_qword_count) {
    return FailPath1(output, UniversalRawPath1ExportError::PacketCapacity,
                     error, "universal PATH1 payload buffer is full");
  }

  const u32 output_offset = output->data_qword_count;
  const u32 source_qword = source_byte_address >> 4;
  for (u32 qword = 0; qword < packet_qwords; qword++) {
    std::memcpy(output->data[output_offset + qword].data(),
                vu_memory + (((source_qword + qword) & 0x3ffu) * 16u),
                16u);
  }
  output->packets[output->packet_count] = {
      source_byte_address, output_offset, packet_qwords, tag_count};
  output->packet_count++;
  output->data_qword_count += packet_qwords;
  output->error = UniversalRawPath1ExportError::None;
  return true;
}

bool GetUniversalVifUnpackPayloadSize(const VifUnpackSpan& span,
                                      u32* payload_size,
                                      std::string* error) {
  if (error)
    error->clear();
  if (!payload_size)
    return Fail(error, "null universal VIF payload-size output");
  DecodedVifCommand decoded;
  decoded.command = span.command;
  decoded.cycle_cl = span.cycle_cl;
  decoded.cycle_wl = span.cycle_wl;
  decoded.mode = span.mode;
  decoded.unsigned_data = span.unsigned_data != 0;
  decoded.start_alignment = span.start_alignment;
  decoded.source_size = span.source_size;
  decoded.destination_qword = span.destination_qword;
  decoded.vector_count = span.vector_count;
  if (span.mode > 3 || span.unsigned_data > 1 ||
      span.start_alignment > StartAlignmentMask ||
      (span.command & 0x80u) != 0 ||
      (span.command & 0x60u) != 0x60u ||
      !RequiredPayloadBytes(decoded, payload_size)) {
    return Fail(error, "unsupported universal VIF UNPACK descriptor");
  }
  return true;
}

bool IsFixedUniversalVifUnpackSupported(const VifUnpackSpan& span) {
  u32 required_bytes = 0;
  return span.mode <= 3 &&
      span.unsigned_data <= 1 && span.start_alignment <= 7 &&
      (span.command & 0x80u) == 0 && (span.command & 0x60u) == 0x60u &&
      span.vector_count != 0 &&
      span.vector_count <= UniversalGpuVuMaximumUnpackVectorsPerCommand &&
      span.destination_qword < 1024 &&
      GetUniversalVifUnpackPayloadSize(
          span, &required_bytes, nullptr) &&
      required_bytes <= span.source_size;
}

bool IsIndependentUniversalVifUnpackSupported(const VifUnpackSpan& span) {
  if (!IsFixedUniversalVifUnpackSupported(span))
    return false;
  const u32 format = span.command & 0x0fu;
  const u32 effective_mode = format == 0x0fu ? 0u : span.mode;
  const u32 cycle_wl = span.cycle_wl == 0 ? 256u : span.cycle_wl;
  return effective_mode <= 1u && span.cycle_cl <= cycle_wl;
}

bool EncodeUniversalVifUnpackCommand(const VifUnpackSpan& span,
                                     u32 payload_offset,
                                     UniversalEpochMicroOp* command,
                                     std::string* error) {
  if (error)
    error->clear();
  if (!command)
    return Fail(error, "null universal VIF command output");
  if (span.source_size == 0 || span.vector_count == 0 ||
      span.destination_qword >= 1024 || span.mode > 3 ||
      span.unsigned_data > 1 ||
      span.start_alignment > StartAlignmentMask ||
      (span.command & 0x80u) != 0 || (span.command & 0x60u) != 0x60u ||
      !IsSupportedFormat(span.command & 0x0fu)) {
    return Fail(error, "unsupported universal VIF UNPACK descriptor");
  }

  DecodedVifCommand decoded;
  decoded.command = span.command;
  decoded.cycle_cl = span.cycle_cl;
  decoded.cycle_wl = span.cycle_wl;
  decoded.mode = span.mode;
  decoded.unsigned_data = span.unsigned_data != 0;
  decoded.start_alignment = span.start_alignment;
  decoded.source_size = span.source_size;
  decoded.destination_qword = span.destination_qword;
  decoded.vector_count = span.vector_count;
  u32 required_bytes = 0;
  if (!RequiredPayloadBytes(decoded, &required_bytes) ||
      required_bytes > span.source_size ||
      payload_offset > std::numeric_limits<u32>::max() - span.source_size) {
    return Fail(error, "universal VIF UNPACK payload is incomplete");
  }

  *command = {};
  command->kind = static_cast<u32>(UniversalEpochCommandKind::VifUnpack);
  command->control = static_cast<u32>(span.command) |
      (static_cast<u32>(span.cycle_cl) << CycleClShift) |
      (static_cast<u32>(span.cycle_wl) << CycleWlShift) |
      (static_cast<u32>(span.mode) << ModeShift) |
      (span.unsigned_data ? UnsignedBit : 0u) |
      (static_cast<u32>(span.start_alignment) << StartAlignmentShift);
  command->arguments[0] = payload_offset;
  command->arguments[1] = span.source_size;
  command->arguments[2] = span.mask;
  command->arguments[3] =
      PackHalves(span.destination_qword, span.vector_count);
  command->arguments[4] = PackHalves(span.vif_top, span.vif_itop);
  return true;
}

bool EncodeUniversalVuExecuteCommand(u32 program_index, u32 start_pc,
                                     u32 maximum_pairs, u16 vif_top,
                                     u16 vif_itop, u32 fbrst, bool resume,
                                     UniversalEpochMicroOp* command,
                                     std::string* error) {
  if (error)
    error->clear();
  if (!command)
    return Fail(error, "null universal VU execute output");
  if (maximum_pairs == 0 ||
      maximum_pairs > UniversalCommandEpochMaximumPairsPerExecute ||
      (!resume && ((start_pc & 7u) != 0 || start_pc > VU1_PROGMASK))) {
    return Fail(error, "invalid bounded universal VU execute descriptor");
  }

  *command = {};
  command->kind = static_cast<u32>(UniversalEpochCommandKind::ExecuteVu1);
  command->control = resume ? ExecuteResumeBit : 0u;
  command->arguments[0] = program_index;
  // TPC is canonical state for MSCNT.  Keep the otherwise-unused explicit-PC
  // word zero so both the CPU oracle and GXP reject stale/non-canonical resume
  // descriptors before effects.
  command->arguments[1] = resume ? 0u : start_pc;
  command->arguments[2] = maximum_pairs;
  command->arguments[3] = PackHalves(vif_top, vif_itop);
  command->arguments[4] = fbrst;
  return true;
}

UniversalEpochMicroOp EncodeUniversalEpochEndCommand() {
  return {};
}

UniversalEpochReferenceResult ExecuteUniversalCommandEpochReference(
    VURegs* vu, const UniversalCommandEpochView& epoch,
    UniversalCommandEpochState* state) {
  UniversalEpochReferenceResult result;
  if (!vu || !state || !epoch.commands || epoch.command_count == 0 ||
      epoch.command_count > UniversalCommandEpochMaximumCommands ||
      epoch.format_version != UniversalCommandEpochFormatVersion) {
    return result;
  }

  for (u32 index = 0; index < epoch.command_count; index++) {
    result.stop_command = index;
    const UniversalEpochMicroOp& command = epoch.commands[index];
    switch (static_cast<UniversalEpochCommandKind>(command.kind)) {
    case UniversalEpochCommandKind::End:
      if (command.control != 0 ||
          std::any_of(command.arguments.begin(), command.arguments.end(),
                      [](u32 value) { return value != 0; })) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
      } else {
        result.stop = UniversalEpochReferenceStop::Completed;
      }
      return result;

    case UniversalEpochCommandKind::VifUnpack: {
      DecodedVifCommand decoded;
      if (!DecodeVifCommand(command, &decoded)) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      u32 required_bytes = 0;
      if (!RequiredPayloadBytes(decoded, &required_bytes)) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      if (!epoch.payload || decoded.source_size < required_bytes ||
          decoded.payload_offset > epoch.payload_size ||
          decoded.source_size > epoch.payload_size - decoded.payload_offset) {
        result.stop = UniversalEpochReferenceStop::PayloadOutOfRange;
        return result;
      }
      if (!ExecuteVifUnpack(decoded, epoch.payload, epoch.payload_size, vu,
                            state)) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      result.unpack_vectors += decoded.vector_count;
      result.commands_executed++;
      break;
    }

    case UniversalEpochCommandKind::ExecuteVu1: {
      if ((command.control & ~ExecuteControlMask) != 0 ||
          command.arguments[5] != 0 || command.arguments[2] == 0 ||
          command.arguments[2] >
              UniversalCommandEpochMaximumPairsPerExecute) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      const u32 program_index = command.arguments[0];
      if (!epoch.programs || program_index >= epoch.program_count ||
          !epoch.programs[program_index]) {
        result.stop = UniversalEpochReferenceStop::ProgramOutOfRange;
        return result;
      }
      const bool resume = (command.control & ExecuteResumeBit) != 0;
      const u32 start_pc = command.arguments[1];
      if ((resume && start_pc != 0) ||
          (!resume && ((start_pc & 7u) != 0 || start_pc > VU1_PROGMASK))) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }

      state->vif_top = LowHalf(command.arguments[3]);
      state->vif_itop = HighHalf(command.arguments[3]);

      // PCSX2 owner: VUops.cpp::_vuXTOP/_vuXITOP.  The pointer-free epoch
      // retains TOP/ITOP independently of the live VIF worker. Materialize
      // those descriptor values into the same register owner read by the
      // canonical lower bodies before starting this accepted Execute command.
      // All descriptor validation above precedes this first effect.
      VIFregisters& vif_regs = vu->GetVifRegs();
      vif_regs.top = state->vif_top;
      vif_regs.itop = state->vif_itop;
      if (vu == &VU1 && THREAD_VU1) {
        vu1Thread.vifRegs.top = state->vif_top;
        vu1Thread.vifRegs.itop = state->vif_itop;
      }
      if (!resume)
        vu->VI[REG_TPC].UL = start_pc >> 3;
      const UniversalReferenceRunResult vu_result =
          ExecuteUniversalReferenceProgramWithFbrst(
              vu, *epoch.programs[program_index], command.arguments[2],
              command.arguments[4]);
      result.vu_pairs += vu_result.executed_pairs;
      if (vu_result.result != UniversalReferenceStepResult::ProgramFinished) {
        result.stop = MapVuStop(vu_result.result);
        return result;
      }
      result.commands_executed++;
      break;
    }

    default:
      result.stop = UniversalEpochReferenceStop::InvalidCommand;
      return result;
    }
  }

  result.stop_command = epoch.command_count;
  result.stop = UniversalEpochReferenceStop::CommandLimitReached;
  return result;
}

namespace {

UniversalEpochReferenceResult ContinueUniversalCommandEpochReferenceImpl(
    VURegs* vu, const UniversalCommandEpochView& epoch,
    UniversalCommandEpochState* state,
    UniversalCommandEpochContinuationState* continuation,
    UniversalRawPath1ContinuationState* path1_continuation,
    UniversalRawPath1Export* private_path1, u32 submission_pair_limit,
    std::string* path1_error) {
  UniversalEpochReferenceResult result;
  const bool owns_path1 = path1_continuation != nullptr ||
                          private_path1 != nullptr;
  if (!vu || !state || !continuation || !epoch.commands ||
      epoch.command_count == 0 ||
      epoch.command_count > UniversalCommandEpochMaximumCommands ||
      epoch.format_version != UniversalCommandEpochFormatVersion ||
      submission_pair_limit == 0 ||
      continuation->command_cursor > epoch.command_count ||
      (continuation->completed && continuation->execute_active) ||
      (owns_path1 && (!path1_continuation || !private_path1)) ||
      (owns_path1 &&
       (private_path1->format_version !=
            UniversalRawPath1ExportFormatVersion ||
        private_path1->packet_count > UniversalRawPath1ExportMaximumPackets ||
        private_path1->data_qword_count >
            UniversalRawPath1ExportDataQwords ||
        private_path1->error != UniversalRawPath1ExportError::None ||
        (path1_continuation->pending_xgkick &&
         ((path1_continuation->pending_xgkick_address & 0x0fu) != 0 ||
          path1_continuation->pending_xgkick_address >= Vu1MemoryBytes)))) ||
      (continuation->execute_active &&
       (continuation->command_cursor >= epoch.command_count ||
        continuation->active_pairs_remaining == 0 ||
        epoch.commands[continuation->command_cursor].kind !=
            static_cast<u32>(UniversalEpochCommandKind::ExecuteVu1)))) {
    return result;
  }
  result.stop_command = continuation->command_cursor;
  if (continuation->completed) {
    result.stop = UniversalEpochReferenceStop::Completed;
    return result;
  }

  u32 submission_pairs_remaining = submission_pair_limit;
  bool path1_failed = false;
  const auto execute_pairs = [&](const UniversalMicroProgram& program,
                                 u32 maximum_pairs,
                                 u32 fbrst) {
    if (!owns_path1) {
      return ExecuteUniversalReferenceProgramWithFbrst(
          vu, program, maximum_pairs, fbrst);
    }

    UniversalReferenceRunResult run;
    vu->VI[REG_TPC].UL <<= 3;
    for (u32 step_index = 0; step_index < maximum_pairs; step_index++) {
      const bool had_pending = path1_continuation->pending_xgkick;
      const u32 pending_address =
          path1_continuation->pending_xgkick_address;
      UniversalReferencePairEvent event;
      run.result = ExecuteUniversalReferenceStepWithFbrstAndEvent(
          vu, program, fbrst, &event);
      if (run.result != UniversalReferenceStepResult::PairCompleted &&
          run.result != UniversalReferenceStepResult::ProgramFinished) {
        break;
      }
      run.executed_pairs++;

      // Normal microVU makes an XGKICK visible after the complete following
      // pair.  Use the post-pair VU-memory generation so same-pair stores are
      // included, exactly like the fixed GXP's local write journal.
      path1_continuation->pending_xgkick = false;
      path1_continuation->pending_xgkick_address = 0;
      if (had_pending &&
          !AppendUniversalRawPath1Packet(
              vu->Mem, pending_address, private_path1, path1_error)) {
        path1_failed = true;
        break;
      }
      if (event.queues_xgkick) {
        path1_continuation->pending_xgkick = true;
        path1_continuation->pending_xgkick_address =
            event.xgkick_address;
      }

      // PCSX2 VU1microInterp.cpp::_vu1FinishProgram() flushes an XGKICK
      // queued by the terminal delay-slot pair.  It has no following guest
      // pair, so publish it at the same terminal drain into the still-private
      // output generation.
      if (run.result == UniversalReferenceStepResult::ProgramFinished &&
          path1_continuation->pending_xgkick) {
        const u32 terminal_address =
            path1_continuation->pending_xgkick_address;
        path1_continuation->pending_xgkick = false;
        path1_continuation->pending_xgkick_address = 0;
        if (!AppendUniversalRawPath1Packet(
                vu->Mem, terminal_address, private_path1, path1_error)) {
          path1_failed = true;
        }
      }
      if (run.result != UniversalReferenceStepResult::PairCompleted ||
          path1_failed) {
        break;
      }
    }
    run.stop_pc = vu->VI[REG_TPC].UL & VU1_PROGMASK;
    vu->VI[REG_TPC].UL >>= 3;
    if (!path1_failed &&
        run.result == UniversalReferenceStepResult::PairCompleted) {
      run.result = UniversalReferenceStepResult::PairLimitReached;
    }
    return run;
  };
  for (u32 command_guard = 0;
       command_guard <= UniversalCommandEpochMaximumCommands;
       command_guard++) {
    result.stop_command = continuation->command_cursor;
    if (continuation->command_cursor >= epoch.command_count) {
      result.stop = UniversalEpochReferenceStop::CommandLimitReached;
      return result;
    }

    const UniversalEpochMicroOp& command =
        epoch.commands[continuation->command_cursor];
    if (continuation->execute_active) {
      if ((command.control & ~ExecuteControlMask) != 0 ||
          command.arguments[5] != 0 || command.arguments[2] == 0 ||
          command.arguments[2] >
              UniversalCommandEpochMaximumPairsPerExecute) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      const u32 program_index = command.arguments[0];
      if (!epoch.programs || program_index >= epoch.program_count ||
          !epoch.programs[program_index]) {
        result.stop = UniversalEpochReferenceStop::ProgramOutOfRange;
        return result;
      }
      if (submission_pairs_remaining == 0) {
        result.stop = UniversalEpochReferenceStop::SubmissionLimitReached;
        return result;
      }

      const u32 run_pairs = std::min(
          submission_pairs_remaining,
          continuation->active_pairs_remaining);
      const UniversalReferenceRunResult vu_result =
          execute_pairs(*epoch.programs[program_index], run_pairs,
                        command.arguments[4]);
      result.vu_pairs += vu_result.executed_pairs;
      continuation->total_vu_pairs += vu_result.executed_pairs;
      submission_pairs_remaining -= vu_result.executed_pairs;
      continuation->active_pairs_remaining -= vu_result.executed_pairs;

      if (path1_failed) {
        result.stop = UniversalEpochReferenceStop::Path1Rejected;
        return result;
      }

      if (vu_result.result == UniversalReferenceStepResult::ProgramFinished) {
        continuation->execute_active = false;
        continuation->active_pairs_remaining = 0;
        continuation->command_cursor++;
        continuation->total_commands_executed++;
        result.commands_executed++;
        if (submission_pairs_remaining == 0) {
          result.stop = UniversalEpochReferenceStop::SubmissionLimitReached;
          result.stop_command = continuation->command_cursor;
          return result;
        }
        continue;
      }
      if (vu_result.result == UniversalReferenceStepResult::PairLimitReached) {
        result.stop = continuation->active_pairs_remaining == 0
            ? UniversalEpochReferenceStop::VuPairLimitReached
            : UniversalEpochReferenceStop::SubmissionLimitReached;
        return result;
      }
      result.stop = MapVuStop(vu_result.result);
      return result;
    }

    switch (static_cast<UniversalEpochCommandKind>(command.kind)) {
    case UniversalEpochCommandKind::End:
      if (command.control != 0 ||
          std::any_of(command.arguments.begin(), command.arguments.end(),
                      [](u32 value) { return value != 0; })) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      continuation->completed = true;
      result.stop = UniversalEpochReferenceStop::Completed;
      return result;

    case UniversalEpochCommandKind::VifUnpack: {
      DecodedVifCommand decoded;
      if (!DecodeVifCommand(command, &decoded)) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      u32 required_bytes = 0;
      if (!RequiredPayloadBytes(decoded, &required_bytes)) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      if (!epoch.payload || decoded.source_size < required_bytes ||
          decoded.payload_offset > epoch.payload_size ||
          decoded.source_size > epoch.payload_size - decoded.payload_offset) {
        result.stop = UniversalEpochReferenceStop::PayloadOutOfRange;
        return result;
      }
      if (!ExecuteVifUnpack(decoded, epoch.payload, epoch.payload_size, vu,
                            state)) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      result.unpack_vectors += decoded.vector_count;
      result.commands_executed++;
      continuation->total_unpack_vectors += decoded.vector_count;
      continuation->total_commands_executed++;
      continuation->command_cursor++;
      break;
    }

    case UniversalEpochCommandKind::ExecuteVu1: {
      if ((command.control & ~ExecuteControlMask) != 0 ||
          command.arguments[5] != 0 || command.arguments[2] == 0 ||
          command.arguments[2] >
              UniversalCommandEpochMaximumPairsPerExecute) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      const u32 program_index = command.arguments[0];
      if (!epoch.programs || program_index >= epoch.program_count ||
          !epoch.programs[program_index]) {
        result.stop = UniversalEpochReferenceStop::ProgramOutOfRange;
        return result;
      }
      const bool resume = (command.control & ExecuteResumeBit) != 0;
      const u32 start_pc = command.arguments[1];
      if ((resume && start_pc != 0) ||
          (!resume && ((start_pc & 7u) != 0 ||
                       start_pc > VU1_PROGMASK))) {
        result.stop = UniversalEpochReferenceStop::InvalidCommand;
        return result;
      }
      if (submission_pairs_remaining == 0) {
        result.stop = UniversalEpochReferenceStop::SubmissionLimitReached;
        return result;
      }

      state->vif_top = LowHalf(command.arguments[3]);
      state->vif_itop = HighHalf(command.arguments[3]);
      VIFregisters& vif_regs = vu->GetVifRegs();
      vif_regs.top = state->vif_top;
      vif_regs.itop = state->vif_itop;
      if (vu == &VU1 && THREAD_VU1) {
        vu1Thread.vifRegs.top = state->vif_top;
        vu1Thread.vifRegs.itop = state->vif_itop;
      }
      if (!resume)
        vu->VI[REG_TPC].UL = start_pc >> 3;
      continuation->active_pairs_remaining = command.arguments[2];
      continuation->execute_active = true;
      break;
    }

    default:
      result.stop = UniversalEpochReferenceStop::InvalidCommand;
      return result;
    }
  }

  result.stop = UniversalEpochReferenceStop::CommandLimitReached;
  return result;
}

}  // namespace

UniversalEpochReferenceResult ContinueUniversalCommandEpochReference(
    VURegs* vu, const UniversalCommandEpochView& epoch,
    UniversalCommandEpochState* state,
    UniversalCommandEpochContinuationState* continuation,
    u32 submission_pair_limit) {
  return ContinueUniversalCommandEpochReferenceImpl(
      vu, epoch, state, continuation, nullptr, nullptr,
      submission_pair_limit, nullptr);
}

UniversalEpochReferenceResult
ContinueUniversalCommandEpochTransactionalReference(
    VURegs* vu, const UniversalCommandEpochView& epoch,
    UniversalCommandEpochState* state,
    UniversalCommandEpochContinuationState* continuation,
    UniversalRawPath1ContinuationState* path1_continuation,
    UniversalRawPath1Export* private_path1, u32 submission_pair_limit,
    std::string* error) {
  if (error)
    error->clear();
  return ContinueUniversalCommandEpochReferenceImpl(
      vu, epoch, state, continuation, path1_continuation, private_path1,
      submission_pair_limit, error);
}

bool CommitUniversalRawPath1ReferenceTransaction(
    const UniversalCommandEpochContinuationState& continuation,
    const UniversalRawPath1ContinuationState& path1_continuation,
    const UniversalRawPath1Export& private_path1,
    UniversalRawPath1Export* committed_path1, std::string* error) {
  if (error)
    error->clear();
  if (!committed_path1)
    return Fail(error, "null committed universal PATH1 generation");
  if (!continuation.completed || continuation.execute_active ||
      path1_continuation.pending_xgkick) {
    return Fail(error,
                "universal PATH1 transaction has not reached terminal End");
  }
  if (private_path1.format_version !=
          UniversalRawPath1ExportFormatVersion ||
      private_path1.error != UniversalRawPath1ExportError::None ||
      private_path1.packet_count > UniversalRawPath1ExportMaximumPackets ||
      private_path1.data_qword_count > UniversalRawPath1ExportDataQwords) {
    return Fail(error, "invalid private universal PATH1 generation");
  }

  u32 expected_qword_offset = 0;
  for (u32 packet_index = 0;
       packet_index < private_path1.packet_count; packet_index++) {
    const UniversalRawPath1PacketDescriptor& packet =
        private_path1.packets[packet_index];
    if ((packet.source_byte_address & 0x0fu) != 0 ||
        packet.source_byte_address >= Vu1MemoryBytes ||
        packet.output_qword_offset != expected_qword_offset ||
        packet.qword_count == 0 || packet.tag_count == 0 ||
        packet.qword_count >
            private_path1.data_qword_count - expected_qword_offset) {
      return Fail(error,
                  "invalid descriptor in private universal PATH1 generation");
    }
    expected_qword_offset += packet.qword_count;
  }
  if (expected_qword_offset != private_path1.data_qword_count)
    return Fail(error, "unowned qwords in private universal PATH1 generation");

  // This is the only authoritative write in the host transaction oracle.
  // Every structural check above is complete before the 256 KiB generation
  // copy starts, so a rejection leaves the caller's prior generation intact.
  if (committed_path1 != &private_path1)
    *committed_path1 = private_path1;
  return true;
}

}  // namespace VitaGpuVu
