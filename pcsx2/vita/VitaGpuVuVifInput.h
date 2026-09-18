// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <memory>

namespace VitaGpuVu {

class GpuVuDraw;
struct StreamBinding;

inline constexpr u32 InputRingSlotCount = 4;
inline constexpr u32 InputRingSlotSize = 2 * 1024 * 1024;
// SGX543 PDS/data fetches may cross the final cache line of an otherwise
// legal buffer access.  libGXM mappings are page-granular, so retain one
// sentinel-filled mapped page after every logical slot. Payloads and generated range
// proofs remain bounded by InputRingSlotSize; this page is fetch padding, not
// additional VIF capacity.
inline constexpr u32 InputRingSlotFetchGuardSize = 4 * 1024;
inline constexpr u32 InputRingSlotFetchGuardWord = 0xd15ea5edu;
inline constexpr u32 InputRingSlotMappedSize =
    InputRingSlotSize + InputRingSlotFetchGuardSize;
// Generated BUFFER0 is one qword-addressed view of the logical input owner.
// The 64-qword Cg declaration must fit wholly inside that owner after rebasing,
// and every dynamic offset emitted by psp2cgc must remain a positive s16.
// Keep these values beside the ring capacity so CPU admission, queue sealing,
// Cg generation, and GS publication cannot silently prove different ABIs.
inline constexpr u32 GeneratedRawInputBufferQwords =
    InputRingSlotSize / 16u;
inline constexpr u32 GeneratedRawInputDeclaredQwords = 64u;
inline constexpr u32 GeneratedRawInputMaximumRelativeQword = 0x7fffu;
static_assert(GeneratedRawInputBufferQwords * 16u == InputRingSlotSize);
static_assert((InputRingSlotMappedSize & (4 * 1024 - 1)) == 0);
static_assert((InputRingSlotFetchGuardSize % sizeof(u32)) == 0);

// All producer and consumer range checks use the logical capacity.  The
// mapped suffix exists only to make a legal final SGX fetch harmless; it must
// never become payload storage.
inline constexpr bool IsInputRingLogicalRange(u32 offset, u32 size) {
  return offset <= InputRingSlotSize &&
         size <= InputRingSlotSize - offset;
}

// The mapped suffix is a physical SGX fetch guard, not guest payload.  Keep
// this check host-testable so a damaged guard can reject a generation before
// sceGxmDraw instead of becoming another Series5 BIF page fault.  On failure,
// damaged_word receives the first mismatching word when supplied.
inline bool IsInputRingFetchGuardIntact(const u32* words, u32 word_count,
                                        u32* damaged_word = nullptr) {
  if (!words || word_count != InputRingSlotFetchGuardSize / sizeof(u32))
    return false;
  for (u32 word = 0; word < word_count; word++) {
    if (words[word] == InputRingSlotFetchGuardWord)
      continue;
    if (damaged_word)
      *damaged_word = word;
    return false;
  }
  return true;
}

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

enum class RawVifPayloadResolveFailure : u8 {
  None,
  InvalidReference,
  InvalidOwner,
  InvalidSlot,
  UnmappedSlot,
  GenerationMismatch,
  LogicalRange,
  InsufficientCommittedPrefix,
  InsufficientPublishedPrefix,
  NoReferences,
  ValidationOwnerUnavailable,
};

struct RawVifPayloadResolveDiagnostics {
  uptr owner = 0;
  u32 slot = 0;
  u32 wanted_generation = 0;
  u32 actual_generation = 0;
  u32 offset = 0;
  u32 size = 0;
  u32 committed_prefix = 0;
  u32 published_prefix = 0;
  u32 references = 0;
};

const char* RawVifPayloadResolveFailureName(
    RawVifPayloadResolveFailure failure);

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
  u32 outer_byte_stride = 0;
  // Complete byte range beginning at payload_byte_offset, including the last
  // 16-byte qword. This remains authoritative for both one-dimensional and
  // rectangular dispatches; invocation_count alone cannot describe a grid.
  u32 payload_byte_extent = 0;
};

// Persistent, value-free ownership map for the private VU1 data-memory
// generation. It retains immutable raw VIF spans and records the latest owner
// independently for every qword. Partial overwrites detach only the affected
// qwords; untouched bytes stay in the mapped input ring until a real CPU
// observer materializes the generation.
//
// The object is shared across speculative CPU-private state copies.  Mutations
// happen only after the replacement state has passed every pre-effect check.
// Its retained ring references are released on Clear()/destruction.
class PersistentVifMemoryProvenance final {
public:
  static std::shared_ptr<PersistentVifMemoryProvenance> Create();

