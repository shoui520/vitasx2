// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuMicroProgram.h"

#include "vita/VitaGpuVuArchitectureLedger.h"

#include "Config.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "VUmicroFast.h"
#include "VUops.h"
#include "Vif.h"
#include "common/Threading.h"
#include "vita/VitaVuBlockCompiler.h"

#if !defined(VITASX2_QEMU_VALIDATION)
#include "common/Console.h"

#include <psp2/kernel/sysmem.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

extern void _vuFlushAll(VURegs* vu);
extern void _vuXGKICKTransferActiveProvider(s32 cycles, bool flush);

namespace VitaGpuVu {
namespace {

constexpr u32 UpperKindShift = 0;
constexpr u32 LowerKindShift = 7;
constexpr u32 KindMask = 0x7fu;
constexpr u32 ValidBit = 1u << 14;
constexpr u32 ExecuteUpperBit = 1u << 15;
constexpr u32 ExecuteLowerBit = 1u << 16;
constexpr u32 ImmediateLowerBit = 1u << 17;
constexpr u32 Ebit = 1u << 18;
constexpr u32 Mbit = 1u << 19;
constexpr u32 Dbit = 1u << 20;
constexpr u32 Tbit = 1u << 21;
constexpr u32 ClipSnapshotBit = 1u << 22;
constexpr u32 LowerDiscardedBit = 1u << 23;
constexpr u32 VfSnapshotShift = 24;
constexpr u32 VfSnapshotMask = 0x1fu;
constexpr u32 InstantQpProducerBit = 1u << 29;
constexpr u32 InstantQpWaitBit = 1u << 30;
constexpr u32 ViBackupWriteBit = 1u << 31;

constexpr u32 MaximumCachedPrograms = 8;
constexpr u32 MaximumPooledPrograms = 16;
static_assert(MaximumPooledPrograms > MaximumCachedPrograms);

// The validation GXP decodes these numeric values directly. Keep this list at
// the CPU/GPU format boundary so an edit to VUInterpFast fails the host build
// instead of silently changing shader semantics.
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ABS) == 2);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::FTOI0) == 3);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::FTOI4) == 4);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::FTOI12) == 5);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::FTOI15) == 6);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ITOF0) == 7);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ITOF4) == 8);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ITOF12) == 9);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ITOF15) == 10);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ADD) == 11);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ADDw) == 17);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::SUB) == 25);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::SUBw) == 31);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MAX) == 39);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MAXw) == 44);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MINI) == 45);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MINIw) == 50);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::CLIP) == 51);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MUL) == 52);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MULw) == 58);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MADD) == 66);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MADDw) == 72);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ADDA) == 18);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::ADDAw) == 24);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::SUBA) == 32);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::SUBAw) == 38);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MULA) == 59);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MULAw) == 65);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MADDA) == 73);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MADDAw) == 79);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MSUB) == 80);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MSUBw) == 86);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MSUBA) == 87);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::MSUBAw) == 93);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::OPMULA) == 94);
static_assert(static_cast<u32>(VUInterpFast::UpperFastKind::OPMSUB) == 95);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::LQ) == 1);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::SQ) == 2);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::IADDIU) == 5);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::ISUBIU) == 6);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::IADD) == 19);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::IOR) == 23);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::LQI) == 24);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::SQD) == 27);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::MOVE) == 30);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::MR32) == 31);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::IBEQ) == 38);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::JALR) == 47);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::MFP) == 48);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::WAITQ) == 49);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::WAITP) == 50);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::DIV) == 53);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::SQRT) == 54);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::RSQRT) == 55);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::XGKICK) == 56);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::ESADD) == 57);
static_assert(static_cast<u32>(VUInterpFast::LowerFastKind::EEXP) == 69);
static_assert(VUInterpFast::UpperFastKindCount <= 128);
static_assert(VUInterpFast::LowerFastKindCount <= 128);

u32 CurrentConfigurationBits() {
  u32 result = 0;
  if (EmuConfig.Speedhacks.vu1AssumeScheduled)
    result |= UniversalConfigurationAssumeScheduled;
  if (EmuConfig.Speedhacks.vu1InstantQP)
    result |= UniversalConfigurationInstantQp;
  if (EmuConfig.Speedhacks.vuFlagHack)
    result |= UniversalConfigurationFlagHack;
  if (EmuConfig.Speedhacks.vu1Instant)
    result |= UniversalConfigurationVu1Instant;
  if (EmuConfig.Cpu.Recompiler.vu1Overflow)
    result |= UniversalConfigurationOverflowClamp;
  if (EmuConfig.Cpu.Recompiler.vu1ExtraOverflow)
    result |= UniversalConfigurationExtraOverflowClamp;
  if (EmuConfig.Cpu.Recompiler.vu1SignOverflow)
    result |= UniversalConfigurationSignOverflowClamp;
  if (EmuConfig.Cpu.Recompiler.vu1Underflow)
    result |= UniversalConfigurationUnderflowClamp;
  result |= (static_cast<u32>(EmuConfig.Cpu.VU1FPCR.GetRoundMode()) & 3u)
            << UniversalConfigurationRoundModeShift;
  if (EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero())
    result |= UniversalConfigurationDenormalsAreZero;
  if (EmuConfig.Cpu.VU1FPCR.GetFlushToZero())
    result |= UniversalConfigurationFlushToZero;
  if (EmuConfig.Speedhacks.vu1ApproximateQ)
    result |= UniversalConfigurationApproximateQ;
  if (EmuConfig.Speedhacks.vu1ApproximateP)
    result |= UniversalConfigurationApproximateP;
  if (EmuConfig.Speedhacks.vu1ApproximateFmac)
    result |= UniversalConfigurationApproximateFmac;
  if (EmuConfig.Speedhacks.vu1ApproximateConversions)
    result |= UniversalConfigurationApproximateConversions;
  return result;
}

bool Fail(std::string* error, const char* message) {
  if (error)
    *error = message;
  return false;
}

u32 EncodeControl(const VitaVU::GpuPairPlan& plan) {
  u32 control =
      (static_cast<u32>(plan.upper_kind) & KindMask) << UpperKindShift;
  control |=
      (static_cast<u32>(plan.lower_kind) & KindMask) << LowerKindShift;
  control |= ValidBit;
  if (plan.exec_upper)
    control |= ExecuteUpperBit;
  if (plan.exec_lower)
    control |= ExecuteLowerBit;
  if (plan.immediate_lower)
    control |= ImmediateLowerBit;
  if (plan.ebit)
    control |= Ebit;
  if (plan.mflag)
    control |= Mbit;
  if (plan.dflag)
    control |= Dbit;
  if (plan.tflag)
    control |= Tbit;
  if (plan.clip_snapshot)
    control |= ClipSnapshotBit;
  if (plan.lower_discarded_by_upper)
    control |= LowerDiscardedBit;
  control |= (static_cast<u32>(plan.vf_snapshot_reg) & VfSnapshotMask)
             << VfSnapshotShift;
  if (plan.instant_qp_producer)
    control |= InstantQpProducerBit;
  if (plan.instant_qp_wait)
    control |= InstantQpWaitBit;
  if (plan.vi_backup_write)
    control |= ViBackupWriteBit;
  return control;
}

u32 ReadWord(const u8* source) {
  u32 result = 0;
  std::memcpy(&result, source, sizeof(result));
  return result;
}

struct CachedProgram {
  std::atomic<u32> references{0};
  u64 identity = 0;
  UniversalMicroProgram program;
};

// A complete serialized PairPlan image is roughly 100 KiB. Allocating one
// from Vita's general C++ heap whenever a new start PC misses the eight-entry
// cache fragmented the process during concurrent Shacc/GXM work and let
// std::bad_alloc escape the MTVU thread. Keep a bounded, title-neutral pool
// instead. Cache ownership and in-flight handles pin entries through the
// existing atomic reference count; pool exhaustion is a pre-effect CPU
// fallback, never a process exception. Every serialized record already owns
// the exact lower and upper source words, so retaining a second 16 KiB source
// image in each entry would only duplicate identity bytes.
#if defined(VITASX2_QEMU_VALIDATION)
std::array<CachedProgram, MaximumPooledPrograms> s_program_pool_storage{};

CachedProgram* GetProgramPool() {
  return s_program_pool_storage.data();
}
#else
constexpr u32 ProgramPoolBackingBytes = 2 * 1024 * 1024;
static_assert(sizeof(CachedProgram) * MaximumPooledPrograms <=
              ProgramPoolBackingBytes);

Threading::KernelMutex s_program_pool_mutex;
std::atomic<CachedProgram*> s_program_pool{nullptr};
SceUID s_program_pool_uid = -1;
bool s_program_pool_failed = false;

CachedProgram* GetProgramPool() {
  if (CachedProgram* const ready =
          s_program_pool.load(std::memory_order_acquire)) {
    return ready;
  }

  std::lock_guard lock(s_program_pool_mutex);
  if (CachedProgram* const ready =
          s_program_pool.load(std::memory_order_relaxed)) {
    return ready;
  }
  if (s_program_pool_failed)
    return nullptr;

  // Sony's Memory Management runtime contract treats USER_MAIN_PHYCONT_RW as
  // ordinary CPU-accessible LPDDR from a separate bounded pool. ShaccCg owns
  // 16 MiB of the 26 MiB process allowance; this permanent 2 MiB semantic
  // cache leaves the mapped USER_RW input generations out of executable BSS
  // and cannot fragment newlib's already-full heap.
  const SceUID uid = sceKernelAllocMemBlock(
      "VitaSX2 PairPlan pool",
      SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW,
      ProgramPoolBackingBytes, nullptr);
  if (uid < 0) {
    s_program_pool_failed = true;
    Console.Warning(
        "GPU-VU: PairPlan PHYCONT pool allocation failed (%08x); "
        "retaining pre-effect CPU MTVU fallback.",
        static_cast<u32>(uid));
    return nullptr;
  }

  void* base = nullptr;
  const int base_result = sceKernelGetMemBlockBase(uid, &base);
  if (base_result < 0 || !base) {
    sceKernelFreeMemBlock(uid);
    s_program_pool_failed = true;
    Console.Warning(
        "GPU-VU: PairPlan PHYCONT pool base lookup failed (%08x); "
        "retaining pre-effect CPU MTVU fallback.",
        static_cast<u32>(base_result));
    return nullptr;
  }

  auto* const pool = static_cast<CachedProgram*>(base);
  for (u32 index = 0; index < MaximumPooledPrograms; index++)
    ::new (static_cast<void*>(pool + index)) CachedProgram{};
  s_program_pool_uid = uid;
  s_program_pool.store(pool, std::memory_order_release);
  Console.WriteLn(
      "GPU-VU: bounded PairPlan cache ready in PHYCONT "
      "(entries=%u bytes=%u uid=%08x).",
      MaximumPooledPrograms, ProgramPoolBackingBytes,
      static_cast<u32>(s_program_pool_uid));
  return pool;
}
#endif

