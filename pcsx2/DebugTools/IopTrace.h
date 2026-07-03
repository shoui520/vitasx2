// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	struct IopTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 skip_records = 0;
	};

	bool StartIopTrace(const IopTraceConfig& config, Error* error = nullptr);
	void StopIopTrace();

	bool IsIopTraceEnabled();
	bool RecordIopPreInstruction(u32 pc, u32 opcode);

	u64 GetIopTraceRecordsWritten();
	bool DidIopTraceHitLimit();
	const std::string& GetIopTraceError();
} // namespace Pcsx2Trace