  PersistentVifMemoryProvenance(const PersistentVifMemoryProvenance&) = delete;
  PersistentVifMemoryProvenance& operator=(
      const PersistentVifMemoryProvenance&) = delete;
  ~PersistentVifMemoryProvenance();

  void Clear();
  bool ApplyDirectAffineSpan(const VifUnpackSpan& span);
  void InvalidateQword(u16 qword_address);

  // Before a non-VIF writer takes ownership of any lanes, preserve the exact
  // prior value of the whole qword in the CPU-private image.  Resolution is
  // validated before either the image or the ownership map is changed.
  bool MaterializeAndInvalidateQword(void* vu_memory, u32 vu_memory_size,
                                     u16 qword_address);

  // A generated transaction which replaces all four lanes can relinquish the
  // prior immutable VIF owner without copying its obsolete value into the
  // CPU-private image. The mask is prevalidated and applied without heap work.
  bool InvalidateFullyOverwrittenQwords(const u32* qword_masks,
                                        u32 mask_word_count);

  // Resolves one exact latest qword without copying the private VU image.
  // The pointer remains valid while this provenance object retains its input
  // generation.
  const u8* ResolveQword(u16 qword_address) const;

  // Publishes all still-raw latest qwords into an exact CPU image at an
  // architectural observer. Unowned qwords are left unchanged.
  bool MaterializeOwnedQwords(void* vu_memory, u32 vu_memory_size) const;

  // Resolves one affine or rectangular memory-input domain to its exact raw
  // payload.  Success requires one still-complete owning span; no value
  // comparison can manufacture ownership.
  bool BindGridRawQwords(u16 first_qword_address,
                         s32 outer_invocation_coefficient,
                         u32 outer_invocation_count,
                         s32 child_invocation_coefficient,
                         u32 child_invocation_count,
                         RawVifPayloadRef* payload,
                         RawQwordBinding* binding) const;

  u32 ActiveSpanCount() const;
  u32 OwnedQwordCount() const;

private:
  PersistentVifMemoryProvenance();

  struct Impl;
  std::unique_ptr<Impl> m_impl;
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

// Rectangular counterpart used by the hardware-driven nested-loop kernel.
// One GXP invocation addresses base + outer*outer_coefficient +
// child*child_coefficient. The complete rectangle must be present in one
// immutable V4-32 source span before the GPU can own the epoch.
bool BindGridRawQwords(const VifUnpackSpan& span,
                      u16 first_qword_address,
                      s32 outer_invocation_coefficient,
                      u32 outer_invocation_count,
                      s32 child_invocation_coefficient,
                      u32 child_invocation_count,
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
  u64 derived_captures = 0;
  u64 derived_bytes = 0;
  u64 publication_batches = 0;
  u64 published_bytes = 0;
  u64 fetch_guard_scans = 0;
  u64 fetch_guard_failures = 0;
  u64 logical_high_water_bytes = 0;
  u64 capture_bypasses = 0;
  u64 capture_bypass_bytes = 0;
  u64 capture_fallbacks = 0;
  u64 slot_reuses = 0;
  u64 ring_waits = 0;
  u64 ring_wait_spins = 0;
  u64 live_references = 0;
  u64 peak_live_references = 0;
  u64 retain_generation_rollbacks = 0;
  u64 retain_range_rollbacks = 0;
  u64 retain_overflow_rejections = 0;
  u64 release_generation_mismatches = 0;
  u64 release_underflow_rejections = 0;
  u64 generation_exhaustions = 0;
  u64 deferred_unpacks = 0;
  u64 affine_span_merges = 0;
  u64 replayed_unpacks = 0;
};

// Between two and four independently mapped LPDDR slots follow the multi-frame
// ownership pattern used by Sony samples and PhyreEngine. Each kernel block is
// 2 MiB, below the Vita's 16 MiB per-block ceiling. The owner keeps every slot
// successfully mapped after the required double-buffer pair instead of
// disabling GPU input ownership when a later optional allocation meets
// fragmented memory. Requiring two slots prevents one generation from
// deadlocking its own outstanding asynchronous references at wraparound.
// CPU0 raw-VIF capture and CPU1 generated sparse-input capture share this one
// bounded arena. Their reservations are serialized inside the owner; immutable
// generation references retain the bytes until asynchronous GPU retirement.
// Generation and physical reference count are changed as one non-wrapping
// ownership state: every successful capture/retain must transfer or release
// exactly one reference, and a zero-reference state cannot be resurrected
// without advancing its generation.
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

