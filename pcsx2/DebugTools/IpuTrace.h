// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	enum IpuTraceKind
	{
		IpuTraceKindDomainStatus = 0,
		IpuTraceKindOutputWrite = 1,
		IpuTraceKindCommandComplete = 2,
	};

	enum IpuTraceStatus
	{
		IpuTraceStatusValid = 0,
		IpuTraceStatusIncomplete = 1,
	};

	struct IpuTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 skip_records = 0;
		bool wait_for_elf_entry = true;
	};

	bool StartIpuTrace(const IpuTraceConfig& config, Error* error = nullptr);
	void StopIpuTrace();

	bool IsIpuTraceEnabled();
	void NotifyIpuElfEntry(u32 pc);
	bool RecordIpuOutputWrite(u32 command_value, const void* data, u32 byte_count);
	bool RecordIpuCommandComplete(u32 command_value);

	u64 GetIpuTraceRecordsWritten();
	bool DidIpuTraceHitLimit();
	const std::string& GetIpuTraceError();
} // namespace Pcsx2Trace
