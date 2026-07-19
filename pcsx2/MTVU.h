// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/Threading.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"

#include <thread>

#define MTVU_LOG(...) do{} while(0)
//#define MTVU_LOG DevCon.WriteLn

// Notes:
// - This class should only be accessed from the EE thread...
// - buffer_size must be power of 2
// - ring-buffer has no complete pending packets when read_pos==write_pos
class VU_Thread final {
	static const s32 buffer_size = (_1mb * 16) / sizeof(s32);

	u32 buffer[buffer_size];
	// Note: keep atomic on separate cache line to avoid CPU conflict
	alignas(__cachelinesize) std::atomic<int> m_ato_read_pos; // Only modified by VU thread
	alignas(__cachelinesize) std::atomic<int> m_ato_write_pos;    // Only modified by EE thread
	alignas(__cachelinesize) int  m_read_pos; // temporary read pos (local to the VU thread)
	int  m_write_pos; // temporary write pos (local to the EE thread)
	Threading::WorkSema semaEvent;
	std::atomic_bool m_shutdown_flag{false};
	// Worker-private execution state. PCSX2's x86 microVU exits generated
	// code directly and therefore never consults the EE-owned VPU_STAT busy
	// bit while MTVU is active. Vita's block dispatcher needs the equivalent
	// private stop condition without racing the EE core.
	bool m_program_active = false;
	bool m_dt_program_end = false;
	u32 m_pending_program_interrupts = 0;
	// Producer-only counters. Keeping these on the EE side avoids adding an
	// atomic RMW to every VU program merely for hardware telemetry.
	u64 m_profile_execute_enqueues = 0;
	u64 m_profile_wait_calls = 0;
	u64 m_profile_ring_waits = 0;
	u64 m_profile_ring_wait_spins = 0;
	u64 m_profile_compile_barriers = 0;
	u64 m_profile_queue_submissions = 0;
	u64 m_profile_queue_words = 0;
	u64 m_profile_pending_words = 0;
	bool m_micro_write_pending = false;
	u32 m_micro_invalidate_start = 0;
	u32 m_micro_invalidate_end = 0;

	Threading::Thread m_thread;

public:
	struct ProducerProfileStats
	{
		u64 execute_enqueues;
		u64 wait_calls;
		u64 ring_waits;
		u64 ring_wait_spins;
		u64 compile_barriers;
		u64 queue_submissions;
		u64 queue_words;
	};

	alignas(16)  vifStruct        vif;
	alignas(16)  VIFregisters     vifRegs;
	Threading::UserspaceSemaphore semaXGkick;
	std::atomic<unsigned int> vuCycles[4]; // Used for VU cycle stealing hack
	u32 vuCycleIdx;  // Used for VU cycle stealing hack
	u32 vuFBRST;

	enum InterruptFlag {
		InterruptFlagFinish = 1 << 0,
		InterruptFlagSignal = 1 << 1,
		InterruptFlagLabel  = 1 << 2,
		InterruptFlagVUEBit = 1 << 3,
		InterruptFlagVUTBit = 1 << 4,
	};

	std::atomic<u32> mtvuInterrupts; // Used for GS Signal, Finish etc, plus VU End/T-Bit
	std::atomic<u64> gsLabel; // Used for GS Label command
	std::atomic<u64> gsSignal; // Used for GS Signal command

	VU_Thread();
	~VU_Thread();

	__fi const Threading::ThreadHandle& GetThreadHandle() const { return m_thread; }

	/// Returns true if the VU thread has been started.
	__fi bool IsOpen() const { return m_thread.Joinable(); }

	/// Ensures the VU thread is started.
	void Open();

	/// Shuts down the VU thread if it is currently running.
	void Close();

	void Reset();

	// Get MTVU to start processing its packets if it isn't already
	void KickStart();

	// Used for assertions...
	bool IsDone();

	// Waits till MTVU is done processing
	void WaitVU();

	void Get_MTVUChanges();
	__fi ProducerProfileStats GetProducerProfileStats() const
	{
		return {m_profile_execute_enqueues, m_profile_wait_calls,
			m_profile_ring_waits, m_profile_ring_wait_spins,
			m_profile_compile_barriers, m_profile_queue_submissions,
			m_profile_queue_words};
	}

	// These methods are VU-worker-only. Release publication in EndProgram()
	// makes completed VU state visible before the EE observes its event bit.
	__fi bool IsProgramActive() const { return m_program_active; }
	void BeginProgram();
	void MarkDtProgramEnd(u32 interrupt_flag);
	void EndProgram(u32 interrupt_flag);

	void ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst);

	void VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size);

	// Writes to VU's Micro Memory (size in bytes)
	void WriteMicroMem(u32 vu_micro_addr, const void* data, u32 size);

	// Writes to VU's Data Memory (size in bytes)
	void WriteDataMem(u32 vu_data_addr, const void* data, u32 size);

	void WriteVIRegs(REG_VI* viRegs);

	void WriteVFRegs(VECTOR* vfRegs);

	void WriteCol(vifStruct& _vif);

	void WriteRow(vifStruct& _vif);

private:
	void ExecuteRingBuffer();

	void WaitOnSize(s32 size);
	void ReserveSpace(s32 size);

	s32 GetReadPos();
	s32 GetWritePos();

	u32* GetWritePtr();

	void CommitWritePos();
	void CommitReadPos();

	u32 Read();
	void Read(void* dest, u32 size);
	void ReadRegs(VIFregisters* dest);

	void Write(u32 val);
	void Write(const void* src, u32 size);
	void WriteRegs(VIFregisters* src);

	u32 Get_vuCycles();
	void PrepareVuCodeForExecute(s32 vu_addr);
};

extern VU_Thread vu1Thread;
