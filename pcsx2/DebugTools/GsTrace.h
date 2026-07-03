// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <string>

class Error;

namespace Pcsx2Trace
{
	enum GsTraceSource
	{
		GsTraceSourcePath1 = 0,
		GsTraceSourcePath2 = 1,
		GsTraceSourcePath3 = 2,
		GsTraceSourcePrivileged = 3,
	};

	enum GsTraceKind
	{
		GsTraceKindRegisterWrite = 0,
		GsTraceKindImageTransfer = 1,
		GsTraceKindRawTransfer = 2,
		GsTraceKindVSync = 3,
		GsTraceKindStateHash = 4,
	};

	enum GsTraceVSyncPhase
	{
		GsTraceVSyncStart = 0,
		GsTraceVSyncGsBlank = 1,
		GsTraceVSyncEnd = 2,
	};

	enum GsTraceStateSection
	{
		GsTraceStateAll = 0,
		GsTraceStateFreeze = 1,
		GsTraceStatePrivilegedRegisters = 2,
		GsTraceStateDrawingEnvironment = 3,
		GsTraceStateVertexRegisters = 4,
		GsTraceStateTransfer = 5,
		GsTraceStateGifPaths = 6,
		GsTraceStateLocalMemory = 7,
	};

	enum GsTraceStateStatus
	{
		GsTraceStateValid = 0,
		GsTraceStateIncomplete = 1,
	};

	enum GsTraceStateTrigger
	{
		GsTraceStateTriggerManual = 0,
		GsTraceStateTriggerVSyncStart = 1,
		GsTraceStateTriggerGsBlank = 2,
		GsTraceStateTriggerVSyncEnd = 3,
		GsTraceStateTriggerTransfer = 4,
		GsTraceStateTriggerPrivilegedWrite = 5,
	};

	struct GsTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 max_instruction_records = 0;
		u64 skip_records = 0;
		bool wait_for_elf_entry = true;
		bool state_snapshots = false;
		bool state_full_dumps = false;
	};

	bool StartGsTrace(const GsTraceConfig& config, Error* error = nullptr);
	void StopGsTrace();

	bool IsGsTraceEnabled();
	bool RecordGsPreEeInstruction(u32 pc);
	void NotifyGsElfEntry(u32 pc);

	bool RecordGsRegisterWrite(u8 source, u8 reg, u8 ctx, u32 low, u32 high);
	bool RecordGsImageTransfer(u8 source, u32 dbp, u32 dbw, u32 dpsm, u32 rrw, u32 rrh,
		const void* data, size_t size);
	bool RecordGsRawTransfer(u8 source, const void* data, size_t size);
	bool RecordGsVSync(u8 phase, u64 ee_cycle);
	bool RecordGsStateHash(u8 section, u8 status, u8 trigger, u64 byte_count,
		u64 data_hash);
	bool RecordGsStateBytes(u8 section, u8 status, u8 trigger, const void* data,
		size_t size, u64 data_hash);
	bool RecordGsPrivilegedWrite(u32 physical, u32 low, u32 high, bool is_64_bit);
	bool IsGsStateTraceEnabled();
	bool IsGsStateFullTraceEnabled();
	u64 HashGsTraceBytes(const void* data, size_t size);

	u64 GetGsTraceRecordsWritten();
	bool DidGsTraceHitLimit();
	const std::string& GetGsTraceError();
} // namespace Pcsx2Trace
