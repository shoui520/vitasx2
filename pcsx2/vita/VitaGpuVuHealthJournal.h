// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <cstddef>

namespace VitaGpuVu::HealthJournal
{
	inline constexpr char FilePath[] = "ux0:data/vitasx2/gpu-vu-health.bin";
	inline constexpr u32 RecordMagic = 0x4a485647u; // "GVHJ", little endian.
	inline constexpr u16 RecordVersion = 1u;
	inline constexpr size_t RecordSize = 4096u;
	inline constexpr size_t RecordCount = 2u;
	inline constexpr size_t FileSize = RecordSize * RecordCount;
	inline constexpr u32 InvalidSlot = ~u32{0};

	// All fields are plain scalars by design. The publishing functions copy them
	// only into atomics protected by the section's single-writer seqlock. They do
	// not allocate, lock, format text, read a clock, or wake the persistence
	// thread. First publication registers the calling thread with one thread-ID
	// query; later publications issue no syscalls. Owners supply timestamps.
	struct GxmCallState
	{
		u64 update_time_us = 0;
		u64 entered_time_us = 0;
		u64 returned_time_us = 0;
		u64 token = 0;
		u64 sequence_begin = 0;
		u64 sequence_end = 0;
		u64 scene = 0;
		u64 program_key_high = 0;
		u64 program_key_low = 0;
		uptr input_owner = 0;
		uptr output_address = 0;
		uptr notification_address = 0;
		uptr buffer0_address = 0;
		uptr buffer1_address = 0;
		uptr buffer2_address = 0;
		uptr buffer4_address = 0;
		uptr index_address = 0;
		s32 result = 0;
		u32 active = 0;
		u32 kind = 0;
		u32 submission_stage = 0;
		u32 program_abi = 0;
		u32 retirement_slot = InvalidSlot;
		u32 input_slot = InvalidSlot;
		u32 input_generation = 0;
		u32 input_first_qword = 0;
		u32 input_last_qword = 0;
		u32 object_count = 0;
		u32 index_count = 0;
		u32 private_transaction_count = 0;
		u32 output_bytes = 0;
		u32 output_maximum_write_word = 0;
		u32 notification_required = 0;
		u32 notification_observed = 0;
		u32 flags = 0;
	};

	struct MtgsState
	{
		u64 update_time_us = 0;
		u64 stage_started_time_us = 0;
		u64 ticket = 0;
		u64 sequence = 0;
		u64 scene = 0;
		u64 last_completion_sequence = 0;
		u32 stage = 0;
		u32 ring_read_position = 0;
		u32 ring_write_position = 0;
		u32 current_command = 0;
		u32 reservations_remaining = 0;
		u32 completion_read_position = 0;
		u32 completion_write_position = 0;
		u32 completion_pending = 0;
		u32 completion_logical_count = 0;
		u32 completion_physical_count = 0;
		u32 submission_marker_result = 0;
		u32 wake_reason_flags = 0;
		u32 flags = 0;
	};

	struct RetirementSlotState
	{
		u64 update_time_us = 0;
		u64 sequence_begin = 0;
		u64 sequence_end = 0;
		u64 scene = 0;
		uptr notification_address = 0;
		u32 submitted = 0;
		u32 stage = 0;
		u32 notification_required = 0;
		u32 notification_observed = 0;
		u32 draw_count = 0;
		u32 private_output_count = 0;
		u32 input_retention_count = 0;
		u32 allocation_count = 0;
		u32 flags = 0;
	};

	struct RetirementState
	{
		u64 update_time_us = 0;
		u32 stage = 0;
		u32 current_slot = InvalidSlot;
		u32 action = 0;
		u32 pass = 0;
		std::array<RetirementSlotState, 4> slots{};
	};

	struct NotificationState
	{
		u64 update_time_us = 0;
		uptr active_address = 0;
		u64 active_sequence = 0;
		uptr pending_address = 0;
		u64 pending_sequence = 0;
		u64 arm_count = 0;
		u64 poll_count = 0;
		u64 wake_count = 0;
		u64 deferral_count = 0;
		u32 stage = 0;
		u32 request = 0;
		u32 completed = 0;
		u32 active_required = 0;
		u32 active_observed = 0;
		u32 pending_required = 0;
		u32 pending_observed = 0;
		u32 flags = 0;
	};

	struct Cpu1State
	{
		u64 update_time_us = 0;
		u64 sequence_front = 0;
		u64 sequence_back = 0;
		u64 private_generation = 0;
		u32 stage = 0;
		u32 pending_physical_count = 0;
		u32 pending_logical_count = 0;
		u32 private_transaction_count = 0;
		u32 private_generation_valid = 0;
		u32 wait_poll_count = 0;
		u32 flags = 0;
	};

	struct LifecycleState
	{
		u64 update_time_us = 0;
		u64 suspend_count = 0;
		u64 resume_count = 0;
		u64 overlay_enter_count = 0;
		u64 overlay_leave_count = 0;
		u32 stage = 0;
		u32 system_ui_overlaid = 0;
		u32 app_state = 0;
		u32 flags = 0;
	};

