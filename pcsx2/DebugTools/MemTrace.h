// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	enum MemTraceRegion
	{
		MemTraceRegionEeRam = 0,
		MemTraceRegionIopRam = 1,
		MemTraceRegionEeScratchpad = 2,
		MemTraceRegionVu0Micro = 3,
		MemTraceRegionVu0Data = 4,
		MemTraceRegionVu1Micro = 5,
		MemTraceRegionVu1Data = 6,
		MemTraceRegionSpu2Ram = 7,
		MemTraceRegionGsLocal = 8,
	};

	static constexpr u32 MemTraceRegionMaskEeRam = 1u << MemTraceRegionEeRam;
	static constexpr u32 MemTraceRegionMaskIopRam = 1u << MemTraceRegionIopRam;
	static constexpr u32 MemTraceRegionMaskEeScratchpad = 1u << MemTraceRegionEeScratchpad;
	static constexpr u32 MemTraceRegionMaskVu0Micro = 1u << MemTraceRegionVu0Micro;
	static constexpr u32 MemTraceRegionMaskVu0Data = 1u << MemTraceRegionVu0Data;
	static constexpr u32 MemTraceRegionMaskVu1Micro = 1u << MemTraceRegionVu1Micro;
	static constexpr u32 MemTraceRegionMaskVu1Data = 1u << MemTraceRegionVu1Data;
	static constexpr u32 MemTraceRegionMaskSpu2Ram = 1u << MemTraceRegionSpu2Ram;
	static constexpr u32 MemTraceRegionMaskGsLocal = 1u << MemTraceRegionGsLocal;
	static constexpr u32 MemTraceAllRegionMask =
		MemTraceRegionMaskEeRam | MemTraceRegionMaskIopRam |
		MemTraceRegionMaskEeScratchpad | MemTraceRegionMaskVu0Micro |
		MemTraceRegionMaskVu0Data | MemTraceRegionMaskVu1Micro |
		MemTraceRegionMaskVu1Data | MemTraceRegionMaskSpu2Ram |
		MemTraceRegionMaskGsLocal;
	static constexpr u32 MemTraceDefaultRegionMask =
		MemTraceRegionMaskEeRam | MemTraceRegionMaskIopRam |
		MemTraceRegionMaskEeScratchpad | MemTraceRegionMaskVu0Data;
	static constexpr u64 MemTraceDefaultHashInterval = 10000;

	enum MemTraceRecordStatus
	{
		MemTraceRecordValid = 0,
		MemTraceRecordIncomplete = 1,
	};

	struct MemTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 max_instruction_records = 0;
		u64 skip_records = 0;
		u64 sample_interval = MemTraceDefaultHashInterval;
		u32 region_mask = MemTraceDefaultRegionMask;
		bool wait_for_elf_entry = true;
	};

	bool StartMemTrace(const MemTraceConfig& config, Error* error = nullptr);
	void StopMemTrace();

	bool IsMemTraceEnabled();
	bool RecordMemPreEeInstruction(u32 pc);
	void NotifyMemElfEntry(u32 pc);

	u64 GetMemTraceRecordsWritten();
	bool DidMemTraceHitLimit();
	const std::string& GetMemTraceError();
} // namespace Pcsx2Trace
