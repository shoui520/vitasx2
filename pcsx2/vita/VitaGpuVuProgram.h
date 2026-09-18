// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "vita/VitaVuBlockCompiler.h"

#include <limits>
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
  // Canonical loop forest derived only from dominator/backedge structure.
  // The generated GPU compiler uses this to retain nested serial control
  // inside one outer parallel/fused invocation instead of turning every basic
  // block into a separate GXM job.
  u32 parent_loop = std::numeric_limits<u32>::max();
  std::vector<u32> child_loops;
  u32 nesting_depth = 0;
  u32 pair_count = 0;
  u32 qword_store_count = 0;
  u32 xgkick_count = 0;
  u32 counter_reg = 0;
  // Zero for unary/count-to-zero loops; otherwise the invariant VI operand
  // compared with counter_reg by IBEQ/IBNE.
  u32 counter_limit_reg = 0;
  s32 counter_step = 0;
  u8 branch_kind = 0;
  bool single_entry = false;
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

// Reducible control schedule shared by the fixed-machine successor and the
// generated GPU JIT.  Blocks belong to their innermost loop; a parent loop's
// exclusive list therefore describes only the serial work surrounding child
// invocations.  This is structural metadata only: runtime VI values still
// choose trip counts and every generated root must retain a pair/output budget.
struct StructuredControlLoop {
  u32 loop_index = 0;
  u32 parent_loop = std::numeric_limits<u32>::max();
  u32 header_block = 0;
  u32 latch_block = 0;
  u32 nesting_depth = 0;
  u32 counter_reg = 0;
  u32 counter_limit_reg = 0;
  s32 counter_step = 0;
  u32 exclusive_pair_count = 0;
  u32 exclusive_qword_store_count = 0;
  u32 exclusive_xgkick_count = 0;
  std::vector<u32> child_loops;
  std::vector<u32> exclusive_blocks;
};

struct StructuredControlPlan {
  u32 entry_block = 0;
  u32 reachable_pair_count = 0;
  u32 qword_store_sites = 0;
  u32 xgkick_sites = 0;
  u32 maximum_nesting_depth = 0;
  std::vector<u32> top_level_blocks;
  std::vector<u32> block_innermost_loop;
  std::vector<StructuredControlLoop> loops;
  bool has_nested_loops = false;
  bool single_invocation_control_proven = false;
};

// Reconstructs VU1 control flow from PairPlan-decoded pairs. Branch pairs
// retain their architectural delay slot in the terminating block, an E pair
// retains the one following pair before ProgramExit, and finite PairPlan-proven
// VI link values turn JR/JALR into ordinary target edges.
bool AnalyzeGpuVu1Program(const u8 *micro, u32 micro_size, u32 start_pc,
                          ProgramAnalysis *analysis, std::string *error);

// Command epochs analyze against the same immutable scheduling/QP contract
// serialized into UniversalPairMicroOp. This prevents live EmuConfig changes
// from making static reachability and the submitted PairPlans disagree.
bool AnalyzeGpuVu1ProgramForConfiguration(
    const u8* micro, u32 micro_size, u32 start_pc, bool assume_scheduled,
    bool instant_qp, ProgramAnalysis* analysis, std::string* error);

// Proves that every reachable cycle is one of the canonical single-entry,
// affine-counter natural loops and that deleting only those latch backedges
// leaves a DAG.  No source identity participates.  A successful plan permits
// a generated shader to retain an arbitrary nested loop forest in one GXM
// invocation; it does not by itself prove value, memory-alias, output-capacity,
// or watchdog safety.
bool BuildStructuredControlPlan(const ProgramAnalysis& analysis,
                                StructuredControlPlan* plan,
                                std::string* error = nullptr);
} // namespace VitaGpuVu