	struct WriterStatus
	{
		u64 last_sequence = 0;
		u64 run_id = 0;
		u32 running = 0;
		u32 open_failures = 0;
		u32 write_failures = 0;
		u32 sync_failures = 0;
	};

	// Persistence-only observations. An active operation is an entry breadcrumb,
	// not proof that the kernel call is still executing (the writer may be
	// descheduled). Only the persistence thread publishes this section.
	enum class WriterOperation : u32
	{
		Open = 1, ReadPrevious, QueryThread, Encode, Write, Sync, Sleep, Close,
	};
	struct WriterActivity
	{
		u64 update_time_us = 0;
		u64 entered_time_us = 0;
		u64 returned_time_us = 0;
		u64 attempted_sequence = 0;
		u64 durable_sequence = 0;
		u64 durable_time_us = 0;
		u32 operation = 0;
		u32 active = 0;
		u32 target = 0;
		s32 result = 0;
	};
	void PublishWriterActivity(const WriterActivity& state);
	// Optional request/reply observer; empty token disables it. The token is
	// 32 hexadecimal digits (128 bits), nonzero, configured per capture.
	// Configure before VM/thread startup. The separate one-hertz UDP observer
	// uses only atomic snapshots, never file I/O, thread queries, or owner locks.
	bool ConfigureLiveProbeBeforeVmStart(const char* token_hex, u32 port);
	constexpr size_t LiveProbeRequestSize = 20u; // "GVHP" + 16 token bytes
	bool ValidateLiveProbeRequest(const u8* request, size_t size);
	enum class ProbeOperation : u32
	{
		Start = 1, Affinity, Priority, LoadModule, Init, Socket, Nonblocking,
		Bind, Receive, Encode, Send,
	};
	struct ProbeActivity
	{
		u64 update_time_us = 0, entered_time_us = 0, returned_time_us = 0;
		u64 received_requests = 0, rejected_requests = 0;
		u64 attempted_replies = 0, sent_replies = 0, last_reply_time_us = 0;
		u32 operation = 0, active = 0;
		s32 result = 0, error_number = 0;
	};
	void PublishProbeActivity(const ProbeActivity& state);
	// Successful CPU1-private no-output executions, not PATH1 completion credits.
	// PCSX2 MTVU.cpp::ExecuteRingBuffer completes even an empty GIF packet.
	// These counters observe the Vita routes; they never supply that completion.
	enum class NoOutputExecutionKind : u32
	{
		StateFormula, PrivateStateLoad, PrivateGeneral, Count,
	};
	void CountNoOutputExecution(NoOutputExecutionKind kind);

	// CPU0-only publisher. Fields describe a host wait, never guest state.
	struct Cpu0State
	{
		u64 update_time_us = 0;
		u64 stage_started_time_us = 0;
		u64 wait_visits = 0;
		u64 execute_enqueued = 0;
		u64 execute_completed = 0;
		u32 stage = 0;
		u32 ring_read = 0;
		u32 ring_write = 0;
		u32 requested_words = 0;
		u32 queued_frames = 0;
		u32 vu_read = 0;
		u32 vu_published_write = 0;
		u32 vu_private_write = 0;
		u32 pending_vif_batch = 0;
	};
	void PublishCpu0(const Cpu0State& state);
	// Independent cumulative observations; the record is NOT a transactional
	// cut across owners. Never use differences for admission or guest ordering.
	enum class HandoffCounter : u32 { Reserved, Completed, Consumed, OwnerPasses, Count };
	void AddHandoffCounter(HandoffCounter counter, u32 count = 1);

	// Lifecycle calls are not hot-path operations. Initialize starts one sleeping
	// Vita-only writer and succeeds after its first synchronized record;
	// Shutdown joins it after one final synchronized record.
	// Non-Vita builds keep the in-memory snapshots available for validation but
	// perform no thread or file operations.
	bool Initialize(u64 run_id = 0);
	void Shutdown();
	bool IsRunning();
	WriterStatus GetWriterStatus();
	// Fatal GPU-VU watchdog paths may request one immediate durable record and
	// wait for it only up to this caller-supplied bound. Owner hot paths never
	// call this function; if storage is wedged, title-local fail-stop proceeds.
	bool FlushForFatal(u32 timeout_us = 150000u);

	void PublishGxmCall(const GxmCallState& state);
	void PublishMtgs(const MtgsState& state);
	void PublishRetirement(const RetirementState& state);
	void PublishNotification(const NotificationState& state);
	void PublishCpu1(const Cpu1State& state);
	void PublishLifecycle(const LifecycleState& state);

	// Host/QEMU validation surface for the exact on-device record encoder. The
	// output is unchanged when it is null or smaller than RecordSize.
	bool BuildRecordForValidation(u8* output, size_t output_size,
		u64 journal_sequence, u64 run_id, u64 timestamp_us);
	// Same encoding, separate sequence domain and magic (GVHL). A live record is
	// not an acknowledgment that any journal record reached durable storage.
	bool BuildLiveRecord(u8* output, size_t output_size,
		u64 stream_sequence, u64 timestamp_us);
	bool ValidateRecord(const u8* record, size_t record_size,
		u64* journal_sequence = nullptr);
} // namespace VitaGpuVu::HealthJournal
