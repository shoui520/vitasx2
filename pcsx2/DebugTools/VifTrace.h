// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	enum VifTraceKind
	{
		VifTraceKindDomainStatus = 0,
		VifTraceKindCommand = 1,
		VifTraceKindUnpack = 2,
	};

	enum VifTraceStatus
	{
		VifTraceStatusValid = 0,
		VifTraceStatusIncomplete = 1,
	};

	struct VifTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 skip_records = 0;
		bool wait_for_elf_entry = true;
	};

	bool StartVifTrace(const VifTraceConfig& config, Error* error = nullptr);
	void StopVifTrace();

	bool IsVifTraceEnabled();
	void NotifyVifElfEntry(u32 pc);
	bool RecordVifCommand(u8 unit, u32 code, u32 stat, u32 cycle, u32 mode,
		u32 num, u32 mask, u32 tag_addr, u32 tag_size, u32 packet_size,
		u32 madr, u32 qwc);
	bool RecordVifUnpack(u8 unit, u32 code, u32 stat, u32 cycle, u32 mode,
		u32 num, u32 mask, u32 tag_addr, u32 tag_size, u32 packet_size,
		u32 cl, u32 wl, const void* source_data, u32 source_bytes,
		const void* vu_mem, u32 vu_mem_bytes);

	u64 GetVifTraceRecordsWritten();
	bool DidVifTraceHitLimit();
	const std::string& GetVifTraceError();
} // namespace Pcsx2Trace
