// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuArchitectureLedger.h"

#include "VUmicroFast.h"

#include <array>
#include <cstring>
#include <iterator>

namespace VitaGpuVu {
namespace {

constexpr const char* VuInstructionOwner =
    "pcsx2/VUops.cpp and pcsx2/VUmicroFast.h";
constexpr const char* VuInstructionRetirement =
    "generated per-kind PCSX2 differential plus fixed-GXP exact-mode gate";
constexpr const char* VifCommandOwner =
    "pcsx2/Vif_Codes.cpp::vifCmdHandler[1] and pcsx2/Vif_Transfer.cpp::_vifTransfer";
constexpr const char* VifCommandRetirement =
    "generated VIF1 command differential plus transactional raw-stream epoch gate";
constexpr const char* VifObserverRetirement =
    "pre-effect VIF1 observer/interrupt differential plus ordered materialization gate";
constexpr const char* VifUnpackOwner =
    "pcsx2/Vif_Unpack.cpp and pcsx2/Vif_Dynarec.h";
constexpr const char* VifUnpackRetirement =
    "generated UNPACK oracle matrix plus transactional VU-memory commit gate";
constexpr const char* PipelineOwner =
    "pcsx2/VUops.cpp and pcsx2/VU1microInterp.cpp::_vu1Exec";
constexpr const char* PipelineRetirement =
    "generated interacting-pair PCSX2 differential plus fixed-GXP exact-mode gate";
constexpr const char* OperandOwner =
    "pcsx2/VUmicroFast.h and pcsx2/VUops.cpp";
constexpr const char* OperandRetirement =
    "generated mask/alias/address PCSX2 differential plus fixed/generated-GXP gate";
constexpr const char* Path1Owner =
    "pcsx2/VUops.cpp::_vuXGKICKTransferMicroVU and pcsx2/Gif_Unit.h";
constexpr const char* Path1Retirement =
    "multi-XGKICK wrap/terminal-flush differential and ordered Vita retirement gate";
constexpr const char* GeneratedOwner =
    "pcsx2/vita/VitaVuBlockCompiler.cpp::AnalyzeGpuVu1PairForConfiguration";
constexpr const char* GeneratedRetirement =
    "unseen-microcode generated-GXP differential, async cold-cache fallback, and Vita cache gate";
constexpr const char* ProductOwner =
    "pcsx2/MTVU.cpp::VU_Thread::ExecuteRingBuffer and pcsx2/vita/GSDeviceGXM.cpp";
constexpr const char* ProductRetirement =
    "sequence-cut PCSX2 differential plus profiler-off asynchronous physical-Vita gate";

#define VITASX2_GPU_VU_UPPER(kind, text, state)                         \
  ArchitectureLedgerEntry{ArchitectureDomain::UpperInstruction,        \
      static_cast<u16>(VUInterpFast::UpperFastKind::kind),              \
      ArchitectureCoverage::state, text, VuInstructionOwner,            \
      ArchitectureCoverage::state ==                                   \
              ArchitectureCoverage::TemporarilyUnsupported              \
          ? VuInstructionRetirement                                     \
          : "generated per-kind PCSX2 differential"},
constexpr ArchitectureLedgerEntry UpperInstructions[] = {
#include "vita/VitaGpuVuArchitectureManifest.inc"
};

#define VITASX2_GPU_VU_LOWER(kind, text, state)                         \
  ArchitectureLedgerEntry{ArchitectureDomain::LowerInstruction,        \
      static_cast<u16>(VUInterpFast::LowerFastKind::kind),              \
      ArchitectureCoverage::state, text, VuInstructionOwner,            \
      ArchitectureCoverage::state ==                                   \
              ArchitectureCoverage::TemporarilyUnsupported              \
          ? VuInstructionRetirement                                     \
          : "generated per-kind PCSX2 differential"},
constexpr ArchitectureLedgerEntry LowerInstructions[] = {
#include "vita/VitaGpuVuArchitectureManifest.inc"
};

constexpr ArchitectureCoverage Vif1CommandCoverage(u16 opcode) {
  switch (opcode) {
  case 0x06:  // MSKPATH3
  case 0x07:  // MARK
  case 0x10:  // FLUSHE
  case 0x11:  // FLUSH
  case 0x13:  // FLUSHA
  case 0x15:  // MSCALF
  case 0x50:  // DIRECT
  case 0x51:  // DIRECTHL
    return ArchitectureCoverage::ArchitecturalObserver;
  default:
    return ArchitectureCoverage::TemporarilyUnsupported;
  }
}

#define VITASX2_VIF_COMMAND(opcode, handler, text)                      \
  ArchitectureLedgerEntry{ArchitectureDomain::Vif1Command,              \
      static_cast<u16>(opcode), Vif1CommandCoverage(opcode), text,       \
      VifCommandOwner, Vif1CommandCoverage(opcode) ==                    \
              ArchitectureCoverage::ArchitecturalObserver               \
          ? VifObserverRetirement                                       \
          : VifCommandRetirement},
constexpr ArchitectureLedgerEntry Vif1Commands[] = {
#include "Vif_CommandManifest.inc"
};

#define VITASX2_GPU_VU_VIF1_UNPACK_FORMAT(format, text, state)          \
  ArchitectureLedgerEntry{ArchitectureDomain::Vif1UnpackFormat,         \
      static_cast<u16>(format), ArchitectureCoverage::state, text,       \
      VifUnpackOwner, VifUnpackRetirement},
constexpr ArchitectureLedgerEntry Vif1UnpackFormats[] = {
#include "vita/VitaGpuVuArchitectureManifest.inc"
};

// Instruction rows classify bodies and operand forms decoded by VUmicroFast.
// These rows classify architectural interactions which cannot truthfully be
// attached to one opcode. Exactness is scoped to one row: for example, an
// arithmetic body may exist while the special-value matrix remains open.
constexpr ArchitectureLedgerEntry OtherArchitecture[] = {
    {ArchitectureDomain::Vif1CommandModifier, 0,
     ArchitectureCoverage::ArchitecturalObserver, "command-irq-bit",
     VifCommandOwner,
     "IRQ-bit VIS/VFS/VSS stall and interrupt-boundary differential"},
    {ArchitectureDomain::Vif1CommandModifier, 1,
     ArchitectureCoverage::TemporarilyUnsupported,
     "partial-command-word-continuation", VifCommandOwner,
     "split-at-every-byte raw VIF1 command-stream differential"},
    {ArchitectureDomain::Vif1CommandModifier, 2,
     ArchitectureCoverage::TemporarilyUnsupported,
     "partial-command-payload-continuation", VifCommandOwner,
     "split-at-every-qword VIF1 payload differential"},
    {ArchitectureDomain::Vif1CommandModifier, 3,
     ArchitectureCoverage::TemporarilyUnsupported,
     "dma-chain-and-mfifo-order", VifCommandOwner,
     "normal/chain/MFIFO epoch-boundary and retained-generation differential"},
    {ArchitectureDomain::Vif1CommandModifier, 4,
     ArchitectureCoverage::TemporarilyUnsupported,
     "reserved-null-and-err-mask", VifCommandOwner,
     "all reserved encodings under ERR.ME0/ME1 command differential"},
    {ArchitectureDomain::Vif1CommandModifier, 5,
     ArchitectureCoverage::ArchitecturalObserver,
     "forcebreak-stop-and-reset", VifCommandOwner,
     "FBRST forcebreak/stop/reset pre-effect cancellation differential"},
    {ArchitectureDomain::Vif1CommandModifier, 6,
     ArchitectureCoverage::TemporarilyUnsupported,
     "queued-program-and-waitforvu", VifCommandOwner,
     "queued MSCAL plus VPU_STAT/waitforvu ordering differential"},
    {ArchitectureDomain::Vif1CommandModifier, 7,
     ArchitectureCoverage::TemporarilyUnsupported,
     "base-offset-top-tops-double-buffer", VifCommandOwner,
     "BASE/OFFSET/TOP/TOPS command-chain differential"},
    {ArchitectureDomain::Vif1CommandModifier, 8,
     ArchitectureCoverage::ArchitecturalObserver,
     "vis-vfs-vss-status-and-mark", VifCommandOwner,
     "MARK and VIF STAT interrupt-state materialization differential"},
    {ArchitectureDomain::Vif1CommandModifier, 9,
     ArchitectureCoverage::TemporarilyUnsupported,
     "pass-and-inprogress-state", VifCommandOwner,
     "pass1/pass2/inprogress continuation differential at every transfer cut"},

    {ArchitectureDomain::Vif1UnpackBehavior, 0,
     ArchitectureCoverage::TemporarilyUnsupported,
     "signed-and-unsigned-extension", VifUnpackOwner,
     "all 8/16-bit sign-bit edge vectors under USN=0/1"},
    {ArchitectureDomain::Vif1UnpackBehavior, 1,
     ArchitectureCoverage::TemporarilyUnsupported,
     "masked-data-row-column-write-protect", VifUnpackOwner,
     "all four mask selectors at every destination lane/cycle"},
    {ArchitectureDomain::Vif1UnpackBehavior, 2,
     ArchitectureCoverage::TemporarilyUnsupported, "mode-zero",
     VifUnpackOwner, "MODE=0 all-format PCSX2 differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 3,
     ArchitectureCoverage::TemporarilyUnsupported, "mode-add-row",
     VifUnpackOwner, "MODE=1 overflow/alias PCSX2 differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 4,
     ArchitectureCoverage::TemporarilyUnsupported,
     "mode-add-row-and-update", VifUnpackOwner,
     "MODE=2 row recurrence and mask interaction differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 5,
     ArchitectureCoverage::TemporarilyUnsupported,
     "mode-replace-row-and-update", VifUnpackOwner,
     "MODE=3 row recurrence and mask interaction differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 6,
     ArchitectureCoverage::TemporarilyUnsupported, "stcycl-skip",
     VifUnpackOwner, "CL>=WL skip-cycle matrix differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 7,
     ArchitectureCoverage::TemporarilyUnsupported, "stcycl-fill",
     VifUnpackOwner, "CL<WL fill-cycle matrix differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 8,
     ArchitectureCoverage::TemporarilyUnsupported, "zero-cl-fill",
     VifUnpackOwner, "CL=0 repeated-source fill differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 9,
     ArchitectureCoverage::TemporarilyUnsupported, "zero-wl-is-256",
     VifUnpackOwner, "WL=0 architectural-256 differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 10,
     ArchitectureCoverage::TemporarilyUnsupported, "zero-num-is-256",
     VifUnpackOwner, "NUM=0 architectural-256 differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 11,
     ArchitectureCoverage::TemporarilyUnsupported,
     "flg-tops-relative-destination", VifUnpackOwner,
     "FLG=0/1 TOPS-relative address differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 12,
     ArchitectureCoverage::TemporarilyUnsupported,
     "vu-memory-destination-wrap", VifUnpackOwner,
     "all formats crossing the 16-KiB VU1-memory boundary"},
    {ArchitectureDomain::Vif1UnpackBehavior, 13,
     ArchitectureCoverage::TemporarilyUnsupported,
     "partial-source-and-start-alignment", VifUnpackOwner,
     "every source-qword cut and V3 start-alignment differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 14,
     ArchitectureCoverage::TemporarilyUnsupported,
     "v2-xyxy-replication", VifUnpackOwner,
     "V2 indeterminate-lane hardware-compatible XYXY differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 15,
     ArchitectureCoverage::TemporarilyUnsupported,
     "v3-v4-width-and-indeterminate-w", VifUnpackOwner,
     "V3 packet-boundary W-lane PCSX2 differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 16,
     ArchitectureCoverage::TemporarilyUnsupported,
     "v4-5-packed-and-mode-ignored", VifUnpackOwner,
     "V4-5 bitfield, mask, and MODE-ignored differential"},
    {ArchitectureDomain::Vif1UnpackBehavior, 17,
     ArchitectureCoverage::TemporarilyUnsupported,
     "row-column-state-persistence", VifUnpackOwner,
     "interleaved STROW/STCOL/UNPACK generation differential"},

    {ArchitectureDomain::Vu1StateAndPipeline, 0,
     ArchitectureCoverage::ImplementedExactly, "vf-vi-acc-i-state",
     PipelineOwner, "complete-state PCSX2 differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 1,
     ArchitectureCoverage::TemporarilyUnsupported,
     "q-p-pending-values-and-publication", PipelineOwner,
     PipelineRetirement},
    {ArchitectureDomain::Vu1StateAndPipeline, 2,
     ArchitectureCoverage::TemporarilyUnsupported, "random-register-r",
     PipelineOwner, PipelineRetirement},
    {ArchitectureDomain::Vu1StateAndPipeline, 3,
     ArchitectureCoverage::TemporarilyUnsupported,
     "branch-delay-link-and-vi-backup", PipelineOwner,
     "taken/not-taken/delay/link/VI-backup exhaustive differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 4,
     ArchitectureCoverage::ImplementedExactly, "fmac-and-ialu-hazards",
     PipelineOwner, "accurate/scheduled pipeline-cycle differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 5,
     ArchitectureCoverage::TemporarilyUnsupported, "fdiv-q-and-waitq",
     PipelineOwner, "complete Q value/timing/D-I STATUS differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 6,
     ArchitectureCoverage::TemporarilyUnsupported, "efu-p-and-waitp",
     PipelineOwner, "complete P value/timing/D-I STATUS differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 7,
     ArchitectureCoverage::TemporarilyUnsupported,
     "delayed-mac-status-clip-publication", PipelineOwner,
     PipelineRetirement},
    {ArchitectureDomain::Vu1StateAndPipeline, 8,
     ArchitectureCoverage::ImplementedExactly,
     "e-bit-and-arithmetic-pipeline-drain", PipelineOwner,
     "E-bit delay and FMAC/IALU/FDIV/EFU terminal differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 9,
     ArchitectureCoverage::ArchitecturalObserver, "d-bit", PipelineOwner,
     "pre-effect D-bit observer/interrupt differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 10,
     ArchitectureCoverage::ArchitecturalObserver, "t-bit", PipelineOwner,
     "pre-effect T-bit observer/interrupt differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 11,
     ArchitectureCoverage::ArchitecturalObserver, "m-bit-debug-observer",
     PipelineOwner, "M-bit debug/observer differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 12,
     ArchitectureCoverage::TemporarilyUnsupported,
     "multiple-and-resumed-execute-chain", VifCommandOwner,
     "interleaved MSCAL/MSCALF/MSCNT state-generation differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 13,
     ArchitectureCoverage::ImplementedExactly,
     "immutable-configuration-identity", PipelineOwner,
     "source/configuration cache-separation differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 14,
     ArchitectureCoverage::TemporarilyUnsupported,
     "rounding-special-values-and-normalization", PipelineOwner,
     "NaN/Inf/signed-zero/denormal/rounding dependent-operation matrix"},
    {ArchitectureDomain::Vu1StateAndPipeline, 15,
     ArchitectureCoverage::ImplementedExactly,
     "instant-q-p-resource-elision", PipelineOwner,
     "Instant-Q/P value and cycle differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 16,
     ArchitectureCoverage::ImplementedExactly,
     "assume-scheduled-pipeline-contract", PipelineOwner,
     "accurate-versus-assume-scheduled cycle/state differential"},
    {ArchitectureDomain::Vu1StateAndPipeline, 17,
     ArchitectureCoverage::TemporarilyUnsupported,
     "overflow-underflow-round-daz-fz-policy", PipelineOwner,
     "all configuration-bit numeric-policy differentials"},
    {ArchitectureDomain::Vu1StateAndPipeline, 18,
     ArchitectureCoverage::ArchitecturalObserver,
     "architectural-state-materialization", PipelineOwner,
     "VIF/EE observer, interrupt, savestate, reset, and shutdown differential"},

    {ArchitectureDomain::Vu1OperandBehavior, 0,
     ArchitectureCoverage::TemporarilyUnsupported,
     "vf0-constant-read-and-write-discard", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 1,
     ArchitectureCoverage::TemporarilyUnsupported,
     "vi0-constant-read-and-write-discard", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 2,
     ArchitectureCoverage::TemporarilyUnsupported,
     "vf-destination-lane-mask", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 3,
     ArchitectureCoverage::TemporarilyUnsupported,
     "acc-destination-lane-mask", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 4,
     ArchitectureCoverage::TemporarilyUnsupported,
     "i-q-and-xyzw-scalar-broadcast", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 5,
     ArchitectureCoverage::TemporarilyUnsupported,
     "source-destination-aliasing", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 6,
     ArchitectureCoverage::TemporarilyUnsupported,
     "simultaneous-upper-lower-vf-priority", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 7,
     ArchitectureCoverage::TemporarilyUnsupported,
     "simultaneous-upper-lower-vi-priority", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 8,
     ArchitectureCoverage::TemporarilyUnsupported,
     "vi-sixteen-bit-arithmetic-and-sign", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 9,
     ArchitectureCoverage::TemporarilyUnsupported,
     "immediate-i-lower-word", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 10,
     ArchitectureCoverage::TemporarilyUnsupported,
     "vu-memory-address-mask-and-wrap", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 11,
     ArchitectureCoverage::TemporarilyUnsupported,
     "pre-post-increment-decrement-address", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 12,
     ArchitectureCoverage::TemporarilyUnsupported,
     "indirect-vi-memory-address", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 13,
     ArchitectureCoverage::TemporarilyUnsupported,
     "load-store-lane-mask", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 14,
     ArchitectureCoverage::TemporarilyUnsupported,
     "lower-discard-and-immediate-pair", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 15,
     ArchitectureCoverage::TemporarilyUnsupported,
     "branch-target-sign-and-program-wrap", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 16,
     ArchitectureCoverage::TemporarilyUnsupported,
     "bal-jalr-link-write", OperandOwner, OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 17,
     ArchitectureCoverage::TemporarilyUnsupported,
     "serialized-read-write-hazard-masks", OperandOwner,
     OperandRetirement},
    {ArchitectureDomain::Vu1OperandBehavior, 18,
     ArchitectureCoverage::TemporarilyUnsupported,
     "same-pair-source-snapshot-before-writes", OperandOwner,
     OperandRetirement},

    {ArchitectureDomain::Path1Output, 0,
     ArchitectureCoverage::TemporarilyUnsupported,
     "one-following-pair-delayed-xgkick", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 1,
     ArchitectureCoverage::TemporarilyUnsupported,
     "terminal-delayed-xgkick-flush", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 2,
     ArchitectureCoverage::TemporarilyUnsupported,
     "multiple-xgkick-issue-order", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 3,
     ArchitectureCoverage::TemporarilyUnsupported,
     "packed-reglist-image-packet-extent", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 4,
     ArchitectureCoverage::TemporarilyUnsupported,
     "packet-read-wrap-at-16-kib", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 5,
     ArchitectureCoverage::TemporarilyUnsupported,
     "complete-packet-transactional-capacity", Path1Owner,
     Path1Retirement},
    {ArchitectureDomain::Path1Output, 6,
     ArchitectureCoverage::TemporarilyUnsupported,
     "persistent-multi-frame-raw-path1-ring", Path1Owner,
     Path1Retirement},
    {ArchitectureDomain::Path1Output, 7,
     ArchitectureCoverage::TemporarilyUnsupported,
     "first-class-ordered-gs-consumer", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 8,
     ArchitectureCoverage::TemporarilyUnsupported,
     "direct-tfxvertex-raw-route-descent", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 9,
     ArchitectureCoverage::TemporarilyUnsupported,
     "no-per-xgkick-wait-or-notification", Path1Owner, Path1Retirement},
    {ArchitectureDomain::Path1Output, 10,
     ArchitectureCoverage::ArchitecturalObserver,
     "path1-path2-path3-order-and-mask", Path1Owner,
     "PATH1/PATH2/PATH3 arbitration and MSKPATH3 differential"},

    {ArchitectureDomain::GeneratedGxp, 0,
     ArchitectureCoverage::TemporarilyUnsupported,
     "general-serial-state-machine-gxp", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 1,
     ArchitectureCoverage::TemporarilyUnsupported,
     "asynchronous-runtime-compilation", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 2,
     ArchitectureCoverage::TemporarilyUnsupported,
     "source-config-abi-compiler-cache-identity", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 3,
     ArchitectureCoverage::TemporarilyUnsupported,
     "reachable-operation-body-specialization", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 4,
     ArchitectureCoverage::TemporarilyUnsupported,
     "reducible-control-flow-and-loop-structure", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 5,
     ArchitectureCoverage::TemporarilyUnsupported,
     "irreducible-pc-driven-control", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 6,
     ArchitectureCoverage::TemporarilyUnsupported,
     "proven-independent-iteration-parallelism", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 7,
     ArchitectureCoverage::TemporarilyUnsupported,
     "vif-unpack-and-vu-memory-fusion", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 8,
     ArchitectureCoverage::TemporarilyUnsupported,
     "automatic-output-route-selection", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 9,
     ArchitectureCoverage::TemporarilyUnsupported,
     "automatic-direct-vu-tfx-fusion", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 10,
     ArchitectureCoverage::TemporarilyUnsupported,
     "compile-delay-or-failure-universal-fallback", GeneratedOwner,
     GeneratedRetirement},
    {ArchitectureDomain::GeneratedGxp, 11,
     ArchitectureCoverage::TemporarilyUnsupported,
     "persistent-cache-lifecycle-and-invalidation", GeneratedOwner,
     GeneratedRetirement},

    {ArchitectureDomain::ProductOwnership, 0,
     ArchitectureCoverage::TemporarilyUnsupported,
     "immutable-mapped-vif-input-ring", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 1,
     ArchitectureCoverage::TemporarilyUnsupported,
     "complete-e-bit-command-epoch-accumulation", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 2,
     ArchitectureCoverage::TemporarilyUnsupported,
     "complete-pre-effect-admission", ProductOwner, ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 3,
     ArchitectureCoverage::TemporarilyUnsupported,
     "monotonic-sequence-number", ProductOwner, ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 4,
     ArchitectureCoverage::TemporarilyUnsupported,
     "private-transactional-next-state", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 5,
     ArchitectureCoverage::TemporarilyUnsupported,
     "dependent-private-state-generation", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 6,
     ArchitectureCoverage::TemporarilyUnsupported,
     "asynchronous-submit-without-epoch-wait", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 7,
     ArchitectureCoverage::TemporarilyUnsupported,
     "notification-retirement-only-on-reuse", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 8,
     ArchitectureCoverage::TemporarilyUnsupported,
     "retained-journal-cpu-replay-from-commit", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 9,
     ArchitectureCoverage::TemporarilyUnsupported,
     "persistent-gpu-owned-vu1-state", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 10,
     ArchitectureCoverage::ArchitecturalObserver,
     "observer-only-cpu-materialization", ProductOwner,
     "VIF/EE observer, interrupt, savestate, reset, shutdown materialization gate"},
    {ArchitectureDomain::ProductOwnership, 11,
     ArchitectureCoverage::TemporarilyUnsupported,
     "reset-savestate-shutdown-ownership", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 12,
     ArchitectureCoverage::TemporarilyUnsupported,
     "bounded-storage-and-slot-backpressure", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 13,
     ArchitectureCoverage::TemporarilyUnsupported,
     "zero-cpu-vu-calls-after-acceptance", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 14,
     ArchitectureCoverage::TemporarilyUnsupported,
     "persistent-multiple-product-slots", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 15,
     ArchitectureCoverage::TemporarilyUnsupported,
     "nonblocking-generated-universal-provider-selection", ProductOwner,
     ProductRetirement},
    {ArchitectureDomain::ProductOwnership, 16,
     ArchitectureCoverage::TemporarilyUnsupported,
     "accepted-rejected-output-bypass-telemetry", ProductOwner,
     "physical log gate for sequence/provider/pairs/output/CPU-bypass/fallback"},
    {ArchitectureDomain::ProductOwnership, 17,
     ArchitectureCoverage::TemporarilyUnsupported,
     "versioned-fixed-gxp-product-asset", ProductOwner,
     "packaged asset/hash/program-check/lifetime physical-Vita gate"},
    {ArchitectureDomain::ProductOwnership, 18,
     ArchitectureCoverage::TemporarilyUnsupported,
     "title-pc-hash-independent-semantic-admission", ProductOwner,
     "unseen-source admission differential with hashes used only for cache identity"},
    {ArchitectureDomain::ProductOwnership, 19,
     ArchitectureCoverage::TemporarilyUnsupported,
     "cpu-fallback-before-first-visible-effect", ProductOwner,
     "forced failure at every command/pair/output cut with byte-exact CPU replay"},
};

static_assert(std::size(UpperInstructions) + 1 ==
              VUInterpFast::UpperFastKindCount);
static_assert(std::size(LowerInstructions) + 1 ==
              VUInterpFast::LowerFastKindCount);
static_assert(std::size(Vif1Commands) == 128);
static_assert(std::size(Vif1UnpackFormats) == 16);

constexpr std::size_t Vif1CommandModifierCount = 10;
constexpr std::size_t Vif1UnpackBehaviorCount = 18;
constexpr std::size_t Vu1StateAndPipelineCount = 19;
constexpr std::size_t Vu1OperandBehaviorCount = 19;
constexpr std::size_t Path1OutputCount = 11;
constexpr std::size_t GeneratedGxpCount = 12;
constexpr std::size_t ProductOwnershipCount = 20;

template <std::size_t FirstSize, std::size_t SecondSize,
          std::size_t ThirdSize>
constexpr std::array<ArchitectureLedgerEntry,
                     FirstSize + SecondSize + ThirdSize>
Concatenate(const ArchitectureLedgerEntry (&first)[FirstSize],
            const ArchitectureLedgerEntry (&second)[SecondSize],
            const ArchitectureLedgerEntry (&third)[ThirdSize]) {
  std::array<ArchitectureLedgerEntry,
             FirstSize + SecondSize + ThirdSize> result{};
  std::size_t output = 0;
  for (std::size_t index = 0; index < FirstSize; index++)
    result[output++] = first[index];
  for (std::size_t index = 0; index < SecondSize; index++)
    result[output++] = second[index];
  for (std::size_t index = 0; index < ThirdSize; index++)
    result[output++] = third[index];
  return result;
}

constexpr auto NonInstructionArchitecture =
    Concatenate(Vif1Commands, Vif1UnpackFormats, OtherArchitecture);

constexpr std::array<std::size_t,
                     static_cast<std::size_t>(ArchitectureDomain::Count)>
    ExpectedDomainCounts{{
        std::size(UpperInstructions),
        std::size(LowerInstructions),
        std::size(Vif1Commands),
        std::size(Vif1UnpackFormats),
        Vif1CommandModifierCount,
        Vif1UnpackBehaviorCount,
        Vu1StateAndPipelineCount,
        Vu1OperandBehaviorCount,
        Path1OutputCount,
        GeneratedGxpCount,
        ProductOwnershipCount,
    }};

static_assert(std::size(OtherArchitecture) ==
              Vif1CommandModifierCount + Vif1UnpackBehaviorCount +
                  Vu1StateAndPipelineCount + Vu1OperandBehaviorCount +
                  Path1OutputCount + GeneratedGxpCount +
                  ProductOwnershipCount);

bool Fail(std::string* error, const char* message) {
  if (error)
    *error = message;
  return false;
}

bool IsCoverageValid(ArchitectureCoverage coverage) {
  switch (coverage) {
  case ArchitectureCoverage::ImplementedExactly:
  case ArchitectureCoverage::ArchitecturalObserver:
  case ArchitectureCoverage::TemporarilyUnsupported:
    return true;
  }
  return false;
}

bool ValidateEntry(const ArchitectureLedgerEntry& entry,
                   std::string* error) {
  if (entry.domain >= ArchitectureDomain::Count ||
      !IsCoverageValid(entry.coverage) || !entry.name ||
      entry.name[0] == '\0' || !entry.pcsx2_owner ||
      entry.pcsx2_owner[0] == '\0' || !entry.retirement_test ||
      entry.retirement_test[0] == '\0') {
    return Fail(error, "GPU-VU architecture ledger has an unclassified row");
  }
  return true;
}

template <std::size_t Size>
bool ValidateInstructionTable(
    const ArchitectureLedgerEntry (&entries)[Size],
    ArchitectureDomain domain, std::size_t expected_count,
    std::string* error) {
  if (Size + 1 != expected_count)
    return Fail(error, "GPU-VU instruction ledger count drifted from PCSX2");
  for (std::size_t index = 0; index < Size; index++) {
    const ArchitectureLedgerEntry& entry = entries[index];
    if (!ValidateEntry(entry, error))
      return false;
    if (entry.domain != domain || entry.id != index + 1)
      return Fail(error, "GPU-VU instruction ledger IDs are not exhaustive");
  }
  return true;
}

bool ValidateDenseNonInstructionDomains(std::string* error) {
  constexpr std::size_t FirstNonInstructionDomain =
      static_cast<std::size_t>(ArchitectureDomain::Vif1Command);
  constexpr std::size_t DomainCount =
      static_cast<std::size_t>(ArchitectureDomain::Count);
  std::array<std::size_t, DomainCount> actual_counts{};

  for (std::size_t index = 0; index < NonInstructionArchitecture.size();
       index++) {
    const ArchitectureLedgerEntry& entry = NonInstructionArchitecture[index];
    if (!ValidateEntry(entry, error))
      return false;
    const std::size_t domain = static_cast<std::size_t>(entry.domain);
    if (domain < FirstNonInstructionDomain)
      return Fail(error, "GPU-VU non-instruction ledger has wrong domain");
    actual_counts[domain]++;

    for (std::size_t previous = 0; previous < index; previous++) {
      const ArchitectureLedgerEntry& other =
          NonInstructionArchitecture[previous];
      if (entry.domain == other.domain && entry.id == other.id)
        return Fail(error, "GPU-VU architecture ledger has a duplicate ID");
    }
  }

  for (std::size_t domain = FirstNonInstructionDomain;
       domain < DomainCount; domain++) {
    if (actual_counts[domain] != ExpectedDomainCounts[domain])
      return Fail(error, "GPU-VU architecture domain count drifted");
    for (std::size_t id = 0; id < ExpectedDomainCounts[domain]; id++) {
      bool found = false;
      for (const ArchitectureLedgerEntry& entry :
           NonInstructionArchitecture) {
        if (static_cast<std::size_t>(entry.domain) == domain &&
            entry.id == id) {
          found = true;
          break;
        }
      }
      if (!found)
        return Fail(error, "GPU-VU architecture domain has an ID gap");
    }
  }
  return true;
}

}  // namespace

const ArchitectureLedgerEntry* GetGpuVuArchitectureLedger(
    std::size_t* count) {
  if (count)
    *count = NonInstructionArchitecture.size();
  return NonInstructionArchitecture.data();
}

const ArchitectureLedgerEntry* GetGpuVuUpperInstructionLedger(
    std::size_t* count) {
  if (count)
    *count = std::size(UpperInstructions);
  return UpperInstructions;
}

const ArchitectureLedgerEntry* GetGpuVuLowerInstructionLedger(
    std::size_t* count) {
  if (count)
    *count = std::size(LowerInstructions);
  return LowerInstructions;
}

const ArchitectureLedgerEntry* GetGpuVuVif1CommandLedger(
    std::size_t* count) {
  if (count)
    *count = std::size(Vif1Commands);
  return Vif1Commands;
}

const ArchitectureLedgerEntry* GetGpuVuVif1UnpackFormatLedger(
    std::size_t* count) {
  if (count)
    *count = std::size(Vif1UnpackFormats);
  return Vif1UnpackFormats;
}

const ArchitectureLedgerEntry* FindGpuVuUpperInstructionLedger(u32 kind) {
  return kind != 0 && kind <= std::size(UpperInstructions)
      ? &UpperInstructions[kind - 1]
      : nullptr;
}

const ArchitectureLedgerEntry* FindGpuVuLowerInstructionLedger(u32 kind) {
  return kind != 0 && kind <= std::size(LowerInstructions)
      ? &LowerInstructions[kind - 1]
      : nullptr;
}

const char* GpuVuUpperInstructionName(u32 kind) {
  const ArchitectureLedgerEntry* entry =
      FindGpuVuUpperInstructionLedger(kind);
  return entry ? entry->name : (kind == 0 ? "none" : "invalid");
}

const char* GpuVuLowerInstructionName(u32 kind) {
  const ArchitectureLedgerEntry* entry =
      FindGpuVuLowerInstructionLedger(kind);
  return entry ? entry->name : (kind == 0 ? "none" : "invalid");
}

bool IsFixedUniversalUpperInstructionBodyImplemented(u32 kind) {
  const ArchitectureLedgerEntry* entry =
      FindGpuVuUpperInstructionLedger(kind);
  return entry &&
      entry->coverage == ArchitectureCoverage::ImplementedExactly;
}

bool IsFixedUniversalLowerInstructionBodyImplemented(u32 kind) {
  const ArchitectureLedgerEntry* entry =
      FindGpuVuLowerInstructionLedger(kind);
  return entry &&
      entry->coverage == ArchitectureCoverage::ImplementedExactly;
}

bool ValidateGpuVuArchitectureLedger(std::string* error) {
  if (error)
    error->clear();
  if (!ValidateInstructionTable(
          UpperInstructions, ArchitectureDomain::UpperInstruction,
          VUInterpFast::UpperFastKindCount, error) ||
      !ValidateInstructionTable(
          LowerInstructions, ArchitectureDomain::LowerInstruction,
          VUInterpFast::LowerFastKindCount, error)) {
    return false;
  }
  return ValidateDenseNonInstructionDomains(error);
}

}  // namespace VitaGpuVu