CachedProgram* AcquireProgramStorage() {
  CachedProgram* const pool = GetProgramPool();
  if (!pool)
    return nullptr;
  for (u32 index = 0; index < MaximumPooledPrograms; index++) {
    CachedProgram& entry = pool[index];
    u32 expected = 0u;
    if (entry.references.compare_exchange_strong(
            expected, 1u, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      return &entry;
    }
  }
  return nullptr;
}

void Retain(CachedProgram* entry) {
  if (entry)
    entry->references.fetch_add(1, std::memory_order_relaxed);
}

void Release(CachedProgram* entry) {
  if (entry)
    entry->references.fetch_sub(1, std::memory_order_acq_rel);
}

struct CacheSlot {
  CachedProgram* entry = nullptr;
  u64 last_use = 0;
};

Threading::KernelMutex s_cache_mutex;
std::array<CacheSlot, MaximumCachedPrograms> s_cache{};
u64 s_cache_clock = 0;
std::atomic<u64> s_next_cache_identity{1};
std::atomic<u64> s_requests{0};
std::atomic<u64> s_hits{0};
std::atomic<u64> s_encodes{0};
std::atomic<u64> s_evictions{0};
std::atomic<u64> s_rejected_inputs{0};

bool Matches(const CachedProgram& entry, const u8* micro, u32 start_pc,
             u32 configuration_bits) {
  if (entry.program.start_pc != start_pc ||
      entry.program.configuration_bits != configuration_bits) {
    return false;
  }
  for (u32 index = 0; index < UniversalMicroProgramPairCount; index++) {
    const UniversalPairMicroOp& pair = entry.program.pairs[index];
    const u8* const source = micro + index * UniversalMicroProgramPairBytes;
    if (pair.lower != ReadWord(source) ||
        pair.upper != ReadWord(source + sizeof(u32))) {
      return false;
    }
  }
  return true;
}

CachedProgram* FindLocked(const u8* micro, u32 start_pc,
                          u32 configuration_bits) {
  for (CacheSlot& slot : s_cache) {
    if (!slot.entry ||
        !Matches(*slot.entry, micro, start_pc, configuration_bits)) {
      continue;
    }
    slot.last_use = ++s_cache_clock;
    Retain(slot.entry);
    return slot.entry;
  }
  return nullptr;
}

struct DecodedControl {
  u8 upper_kind = 0;
  u8 lower_kind = 0;
  u8 vf_snapshot_reg = 0;
  bool valid = false;
  bool exec_upper = false;
  bool exec_lower = false;
  bool immediate_lower = false;
  bool ebit = false;
  bool dflag = false;
  bool tflag = false;
  bool clip_snapshot = false;
  bool lower_discarded = false;
  bool vi_backup_write = false;
};

DecodedControl DecodeControl(u32 control) {
  DecodedControl result;
  result.upper_kind =
      static_cast<u8>((control >> UpperKindShift) & KindMask);
  result.lower_kind =
      static_cast<u8>((control >> LowerKindShift) & KindMask);
  result.vf_snapshot_reg =
      static_cast<u8>((control >> VfSnapshotShift) & VfSnapshotMask);
  result.valid = (control & ValidBit) != 0;
  result.exec_upper = (control & ExecuteUpperBit) != 0;
  result.exec_lower = (control & ExecuteLowerBit) != 0;
  result.immediate_lower = (control & ImmediateLowerBit) != 0;
  result.ebit = (control & Ebit) != 0;
  result.dflag = (control & Dbit) != 0;
  result.tflag = (control & Tbit) != 0;
  result.clip_snapshot = (control & ClipSnapshotBit) != 0;
  result.lower_discarded = (control & LowerDiscardedBit) != 0;
  result.vi_backup_write = (control & ViBackupWriteBit) != 0;
  return result;
}

bool AnalyzeExecutionRegisters(const UniversalPairMicroOp& op,
                               const DecodedControl& control,
                               _VURegsNum* upper, _VURegsNum* lower) {
  if (!upper || !lower)
    return false;

  const auto decode = [](_VURegsNum* output, u32 vi_read, u32 vi_write,
                         u32 access0, u32 access1, bool upper) {
    *output = {};
    output->VIread = vi_read;
    output->VIwrite = vi_write;
    output->VFwrite = static_cast<u8>(access0);
    output->VFwxyzw = static_cast<u8>(access0 >> 8);
    output->VFread0 = static_cast<u8>(access0 >> 16);
    output->VFr0xyzw = static_cast<u8>(access0 >> 24);
    output->VFread1 = static_cast<u8>(access1);
    output->VFr1xyzw = static_cast<u8>(access1 >> 8);
    const u8 pipe_and_backup = static_cast<u8>(access1 >> 16);
    output->pipe = pipe_and_backup & 7u;
    output->cycles = static_cast<s8>(access1 >> 24);
    // PairPlan uses slot 32 for ACC dependency ownership even though the
    // architectural VF file ends at 31.  The fixed executor carries ACC as
    // its 33rd vector state slot for the same reason.
    // PCSX2 retains 0xff in an unused lane-mask byte.  It is not an invalid
    // architectural mask: the corresponding read register is zero and the
    // stall helpers ignore that slot. Preserve the byte instead of
    // normalizing it, because the serialized record is meant to reproduce
    // _VURegsNum exactly.
    return (!upper || (pipe_and_backup & 0xf8u) == 0) &&
           output->VFwrite <= 32 && output->VFread0 <= 32 &&
           output->VFread1 <= 32 && output->pipe <= VUPIPE_XGKICK;
  };

  if (!decode(upper, op.upper_vi_read, op.upper_vi_write,
              op.upper_vf_access0, op.upper_vf_access1, true) ||
      !decode(lower, op.lower_vi_read, op.lower_vi_write,
              op.lower_vf_access0, op.lower_vf_access1, false)) {
    return false;
  }

  const u8 vi_backup_reg =
      static_cast<u8>((op.lower_vf_access1 >> 19) & 31u);
  if (!control.vi_backup_write && vi_backup_reg != 0)
    return false;

  // Empty instruction sides must carry an empty dependency record.  This
  // catches stale/corrupt cache entries before any reference or GPU state is
  // mutated while still retaining the discarded-lower pipeline contract.
  const auto empty = [](const _VURegsNum& regs) {
    return regs.pipe == 0 && regs.VFwrite == 0 && regs.VFwxyzw == 0 &&
           regs.VFr0xyzw == 0 && regs.VFr1xyzw == 0 && regs.VFread0 == 0 &&
           regs.VFread1 == 0 && regs.VIwrite == 0 && regs.VIread == 0 &&
           regs.cycles == 0;
  };
  if (!control.exec_upper && !empty(*upper))
    return false;
  if (!control.exec_lower && !control.lower_discarded && !empty(*lower))
    return false;
  return true;
}

u32 PackVfAccess0(const VitaVU::GpuPairPlan& plan, bool upper) {
  const u32 write = upper ? plan.upper_vf_write : plan.lower_vf_write;
  const u32 write_mask =
      upper ? plan.upper_vf_write_mask : plan.lower_vf_write_mask;
  const u32 read0 = upper ? plan.upper_vf_read0 : plan.lower_vf_read0;
  const u32 read0_mask =
      upper ? plan.upper_vf_read0_mask : plan.lower_vf_read0_mask;
  return write | (write_mask << 8) | (read0 << 16) | (read0_mask << 24);
}

bool PackVfAccess1(const VitaVU::GpuPairPlan& plan, bool upper,
                   u32* output) {
  if (!output)
    return false;
  const u32 read1 = upper ? plan.upper_vf_read1 : plan.lower_vf_read1;
  const u32 read1_mask =
      upper ? plan.upper_vf_read1_mask : plan.lower_vf_read1_mask;
  const s32 cycles = upper ? plan.upper_cycles : plan.lower_cycles;
  // VUPipeState is a byte-sized semantic tag in the source structure.  Keep
  // the explicit range check here so a future PCSX2 pipeline extension cannot
  // silently truncate the fixed GPU ABI.
  if (cycles < std::numeric_limits<s8>::min() ||
      cycles > std::numeric_limits<s8>::max()) {
    return false;
  }
  u32 pipe = upper ? plan.upper_pipe : plan.lower_pipe;
  if (pipe > VUPIPE_XGKICK)
    return false;
  if (!upper) {
    if (plan.vi_backup_reg > 15 ||
        (!plan.vi_backup_write && plan.vi_backup_reg != 0))
      return false;
    pipe |= static_cast<u32>(plan.vi_backup_reg) << 3;
  }
  *output = read1 | (read1_mask << 8) |
            (pipe << 16) |
            (static_cast<u32>(static_cast<u8>(cycles)) << 24);
  return true;
}

void FinishReferenceProgram(VURegs* vu, bool publish_external_effects) {
  // PCSX2 owner: VU1microInterp.cpp::_vu1FinishProgram(). This reference path
  // is not a product executor; it deliberately uses the same canonical pipe
  // owners so a serialized-program differential includes the terminal state.
  vu->VIBackupCycles = 0;
  _vuFlushAll(vu);
  if (publish_external_effects && !THREAD_VU1) {
    VU0.VI[REG_VPU_STAT].UL &= ~0x100u;
    vif1Regs.stat.VEW = false;
  }
  if (publish_external_effects && vu->xgkickenable)
    _vuXGKICKTransferActiveProvider(0, true);
  if (publish_external_effects && INSTANT_VU1)
    vu->xgkicklastcycle = cpuRegs.cycle;
  if (publish_external_effects && THREAD_VU1)
    vu1Thread.EndProgram(VU_Thread::InterruptFlagVUEBit);
}

}  // namespace

