// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;

namespace Pcsx2Trace
{
	// DEV9 and FireWire are not yet part of the portable replay schema.  The
	// bounded replay frontends therefore admit a continuation only when neither
	// device is touched after the restored start seam.  These counters are armed
	// explicitly by those frontends; ordinary execution remains unobserved.
	enum class PortableReplayExternalDeviceAccess : u8
	{
		Dev9Read,
		Dev9Write,
		Dev9Dma,
		Dev9IrqScheduled,
		Dev9IrqDelivered,
		FireWireRead,
		FireWireWrite,
		FireWireIrq,
	};

	struct PortableReplayExternalDeviceAccessCounts
	{
		u64 dev9_reads = 0;
		u64 dev9_writes = 0;
		u64 dev9_dma = 0;
		u64 dev9_irq_scheduled = 0;
		u64 dev9_irq_delivered = 0;
		u64 firewire_reads = 0;
		u64 firewire_writes = 0;
		u64 firewire_irq = 0;

		bool IsZero() const
		{
			return dev9_reads == 0 && dev9_writes == 0 && dev9_dma == 0 &&
				dev9_irq_scheduled == 0 && dev9_irq_delivered == 0 &&
				firewire_reads == 0 && firewire_writes == 0 && firewire_irq == 0;
		}
	};

	void BeginPortableReplayExternalDeviceAccessWindow();
	void EndPortableReplayExternalDeviceAccessWindow();
	void NotifyPortableReplayExternalDeviceAccess(PortableReplayExternalDeviceAccess access);
	PortableReplayExternalDeviceAccessCounts GetPortableReplayExternalDeviceAccessCounts();

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
		// Gate eligible VU1 completions until this many VSyncEnd transitions
		// have completed since the trace became active (normally ELF entry).
		// This is a temporal gate; workload progress must be corroborated by the
		// captured architectural, VU, device, and memory state.
		u64 after_vsync_frames = 0;
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
	// Projection v5 compares published architecture, all EE TLB entries, counter
	// continuation state, canonical active pipelines, and logical SIF/VIF transfer
	// continuation. It also records the completed VSync-frame ordinal used by the
	// provider-independent temporal gate. A record is emitted only while both VUs
	// are idle, so provider-private branch/backup state is not continuation-semantic
	// at the checkpoint.
	bool RecordPendingMachineCheckpointAtEventTest();

	// Validation-only replay boundary. The caller must have restored a portable
	// replay state at a PCSX2-owned quiescent seam and must call this before any
	// guest instruction executes. Unlike the VU-completion route, this record is
	// not subject to workload gates or skip_records.
	bool RecordMachineCheckpointAtReplayStart();

	u64 GetMachineCheckpointTraceRecordsWritten();
	bool DidMachineCheckpointTraceHitLimit();
	const std::string& GetMachineCheckpointTraceError();
} // namespace Pcsx2Trace
