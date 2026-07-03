// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;
struct StereoOut32;

namespace Pcsx2Trace
{
	enum Spu2TraceKind
	{
		Spu2TraceKindDomainStatus = 0,
		Spu2TraceKindOutputSample = 1,
	};

	enum Spu2TraceStatus
	{
		Spu2TraceStatusValid = 0,
		Spu2TraceStatusIncomplete = 1,
	};

	struct Spu2TraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 skip_records = 0;
		bool wait_for_elf_entry = true;
	};

	bool StartSpu2Trace(const Spu2TraceConfig& config, Error* error = nullptr);
	void StopSpu2Trace();

	bool IsSpu2TraceEnabled();
	void NotifySpu2ElfEntry(u32 pc);
	bool RecordSpu2OutputSample(u64 sample_index, const StereoOut32& core0,
		const StereoOut32& core1, const StereoOut32& output);

	u64 GetSpu2TraceRecordsWritten();
	bool DidSpu2TraceHitLimit();
	const std::string& GetSpu2TraceError();
} // namespace Pcsx2Trace
