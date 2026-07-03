// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;
struct VURegs;

namespace Pcsx2Trace
{
	enum VuTraceKind
	{
		VuTraceKindDomainStatus = 0,
		VuTraceKindMicroStep = 1,
	};

	enum VuTraceStatus
	{
		VuTraceStatusValid = 0,
		VuTraceStatusIncomplete = 1,
	};

	static constexpr u32 VuTraceUnitMaskVu0 = 1u << 0;
	static constexpr u32 VuTraceUnitMaskVu1 = 1u << 1;
	static constexpr u32 VuTraceUnitMaskBoth = VuTraceUnitMaskVu0 | VuTraceUnitMaskVu1;

	struct VuTraceConfig
	{
		std::string output_path;
		u64 max_records = 0;
		u64 max_instruction_records = 0;
		u64 skip_records = 0;
		u32 unit_mask = VuTraceUnitMaskBoth;
		bool wait_for_elf_entry = true;
	};

	bool StartVuTrace(const VuTraceConfig& config, Error* error = nullptr);
	void StopVuTrace();

	bool IsVuTraceEnabled();
	bool RecordVuPreEeInstruction(u32 pc);
	void NotifyVuElfEntry(u32 pc);
	bool RecordVuMicroStep(u8 unit, u16 vu_pc, u32 upper_op, u32 lower_op,
		const VURegs& vu);

	u64 GetVuTraceRecordsWritten();
	bool DidVuTraceHitLimit();
	const std::string& GetVuTraceError();
} // namespace Pcsx2Trace