u32 GetCurrentUniversalMicroProgramConfigurationBits() {
  return CurrentConfigurationBits();
}

bool EncodeUniversalPairMicroOp(const VitaVU::GpuPairPlan& plan,
                                UniversalPairMicroOp* encoded,
                                std::string* error) {
  if (!encoded)
    return Fail(error, "null universal GPU-VU pair output");
  if (plan.upper_kind > KindMask || plan.lower_kind > KindMask)
    return Fail(error, "PairPlan kind does not fit universal GPU-VU format");

  u32 upper_access1 = 0;
  u32 lower_access1 = 0;
  if (!PackVfAccess1(plan, true, &upper_access1) ||
      !PackVfAccess1(plan, false, &lower_access1)) {
    return Fail(error,
                "PairPlan dependency metadata does not fit universal "
                "GPU-VU format");
  }

  *encoded = {};
  encoded->upper = plan.upper;
  encoded->lower = plan.lower;
  encoded->control = EncodeControl(plan);
  encoded->upper_vi_read = plan.upper_vi_read;
  encoded->upper_vi_write = plan.upper_vi_write;
  encoded->lower_vi_read = plan.lower_vi_read;
  encoded->lower_vi_write = plan.lower_vi_write;
  encoded->upper_vf_access0 = PackVfAccess0(plan, true);
  encoded->upper_vf_access1 = upper_access1;
  encoded->lower_vf_access0 = PackVfAccess0(plan, false);
  encoded->lower_vf_access1 = lower_access1;
  if (error)
    error->clear();
  return true;
}

bool EncodeUniversalMicroProgram(const u8* micro, u32 micro_size,
                                 u32 start_pc,
                                 UniversalMicroProgram* program,
                                 std::string* error) {
  return EncodeUniversalMicroProgramForConfiguration(
      micro, micro_size, start_pc, CurrentConfigurationBits(), program,
      error);
}

bool EncodeUniversalMicroProgramForConfiguration(
    const u8* micro, u32 micro_size, u32 start_pc,
    u32 configuration_bits, UniversalMicroProgram* program,
    std::string* error) {
  if (!micro || !program)
    return Fail(error, "null universal GPU-VU program input");
  if (micro_size != VU1_PROGSIZE)
    return Fail(error, "universal GPU-VU input is not a complete VU1 image");
  if ((start_pc & 7u) != 0 || start_pc > VU1_PROGMASK)
    return Fail(error, "universal GPU-VU start PC is not a VU1 pair address");
  if ((configuration_bits & ~UniversalConfigurationKnownMask) != 0)
    return Fail(error, "unknown universal GPU-VU execution configuration");

  *program = {};
  program->format_version = UniversalMicroProgramFormatVersion;
  program->configuration_bits = configuration_bits;
  program->start_pc = start_pc;
  const bool assume_scheduled =
      (configuration_bits & UniversalConfigurationAssumeScheduled) != 0;
  const bool instant_qp =
      (configuration_bits & UniversalConfigurationInstantQp) != 0;
  for (u32 index = 0; index < program->pairs.size(); index++) {
    const u32 pc = index * UniversalMicroProgramPairBytes;
    UniversalPairMicroOp& encoded = program->pairs[index];
    encoded.lower = ReadWord(micro + pc);
    encoded.upper = ReadWord(micro + pc + sizeof(u32));

    VitaVU::GpuPairPlan plan;
    if (!VitaVU::AnalyzeGpuVu1PairForConfiguration(
            pc, encoded.upper, encoded.lower, assume_scheduled, instant_qp,
            &plan)) {
      encoded.control = 0;
      program->invalid_pair_count++;
      continue;
    }
    if (!EncodeUniversalPairMicroOp(plan, &encoded, error))
      return false;
    program->valid_pair_count++;
  }
  return true;
}

UniversalMicroProgramHandle::UniversalMicroProgramHandle(void* entry)
    : m_entry(entry) {}

UniversalMicroProgramHandle::UniversalMicroProgramHandle(
    UniversalMicroProgramHandle&& other) noexcept
    : m_entry(std::exchange(other.m_entry, nullptr)) {}

UniversalMicroProgramHandle& UniversalMicroProgramHandle::operator=(
    UniversalMicroProgramHandle&& other) noexcept {
  if (this != &other) {
    Release(static_cast<CachedProgram*>(m_entry));
    m_entry = std::exchange(other.m_entry, nullptr);
  }
  return *this;
}

UniversalMicroProgramHandle::~UniversalMicroProgramHandle() {
  Release(static_cast<CachedProgram*>(m_entry));
}

const UniversalMicroProgram* UniversalMicroProgramHandle::Get() const {
  const auto* entry = static_cast<const CachedProgram*>(m_entry);
  return entry ? &entry->program : nullptr;
}

u64 UniversalMicroProgramHandle::Identity() const {
  const auto* entry = static_cast<const CachedProgram*>(m_entry);
  return entry ? entry->identity : 0;
}

const UniversalMicroProgram& UniversalMicroProgramHandle::operator*() const {
  return *Get();
}

const UniversalMicroProgram* UniversalMicroProgramHandle::operator->() const {
  return Get();
}

UniversalMicroProgramHandle::operator bool() const {
  return m_entry != nullptr;
}

UniversalMicroProgramHandle PrepareUniversalMicroProgram(
    const u8* micro, u32 micro_size, u32 start_pc, std::string* error) {
  return PrepareUniversalMicroProgramForConfiguration(
      micro, micro_size, start_pc, CurrentConfigurationBits(), error);
}

UniversalMicroProgramHandle PrepareUniversalMicroProgramForConfiguration(
    const u8* micro, u32 micro_size, u32 start_pc, u32 configuration_bits,
    std::string* error) {
  s_requests.fetch_add(1, std::memory_order_relaxed);
  if (!micro || micro_size != VU1_PROGSIZE || (start_pc & 7u) != 0 ||
      start_pc > VU1_PROGMASK ||
      (configuration_bits & ~UniversalConfigurationKnownMask) != 0) {
    s_rejected_inputs.fetch_add(1, std::memory_order_relaxed);
    if (error)
      *error = "invalid universal GPU-VU cache request";
    return {};
  }

  {
    std::lock_guard lock(s_cache_mutex);
    if (CachedProgram* hit =
            FindLocked(micro, start_pc, configuration_bits)) {
      s_hits.fetch_add(1, std::memory_order_relaxed);
      return UniversalMicroProgramHandle(hit);
    }
  }

  CachedProgram* const candidate = AcquireProgramStorage();
  if (!candidate) {
    s_rejected_inputs.fetch_add(1, std::memory_order_relaxed);
    if (error)
      *error = "universal GPU-VU PairPlan pool is full";
    return {};
  }
  candidate->identity =
      s_next_cache_identity.fetch_add(1, std::memory_order_relaxed);
  if (!EncodeUniversalMicroProgramForConfiguration(
          micro, micro_size, start_pc, configuration_bits,
          &candidate->program, error)) {
    s_rejected_inputs.fetch_add(1, std::memory_order_relaxed);
    Release(candidate);
    return {};
  }
  s_encodes.fetch_add(1, std::memory_order_relaxed);

  CachedProgram* displaced = nullptr;
  CachedProgram* result = nullptr;
  {
    std::lock_guard lock(s_cache_mutex);
    if (CachedProgram* hit =
            FindLocked(micro, start_pc, configuration_bits)) {
      s_hits.fetch_add(1, std::memory_order_relaxed);
      result = hit;
      Release(candidate);
    } else {
      CacheSlot* target = nullptr;
      for (CacheSlot& slot : s_cache) {
        if (!slot.entry) {
          target = &slot;
          break;
        }
        if (!target || slot.last_use < target->last_use)
          target = &slot;
      }
      displaced = target->entry;
      target->entry = candidate;
      target->last_use = ++s_cache_clock;
      result = target->entry;
      Retain(result);
      if (displaced)
        s_evictions.fetch_add(1, std::memory_order_relaxed);
    }
  }
  Release(displaced);
  return UniversalMicroProgramHandle(result);
}

void ClearUniversalMicroProgramCache() {
  std::array<CachedProgram*, MaximumCachedPrograms> retired{};
  {
    std::lock_guard lock(s_cache_mutex);
    for (u32 index = 0; index < s_cache.size(); index++) {
      retired[index] = s_cache[index].entry;
      s_cache[index] = {};
    }
    s_cache_clock = 0;
  }
  for (CachedProgram* entry : retired)
    Release(entry);
}

UniversalMicroProgramCacheStatistics
GetUniversalMicroProgramCacheStatistics() {
  UniversalMicroProgramCacheStatistics result;
  result.requests = s_requests.load(std::memory_order_relaxed);
  result.hits = s_hits.load(std::memory_order_relaxed);
  result.encodes = s_encodes.load(std::memory_order_relaxed);
  result.evictions = s_evictions.load(std::memory_order_relaxed);
  result.rejected_inputs =
      s_rejected_inputs.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(s_cache_mutex);
    for (const CacheSlot& slot : s_cache) {
      if (slot.entry)
        result.resident_programs++;
    }
  }
  result.resident_source_bytes =
      result.resident_programs * static_cast<u32>(VU1_PROGSIZE);
  result.resident_encoded_bytes =
      result.resident_programs * static_cast<u32>(sizeof(UniversalMicroProgram));
  return result;
}

void ResetUniversalMicroProgramCacheStatistics() {
  s_requests.store(0, std::memory_order_relaxed);
  s_hits.store(0, std::memory_order_relaxed);
  s_encodes.store(0, std::memory_order_relaxed);
  s_evictions.store(0, std::memory_order_relaxed);
  s_rejected_inputs.store(0, std::memory_order_relaxed);
}

