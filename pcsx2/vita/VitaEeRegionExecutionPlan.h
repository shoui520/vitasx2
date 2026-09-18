// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/VitaEeRegionIR.h"

#include <array>
#include <bitset>
#include <string>
#include <vector>

namespace VitaEE::RegionExecution
{
	enum class StateClass : u8
	{
		Gpr,
		Hi,
		Lo,
		Sa,
		Fpr,
		Fcr0,
		Fcr31,
		Acc,
		AccFlag,
		Vu0Vf,
		Vu0Acc,
		Vu0MacFlag,
		Vu0StatusFlag,
		Vu0ClipFlag,
		Vu0Q,
		Vu0Vi,
		Vu0MicroMacFlag,
		Vu0MicroClipFlag,
		Vu0MicroStatusFlag,
		Cycle,
	};

	struct StateSlot
	{
		StateClass state_class = StateClass::Gpr;
		u8 index = 0;
	};

	constexpr size_t STATE_SLOT_COUNT = 153;
	using StateMask = std::bitset<STATE_SLOT_COUNT>;
	using StateWordMasks = std::array<u8, STATE_SLOT_COUNT>;

	// Exact relationship between the first two architectural words of a value.
	// This is semantic dataflow owned by the verified execution plan, not a host
	// allocation hint: a backend may omit word one only when it also preserves the
	// corresponding entry and exit contracts.
	enum class Low32Extension : u8
	{
		None,
		Sign,
		Zero,
	};

	struct EntryLow32GuardCandidate
	{
		RegionIR::ValueId parameter = RegionIR::INVALID_VALUE;
		u16 state_slot = 0;
		Low32Extension extension = Low32Extension::None;
	};

	enum class ExitSiteKind : u8
	{
		EntryEvent,
		EntryFallback,
		BlockEntry,
		Guarded,
		Observer,
		Memory,
		Taken,
		NotTaken,
	};

	struct ExitContractView
	{
		const RegionIR::StateMap* state = nullptr;
		const RegionIR::Transfer* transfer = nullptr;
		u32 static_pc = 0;
		bool cycle_commit_deferred = false;
		u32 pending_raw_cycles = 0;
		bool event_horizon_check = false;
	};

	struct ExitSite
	{
		ExitSiteKind kind = ExitSiteKind::EntryEvent;
		u32 block = RegionIR::INVALID_BLOCK;
		u32 ordinal = 0;
		StateMask dirty_state{};
		// One bit per 32-bit architectural word. This refines dirty_state so a
		// scalar EE write never forces untouched upper GPR lanes through NEON or
		// back to canonical memory. Bits above StateWordCount(slot) are zero.
		StateWordMasks dirty_words{};
	};

	struct Plan
	{
		std::vector<ExitSite> exits;
		// Includes every dirty canonical binding and every exit-PC value. This is
		// the first allocator-facing use count; ordinary node and internal-edge
		// liveness are added by the region allocator rather than inferred by A32
		// emission.
		std::vector<u32> value_exit_uses;
		// Union of 32-bit words demanded from each value by all exit sites. The
		// allocator uses this to retain scalar fragments of architectural I128
		// values without pretending every low-half update is a full qword result.
		std::vector<u8> value_exit_word_uses;
		// For a Parameter value, one bit per word which the CFG fixed point proves
		// remains identical to that parameter's canonical architectural state slot
		// on every incoming edge. Allocators may rematerialize only these exact
		// words from canonical state; any changed predecessor clears the fact.
		std::vector<u8> value_canonical_parameter_words;
		// Verifier-backed scalar representation facts. Entry-block facts derived
		// from internal predecessors are conditional on the external canonical
		// value satisfying the matching candidate guard below. They do not become
		// allocation facts merely by appearing here.
		std::vector<Low32Extension> value_low32_extensions;
		// Entry-state predicates required before the corresponding extension fact
		// is unconditional. An empty mask means the fact is derived entirely from
		// executed IR (for example ADDIU or a narrow load).
		std::vector<StateMask> value_low32_entry_guards;
		std::vector<EntryLow32GuardCandidate> entry_low32_guard_candidates;
		u32 dirty_state_bindings = 0;
		u32 dirty_state_words = 0;
	};

	enum class BuildFailure : u8
	{
		None,
		InvalidProgram,
		InvalidStateValue,
		InvalidExitSite,
	};

	struct BuildResult
	{
		Plan plan{};
		BuildFailure failure = BuildFailure::None;
		u32 block = RegionIR::INVALID_BLOCK;
		u32 value = RegionIR::INVALID_VALUE;
		std::string detail;

		explicit operator bool() const { return failure == BuildFailure::None; }
	};

	StateSlot DecodeStateSlot(size_t slot);
	u8 StateWordCount(size_t slot);
	// Byte offset of one 32-bit component in RegionIR::CanonicalState. This is
	// the single mechanical layout contract used by allocated backends for entry
	// seeding and exit materialization; SIZE_MAX rejects an invalid slot/word.
	size_t CanonicalStateWordOffset(size_t slot, u8 word);
	RegionIR::ValueId StateValue(const RegionIR::StateMap& state, size_t slot);
	const RegionIR::Transfer* ResolveTransfer(
		const RegionIR::Program& program, const ExitSite& site);
	bool ResolveExitContract(const RegionIR::Program& program,
		const ExitSite& site, ExitContractView* contract);
	BuildResult Build(const RegionIR::Program& program);
} // namespace VitaEE::RegionExecution
