// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>

namespace VitaGpuVu {

class GpuVuDraw;

inline constexpr u32 InputRingSlotCount = 4;
inline constexpr u32 InputRingSlotSize = 2 * 1024 * 1024;

enum class RawVifCaptureMode : u8 {
  ContinueEpoch,
  BeginVuCommandEpoch,
};

// Opaque ownership of one immutable byte range in the Vita GPU-readable VIF
// ring. The owner remains alive until every copied reference is released.
struct RawVifPayloadRef {
  uptr owner = 0;
  u32 slot = 0;
  u32 generation = 0;
  u32 offset = 0;
  u32 size = 0;

  bool IsValid() const {
    return owner != 0 && generation != 0 && size != 0;
  }
};

// Exact metadata captured at PCSX2's completed nVifUnpack<1>() boundary. This
// is one VIF command, not one vertex. Destination addresses are VU-memory
// qwords; source bytes remain packed exactly as DMA supplied them.
struct VifUnpackSpan {
  RawVifPayloadRef payload;
  u64 sequence = 0;
  u32 source_size = 0;
  u32 tag_size_words = 0;
  u32 mask = 0;
  u16 destination_qword = 0;
  u16 vector_count = 0;
  u16 vif_top = 0;
  u16 vif_itop = 0;
  u8 command = 0;
  u8 cycle_cl = 0;
  u8 cycle_wl = 0;
  u8 mode = 0;
  u8 unsigned_data = 0;
  u8 start_alignment = 0;
  u8 reserved[2]{};
};

struct RawQwordBinding {
  RawVifPayloadRef payload;
  u32 payload_byte_offset = 0;
  u32 byte_stride = 0;
  u32 invocation_count = 0;
};

// Phase-one direct input requires the source representation already to be one
// raw VU qword per VIF vector. Other UNPACK formats remain valid GPU programs;
// they simply require a generated unpack expression or the universal executor.
bool IsDirectAffineV4_32Span(const VifUnpackSpan& span);

// Returns the exact source extent consumed by PCSX2's mode-zero V4-32 unpack
// loop. nVifUnpack() gives MTVU one trailing safety word beyond a completed
// tag; that word is not part of this proven command's source vectors.
bool GetDirectAffineV4_32PayloadSize(const VifUnpackSpan& span,
                                    u32* payload_size);

// Returns true when applying newer after older makes every VU-memory write
// performed by older unobservable. Both ranges may wrap at qword 0x3ff.
bool DirectAffineSpanFullyOverwrites(const VifUnpackSpan& newer,
                                     const VifUnpackSpan& older);

// Coalesces two completed, adjacent V4-32 commands when both their immutable
// source bytes and wrapping VU-memory destinations are contiguous. On success
// earlier owns the combined range and later's generation reference is
// released. This is an exact journal representation change: PCSX2's
// _nVifUnpackLoop<1>() would perform the same ordered qword writes.
bool MergeAdjacentDirectAffineV4_32Spans(VifUnpackSpan* earlier,
                                         VifUnpackSpan* later);

// Proves that an invocation-indexed VU-memory address range is backed by one
// contiguous raw V4-32 span. VU memory wraps at 1024 qwords, while the source
// byte range itself must remain in bounds.
bool BindAffineRawQwords(const VifUnpackSpan& span,
                        u16 first_qword_address,
                        s32 invocation_coefficient,
                        u32 invocation_count,
                        RawQwordBinding* binding);

// Materializes the exact VU-memory effect proven by
// IsDirectAffineV4_32Span(). This is the CPU fallback/observation seam for an
// immutable raw span: one or two bulk copies replace reconstructing and
// redispatching a general VIF UNPACK on the 496 MHz worker.
bool MaterializeDirectAffineV4_32Span(const VifUnpackSpan& span,
                                     const void* source,
                                     void* vu_memory,
                                     u32 vu_memory_size);

struct InputRingStatistics {
  u64 captures = 0;
  u64 captured_bytes = 0;
  u64 publication_batches = 0;
  u64 published_bytes = 0;
  u64 capture_bypasses = 0;
  u64 capture_bypass_bytes = 0;
  u64 capture_fallbacks = 0;
  u64 slot_reuses = 0;
  u64 ring_waits = 0;
  u64 ring_wait_spins = 0;
  u64 live_references = 0;
  u64 peak_live_references = 0;
  u64 deferred_unpacks = 0;
  u64 affine_span_merges = 0;
  u64 replayed_unpacks = 0;
};

// Four fixed, independently mapped LPDDR slots follow the multi-frame
// ownership pattern used by Sony samples and PhyreEngine. Each kernel block is
// 2 MiB, below the Vita's 16 MiB per-block ceiling. One phase-one V4-32
// command is at most 4 KiB, so each slot holds 512 maximum-sized commands.
class InputRing final {
public:
  InputRing();
  InputRing(const InputRing&) = delete;
  InputRing& operator=(const InputRing&) = delete;
  ~InputRing();

  bool Initialize();
  bool Shutdown();
  bool IsReady() const;

private:
  struct Impl;
  Impl* m_impl = nullptr;

  friend bool CaptureRawVifPayload(const void*, u32, RawVifCaptureMode,
                                   RawVifPayloadRef*);
  friend const u8* ResolveRawVifPayload(const RawVifPayloadRef&);
  friend const u8* ResolveGpuRawVifPayload(const RawVifPayloadRef&);
  friend bool PublishPendingRawVifPayloads(const GpuVuDraw*, u32);
  friend bool RetainRawVifPayload(const RawVifPayloadRef&);
  friend void ReleaseRawVifPayload(RawVifPayloadRef*);
  friend u32 GetRawVifPayloadGenerationReferenceCount(
      const RawVifPayloadRef&);
};

// Producer capture, worker resolution, and explicit ownership transfer. A
// retained reference may be handed from MTVU to an ordered GS descriptor.
bool CaptureRawVifPayload(const void* source, u32 size,
                          RawVifCaptureMode mode,
                          RawVifPayloadRef* payload);
// Proves the current generated-shader ABI can bind every raw stream in one
// mapped slot-generation and keep its dynamically indexed qword offsets in
// the positive signed-16 address range emitted by psp2cgc.
bool HasSingleAddressableRawInputWindow(const GpuVuDraw& draw);
// CPU-side analysis and replay read the same immutable cacheable mapping SGX
// consumes. USER_RW mappings are CPU/GPU coherent under libGXM.
const u8* ResolveRawVifPayload(const RawVifPayloadRef& payload);
// The GS owner may bind only bytes whose direct-capture range crossed the
// ordered direct-run publication boundary.
const u8* ResolveGpuRawVifPayload(const RawVifPayloadRef& payload);
bool PublishPendingRawVifPayloads(const GpuVuDraw* first_draw,
                                 u32 draw_count);
bool RetainRawVifPayload(const RawVifPayloadRef& payload);
void ReleaseRawVifPayload(RawVifPayloadRef* payload);
u32 GetRawVifPayloadGenerationReferenceCount(
    const RawVifPayloadRef& payload);

void RecordDeferredVifUnpack();
void RecordReplayedVifUnpack();
void RecordCaptureBypass(u32 size);
InputRingStatistics GetInputRingStatistics();

} // namespace VitaGpuVu