UniversalFixedPairSupport ClassifyUniversalFixedPairSupport(
    const UniversalPairMicroOp& op, u32 configuration_bits) {
  if ((configuration_bits & ~UniversalConfigurationKnownMask) != 0)
    return UniversalFixedPairSupport::UnsupportedConfiguration;

  // The current GXP arithmetic bodies are attested only for the Vita product
  // numeric mode used by the v14 CPU/GPU differential. Keep this check beside
  // the host classifier so a census cannot count a pair the shader rejects at
  // its global preflight (shader configuration mask 0x0ff0 == 0x0c10).
  constexpr u32 NumericConfigurationMask = 0x0ff0u;
  constexpr u32 FixedGxpNumericConfiguration = 0x0c10u;
  if ((configuration_bits & NumericConfigurationMask) !=
      FixedGxpNumericConfiguration) {
    return UniversalFixedPairSupport::UnsupportedNumericConfiguration;
  }

  const DecodedControl control = DecodeControl(op.control);
  _VURegsNum upper{};
  _VURegsNum lower{};
  if (!control.valid ||
      !AnalyzeExecutionRegisters(op, control, &upper, &lower)) {
    return UniversalFixedPairSupport::InvalidMetadata;
  }

  // Advance on an empty copy to apply the same negative-latency and Q/P pipe
  // metadata validation as the fixed pipeline owner. A single pair cannot
  // overflow a dynamic queue, and the helper never mutates on failure.
  UniversalFixedPipelineState metadata_state;
  if (!AdvanceUniversalFixedPipeline(
          op, configuration_bits, &metadata_state, nullptr)) {
    return UniversalFixedPairSupport::InvalidMetadata;
  }

  const u32 upper_kind = control.upper_kind;
  const bool supported_upper = !control.exec_upper ||
      IsFixedUniversalUpperInstructionBodyImplemented(upper_kind);
  if (!supported_upper)
    return UniversalFixedPairSupport::UnsupportedUpperBody;

  if (control.immediate_lower || !control.exec_lower ||
      control.lower_discarded) {
    return UniversalFixedPairSupport::Supported;
  }

  const u32 lower_kind = control.lower_kind;
  if ((configuration_bits & UniversalConfigurationApproximateQ) != 0 &&
      (lower_kind == 53u || lower_kind == 55u)) {
    return UniversalFixedPairSupport::ApproximateQArithmetic;
  }
  bool supported_lower =
      IsFixedUniversalLowerInstructionBodyImplemented(lower_kind);
  // ERSADD and ESQRT currently have only the explicitly configured Vita
  // approximate-P implementation. The architectural ledger remains honest
  // about exact-mode coverage while this conditional product tier is tested.
  if ((lower_kind ==
           static_cast<u32>(VUInterpFast::LowerFastKind::ERSADD) ||
       lower_kind ==
           static_cast<u32>(VUInterpFast::LowerFastKind::ESQRT)) &&
      (configuration_bits & UniversalConfigurationApproximateP) != 0) {
    supported_lower = true;
  }
  return supported_lower ? UniversalFixedPairSupport::Supported :
      UniversalFixedPairSupport::UnsupportedLowerBody;
}

const char* UniversalFixedPairSupportName(
    UniversalFixedPairSupport support) {
  switch (support) {
  case UniversalFixedPairSupport::Supported:
    return "supported";
  case UniversalFixedPairSupport::UnsupportedConfiguration:
    return "unsupported_configuration";
  case UniversalFixedPairSupport::UnsupportedNumericConfiguration:
    return "unsupported_numeric_configuration";
  case UniversalFixedPairSupport::InvalidMetadata:
    return "invalid_metadata";
  case UniversalFixedPairSupport::UnsupportedUpperBody:
    return "unsupported_upper_body";
  case UniversalFixedPairSupport::UnsupportedLowerBody:
    return "unsupported_lower_body";
  case UniversalFixedPairSupport::ApproximateQArithmetic:
    return "approximate_q_arithmetic";
  case UniversalFixedPairSupport::Count:
    break;
  }
  return "invalid";
}

bool AdvanceUniversalFixedPipeline(
    const UniversalPairMicroOp& op, u32 configuration_bits,
    UniversalFixedPipelineState* state, std::string* error) {
  if (!state)
    return Fail(error, "missing fixed pipeline state");
  if ((configuration_bits & ~UniversalConfigurationKnownMask) != 0)
    return Fail(error, "unknown fixed pipeline configuration");

  UniversalFixedPipelineState candidate = *state;
  UniversalFixedPipelineState* const output = state;
  state = &candidate;

  const DecodedControl control = DecodeControl(op.control);
  _VURegsNum upper{};
  _VURegsNum lower{};
  if (!control.valid ||
      !AnalyzeExecutionRegisters(op, control, &upper, &lower)) {
    return Fail(error, "invalid fixed pipeline metadata");
  }

  const bool assume_scheduled =
      (configuration_bits & UniversalConfigurationAssumeScheduled) != 0;
  const bool instant_qp =
      (configuration_bits & UniversalConfigurationInstantQp) != 0;
  const bool lower_fdiv = lower.pipe == VUPIPE_FDIV;
  const bool lower_efu = lower.pipe == VUPIPE_EFU;
  const bool lower_qp = lower_fdiv || lower_efu;
  const bool fdiv_producer =
      lower_fdiv && (lower.VIwrite & (1u << REG_Q)) != 0;
  const bool efu_producer =
      lower_efu && (lower.VIwrite & (1u << REG_P)) != 0;
  const bool explicit_waitq =
      lower_fdiv &&
      control.lower_kind ==
          static_cast<u8>(VUInterpFast::LowerFastKind::WAITQ);
  const bool explicit_waitp =
      lower_efu &&
      control.lower_kind ==
          static_cast<u8>(VUInterpFast::LowerFastKind::WAITP);
  const bool explicit_qp_wait = explicit_waitq || explicit_waitp;
  if (lower.pipe == VUPIPE_IALU && lower.cycles < 0)
    return Fail(error, "negative fixed IALU latency");
  if (lower_qp && lower.cycles < 0)
    return Fail(error, "negative fixed Q/P latency");
  if (lower_fdiv) {
    if ((lower.VIwrite & ~(1u << REG_Q)) != 0)
      return Fail(error, "invalid fixed FDIV writer metadata");
    if (fdiv_producer) {
      if (lower.cycles == 0)
        return Fail(error, "zero fixed FDIV producer latency");
    } else if (!explicit_waitq || lower.cycles != 0) {
      return Fail(error, "invalid fixed WAITQ metadata");
    }
  }
  if (lower_efu) {
    if ((lower.VIwrite & ~(1u << REG_P)) != 0)
      return Fail(error, "invalid fixed EFU writer metadata");
    if (efu_producer) {
      if (lower.cycles == 0)
        return Fail(error, "zero fixed EFU producer latency");
    } else if (!explicit_waitp || lower.cycles != 0) {
      return Fail(error, "invalid fixed WAITP metadata");
    }
  }

  const u64 cycle_before_pair = state->cycle;
  state->cycle++;

  const auto test_fmac = [&](const _VURegsNum& reader) {
    for (u32 i = 0; i < state->fmac_count; i++) {
      const UniversalFixedPipelineFmacEntry& writer = state->fmac[i];
      const auto overlaps = [](u8 write, u8 write_mask, u8 read,
                               u8 read_mask) {
        return write != 0 && write == read &&
               (write_mask & read_mask) != 0;
      };
      if (overlaps(writer.upper_write, writer.upper_mask,
                   reader.VFread0, reader.VFr0xyzw) ||
          overlaps(writer.upper_write, writer.upper_mask,
                   reader.VFread1, reader.VFr1xyzw) ||
          overlaps(writer.lower_write, writer.lower_mask,
                   reader.VFread0, reader.VFr0xyzw) ||
          overlaps(writer.lower_write, writer.lower_mask,
                   reader.VFread1, reader.VFr1xyzw)) {
        state->cycle = std::max(state->cycle, writer.ready_cycle);
      }
    }
  };

  const auto test_fdiv = [&]() {
    // PCSX2 owner: VUops.cpp::_vuTestFDIVStalls(). Source FMAC hazards are
    // tested first; then a pending Q producer can advance to its ready cycle.
    test_fmac(lower);
    if (state->fdiv_enabled != 0)
      state->cycle = std::max(state->cycle, state->fdiv_ready_cycle);
  };

  const auto test_efu = [&]() {
    // PCSX2 owner: VUops.cpp::_vuTestEFUStalls(). An EFU resource stall
    // decrements the pending latency once, so the following _vuTestPipes()
    // can publish P one cycle earlier than the original producer latency.
    test_fmac(lower);
    if (state->efu_enabled != 0) {
      if (state->efu_ready_cycle != 0)
        state->efu_ready_cycle--;
      state->cycle = std::max(state->cycle, state->efu_ready_cycle);
    }
  };

  if (!assume_scheduled) {
    if (upper.pipe == VUPIPE_FMAC)
      test_fmac(upper);
  }

  if (instant_qp && lower_qp) {
    // PCSX2 owner: VUops.cpp::_vuTestLowerStalls(). Instant Q/P removes only
    // the delayed scalar-result resource; Accurate mode still preserves the
    // producer's FMAC source dependency.
    if (!assume_scheduled)
      test_fmac(lower);
  } else if (!assume_scheduled || explicit_qp_wait) {
    if (lower.pipe == VUPIPE_FMAC) {
      test_fmac(lower);
    } else if (lower_fdiv) {
      test_fdiv();
    } else if (lower_efu) {
      test_efu();
    } else if (lower.pipe == VUPIPE_BRANCH) {
      for (u32 i = 0; i < state->ialu_count; i++) {
        if ((state->ialu[i].write_mask & lower.VIread) != 0)
          state->cycle = std::max(state->cycle,
                                  state->ialu[i].ready_cycle);
      }
    }
  }

  const auto retire_ready = [&](auto& queue, u8* count) {
    while (*count != 0 && queue[0].ready_cycle <= state->cycle) {
      for (u32 i = 1; i < *count; i++)
        queue[i - 1] = queue[i];
      queue[--(*count)] = {};
    }
  };
  retire_ready(state->fmac, &state->fmac_count);
  retire_ready(state->ialu, &state->ialu_count);
  if (state->fdiv_enabled != 0 &&
      (instant_qp || state->fdiv_ready_cycle <= state->cycle)) {
    state->fdiv_enabled = 0;
    state->fdiv_ready_cycle = 0;
  }
  if (state->efu_enabled != 0 &&
      (instant_qp || state->efu_ready_cycle <= state->cycle)) {
    state->efu_enabled = 0;
    state->efu_ready_cycle = 0;
  }

  if (state->vi_backup_cycles != 0) {
    const u64 elapsed = state->cycle - cycle_before_pair;
    state->vi_backup_cycles = static_cast<u8>(
        elapsed >= state->vi_backup_cycles ? 0 :
        state->vi_backup_cycles - elapsed);
  }

  const bool adds_fmac = upper.pipe == VUPIPE_FMAC ||
                         lower.pipe == VUPIPE_FMAC;
  if (adds_fmac) {
    if (state->fmac_count == state->fmac.size())
      return Fail(error, "fixed FMAC pipeline overflow");
    UniversalFixedPipelineFmacEntry& entry =
        state->fmac[state->fmac_count++];
    entry.ready_cycle = state->cycle + 4;
    if (upper.pipe == VUPIPE_FMAC) {
      entry.upper_write = upper.VFwrite;
      entry.upper_mask = upper.VFwxyzw;
    }
    if (lower.pipe == VUPIPE_FMAC) {
      entry.lower_write = lower.VFwrite;
      entry.lower_mask = lower.VFwxyzw;
    }
  }

  if (!assume_scheduled && lower.pipe == VUPIPE_IALU && lower.cycles != 0) {
    if (state->ialu_count == state->ialu.size())
      return Fail(error, "fixed IALU pipeline overflow");
    UniversalFixedPipelineIaluEntry& entry =
        state->ialu[state->ialu_count++];
    entry.ready_cycle = state->cycle + static_cast<u32>(lower.cycles);
    entry.write_mask = lower.VIwrite;
  }

  if (!instant_qp && fdiv_producer) {
    state->fdiv_enabled = 1;
    state->fdiv_ready_cycle = state->cycle + static_cast<u32>(lower.cycles);
  }
  if (!instant_qp && efu_producer) {
    state->efu_enabled = 1;
    state->efu_ready_cycle = state->cycle + static_cast<u32>(lower.cycles);
  }

  if (control.vi_backup_write) {
    state->vi_backup_register =
        static_cast<u8>((op.lower_vf_access1 >> 19) & 31u);
    state->vi_backup_cycles = 2;
  }
  *output = candidate;
  return true;
}