  bool CapturePayload(const void* source, u32 size,
                      RawVifCaptureMode mode,
                      RawVifPayloadRef* payload);
  bool PublishRawVifPayloads(const RawVifPayloadRef* payloads,
                             u32 payload_count);

  friend bool CaptureRawVifPayload(const void*, u32, RawVifCaptureMode,
                                   RawVifPayloadRef*);
  friend bool CaptureDerivedGpuVuPayload(const void*, u32,
                                         RawVifPayloadRef*);
  friend const u8* ResolveRawVifPayload(const RawVifPayloadRef&,
                                        RawVifPayloadResolveFailure*,
                                        RawVifPayloadResolveDiagnostics*);
  friend const u8* ResolveGpuRawVifPayload(const RawVifPayloadRef&,
                                           RawVifPayloadResolveFailure*,
                                           RawVifPayloadResolveDiagnostics*);
  friend const u8* ResolveRawVifPayloadInternal(
      const RawVifPayloadRef&, bool, RawVifPayloadResolveFailure*,
      RawVifPayloadResolveDiagnostics*);
  friend bool PublishPendingRawVifPayloads(const GpuVuDraw*, u32);
  friend bool PublishPendingRawVifPayloads(const VifUnpackSpan*, u32);
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
// Copies one CPU-resolved sparse generated-program input table directly into
// the GPU-coherent immutable ring used by raw VIF capture. CPU0 and CPU1 are
// distinct producers, so InputRing serializes only reservation/publication;
// payload ownership remains generation based and asynchronous. Keeping the
// sparse table beside the raw bytes lets one BUFFER0 window retain raw spans
// instead of rebuilding an all-compact VU-memory table. This is input packing
// only; it performs no VU arithmetic.
bool CaptureDerivedGpuVuPayload(const void* source, u32 size,
                                RawVifPayloadRef* payload);
// True only while the one shared raw/sparse arena is available.
bool DerivedGpuVuPayloadSharesRawInputArena();
// Proves the current generated-shader ABI can bind every raw stream in one
// mapped slot-generation and keep its dynamically indexed qword offsets in
// the positive signed-16 address range emitted by psp2cgc.
bool HasSingleAddressableRawInputWindow(
    const RawVifPayloadRef* payloads, size_t payload_count,
    const StreamBinding* bindings, size_t binding_count);
bool HasSingleAddressableRawInputWindow(const GpuVuDraw& draw);
// CPU-side analysis and replay read the same immutable cacheable mapping SGX
// consumes. USER_RW mappings are CPU/GPU coherent under libGXM.
const u8* ResolveRawVifPayload(
    const RawVifPayloadRef& payload,
    RawVifPayloadResolveFailure* failure = nullptr,
    RawVifPayloadResolveDiagnostics* diagnostics = nullptr);
// The GS owner may bind only bytes whose direct-capture range crossed the
// ordered direct-run publication boundary.
const u8* ResolveGpuRawVifPayload(
    const RawVifPayloadRef& payload,
    RawVifPayloadResolveFailure* failure = nullptr,
    RawVifPayloadResolveDiagnostics* diagnostics = nullptr);
bool PublishPendingRawVifPayloads(const GpuVuDraw* first_draw,
                                 u32 draw_count);
// Publishes the immutable generations referenced by one canonical universal
// epoch. This is the same release boundary as the generated direct-run
// overload; it performs no payload copy and does not transfer ownership.
bool PublishPendingRawVifPayloads(const VifUnpackSpan* spans,
                                 u32 span_count);
bool RetainRawVifPayload(const RawVifPayloadRef& payload);
void ReleaseRawVifPayload(RawVifPayloadRef* payload);
u32 GetRawVifPayloadGenerationReferenceCount(
    const RawVifPayloadRef& payload);

void RecordDeferredVifUnpack();
void RecordReplayedVifUnpack();
void RecordCaptureBypass(u32 size);
InputRingStatistics GetInputRingStatistics();

} // namespace VitaGpuVu
