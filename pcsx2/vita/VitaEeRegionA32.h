// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/vita/VitaEeRegionIR.h"

#include <array>
#include <cstddef>

namespace VitaA32
{
	class CodeBuffer;
}

namespace VitaEE::RegionA32
{
	// This is a private generated-code ABI. It is intentionally independent of
	// cpuRegisters so the product-disabled Phase 3 gate can prove every field a
	// region observes or publishes before it is connected to the EE provider.
	struct ExecutionResult
	{
		u32 completed = 0;
		u32 reason = static_cast<u32>(RegionIR::ExitReason::RegionBoundary);
		u32 cycle_commit_deferred = 0;
		u32 pending_raw_cycles = 0;
		u32 memory_address = 0;
	};

	struct ExecutionContext
	{
		RegionIR::CanonicalState* state = nullptr;
		// Raw PCSX2 VTLBVirtual table and its additive host-memory base. The
		// generated load path duplicates VTLBVirtual::{isHandler,assumePtr}.
		const u32* vmap = nullptr;
		const u8* host_memory_base = nullptr;
		u8* main_ram = nullptr;
		// Inclusive last host address at which a 32-bit main-RAM read can begin.
		const u8* main_ram_last_word = nullptr;
		// Nonzero only while PCSX2's default low-main-RAM identity proof holds.
		u32 identity_main_ram_limit = 0;
		u32 next_event_cycle_low = UINT32_MAX;
		u32 next_event_cycle_high = UINT32_MAX;
		ExecutionResult result{};
	};

	enum class CompileFailure : u8
	{
		None,
		InvalidProgram,
		UnattestedSource,
		UnsupportedInstruction,
		UnsupportedMemory,
		RegisterPressure,
		CodeCapacity,
		Emission,
		Patch,
	};

	struct CompileOptions
	{
		u32 max_code_bytes = 2048;
		u8 max_mapped_gprs = 4;
	};

	struct CompileResult
	{
		CompileFailure failure = CompileFailure::None;
		u32 failure_pc = 0;
		u32 code_bytes = 0;
		u32 hot_code_bytes = 0;
		u32 cold_code_bytes = 0;
		u32 host_instructions = 0;
		u32 memory_loads = 0;
		u8 mapped_gpr_count = 0;
		std::array<u8, 4> mapped_gprs{};

		explicit operator bool() const { return failure == CompileFailure::None; }
	};

	using GeneratedRegion = u32 (*)(ExecutionContext* context);

	CompileResult Compile(const RegionIR::Program& program,
		VitaA32::CodeBuffer& code,
		const CompileOptions& options = {});
	const char* CompileFailureName(CompileFailure failure);
} // namespace VitaEE::RegionA32