void FinishUniversalFixedPipeline(UniversalFixedPipelineState* state) {
  if (!state)
    return;
  if (state->fdiv_enabled != 0)
    state->cycle = std::max(state->cycle, state->fdiv_ready_cycle);
  if (state->efu_enabled != 0)
    state->cycle = std::max(state->cycle, state->efu_ready_cycle);
  for (u32 i = 0; i < state->fmac_count; i++)
    state->cycle = std::max(state->cycle, state->fmac[i].ready_cycle);
  for (u32 i = 0; i < state->ialu_count; i++)
    state->cycle = std::max(state->cycle, state->ialu[i].ready_cycle);
  state->fmac = {};
  state->ialu = {};
  state->fmac_count = 0;
  state->ialu_count = 0;
  state->fdiv_enabled = 0;
  state->efu_enabled = 0;
  state->fdiv_ready_cycle = 0;
  state->efu_ready_cycle = 0;
  state->vi_backup_cycles = 0;
}

UniversalReferenceStepResult ExecuteUniversalReferenceStep(
    VURegs* vu, const UniversalMicroProgram& program) {
  const u32 fbrst = THREAD_VU1 ? vu1Thread.vuFBRST : VU0.VI[REG_FBRST].UL;
  return ExecuteUniversalReferenceStepWithFbrst(vu, program, fbrst);
}

UniversalReferenceStepResult ExecuteUniversalReferenceStepWithFbrst(
    VURegs* vu, const UniversalMicroProgram& program, u32 fbrst) {
  return ExecuteUniversalReferenceStepWithFbrstAndEvent(
      vu, program, fbrst, nullptr);
}

static UniversalReferenceStepResult ExecuteUniversalReferenceStepImpl(
    VURegs* vu, const UniversalMicroProgram& program, u32 fbrst,
    UniversalReferencePairEvent* event, bool publish_external_effects,
    const u16* private_vif_top = nullptr,
    const u16* private_vif_itop = nullptr) {
  if (event)
    *event = {};
  if (!vu || program.format_version != UniversalMicroProgramFormatVersion ||
      program.configuration_bits != CurrentConfigurationBits()) {
    return UniversalReferenceStepResult::InvalidEncoding;
  }

  const u32 pc = vu->VI[REG_TPC].UL & VU1_PROGMASK;
  const UniversalPairMicroOp& op =
      program.pairs[pc / UniversalMicroProgramPairBytes];
  const DecodedControl control = DecodeControl(op.control);
  if (!control.valid)
    return UniversalReferenceStepResult::InvalidEncoding;

  // D/T publication is a real CPU-visible interrupt observation. A future GPU
  // chain must close and publish before this pair; leave it untouched here so
  // the canonical CPU owner can replay the pair exactly.
  if (control.dflag && (fbrst & 0x400u) != 0)
    return UniversalReferenceStepResult::DbitObserver;
  if (control.tflag && (fbrst & 0x800u) != 0)
    return UniversalReferenceStepResult::TbitObserver;

  _VURegsNum upper_regs{};
  _VURegsNum lower_regs{};
  if (!AnalyzeExecutionRegisters(op, control, &upper_regs, &lower_regs))
    return UniversalReferenceStepResult::InvalidEncoding;

  // PCSX2 owner: VUops.cpp::_vuXGKICK().  Capture the VI source before the
  // pair executes, from the already-decoded PairPlan control rather than a
  // second opcode classifier.  The lower body can flush an older XGKICK, but
  // this event describes only the new packet queued by this pair.
  if (event && vu == &VU1 && control.exec_lower &&
      !control.immediate_lower && !control.lower_discarded &&
      control.lower_kind ==
          static_cast<u32>(VUInterpFast::LowerFastKind::XGKICK)) {
    const u32 source_vi = (op.lower >> 11) & 0x0fu;
    event->xgkick_address =
        (vu->VI[source_vi].US[0] & 0x03ffu) * 16u;
    event->queues_xgkick = true;
  }

  vu->cycle++;
  vu->VI[REG_TPC].UL = pc + UniversalMicroProgramPairBytes;
  if (control.ebit)
    vu->ebit = 2;

  const u32 cycles_before_op = static_cast<u32>(vu->cycle - 1);
  const auto execute_lower = [&](u32 code,
                                 VUInterpFast::LowerFastKind kind) {
    // A scratch VU1 image has no private VIFregisters member. Avoid consulting
    // or mutating live vif1Regs when a mixed-provider continuation executes
    // XTOP/XITOP against its immutable command snapshot.
    if (private_vif_top && private_vif_itop) {
      const u32 destination = VUInterpFast::It(code);
      if (kind == VUInterpFast::LowerFastKind::XTOP) {
        if (destination != 0u)
          vu->VI[destination].US[0] = *private_vif_top;
        return;
      }
      if (kind == VUInterpFast::LowerFastKind::XITOP) {
        if (destination != 0u)
          vu->VI[destination].US[0] = *private_vif_itop;
        return;
      }
    }
    VUInterpFast::ExecuteLowerNoUpperKnownKind(vu, code, kind);
  };
  if (!control.exec_upper) {
    vu->code = op.lower;
    _vuTestLowerStalls(vu, &lower_regs);
    _vuTestPipes(vu);
    if (vu->VIBackupCycles > 0) {
      vu->VIBackupCycles -=
          std::min(static_cast<u8>(vu->cycle - cycles_before_op),
                   vu->VIBackupCycles);
    }
    if (control.exec_lower) {
      execute_lower(
          op.lower,
          static_cast<VUInterpFast::LowerFastKind>(control.lower_kind));
    }
  } else {
    vu->code = op.upper;
    _vuTestUpperStalls(vu, &upper_regs);
    if (control.immediate_lower) {
      _vuTestPipes(vu);
      if (vu->VIBackupCycles > 0) {
        vu->VIBackupCycles -=
            std::min(static_cast<u8>(vu->cycle - cycles_before_op),
                     vu->VIBackupCycles);
      }
      VUInterpFast::ExecuteUpperNoLowerKnownKind(
          vu, op.upper,
          static_cast<VUInterpFast::UpperFastKind>(control.upper_kind));
      vu->VI[REG_I].UL = op.lower;
    } else if (control.lower_kind == 0 && !control.lower_discarded) {
      _vuTestPipes(vu);
      if (vu->VIBackupCycles > 0) {
        vu->VIBackupCycles -=
            std::min(static_cast<u8>(vu->cycle - cycles_before_op),
                     vu->VIBackupCycles);
      }
      VUInterpFast::ExecuteUpperNoLowerKnownKind(
          vu, op.upper,
          static_cast<VUInterpFast::UpperFastKind>(control.upper_kind));
      vu->code = op.lower;
    } else {
      vu->code = op.lower;
      _vuTestLowerStalls(vu, &lower_regs);
      _vuTestPipes(vu);
      if (vu->VIBackupCycles > 0) {
        vu->VIBackupCycles -=
            std::min(static_cast<u8>(vu->cycle - cycles_before_op),
                     vu->VIBackupCycles);
      }

      VECTOR old_vf{};
      VECTOR new_vf{};
      REG_VI old_clip{};
      REG_VI new_clip{};
      if (control.vf_snapshot_reg)
        old_vf = vu->VF[control.vf_snapshot_reg];
      if (control.clip_snapshot)
        old_clip = vu->VI[REG_CLIP_FLAG];

      VUInterpFast::ExecuteUpperNoLowerKnownKind(
          vu, op.upper,
          static_cast<VUInterpFast::UpperFastKind>(control.upper_kind));
      if (!control.lower_discarded) {
        if (control.vf_snapshot_reg) {
          new_vf = vu->VF[control.vf_snapshot_reg];
          vu->VF[control.vf_snapshot_reg] = old_vf;
        }
        if (control.clip_snapshot) {
          new_clip = vu->VI[REG_CLIP_FLAG];
          vu->VI[REG_CLIP_FLAG] = old_clip;
        }
        execute_lower(
            op.lower,
            static_cast<VUInterpFast::LowerFastKind>(control.lower_kind));
        if (control.vf_snapshot_reg)
          vu->VF[control.vf_snapshot_reg] = new_vf;
        if (control.clip_snapshot)
          vu->VI[REG_CLIP_FLAG] = new_clip;
      }
    }
  }

  if (upper_regs.pipe == VUPIPE_FMAC || lower_regs.pipe == VUPIPE_FMAC)
    _vuClearFMAC(vu);
  _vuAddUpperStalls(vu, &upper_regs);
  _vuAddLowerStalls(vu, &lower_regs);

  if (vu->branch > 0 && vu->branch-- == 1) {
    vu->VI[REG_TPC].UL = vu->branchpc;
    if (vu->takedelaybranch) {
      vu->branch = 1;
      vu->branchpc = vu->delaybranchpc;
      vu->takedelaybranch = false;
    }
  }

  bool finished = false;
  if (vu->ebit > 0 && vu->ebit-- == 1) {
    FinishReferenceProgram(vu, publish_external_effects);
    finished = true;
  }

  if (upper_regs.pipe == VUPIPE_FMAC || lower_regs.pipe == VUPIPE_FMAC)
    vu->fmacwritepos = (vu->fmacwritepos + 1) & 3;
  return finished ? UniversalReferenceStepResult::ProgramFinished
                  : UniversalReferenceStepResult::PairCompleted;
}

