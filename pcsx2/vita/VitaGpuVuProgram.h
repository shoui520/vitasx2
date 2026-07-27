// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "vita/VitaVuBlockCompiler.h"

#include <string>
#include <vector>

namespace VitaGpuVu {
enum class ControlEdgeKind : u8 {
  Fallthrough,
  BranchTaken,
  BranchNotTaken,
  ResolvedIndirect,
  ProgramExit,
  ExternalExit,
};

struct ControlEdge {
  ControlEdgeKind kind = ControlEdgeKind::Fallthrough;
  u32 target_pc = 0;
  u32 target_block = 0;
  bool has_target = false;
};

struct ProgramPair {
  VitaVU::GpuPairPlan plan;
  bool delayed_pair = false;
};

struct BasicBlock {
  u32 start_pc = 0;
  std::vector<ProgramPair> pairs;
  std::vector<ControlEdge> successors;
  std::vector<u32> predecessors;
  u32 branch_pc = 0;
  u8 branch_kind = 0;
  bool has_branch = false;
  bool conditional_branch = false;
  bool indirect_branch = false;
  bool branch_in_delay_slot = false;
  bool ends_program = false;
  // PCSX2/VU owner: an E-bit retires one architectural delay pair and
  // publishes the byte PC immediately after it for a later VIF MSCNT.
  u32 resume_pc = 0;
  bool has_resume_pc = false;
  bool has_external_exit = false;
  // True only when control can arrive here from this analysis' external entry.
  // BuildBlocks decodes the whole 16 KiB image, so one image analyzed at an
  // explicit MSCAL address and at a later MSCNT resume address shares block
  // storage but not reachability. Consumers which reason about what this entry
  // actually executes must filter on this; a VU1 image routinely contains
  // prologue blocks a resume entry deliberately branches past.
  bool reachable_from_entry = false;
};

struct NaturalLoop {
  u32 header_block = 0;
  u32 latch_block = 0;
  std::vector<u32> blocks;
  u32 counter_reg = 0;
  s32 counter_step = 0;
  u8 branch_kind = 0;
  bool branch_taken_repeats = false;
  bool affine_counter = false;
};

struct ProgramAnalysis {
  u32 start_pc = 0;
  std::vector<BasicBlock> blocks;
  std::vector<NaturalLoop> natural_loops;
  u32 resolved_indirect_edges = 0;
  bool complete_cfg = false;
  bool has_unresolved_indirect_control = false;
  bool has_branch_in_delay_slot = false;
  bool has_external_exit = false;
  bool has_program_exit = false;
  bool every_block_can_reach_program_exit = false;
  // Unique, sorted post-E TPCs reachable from this external entry.
  std::vector<u32> resume_pcs;
};

// Reconstructs VU1 control flow from PairPlan-decoded pairs. Branch pairs
// retain their architectural delay slot in the terminating block, an E pair
// retains the one following pair before ProgramExit, and finite PairPlan-proven
// VI link values turn JR/JALR into ordinary target edges.
bool AnalyzeGpuVu1Program(const u8 *micro, u32 micro_size, u32 start_pc,
                          ProgramAnalysis *analysis, std::string *error);
} // namespace VitaGpuVu
