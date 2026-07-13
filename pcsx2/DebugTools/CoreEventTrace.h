// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>

class Error;

namespace Pcsx2Trace
{
	enum CoreEventTraceExecutionProvider : u32
	{
		CoreEventTraceExecutionEeRecompiler = 1u << 0,
		CoreEventTraceExecutionIopRecompiler = 1u << 1,
		CoreEventTraceExecutionVu0Recompiler = 1u << 2,
		CoreEventTraceExecutionVu1Recompiler = 1u << 3,
	};

	enum class CoreEventKind : u8
	{
		DomainStatus = 0,
		Scheduler = 1,
		Event = 2,
		Sif = 3,
		Dmac = 4,
		Irq = 5,
	};

	enum class CoreEventPhase : u8
	{
		Enter = 1,
		Before = 2,
		After = 3,
		Schedule = 4,
		DispatchBegin = 5,
		DispatchEnd = 6,
		Exit = 7,
		Executed = 8,
		Queued = 9,
		Ignored = 10,
	};

	enum class CoreEventDomain : u8
	{
		Ee = 1,
		Iop = 2,
		Sif0 = 3,
		Sif1 = 4,
		Sio2 = 5,
	};

	enum class CoreEventId : u8
	{
		None = 0,
		EeDmacSif0 = 1,
		EeDmacSif1 = 2,
		IopSif0 = 3,
		IopSif1 = 4,
	};

	struct CoreEventTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 skip_records = 0;
		u64 after_sif_records = 0;
		u32 execution_provider_mask = 0;
		bool wait_for_elf_entry = true;
	};

	bool StartCoreEventTrace(const CoreEventTraceConfig& config,
		Error* error = nullptr);
	void StopCoreEventTrace();

	bool IsCoreEventTraceEnabled();
	void NotifyCoreEventElfEntry(u32 pc);
	bool RecordCoreEvent(CoreEventKind kind, CoreEventPhase phase,
		CoreEventDomain domain, CoreEventId event_id,
		u64 target_cycle, const std::array<u32, 11>& fields);
	bool RecordCoreEvent(CoreEventKind kind, CoreEventPhase phase,
		CoreEventDomain domain, CoreEventId event_id,
		u64 target_cycle, u32 field0, u32 field1, u32 field2,
		u32 field3, u32 field4, u32 field5, u32 field6, u32 field7,
		u32 field8, u32 field9, u32 field10);

	u64 GetCoreEventTraceRecordsWritten();
	bool DidCoreEventTraceHitLimit();
	const std::string& GetCoreEventTraceError();
} // namespace Pcsx2Trace