UniversalReferenceStepResult ExecuteUniversalReferenceStepWithFbrstAndEvent(
    VURegs* vu, const UniversalMicroProgram& program, u32 fbrst,
    UniversalReferencePairEvent* event) {
  return ExecuteUniversalReferenceStepImpl(
      vu, program, fbrst, event, true);
}

UniversalReferenceRunResult ExecuteUniversalReferenceProgram(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs) {
  const u32 fbrst = THREAD_VU1 ? vu1Thread.vuFBRST : VU0.VI[REG_FBRST].UL;
  return ExecuteUniversalReferenceProgramWithFbrst(
      vu, program, maximum_pairs, fbrst);
}

UniversalReferenceRunResult ExecuteUniversalReferenceProgramWithFbrst(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs,
    u32 fbrst) {
  UniversalReferenceRunResult result;
  if (!vu || maximum_pairs == 0) {
    result.stop_pc = vu ? vu->VI[REG_TPC].UL & VU1_PROGMASK : 0;
    return result;
  }

  // PCSX2 owner: InterpVU1::Execute(). Canonical TPC is a pair index outside
  // the provider and a byte address while VU branch/step semantics execute.
  vu->VI[REG_TPC].UL <<= 3;
  for (u32 step = 0; step < maximum_pairs; step++) {
    result.result = ExecuteUniversalReferenceStepWithFbrst(
        vu, program, fbrst);
    if (result.result == UniversalReferenceStepResult::PairCompleted ||
        result.result == UniversalReferenceStepResult::ProgramFinished) {
      result.executed_pairs++;
    }
    if (result.result != UniversalReferenceStepResult::PairCompleted) {
      result.stop_pc = vu->VI[REG_TPC].UL & VU1_PROGMASK;
      vu->VI[REG_TPC].UL >>= 3;
      return result;
    }
  }
  result.stop_pc = vu->VI[REG_TPC].UL & VU1_PROGMASK;
  result.result = UniversalReferenceStepResult::PairLimitReached;
  vu->VI[REG_TPC].UL >>= 3;
  return result;
}

bool BuildUniversalStateLoadFormula(
    const UniversalMicroProgram& program, u32 maximum_pairs,
    UniversalStateLoadFormula* formula, std::string* error) {
  if (formula)
    *formula = {};
  if (error)
    error->clear();
  if (!formula)
    return Fail(error, "state-load formula output is missing");
  if (maximum_pairs == 0u ||
      maximum_pairs > UniversalStateLoadFormulaMaximumOps) {
    return Fail(error, "state-load formula pair bound is invalid");
  }
  if (program.format_version != UniversalMicroProgramFormatVersion ||
      (program.configuration_bits & ~UniversalConfigurationKnownMask) != 0u) {
    return Fail(error, "state-load formula configuration is invalid");
  }

  UniversalStateLoadFormula candidate;
  candidate.configuration_bits = program.configuration_bits;
  candidate.start_pc = program.start_pc & VU1_PROGMASK;
  UniversalFixedPipelineState pipeline;
  const bool assume_scheduled =
      (program.configuration_bits &
       UniversalConfigurationAssumeScheduled) != 0u;
  const auto empty_regs = [](const _VURegsNum& regs) {
    return regs.pipe == VUPIPE_NONE && regs.VFwrite == 0u &&
        regs.VFwxyzw == 0u && regs.VFread0 == 0u &&
        regs.VFr0xyzw == 0u && regs.VFread1 == 0u &&
        regs.VFr1xyzw == 0u && regs.VIwrite == 0u &&
        regs.VIread == 0u && regs.cycles == 0;
  };

  u32 pc = candidate.start_pc;
  bool terminal_pair_next = false;
  bool complete = false;
  for (u32 pair = 0u; pair < maximum_pairs; pair++) {
    const UniversalPairMicroOp& op =
        program.pairs[pc / UniversalMicroProgramPairBytes];
    const DecodedControl control = DecodeControl(op.control);
    _VURegsNum upper{};
    _VURegsNum lower{};
    if (!control.valid ||
        !AnalyzeExecutionRegisters(op, control, &upper, &lower) ||
        control.immediate_lower || control.dflag || control.tflag ||
        control.clip_snapshot || control.lower_discarded ||
        control.vf_snapshot_reg != 0u || (op.control & Mbit) != 0u ||
        (op.control & (InstantQpProducerBit | InstantQpWaitBit)) != 0u) {
      return Fail(error, "state-load formula encountered unsupported metadata");
    }
    if (control.exec_upper) {
      if (control.upper_kind !=
              static_cast<u8>(VUInterpFast::UpperFastKind::NOP) ||
          !empty_regs(upper)) {
        return Fail(error, "state-load formula encountered an upper effect");
      }
    } else if (control.upper_kind !=
               static_cast<u8>(VUInterpFast::UpperFastKind::None)) {
      return Fail(error, "state-load formula upper metadata is inconsistent");
    }

    UniversalStateLoadFormulaOp formula_op;
    bool has_formula_op = false;
    const auto kind =
        static_cast<VUInterpFast::LowerFastKind>(control.lower_kind);
    bool expected_backup = false;
    u32 expected_backup_reg = 0u;
    if (!control.exec_lower) {
      if (kind != VUInterpFast::LowerFastKind::None)
        return Fail(error, "state-load formula lower metadata is inconsistent");
    } else if (kind == VUInterpFast::LowerFastKind::None) {
      if (!empty_regs(lower))
        return Fail(error, "state-load formula NOP metadata is inconsistent");
    } else if (kind == VUInterpFast::LowerFastKind::IADDIU) {
      const u32 is = VUInterpFast::Is(op.lower);
      const u32 it = VUInterpFast::It(op.lower);
      if (lower.pipe != VUPIPE_IALU || lower.VFwrite != 0u ||
          lower.VFwxyzw != 0u || lower.VFread0 != 0u ||
          lower.VFr0xyzw != 0u || lower.VFread1 != 0u ||
          lower.VFr1xyzw != 0u || lower.VIread != (1u << is) ||
          lower.VIwrite != (1u << it) || lower.cycles != 0) {
        return Fail(error, "state-load formula IADDIU metadata is inconsistent");
      }
      expected_backup = !assume_scheduled && it != 0u;
      expected_backup_reg = it;
      formula_op.kind = UniversalStateLoadFormulaOpKind::Iaddiu;
      formula_op.source_vi = static_cast<u8>(is);
      formula_op.destination = static_cast<u8>(it);
      formula_op.immediate = static_cast<u16>(VUInterpFast::Imm15(op.lower));
      has_formula_op = true;
      if (it != 0u)
        candidate.vi_write_mask |= 1u << it;
    } else if (kind == VUInterpFast::LowerFastKind::LQI) {
      const u32 is = VUInterpFast::Is(op.lower);
      const u32 ft = VUInterpFast::Ft(op.lower);
      const u32 mask = VUInterpFast::XYZW(op.lower);
      if (lower.pipe != VUPIPE_FMAC || lower.VFwrite != ft ||
          lower.VFwxyzw != mask || lower.VFread0 != 0u ||
          lower.VFr0xyzw != 0u || lower.VFread1 != 0u ||
          lower.VFr1xyzw != 0u || lower.VIread != (1u << is) ||
          lower.VIwrite != (1u << is) || lower.cycles != 0) {
        return Fail(error, "state-load formula LQI metadata is inconsistent");
      }
      expected_backup = !assume_scheduled;
      expected_backup_reg = is;
      formula_op.kind = UniversalStateLoadFormulaOpKind::Lqi;
      formula_op.source_vi = static_cast<u8>(is);
      formula_op.destination = static_cast<u8>(ft);
      formula_op.lane_mask = static_cast<u8>(mask);
      formula_op.increment_source = VUInterpFast::Fs(op.lower) != 0u;
      has_formula_op = true;
      if (formula_op.increment_source != 0u && is != 0u)
        candidate.vi_write_mask |= 1u << is;
    } else {
      return Fail(error, "state-load formula encountered an unsupported pair");
    }

    const u32 encoded_backup_reg =
        (op.lower_vf_access1 >> 19) & 31u;
    if (control.vi_backup_write != expected_backup ||
        encoded_backup_reg !=
            (expected_backup ? expected_backup_reg : 0u)) {
      return Fail(error, "state-load formula VI-backup metadata is inconsistent");
    }
    if (!AdvanceUniversalFixedPipeline(
            op, program.configuration_bits, &pipeline, error)) {
      return false;
    }
    if (has_formula_op) {
      if (candidate.operation_count >= candidate.operations.size())
        return Fail(error, "state-load formula operation capacity exceeded");
      candidate.operations[candidate.operation_count++] = formula_op;
    }

    candidate.executed_pairs++;
    pc = (pc + UniversalMicroProgramPairBytes) & VU1_PROGMASK;
    if (terminal_pair_next) {
      if (control.ebit)
        return Fail(error, "state-load formula has a nested terminal bit");
      complete = true;
      break;
    }
    terminal_pair_next = control.ebit;
  }
  if (!complete)
    return Fail(error, "state-load formula has no complete E-bit path");

  FinishUniversalFixedPipeline(&pipeline);
  candidate.final_tpc = pc;
  candidate.cycle_count = static_cast<u32>(pipeline.cycle);
  *formula = candidate;
  return true;
}

