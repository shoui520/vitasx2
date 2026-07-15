// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	struct EeTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 skip_records = 0;
		u64 index_offset = 0;
		bool wait_for_elf_entry = true;
		// Capture one complete EE trace-schema record at the final ELF-entry
		// owner boundary. The snapshot consumes record zero; a following
		// pre-instruction hook for the same entry instruction is de-duplicated.
		bool record_elf_entry_state = false;
	};

	bool StartEeTrace(const EeTraceConfig& config, Error* error = nullptr);
	void StopEeTrace();

	bool IsEeTraceEnabled();
	bool RecordEePreInstruction(u32 pc, u32 opcode);
	void NotifyEeElfEntry(u32 pc);
	void RecordEeElfEntryState(u32 pc);

	u64 GetEeTraceRecordsWritten();
	bool DidEeTraceHitLimit();
	const std::string& GetEeTraceError();
} // namespace Pcsx2Trace
