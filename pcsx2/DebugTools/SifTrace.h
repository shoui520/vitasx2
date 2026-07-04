// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	enum SifTraceKind
	{
		SifTraceKindDomainStatus = 0,
		SifTraceKindFifoData = 1,
		SifTraceKindFifoTag = 2,
	};

	enum SifTraceDirection
	{
		SifTraceDirectionIopToFifo = 0,
		SifTraceDirectionFifoToEe = 1,
		SifTraceDirectionEeToFifo = 2,
		SifTraceDirectionFifoToIop = 3,
	};

	enum SifTraceStatus
	{
		SifTraceStatusValid = 0,
		SifTraceStatusIncomplete = 1,
	};

	struct SifTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 skip_records = 0;
		bool wait_for_elf_entry = true;
	};

	bool StartSifTrace(const SifTraceConfig& config, Error* error = nullptr);
	void StopSifTrace();

	bool IsSifTraceEnabled();
	void NotifySifElfEntry(u32 pc);
	bool RecordSifTransfer(u8 kind, u8 channel, u8 direction, const void* data, u32 word_count,
		u32 ee_madr, u32 iop_madr, u32 ee_qwc, u32 iop_counter, u32 fifo_before, u32 fifo_after,
		u32 chcr, u32 tag, u32 tadr);

	u64 GetSifTraceRecordsWritten();
	bool DidSifTraceHitLimit();
	const std::string& GetSifTraceError();
} // namespace Pcsx2Trace