bool EvaluateUniversalStateLoadFormula(
    const UniversalStateLoadFormula& formula,
    const std::array<u16, 16>& initial_vi,
    UniversalStateLoadReadMemoryWord read_memory_word, void* memory_user,
    UniversalStateLoadFormulaResult* result, std::string* error) {
  if (result)
    *result = {};
  if (error)
    error->clear();
  if (!result ||
      formula.format_version != UniversalStateLoadFormulaFormatVersion ||
      formula.operation_count > formula.operations.size() ||
      formula.executed_pairs == 0u || formula.cycle_count == 0u ||
      (formula.vi_write_mask & ~0xfffeu) != 0u) {
    return Fail(error, "state-load formula is invalid");
  }

  UniversalStateLoadFormulaResult candidate;
  candidate.vi = initial_vi;
  candidate.vi[0] = 0u;
  for (u32 index = 0u; index < formula.operation_count; index++) {
    const UniversalStateLoadFormulaOp& op = formula.operations[index];
    if (op.source_vi >= candidate.vi.size() || op.destination >= 32u ||
        op.reserved != 0u || (op.lane_mask & ~0x0fu) != 0u) {
      return Fail(error, "state-load formula operation is corrupt");
    }
    switch (op.kind) {
    case UniversalStateLoadFormulaOpKind::Iaddiu:
      if (op.destination >= candidate.vi.size() || op.lane_mask != 0u ||
          op.increment_source != 0u) {
        return Fail(error, "state-load formula IADDIU operation is corrupt");
      }
      if (op.destination != 0u) {
        const s32 value = static_cast<s32>(
            static_cast<s16>(candidate.vi[op.source_vi])) + op.immediate;
        candidate.vi[op.destination] = static_cast<u16>(value);
      }
      break;
    case UniversalStateLoadFormulaOpKind::Lqi: {
      if (op.immediate != 0u)
        return Fail(error, "state-load formula LQI operation is corrupt");
      const u16 qword = candidate.vi[op.source_vi] & 0x3ffu;
      if (op.destination != 0u && op.lane_mask != 0u) {
        if (!read_memory_word)
          return Fail(error, "state-load formula has no memory reader");
        for (u32 lane = 0u; lane < 4u; lane++) {
          const u8 bit = static_cast<u8>(0x8u >> lane);
          if ((op.lane_mask & bit) == 0u)
            continue;
          u32 value = 0u;
          if (!read_memory_word(memory_user, qword,
                                static_cast<u8>(lane), &value)) {
            return Fail(error, "state-load formula read unavailable memory");
          }
          candidate.vf_values[op.destination][lane] = value;
          candidate.vf_lane_masks[op.destination] |= bit;
        }
      }
      if (op.increment_source != 0u && op.source_vi != 0u)
        candidate.vi[op.source_vi]++;
      break;
    }
    default:
      return Fail(error, "state-load formula operation kind is invalid");
    }
    candidate.vi[0] = 0u;
  }
  *result = candidate;
  return true;
}

bool ExecuteUniversalPrivateStateLoadReference(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs,
    u32 fbrst, const u32* unavailable_memory_words,
    u32 unavailable_memory_word_count, UniversalReferenceRunResult* result,
    std::string* error) {
  if (result)
    *result = {};
  if (error)
    error->clear();
  if (!vu || !vu->Mem || vu->idx != 1u)
    return Fail(error, "private state-load bridge requires a VU1 image");
  if (maximum_pairs == 0u || maximum_pairs > 32u)
    return Fail(error, "private state-load bridge pair bound is invalid");
  if (program.format_version != UniversalMicroProgramFormatVersion ||
      program.configuration_bits != CurrentConfigurationBits()) {
    return Fail(error, "private state-load bridge configuration mismatch");
  }
  if (vu->branch != 0u || vu->ebit != 0u || vu->xgkickenable != 0u ||
      vu->fmaccount != 0u || vu->ialucount != 0u ||
      vu->fdiv.enable != 0u || vu->efu.enable != 0u) {
    return Fail(error, "private state-load bridge requires a quiescent boundary");
  }

  const u32 initial_pc =
      (vu->VI[REG_TPC].UL << 3) & VU1_PROGMASK;
  if ((program.start_pc & VU1_PROGMASK) != initial_pc)
    return Fail(error, "private state-load bridge entry mismatch");

  const auto empty_regs = [](const _VURegsNum& regs) {
    return regs.pipe == VUPIPE_NONE && regs.VFwrite == 0u &&
        regs.VFwxyzw == 0u && regs.VFread0 == 0u &&
        regs.VFr0xyzw == 0u && regs.VFread1 == 0u &&
        regs.VFr1xyzw == 0u && regs.VIwrite == 0u &&
        regs.VIread == 0u && regs.cycles == 0;
  };
  const bool assume_scheduled =
      (program.configuration_bits &
       UniversalConfigurationAssumeScheduled) != 0u;
  const auto validate_pair = [&](const UniversalPairMicroOp& op) {
    const DecodedControl control = DecodeControl(op.control);
    _VURegsNum upper{};
    _VURegsNum lower{};
    if (!control.valid ||
        !AnalyzeExecutionRegisters(op, control, &upper, &lower) ||
        control.immediate_lower || control.dflag || control.tflag ||
        control.clip_snapshot || control.lower_discarded ||
        control.vf_snapshot_reg != 0u || (op.control & Mbit) != 0u ||
        (op.control & (InstantQpProducerBit | InstantQpWaitBit)) != 0u) {
      return false;
    }

    if (control.exec_upper) {
      if (control.upper_kind !=
              static_cast<u8>(VUInterpFast::UpperFastKind::NOP) ||
          !empty_regs(upper)) {
        return false;
      }
    } else if (control.upper_kind !=
                   static_cast<u8>(VUInterpFast::UpperFastKind::None)) {
      return false;
    }

    const auto kind =
        static_cast<VUInterpFast::LowerFastKind>(control.lower_kind);
    bool expected_backup = false;
    u32 expected_backup_reg = 0u;
    if (!control.exec_lower) {
      if (kind != VUInterpFast::LowerFastKind::None)
        return false;
    } else if (kind == VUInterpFast::LowerFastKind::None) {
      if (!empty_regs(lower))
        return false;
    } else if (kind == VUInterpFast::LowerFastKind::IADDIU) {
      const u32 is = VUInterpFast::Is(op.lower);
      const u32 it = VUInterpFast::It(op.lower);
      if (lower.pipe != VUPIPE_IALU || lower.VFwrite != 0u ||
          lower.VFwxyzw != 0u || lower.VFread0 != 0u ||
          lower.VFr0xyzw != 0u || lower.VFread1 != 0u ||
          lower.VFr1xyzw != 0u || lower.VIread != (1u << is) ||
          lower.VIwrite != (1u << it) || lower.cycles != 0) {
        return false;
      }
      expected_backup = !assume_scheduled && it != 0u;
      expected_backup_reg = it;
    } else if (kind == VUInterpFast::LowerFastKind::LQI) {
      const u32 is = VUInterpFast::Is(op.lower);
      const u32 ft = VUInterpFast::Ft(op.lower);
      const u32 mask = VUInterpFast::XYZW(op.lower);
      if (lower.pipe != VUPIPE_FMAC || lower.VFwrite != ft ||
          lower.VFwxyzw != mask || lower.VFread0 != 0u ||
          lower.VFr0xyzw != 0u || lower.VFread1 != 0u ||
          lower.VFr1xyzw != 0u || lower.VIread != (1u << is) ||
          lower.VIwrite != (1u << is) || lower.cycles != 0) {
        return false;
      }
      expected_backup = !assume_scheduled;
      expected_backup_reg = is;
    } else {
      return false;
    }

    const u32 encoded_backup_reg =
        (op.lower_vf_access1 >> 19) & 31u;
    return control.vi_backup_write == expected_backup &&
        encoded_backup_reg ==
            (expected_backup ? expected_backup_reg : 0u);
  };

  u32 proven_pairs = 0u;
  u32 pc = initial_pc;
  bool terminal_pair_next = false;
  bool complete = false;
  for (; proven_pairs < maximum_pairs; proven_pairs++) {
    const UniversalPairMicroOp& op =
        program.pairs[pc / UniversalMicroProgramPairBytes];
    const DecodedControl control = DecodeControl(op.control);
    if (!validate_pair(op))
      return Fail(error, "private state-load bridge encountered an unsupported pair");
    if (terminal_pair_next) {
      if (control.ebit)
        return Fail(error, "private state-load bridge has a nested terminal bit");
      proven_pairs++;
      complete = true;
      break;
    }
    terminal_pair_next = control.ebit;
    pc = (pc + UniversalMicroProgramPairBytes) & VU1_PROGMASK;
  }
  if (!complete)
    return Fail(error, "private state-load bridge has no complete E-bit path");

  // The complete structural path is known before this copy begins. Dynamic
  // memory availability can still reject an LQI, so execute against a local
  // register candidate and publish it only after terminal completion. The
  // supported instruction set contains no VU-memory writer.
  VURegs candidate;
  std::memcpy(&candidate, vu, sizeof(candidate));
  candidate.VI[REG_TPC].UL <<= 3;
  UniversalReferenceRunResult execution;
  for (u32 step = 0u; step < proven_pairs; step++) {
    const u32 step_pc = candidate.VI[REG_TPC].UL & VU1_PROGMASK;
    const UniversalPairMicroOp& op =
        program.pairs[step_pc / UniversalMicroProgramPairBytes];
    const DecodedControl control = DecodeControl(op.control);
    if (control.exec_lower &&
        control.lower_kind ==
            static_cast<u8>(VUInterpFast::LowerFastKind::LQI) &&
        VUInterpFast::Ft(op.lower) != 0u) {
      const u32 qword =
          candidate.VI[VUInterpFast::Is(op.lower)].US[0] & 0x3ffu;
      const u32 mask = VUInterpFast::XYZW(op.lower);
      for (u32 lane = 0u; lane < 4u; lane++) {
        if ((mask & (0x8u >> lane)) == 0u)
          continue;
        const u32 word = qword * 4u + lane;
        const u32 bitmap_word = word >> 5;
        if (unavailable_memory_words &&
            (bitmap_word >= unavailable_memory_word_count ||
             (unavailable_memory_words[bitmap_word] &
              (1u << (word & 31u))) != 0u)) {
          return Fail(error, "private state-load bridge read unavailable GPU memory");
        }
      }
    }

    execution.result = ExecuteUniversalReferenceStepImpl(
        &candidate, program, fbrst, nullptr, false);
    if (execution.result == UniversalReferenceStepResult::PairCompleted ||
        execution.result == UniversalReferenceStepResult::ProgramFinished) {
      execution.executed_pairs++;
    }
    if (execution.result != UniversalReferenceStepResult::PairCompleted)
      break;
  }
  execution.stop_pc = candidate.VI[REG_TPC].UL & VU1_PROGMASK;
  candidate.VI[REG_TPC].UL >>= 3;
  if (execution.result != UniversalReferenceStepResult::ProgramFinished ||
      execution.executed_pairs != proven_pairs) {
    return Fail(error, "private state-load bridge did not reach its terminal pair");
  }

  std::memcpy(vu, &candidate, sizeof(candidate));
  if (result)
    *result = execution;
  return true;
}

