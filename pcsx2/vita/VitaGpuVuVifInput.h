// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>

namespace VitaGpuVu {

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

// Proves that an invocation-indexed VU-memory address range is backed by one
// contiguous raw V4-32 span. VU memory wraps at 1024 qwords, while the source
// byte range itself must remain in bounds.
bool BindAffineRawQwords(const VifUnpackSpan& span,
                        u16 first_qword_address,
                        s32 invocation_coefficient,
                        u32 invocation_count,
                        RawQwordBinding* binding);

struct InputRingStatistics {
  u64 captures = 0;
  u64 captured_bytes = 0;
  u64 disconnected_bypasses = 0;
  u64 disconnected_bypass_bytes = 0;
  u64 capture_fallbacks = 0;
  u64 slot_reuses = 0;
  u64 ring_waits = 0;
  u64 ring_wait_spins = 0;
  u64 live_references = 0;
  u64 peak_live_references = 0;
  u64 deferred_unpacks = 0;
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

  friend bool CaptureRawVifPayload(const void*, u32, RawVifPayloadRef*);
  friend const u8* ResolveRawVifPayload(const RawVifPayloadRef&);
  friend bool RetainRawVifPayload(const RawVifPayloadRef&);
  friend void ReleaseRawVifPayload(RawVifPayloadRef*);
};

// Producer capture, worker resolution, and explicit ownership transfer. A
// retained reference may be handed from MTVU to an ordered GS descriptor.
bool CaptureRawVifPayload(const void* source, u32 size,
                          RawVifPayloadRef* payload);
const u8* ResolveRawVifPayload(const RawVifPayloadRef& payload);
bool RetainRawVifPayload(const RawVifPayloadRef& payload);
void ReleaseRawVifPayload(RawVifPayloadRef* payload);

void RecordDeferredVifUnpack();
void RecordReplayedVifUnpack();
void RecordDisconnectedCaptureBypass(u32 size);
InputRingStatistics GetInputRingStatistics();

} // namespace VitaGpuVu
