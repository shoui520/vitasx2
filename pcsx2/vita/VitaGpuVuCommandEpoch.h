// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>
#include <string>

struct VURegs;

namespace VitaGpuVu {

struct UniversalMicroProgram;
struct VifUnpackSpan;

// Flat command ABI shared by the CPU oracle and the fixed GXP executor. The
// words contain no host pointers and may be copied directly into a GXM uniform
// buffer. Kind-specific encoders below are the only supported construction
// path; unused argument words must remain zero so newer formats fail closed.
struct UniversalEpochMicroOp {
  u32 kind = 0;
  u32 control = 0;
  std::array<u32, 6> arguments{};
};

static_assert(sizeof(UniversalEpochMicroOp) == 32);
static_assert(alignof(UniversalEpochMicroOp) == alignof(u32));

inline constexpr u32 UniversalCommandEpochFormatVersion = 1;
inline constexpr u32 UniversalCommandEpochMaximumCommands = 64;
// Words 96..103 extend the common VU execution state with VIF1's mutable
// STROW/STCOL values.  MODE 2/3 UNPACK changes row state, so these words are
// part of the transactional generation rather than host-side command data.
inline constexpr u32 UniversalGpuVuStateVifRowWord = 96;
// Product-only generated-provider seam. A nonzero enable requests that the
// fixed command/entry core stop before executing the exact byte PC in the
// target word. Both words live in the pointer-free private state ABI and are
// cleared before the generated tail resumes.
inline constexpr u32 UniversalGpuVuStateStructuredTargetPcWord = 37;
inline constexpr u32 UniversalGpuVuStateStructuredTargetEnableWord = 38;
inline constexpr u32 UniversalGpuVuStateVifColumnWord = 100;
inline constexpr u32 UniversalGpuVuStateUnpackVectorWord = 104;
inline constexpr u32 UniversalGpuVuStateUnpackSourceWord = 105;
inline constexpr u32 UniversalGpuVuStateUnpackDestinationWord = 106;
inline constexpr u32 UniversalGpuVuStateUnpackCycleWord = 107;
inline constexpr u32 UniversalGpuVuStateUnpackGeneratedWord = 108;
inline constexpr u32 UniversalGpuVuStateUnpackCommandWord = 109;
// Keep two words of versioned state headroom. Structured generated admission
// proves that its entry dominates every reachable terminal before any jobs are
// queued; no shader-private pass-through marker is permitted to repair an
// entry which can terminate before reaching the generated transaction.
inline constexpr u32 UniversalGpuVuStateWordCount = 112;
inline constexpr u32 UniversalGpuVuMaximumUnpackVectorsPerCommand = 256;
inline constexpr u32 UniversalGpuVuUnpackVectorsPerSubmission = 128;
// One fixed GXP invocation keeps all transient VU pipeline state in shader
// locals until the terminal E-bit drain.  This bound is deliberately large
// enough for observed complete retail/sample executions while still stopping
// malformed non-terminating microprograms in finite GPU work.  A budget stop
// is a scratch-state admission failure; it must never be published and replayed
// after guest-visible effects.
inline constexpr u32 UniversalCommandEpochMaximumPairsPerExecute = 16384;

// Coarse PATH1 output ABI shared by the CPU oracle and the fixed GXP
// executor. One record describes one complete EOP-bounded XGKICK packet; the
// packet bytes are copied only after a full bounds/tag preflight succeeds.
// Product slots form a four-generation, sequence-owned mapped output ring.
// One generation uses the four writable 64 KiB shader buffers bound by the
// fixed GXP. The first page owns the header and descriptors; its data tail and
// the three following pages are physically
// contiguous, so ordered GS retirement sees one complete-packet byte stream.
// Exhaustion remains transactional: no prefix commits.
inline constexpr u32 UniversalRawPath1ExportFormatVersion = 1;
inline constexpr u32 UniversalRawPath1ExportPageWords = 16 * 1024;
inline constexpr u32 UniversalRawPath1ExportPageCount = 4;
inline constexpr u32 UniversalRawPath1ExportBufferWords =
    UniversalRawPath1ExportPageWords * UniversalRawPath1ExportPageCount;
inline constexpr u32 UniversalRawPath1ExportDescriptorWordOffset = 16;
inline constexpr u32 UniversalRawPath1ExportDescriptorWords = 4;
inline constexpr u32 UniversalRawPath1ExportMaximumPackets = 256;
inline constexpr u32 UniversalRawPath1ExportDataWordOffset =
    UniversalRawPath1ExportDescriptorWordOffset +
    UniversalRawPath1ExportMaximumPackets *
        UniversalRawPath1ExportDescriptorWords +
    16;
inline constexpr u32 UniversalRawPath1ExportDataQwords =
    (UniversalRawPath1ExportBufferWords -
     UniversalRawPath1ExportDataWordOffset) /
    4;
inline constexpr u32 UniversalRawPath1ExportFirstPageDataQwords =
    (UniversalRawPath1ExportPageWords -
     UniversalRawPath1ExportDataWordOffset) /
    4;
inline constexpr u32 UniversalRawPath1ExportFollowingPageQwords =
    UniversalRawPath1ExportPageWords / 4;

struct UniversalRawPath1PacketDescriptor {
  u32 source_byte_address = 0;
  u32 output_qword_offset = 0;
  u32 qword_count = 0;
  u32 tag_count = 0;
};

enum class UniversalRawPath1ExportError : u32 {
  None = 0,
  InvalidState = 1,
  InvalidAddress = 2,
  PacketExceedsVuMemory = 3,
  PacketCapacity = 4,
  DescriptorCapacity = 5,
};

struct UniversalRawPath1Export {
  u32 format_version = UniversalRawPath1ExportFormatVersion;
  u32 packet_count = 0;
  u32 data_qword_count = 0;
  UniversalRawPath1ExportError error = UniversalRawPath1ExportError::None;
  std::array<u32, 12> reserved_header{};
  std::array<UniversalRawPath1PacketDescriptor,
             UniversalRawPath1ExportMaximumPackets>
      packets{};
  std::array<u32, 16> reserved_descriptors{};
  std::array<std::array<u32, 4>, UniversalRawPath1ExportDataQwords> data{};
};

static_assert(offsetof(UniversalRawPath1Export, packets) ==
              UniversalRawPath1ExportDescriptorWordOffset * sizeof(u32));
static_assert(offsetof(UniversalRawPath1Export, data) ==
              UniversalRawPath1ExportDataWordOffset * sizeof(u32));
static_assert(sizeof(UniversalRawPath1Export) ==
              UniversalRawPath1ExportBufferWords * sizeof(u32));

// PCSX2 oracle: Gif_Unit.h::GetGSPacketSize(GIF_PATH_1, ..., flush=true).
// Appends the exact tag and payload qwords, including 16 KiB VU-memory wrap,
// through the first EOP tag. A rejected packet changes only error metadata;
// packet descriptors, payload bytes, and committed counts remain untouched.
bool AppendUniversalRawPath1Packet(
    const u8* vu_memory, u32 source_byte_address,
    UniversalRawPath1Export* output, std::string* error = nullptr);

enum class UniversalEpochCommandKind : u32 {
  End = 0,
  VifUnpack = 1,
  ExecuteVu1 = 2,
};

// Encodes one completed VIF1 UNPACK. The raw payload remains in a separate
// immutable byte buffer and is addressed by payload_offset/source_size.
// Interrupting UNPACKs remain observation forms. Zero WL follows PCSX2's
// 256-cycle expansion and zero CL retains the hardware fill semantics.
bool GetUniversalVifUnpackPayloadSize(const VifUnpackSpan& span,
                                      u32* payload_size,
                                      std::string* error = nullptr);
bool IsFixedUniversalVifUnpackSupported(const VifUnpackSpan& span);
// True when every vector in a 128-vector submission is independent: row
// state is read-only and the destination advances monotonically by one. Such
// commands use the sparse mirrored-bank GXP and avoid the serial write journal.
bool IsIndependentUniversalVifUnpackSupported(const VifUnpackSpan& span);
bool EncodeUniversalVifUnpackCommand(const VifUnpackSpan& span,
                                     u32 payload_offset,
                                     UniversalEpochMicroOp* command,
                                     std::string* error = nullptr);

// Encodes one bounded MSCAL/MSCNT-equivalent execution. An explicit command
// replaces TPC with start_pc; a resume command retains canonical TPC. The
// program index addresses the immutable program table in the epoch view.
bool EncodeUniversalVuExecuteCommand(u32 program_index, u32 start_pc,
                                     u32 maximum_pairs, u16 vif_top,
                                     u16 vif_itop, u32 fbrst, bool resume,
                                     UniversalEpochMicroOp* command,
                                     std::string* error = nullptr);

UniversalEpochMicroOp EncodeUniversalEpochEndCommand();

struct UniversalCommandEpochView {
  u32 format_version = UniversalCommandEpochFormatVersion;
  const UniversalEpochMicroOp* commands = nullptr;
  u32 command_count = 0;
  const u8* payload = nullptr;
  u32 payload_size = 0;
  const UniversalMicroProgram* const* programs = nullptr;
  u32 program_count = 0;
};

// VIF row state is mutable under MODE 2/3 and therefore must remain resident
// across all UNPACK commands in an epoch. Column state is read-only to UNPACK,
// but is carried beside it so mask selectors have no canonical-state seam.
struct UniversalCommandEpochState {
  std::array<u32, 4> row{};
  std::array<u32, 4> column{};
  u16 vif_top = 0;
  u16 vif_itop = 0;
};

enum class UniversalEpochReferenceStop : u8 {
  Completed,
  // A private transaction reached this invocation's watchdog-safe pair
  // boundary.  The command cursor and complete VU pipeline state remain
  // resumable; this is neither architectural pair exhaustion nor rejection.
  SubmissionLimitReached,
  CommandLimitReached,
  InvalidFormat,
  InvalidCommand,
  PayloadOutOfRange,
  ProgramOutOfRange,
  VuObserver,
  VuPairLimitReached,
  VuInvalidEncoding,
  Path1Rejected,
};

struct UniversalEpochReferenceResult {
  UniversalEpochReferenceStop stop =
      UniversalEpochReferenceStop::InvalidFormat;
  u32 stop_command = 0;
  u32 commands_executed = 0;
  u32 unpack_vectors = 0;
  u32 vu_pairs = 0;
};

// Pointer-free host mirror of state words 59--62 in the fixed GXP.  It owns
// only command-level continuation; VURegs remains the PCSX2-owned VF/VI/ACC/
// Q/P/pipeline generation.  A caller may retain this beside a private VU
// generation and invoke ContinueUniversalCommandEpochReference() with any
// nonzero watchdog slice until Completed.
struct UniversalCommandEpochContinuationState {
  u32 command_cursor = 0;
  u32 active_pairs_remaining = 0;
  u32 total_commands_executed = 0;
  u32 total_unpack_vectors = 0;
  u32 total_vu_pairs = 0;
  bool execute_active = false;
  bool completed = false;
};

// Pointer-free mirror of fixed-GXP state words 33--34.  A queued XGKICK is
// retained across watchdog-bounded submissions and becomes one complete raw
// PATH1 packet only after the following pair, or during the terminal E-bit
// drain.  The output buffer supplied to the transactional reference remains a
// private generation until CommitUniversalRawPath1ReferenceTransaction().
struct UniversalRawPath1ContinuationState {
  u32 pending_xgkick_address = 0;
  bool pending_xgkick = false;
};

// Independent reference for the serialized command format. VIF decoding is
// derived from PCSX2 Vif_Unpack.cpp and VitaVifInterpreter.cpp, while VU pair
// execution delegates to the already-differential UniversalMicroProgram
// reference. All bounds and unsupported observers are checked before the
// corresponding command mutates PS2 state.
UniversalEpochReferenceResult ExecuteUniversalCommandEpochReference(
    VURegs* vu, const UniversalCommandEpochView& epoch,
    UniversalCommandEpochState* state);

// Resumable counterpart used to differential-test every fixed-GXP dependency
// cut.  UNPACK commands remain atomic in this command-level owner (the fixed
// VIF kernel has its own vector continuation oracle); Execute commands stop at
// submission_pair_limit without draining or reinitializing VU pipeline state.
// Invalid continuation metadata is rejected before effects.
UniversalEpochReferenceResult ContinueUniversalCommandEpochReference(
    VURegs* vu, const UniversalCommandEpochView& epoch,
    UniversalCommandEpochState* state,
    UniversalCommandEpochContinuationState* continuation,
    u32 submission_pair_limit);

// PATH1-owning continuation oracle.  It uses the same PCSX2-backed VU pair
// executor as ContinueUniversalCommandEpochReference(), but captures XGKICK
// queue events into a private complete-packet output generation.  A malformed
// packet or capacity failure terminates the private transaction; no caller-
// owned committed generation is touched.
UniversalEpochReferenceResult
ContinueUniversalCommandEpochTransactionalReference(
    VURegs* vu, const UniversalCommandEpochView& epoch,
    UniversalCommandEpochState* state,
    UniversalCommandEpochContinuationState* continuation,
    UniversalRawPath1ContinuationState* path1_continuation,
    UniversalRawPath1Export* private_path1, u32 submission_pair_limit,
    std::string* error = nullptr);

// Validates terminal command/PATH1 state in full before copying the private
// output generation.  On failure, committed_path1 is byte-for-byte unchanged.
bool CommitUniversalRawPath1ReferenceTransaction(
    const UniversalCommandEpochContinuationState& continuation,
    const UniversalRawPath1ContinuationState& path1_continuation,
    const UniversalRawPath1Export& private_path1,
    UniversalRawPath1Export* committed_path1,
    std::string* error = nullptr);

}  // namespace VitaGpuVu
