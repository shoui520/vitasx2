// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <cstddef>
#include <string>

namespace VitaGpuVu {

enum class ArchitectureCoverage : u8 {
  ImplementedExactly,
  ArchitecturalObserver,
  TemporarilyUnsupported,
};

enum class ArchitectureDomain : u8 {
  UpperInstruction,
  LowerInstruction,
  Vif1Command,
  Vif1UnpackFormat,
  Vif1CommandModifier,
  Vif1UnpackBehavior,
  Vu1StateAndPipeline,
  Vu1OperandBehavior,
  Path1Output,
  GeneratedGxp,
  ProductOwnership,
  Count,
};

struct ArchitectureLedgerEntry {
  ArchitectureDomain domain;
  u16 id;
  ArchitectureCoverage coverage;
  const char* name;
  const char* pcsx2_owner;
  const char* retirement_test;
};

const ArchitectureLedgerEntry* GetGpuVuArchitectureLedger(std::size_t* count);
const ArchitectureLedgerEntry* GetGpuVuUpperInstructionLedger(
    std::size_t* count);
const ArchitectureLedgerEntry* GetGpuVuLowerInstructionLedger(
    std::size_t* count);
const ArchitectureLedgerEntry* GetGpuVuVif1CommandLedger(
    std::size_t* count);
const ArchitectureLedgerEntry* GetGpuVuVif1UnpackFormatLedger(
    std::size_t* count);

const ArchitectureLedgerEntry* FindGpuVuUpperInstructionLedger(u32 kind);
const ArchitectureLedgerEntry* FindGpuVuLowerInstructionLedger(u32 kind);
const char* GpuVuUpperInstructionName(u32 kind);
const char* GpuVuLowerInstructionName(u32 kind);
// These predicates cover the decoded instruction body only. Operand aliases,
// pipelines, flags, observers, and output ownership are separate ledger rows
// and must be preflighted independently before an epoch can be admitted.
bool IsFixedUniversalUpperInstructionBodyImplemented(u32 kind);
bool IsFixedUniversalLowerInstructionBodyImplemented(u32 kind);

// Exhaustively verifies that every decodable PCSX2 fast kind, every seven-bit
// VIF1 command-table index, every UNPACK format ID, and every declared
// cross-cutting machine behavior has exactly one row. All rows name their
// PCSX2 owner and a concrete proof or retirement gate. Enum/manifest growth,
// duplicate IDs, and accidentally unclassified behavior therefore fail the
// validation suite instead of becoming a retail rejection bucket.
bool ValidateGpuVuArchitectureLedger(std::string* error = nullptr);

}  // namespace VitaGpuVu