bool ExecuteUniversalPrivateNoOutputReference(
    VURegs* vu, const UniversalMicroProgram& program, u32 maximum_pairs,
    u32 fbrst, u16 vif_top, u16 vif_itop,
    u32* unavailable_memory_words, u32 unavailable_memory_word_count,
    UniversalReferenceRunResult* result, std::string* error) {
  if (result)
    *result = {};
  if (error)
    error->clear();
  constexpr u32 MemoryWordCount = VU1_MEMSIZE / sizeof(u32);
  constexpr u32 MemoryMaskWordCount = (MemoryWordCount + 31u) / 32u;
  constexpr u32 MaximumPrivateContinuationPairs = 16384u;
  if (!vu || !vu->Mem || vu->idx != 1u)
    return Fail(error, "private no-output continuation requires a VU1 image");
  if (maximum_pairs == 0u ||
      maximum_pairs > MaximumPrivateContinuationPairs) {
    return Fail(error, "private no-output continuation pair bound is invalid");
  }
  if (!unavailable_memory_words ||
      unavailable_memory_word_count < MemoryMaskWordCount) {
    return Fail(error, "private no-output continuation has no availability map");
  }
  if (program.format_version != UniversalMicroProgramFormatVersion ||
      program.configuration_bits != CurrentConfigurationBits()) {
    return Fail(error, "private no-output continuation configuration mismatch");
  }
  if (vu->branch != 0u || vu->ebit != 0u || vu->xgkickenable != 0u ||
      vu->fmaccount != 0u || vu->ialucount != 0u ||
      vu->fdiv.enable != 0u || vu->efu.enable != 0u) {
    return Fail(error,
                "private no-output continuation requires a quiescent boundary");
  }

  // GeneratedLoopKernelPrivateState retains the ordinary VI file plus Q/P/I.
  // A continuation which consumes another scalar/flag generation cannot be
  // evaluated from that private ABI and therefore remains a real observer.
  constexpr u32 RepresentedViMask = 0xffffu | (1u << REG_Q) |
      (1u << REG_P) | (1u << REG_I);
  UniversalReferenceRunResult execution;
  const auto word_is_unavailable = [&](u32 word) {
    return (unavailable_memory_words[word >> 5] &
            (1u << (word & 31u))) != 0u;
  };
  const auto clear_written_words = [&](u32 qword, u32 lane_mask) {
    const u32 address = qword & 0x3ffu;
    execution.written_memory_qwords[address >> 5] |=
        1u << (address & 31u);
    for (u32 lane = 0u; lane < 4u; lane++) {
      if ((lane_mask & (0x8u >> lane)) == 0u)
        continue;
      const u32 word = ((qword & 0x3ffu) * 4u) + lane;
      unavailable_memory_words[word >> 5] &= ~(1u << (word & 31u));
    }
  };
  const auto reads_available = [&](u32 qword, u32 lane_mask) {
    for (u32 lane = 0u; lane < 4u; lane++) {
      if ((lane_mask & (0x8u >> lane)) == 0u)
        continue;
      const u32 word = ((qword & 0x3ffu) * 4u) + lane;
      if (word_is_unavailable(word))
        return false;
    }
    return true;
  };

  vu->VI[REG_TPC].UL <<= 3;
  for (u32 step = 0u; step < maximum_pairs; step++) {
    const u32 pc = vu->VI[REG_TPC].UL & VU1_PROGMASK;
    const UniversalPairMicroOp& op =
        program.pairs[pc / UniversalMicroProgramPairBytes];
    const DecodedControl control = DecodeControl(op.control);
    _VURegsNum upper{};
    _VURegsNum lower{};
    if (!control.valid ||
        !AnalyzeExecutionRegisters(op, control, &upper, &lower)) {
      execution.result = UniversalReferenceStepResult::InvalidEncoding;
      execution.stop_pc = pc;
      vu->VI[REG_TPC].UL >>= 3;
      if (result)
        *result = execution;
      return Fail(error,
                  "private no-output continuation has invalid metadata");
    }
    if (((upper.VIread | upper.VIwrite | lower.VIread | lower.VIwrite) &
         ~RepresentedViMask) != 0u) {
      execution.stop_pc = pc;
      vu->VI[REG_TPC].UL >>= 3;
      if (result)
        *result = execution;
      return Fail(
          error,
          "private no-output continuation needs unrepresented scalar state");
    }
    if ((control.dflag && (fbrst & 0x400u) != 0u) ||
        (control.tflag && (fbrst & 0x800u) != 0u)) {
      execution.stop_pc = pc;
      vu->VI[REG_TPC].UL >>= 3;
      if (result)
        *result = execution;
      return Fail(error,
                  "private no-output continuation reached a D/T observer");
    }

    u32 read_qword = 0u;
    u32 read_mask = 0u;
    u32 write_qword = 0u;
    u32 write_mask = 0u;
    if (control.exec_lower && !control.immediate_lower &&
        !control.lower_discarded) {
      using Kind = VUInterpFast::LowerFastKind;
      const Kind kind = static_cast<Kind>(control.lower_kind);
      const u32 code = op.lower;
      const u32 mask = VUInterpFast::XYZW(code);
      const u32 is = VUInterpFast::Is(code);
      const u32 it = VUInterpFast::It(code);
      const auto base_imm = [&](u32 reg) {
        return static_cast<u32>(
            static_cast<s32>(static_cast<s16>(vu->VI[reg].US[0])) +
            VUInterpFast::Imm11(code)) & 0x3ffu;
      };
      switch (kind) {
      case Kind::LQ:
        if (VUInterpFast::Ft(code) != 0u) {
          read_qword = base_imm(is);
          read_mask = mask;
        }
        break;
      case Kind::LQI:
        if (VUInterpFast::Ft(code) != 0u) {
          read_qword = vu->VI[is].US[0] & 0x3ffu;
          read_mask = mask;
        }
        break;
      case Kind::LQD:
        if (VUInterpFast::Ft(code) != 0u) {
          read_qword =
              (vu->VI[is].US[0] - (is != 0u ? 1u : 0u)) & 0x3ffu;
          read_mask = mask;
        }
        break;
      case Kind::ILW:
        if (it != 0u) {
          read_qword = base_imm(is);
          read_mask = mask;
        }
        break;
      case Kind::ILWR:
        if (it != 0u) {
          read_qword = vu->VI[is].US[0] & 0x3ffu;
          read_mask = mask;
        }
        break;
      case Kind::SQ:
        write_qword = base_imm(it);
        write_mask = mask;
        break;
      case Kind::SQI:
        write_qword = vu->VI[it].US[0] & 0x3ffu;
        write_mask = mask;
        break;
      case Kind::SQD:
        write_qword =
            (vu->VI[it].US[0] -
             (VUInterpFast::Ft(code) != 0u ? 1u : 0u)) & 0x3ffu;
        write_mask = mask;
        break;
      case Kind::ISW:
        write_qword = base_imm(is);
        write_mask = mask;
        break;
      case Kind::ISWR:
        write_qword = vu->VI[is].US[0] & 0x3ffu;
        write_mask = mask;
        break;
      case Kind::XGKICK:
        execution.stop_pc = pc;
        vu->VI[REG_TPC].UL >>= 3;
        if (result)
          *result = execution;
        return Fail(error,
                    "private no-output continuation reached XGKICK");
      default:
        break;
      }
    }
    if (read_mask != 0u && !reads_available(read_qword, read_mask)) {
      execution.stop_pc = pc;
      vu->VI[REG_TPC].UL >>= 3;
      if (result)
        *result = execution;
      return Fail(
          error,
          "private no-output continuation read unavailable GPU memory");
    }

    execution.result = ExecuteUniversalReferenceStepImpl(
        vu, program, fbrst, nullptr, false, &vif_top, &vif_itop);
    if (execution.result == UniversalReferenceStepResult::PairCompleted ||
        execution.result == UniversalReferenceStepResult::ProgramFinished) {
      execution.executed_pairs++;
      if (write_mask != 0u)
        clear_written_words(write_qword, write_mask);
    }
    if (execution.result != UniversalReferenceStepResult::PairCompleted) {
      execution.stop_pc = vu->VI[REG_TPC].UL & VU1_PROGMASK;
      vu->VI[REG_TPC].UL >>= 3;
      if (result)
        *result = execution;
      if (execution.result != UniversalReferenceStepResult::ProgramFinished) {
        return Fail(
            error,
            "private no-output continuation stopped before terminal E-bit");
      }
      return true;
    }
  }

  execution.stop_pc = vu->VI[REG_TPC].UL & VU1_PROGMASK;
  execution.result = UniversalReferenceStepResult::PairLimitReached;
  vu->VI[REG_TPC].UL >>= 3;
  if (result)
    *result = execution;
  return Fail(error,
              "private no-output continuation exceeded its pair bound");
}

}  // namespace VitaGpuVu
