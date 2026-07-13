// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	// Stable order of the memory hashes stored in every machine checkpoint.
	// Keep this independent of the host memory layout: each entry names PS2
	// state rather than a C++ allocation.
	enum MachineCheckpointMemoryRegion : u32
	{
		MachineCheckpointMemoryEeRam = 0,
		MachineCheckpointMemoryIopRam = 1,
		MachineCheckpointMemoryEeScratchpad = 2,
		MachineCheckpointMemoryVu0Micro = 3,
		MachineCheckpointMemoryVu0Data = 4,
		MachineCheckpointMemoryVu1Micro = 5,
		MachineCheckpointMemoryVu1Data = 6,
		MachineCheckpointMemoryEeHardware = 7,
		MachineCheckpointMemoryIopHardware = 8,
		MachineCheckpointMemorySpu2Ram = 9,
		MachineCheckpointMemoryGsLocal = 10,
		MachineCheckpointMemoryRegionCount = 11,
	};

	struct MachineCheckpointTraceConfig
	{
		std::string output_path;
		// Validation-only raw state snapshots. When non-empty, one bounded set of
		// EE RAM, IOP RAM, and projection-detail files is written per checkpoint.
		std::string diagnostic_dump_directory;
		u64 max_records = 1;
		u64 skip_records = 0;
		u64 after_sif_records = 0;
		u64 after_vif_records = 0;
		bool wait_for_elf_entry = true;
	};

	bool StartMachineCheckpointTrace(const MachineCheckpointTraceConfig& config,
		Error* error = nullptr);
	void StopMachineCheckpointTrace();

	bool IsMachineCheckpointTraceEnabled();
	void NotifyMachineCheckpointElfEntry(u32 pc);

	// vu1ExecMicro() marks the architectural beginning of a microprogram. The
	// interpreter and native providers report a completion only after their
	// public VU state has been written back and VPU_STAT is no longer busy.
	void NotifyMachineCheckpointVu1ProgramStarted();
	void NotifyMachineCheckpointVu1ExecutionCompleted();

	// A VU provider can finish while an EE event test is still mutating IOP and
	// device state. Snapshot only after _cpuEventTest_Shared() has completed.
	// Projection v4 compares published architecture, all EE TLB entries, counter
	// continuation state, canonical active pipelines, and logical SIF/VIF transfer
	// continuation. A record is emitted only while both VUs are idle, so provider-
	// private branch/backup state is not continuation-semantic at the checkpoint.
	bool RecordPendingMachineCheckpointAtEventTest();

	u64 GetMachineCheckpointTraceRecordsWritten();
	bool DidMachineCheckpointTraceHitLimit();
	const std::string& GetMachineCheckpointTraceError();
} // namespace Pcsx2Trace
