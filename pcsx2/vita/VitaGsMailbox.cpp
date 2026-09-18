// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Gif_Unit.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/GsTrace.h"
#include "GS.h"
#include "GS/GSPerfMon.h"
#include "GS/GSState.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "MTGS.h"
#include "MTVU.h"
#include "PerformanceMetrics.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FPControl.h"
#include "common/SingleWaiterProgressEvent.h"
#include "common/Timer.h"
#include "common/WrappedMemCopy.h"
#include "vita/GSDeviceGXM.h"
#include "vita/VitaGxmGsState.h"
#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
#include "vita/VitaGsDrawTrace.h"
#endif
#include "vita/VitaGsMailbox.h"
#include "vita/VitaGsMemory.h"
#include "vita/VitaCore.h"
#include "vita/VitaGpuVuDirectProgram.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuGeneratedUniversal.h"
#include "vita/VitaGpuVuHealthJournal.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuUniversalEpoch.h"
#include "vita/VitaGpuVuVifInput.h"
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
#include "vita/VitaGpuVuOpportunityCensus.h"
#endif
#include "vita/VitaPerformanceTelemetry.h"
#if defined(VITASX2_VIF_EPOCH_CENSUS)
#include "vita/VitaVifEpochCensus.h"
#endif
#include "vita/VitaVuBlockCompiler.h"
#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "vita/GSDeviceGXM.h"
#endif

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
#include <malloc.h>
#include <psp2/appmgr.h>
#include <psp2common/defs.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

extern "C"
{
	extern unsigned int _newlib_heap_size_user;
}
#endif

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
namespace
{
	std::atomic<u64> s_mtvu_path1_completion_deferrals_for_validation{0};
}
#endif

Pcsx2Config::GSOptions GSConfig;

GSRendererType GSGetCurrentRenderer()
{
#if (defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION) || \
	(defined(VITASX2_VITA_SOFTWARE_GS_CONTROL) && \
	 VITASX2_VITA_SOFTWARE_GS_CONTROL)
	return GSRendererType::SW;
#else
	// Vita has one hardware backend. Auto is the existing PCSX2 renderer value
	// which selects GSRendererHW; the actual API is RenderAPI::GXM.
	return GSRendererType::Auto;
#endif
}

bool GSIsHardwareRenderer()
{
#if (defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION) || \
	(defined(VITASX2_VITA_SOFTWARE_GS_CONTROL) && \
	 VITASX2_VITA_SOFTWARE_GS_CONTROL)
	return false;
#else
	return true;
#endif
}

namespace MTGS
{
	struct BufferedData
	{
		alignas(16) u128 ring[RingBufferSize];
		alignas(16) u8 regs[Ps2MemSize::GSregs];

		u128& operator[](u32 index)
		{
			pxAssert(index < RingBufferSize);
			return ring[index];
		}
	};

	union PacketTag
	{
		struct
		{
			u32 command;
			u32 data[3];
		};
		struct
		{
			u32 pointer_command;
			u32 pointer_data;
			uptr pointer;
		};
	};
	static_assert(sizeof(PacketTag) <= sizeof(u128));
	static_assert(offsetof(PacketTag, pointer) == 2 * sizeof(u32));

	struct RingVSyncSnapshot
	{
		u8 regset1[0x0f0];
		u32 csr;
		u32 imr;
		GSRegSIGBLID siglblid;
		u32 registers_written;
		u32 padding[3];
	};
	static_assert(sizeof(RingVSyncSnapshot) == 17 * sizeof(u128));

	alignas(__cachelinesize) static BufferedData s_ring;
	alignas(__cachelinesize) static std::atomic<u32> s_read_pos{0};
	alignas(__cachelinesize) static std::atomic<u32> s_write_pos{0};
	static u32 s_packet_start_pos = 0;
	static u32 s_packet_size = 0;
	static u32 s_packet_write_pos = 0;
	static std::atomic<bool> s_signal_ring_enabled{false};
	static std::atomic<int> s_signal_ring_position{0};
	static std::atomic<int> s_queued_frame_count{0};
	static std::atomic<bool> s_vsync_signal_listener{false};
	static Threading::WorkSema s_work_sema;
	static Threading::UserspaceSemaphore s_ring_reset_sema;
	static Threading::UserspaceSemaphore s_vsync_sema;
	static Threading::UserspaceSemaphore s_open_or_close_done;
	static int s_copy_data_tally = 0;
	// Gif_Path::ExecuteGSPacket() creates one logical PATH1 reservation for
	// every VU1 dispatch. Adjacent reservations have no payload and therefore
	// need only one physical MTGS command. The EE producer may extend an open
	// command until the GS worker atomically claims it; a later non-PATH1
	// command closes the run before publication, preserving exact ordering.
	//
	// Keep the run count outside PacketTag. PacketTag is ordinary ring storage,
	// whereas this word is concurrently extended by the EE and claimed by the
	// GS owner. The open bit makes that handoff race-free without a mutex.
	static constexpr u32 MtvuReservationRunOpen = 0x80000000u;
	static constexpr u32 MtvuReservationRunCountMask = 0x7fffffffu;
	static constexpr u32 MaximumMtvuReservationRun = 256;
	static constexpr u32 NoOpenMtvuReservationRun = RingBufferSize;
	alignas(__cachelinesize)
		static std::array<std::atomic<u32>, RingBufferSize>
			s_mtvu_reservation_runs{};
	// EE producer only.
	static u32 s_open_mtvu_reservation_run = NoOpenMtvuReservationRun;
	// GS worker only. A command can span several independently published MTVU
	// completion records, so retain its unconsumed logical count at the ring
	// head without modifying shared PacketTag storage.
	static u32 s_mtvu_reservations_remaining = 0;
	static Threading::Thread s_thread;
	static std::atomic_bool s_open_flag{false};
	static std::atomic_bool s_shutdown_flag{false};
	static std::atomic_bool s_gpu_vu_fragment_completion_isolation_enabled{false};
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	// GXM writes completion notifications into its mapped notification region,
	// but exposes no callback which can wake the ordered GS owner. Keep one
	// process-lifetime bridge asleep while no universal transaction is in flight
	// and poll an armed notification with a kernel delay while one is pending.
	// This leaves EE, MTVU, and MTGS free; unlike sceGxmNotificationWait(), the
	// bounded poll also remains cancellable if a GPU fault occurs during shutdown.
	static Threading::Thread s_gpu_vu_notification_thread;
	static Threading::KernelSemaphore s_gpu_vu_notification_request_sema;
	static Threading::KernelSemaphore s_gpu_vu_notification_startup_sema;
	static std::atomic<u32> s_gpu_vu_notification_startup_result{0};
	static std::atomic<s32> s_gpu_vu_notification_startup_pinned_priority{-1};
	static std::atomic<s32> s_gpu_vu_notification_startup_effective_priority{-1};
	static std::atomic<s32> s_gpu_vu_notification_startup_priority_result{-1};
	static std::atomic_bool s_gpu_vu_notification_shutdown{false};
	static std::atomic_bool s_gpu_vu_notification_available{false};
	static std::atomic<uptr> s_gpu_vu_notification_address{0};
	static std::atomic<u32> s_gpu_vu_notification_value{0};
	static std::atomic<u32> s_gpu_vu_notification_start_value{0};
	static std::atomic<u32> s_gpu_vu_notification_job_base{0};
	static std::atomic<u32> s_gpu_vu_notification_job_count{0};
	static std::atomic<u64> s_gpu_vu_notification_sequence{0};
	static std::atomic<u32> s_gpu_vu_notification_observed{0};
	static std::atomic<u32> s_gpu_vu_notification_request{0};
	static std::atomic<u32> s_gpu_vu_notification_completed{0};
	static std::atomic<u64> s_gpu_vu_notification_arms{0};
	static std::atomic<u64> s_gpu_vu_notification_polls{0};
	static std::atomic<u64> s_gpu_vu_notification_wakes{0};
	static std::atomic<u64> s_gpu_vu_notification_deferrals{0};
	// A different retirement notification can be submitted while the bridge is
	// still observing an older same-context fence.  Keep the newest such fence
	// durably recorded.  GXM context ordering means completion of the newest
	// target subsumes every intermediate vertex notification, while retaining a
	// separate mailbox prevents an already-reached old target from hiding all
	// later SGX work.
	//
	// State: 0 empty, 1 producer publishing, 2 ready, 3 consumer claiming.
	static std::atomic<u32> s_gpu_vu_notification_pending_state{0};
	static std::atomic<uptr> s_gpu_vu_notification_pending_address{0};
	static std::atomic<u32> s_gpu_vu_notification_pending_value{0};
	static std::atomic<u32> s_gpu_vu_notification_pending_start_value{0};
	static std::atomic<u32> s_gpu_vu_notification_pending_job_base{0};
	static std::atomic<u32> s_gpu_vu_notification_pending_job_count{0};
	static std::atomic<u64> s_gpu_vu_notification_pending_sequence{0};
	static std::atomic<u64> s_gpu_vu_notification_pending_queues{0};
	static std::atomic<u64> s_gpu_vu_notification_pending_replacements{0};
	static std::atomic<u64> s_gpu_vu_notification_pending_chains{0};
	static std::atomic<u32> s_gpu_vu_notification_timeout_request{0};
	static std::atomic<u32> s_gpu_vu_notification_timeout_observed{0};
	static std::atomic<u64> s_gpu_vu_notification_timeout_elapsed_us{0};
	// sceGxmDraw()/MidSceneFlush()/EndScene() can block the context-owning
	// thread before the notification bridge is armed.  Keep this independent
	// ticket visible to the already process-lifetime notification thread so a
	// malformed generated command stream cannot turn that blind interval into
	// a system-level hang.
	static std::atomic<u32> s_gpu_vu_submission_watchdog_next{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_active{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_stage{0};
	static std::atomic<u64> s_gpu_vu_submission_watchdog_sequence{0};
	static std::atomic<u64> s_gpu_vu_submission_watchdog_scene{0};
	static std::atomic<uptr> s_gpu_vu_submission_watchdog_input_owner{0};
	static std::atomic<uptr> s_gpu_vu_submission_watchdog_output_address{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_input_slot{~u32{0}};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_input_generation{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_input_payload_count{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_stream_count{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_object_count{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_private_count{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_output_bytes{0};
	static std::atomic<u32> s_gpu_vu_submission_watchdog_retirement_slot{~u32{0}};
	static std::atomic<u32> s_gpu_vu_submission_consumer_updates{0};
	static std::atomic<u32> s_gpu_vu_submission_consumer_total_draws{0};
	static std::atomic<u32> s_gpu_vu_submission_consumer_state_first{0};
	static std::atomic<u32> s_gpu_vu_submission_consumer_state_end{0};
	static std::atomic<u32> s_gpu_vu_submission_consumer_group_first{0};
	static std::atomic<u32> s_gpu_vu_submission_consumer_group_end{0};
	static std::atomic<u32> s_gpu_vu_submission_consumer_group_index{0};
	static std::atomic<u32> s_gpu_vu_submission_consumer_flags{0};
	// The generated PATH1 producer and MTGS consumer deliberately own different
	// halves of one counted reservation run. Mirror their scalar state here so
	// the process-lifetime health thread can distinguish a GXM/SGX stall from a
	// publication hole without racing either owner's non-atomic fields.
	static std::atomic<u32> s_gpu_vu_path1_pending_direct_count{0};
	static std::atomic<u32> s_gpu_vu_path1_pending_generated{0};
	static std::atomic<u32> s_gpu_vu_path1_last_publication_kind{0};
	static std::atomic<u32> s_gpu_vu_path1_last_publication_count{0};
	static std::atomic<u32> s_gpu_vu_path1_reservations_remaining{0};
	static std::atomic<u32> s_gpu_vu_path1_last_completion_count{0};
	static std::atomic<u32> s_gpu_vu_path1_last_completion_prefix{0};
	static std::atomic<u32> s_gpu_vu_path1_last_completion_submit{0};
	static std::atomic<u32> s_gpu_vu_path1_last_ring_advance{0};
	static std::atomic<u32> s_gpu_vu_submission_ownership_action{0};
	static std::atomic<uptr> s_gpu_vu_submission_ownership_input_owner{0};
	static std::atomic<u32> s_gpu_vu_submission_ownership_input_slot{~u32{0}};
	static std::atomic<u32> s_gpu_vu_submission_ownership_input_generation{0};
	static std::atomic<u32> s_gpu_vu_submission_ownership_input_offset{0};
	static std::atomic<u32> s_gpu_vu_submission_ownership_input_size{0};
	static std::atomic<u32> s_gpu_vu_submission_ownership_input_references{0};
	static std::atomic<u32> s_gpu_vu_submission_ownership_draw_index{~u32{0}};
	static std::atomic<u32> s_gpu_vu_submission_ownership_input_index{~u32{0}};
	static std::atomic<u32> s_gpu_vu_submission_ownership_retained_count{0};
	static std::atomic<u32> s_gpu_vu_submission_ownership_descriptor_count{0};
	static std::atomic<Common::Timer::Value>
		s_gpu_vu_submission_watchdog_started{0};
	static std::atomic<Common::Timer::Value>
		s_gpu_vu_submission_watchdog_stage_started{0};
	static std::atomic<Common::Timer::Value>
		s_gpu_vu_submission_watchdog_last_report{0};
	static constexpr u32 GpuVuGxmSubmissionStageCount =
		static_cast<u32>(VitaGS::GpuVuGxmSubmissionStage::Count);
	static std::array<std::atomic<u64>, GpuVuGxmSubmissionStageCount>
		s_gpu_vu_submission_stage_samples{};
	static std::array<std::atomic<u64>, GpuVuGxmSubmissionStageCount>
		s_gpu_vu_submission_stage_total_us{};
	static std::array<std::atomic<u64>, GpuVuGxmSubmissionStageCount>
		s_gpu_vu_submission_stage_max_us{};
	static std::atomic<u32> s_gpu_vu_submission_last_stage{0};
	static std::atomic<u64> s_gpu_vu_submission_last_stage_us{0};
	static std::atomic<u32> s_gpu_vu_submission_slowest_stage{0};
	static std::atomic<u64> s_gpu_vu_submission_slowest_stage_us{0};
	static std::atomic<u32> s_gpu_vu_submission_last_completed_ticket{0};
	static std::atomic<u64> s_gpu_vu_submission_last_completed_us{0};
	static std::atomic<u64> s_gpu_vu_submission_max_completed_us{0};
	static std::atomic<Common::Timer::Value>
		s_gpu_vu_submission_ownership_action_started{0};
	static constexpr u32 GpuVuGxmOwnershipActionCount =
		static_cast<u32>(VitaGS::GpuVuGxmOwnershipAction::Count);
	static std::array<std::atomic<u64>, GpuVuGxmOwnershipActionCount>
		s_gpu_vu_ownership_action_samples{};
	static std::array<std::atomic<u64>, GpuVuGxmOwnershipActionCount>
		s_gpu_vu_ownership_action_total_us{};
	static std::array<std::atomic<u64>, GpuVuGxmOwnershipActionCount>
		s_gpu_vu_ownership_action_max_us{};
	static std::atomic<u32> s_gpu_vu_ownership_last_action{0};
	static std::atomic<u64> s_gpu_vu_ownership_last_action_us{0};
	static std::atomic<u32> s_gpu_vu_ownership_slowest_action{0};
	static std::atomic<u64> s_gpu_vu_ownership_slowest_action_us{0};
	// A generated command can corrupt the immediate context without blocking
	// its own sceGxmDraw().  In that case an unrelated GS draw is often the first
	// libGXM call which stops returning.  Keep an independent single-MTGS-owner
	// call journal after generated work has started so that interval is not
	// hidden by an already-completed retirement fence.
	static std::atomic_bool s_gpu_vu_gxm_call_health_enabled{false};
	static std::atomic<u32> s_gpu_vu_gxm_call_next{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_active{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_kind{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_sequence{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_scene{0};
	static std::atomic<Common::Timer::Value> s_gpu_vu_gxm_call_started{0};
	static std::atomic<Common::Timer::Value> s_gpu_vu_gxm_call_journal_time{0};
	static std::atomic<u64> s_gpu_vu_gxm_journal_generated_scene{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_journal_token{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_completed{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_completed_kind{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_completed_sequence{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_completed_scene{0};
	static std::atomic<s32> s_gpu_vu_gxm_call_completed_result{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_sequence_end{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_program_key_high{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_program_key_low{0};
	static std::atomic<u64> s_gpu_vu_gxm_last_generated_scene{0};
	static std::atomic<u64> s_gpu_vu_gxm_last_generated_sequence{0};
	static std::atomic<uptr> s_gpu_vu_gxm_call_input_owner{0};
	static std::atomic<uptr> s_gpu_vu_gxm_call_output_address{0};
	static std::atomic<uptr> s_gpu_vu_gxm_fragment_address{0};
	static std::atomic<uptr> s_gpu_vu_gxm_scene_render_target{0};
	static std::atomic<uptr> s_gpu_vu_gxm_scene_depth_target{0};
	static std::atomic<uptr> s_gpu_vu_gxm_call_vertex_program{0};
	static std::atomic<uptr> s_gpu_vu_gxm_call_fragment_program{0};
	static std::atomic<uptr> s_gpu_vu_gxm_call_texture_object{0};
	static std::atomic<uptr> s_gpu_vu_gxm_call_texture_descriptor{0};
	static std::atomic<uptr> s_gpu_vu_gxm_call_texture_data{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_ps_selector_low{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_ps_selector_high{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_input_slot{~u32{0}};
	static std::atomic<u32> s_gpu_vu_gxm_call_input_generation{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_input_first_qword{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_input_last_qword{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_retirement_slot{~u32{0}};
	static std::atomic<u32> s_gpu_vu_gxm_call_program_abi{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_phase_index{~u32{0}};
	static std::atomic<u32> s_gpu_vu_gxm_call_module_index{~u32{0}};
	static std::atomic<u32> s_gpu_vu_gxm_call_object_index{~u32{0}};
	static std::atomic<u32> s_gpu_vu_gxm_call_group_index{~u32{0}};
	static std::atomic<u32> s_gpu_vu_gxm_call_index_count{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_index_minimum{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_index_maximum{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_object_count{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_private_count{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_output_bytes{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_output_maximum_write_word{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_output_payload_capacity_words{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_output_probe_maximum_write_word{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_output_probe_capacity_words{0};
	static std::atomic<u32> s_gpu_vu_gxm_fragment_submitted{0};
	static std::atomic<u32> s_gpu_vu_gxm_fragment_completed{0};
	static std::atomic<u32> s_gpu_vu_gxm_fragment_oldest{0};
	static std::atomic<u64> s_gpu_vu_gxm_fragment_completed_scene{0};
	static std::atomic<u64> s_gpu_vu_gxm_fragment_completed_generated_scene{0};
	static std::atomic<u64> s_gpu_vu_gxm_fragment_completed_generated_sequence{0};
	static std::atomic<u64> s_gpu_vu_gxm_fragment_oldest_scene{0};
	static std::atomic<u64> s_gpu_vu_gxm_fragment_oldest_generated_scene{0};
	static std::atomic<u64> s_gpu_vu_gxm_fragment_oldest_generated_sequence{0};
	static std::atomic<u32> s_gpu_vu_gxm_scenes_since_generated{0};
	static std::atomic<u32> s_gpu_vu_gxm_scene_flags{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_primitive_type{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_topology{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_sampler_key{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_blend_key{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_color_mask_key{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_depth_key{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_texture_type{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_texture_format{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_texture_width{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_texture_height{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_texture_stride{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_texture_mipmap_count{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_texture_sampler_state{0};
	static constexpr u32 GpuVuGxmCallKindCount =
		static_cast<u32>(VitaGS::GpuVuGxmCallKind::Count);
	static const char* GpuVuGxmCallKindName(u32 kind);
	static std::array<std::atomic<u64>, GpuVuGxmCallKindCount>
		s_gpu_vu_gxm_call_samples{};
	static std::array<std::atomic<u64>, GpuVuGxmCallKindCount>
		s_gpu_vu_gxm_call_total_us{};
	static std::array<std::atomic<u64>, GpuVuGxmCallKindCount>
		s_gpu_vu_gxm_call_max_us{};
	static std::atomic<u64> s_gpu_vu_gxm_call_completed_elapsed_us{0};
	static std::atomic<u32> s_gpu_vu_gxm_call_slowest_kind{0};
	static std::atomic<u64> s_gpu_vu_gxm_call_slowest_us{0};
	// MTGS is the sole writer. The journal publisher copies this plain state
	// into its own atomic section immediately around the real libGXM boundary.
	static VitaGpuVu::HealthJournal::GxmCallState s_gpu_vu_health_gxm_call;
	// _sceAppMgrGetAppState() is a non-destructive lifecycle sample. Never call
	// sceAppMgrReceiveSystemEvent() from this diagnostic owner: Sony documents
	// that receiving deletes the event from the application's queue, so doing so
	// would change the suspend/resume behavior being investigated.
	static std::atomic<Common::Timer::Value> s_gpu_vu_lifecycle_last_poll{0};
	static std::atomic<u64> s_gpu_vu_lifecycle_poll_errors{0};
	static std::atomic<u64> s_gpu_vu_lifecycle_last_poll_gap_us{0};
	static std::atomic<u32> s_gpu_vu_lifecycle_system_ui_overlaid{0};
	static std::atomic<u32> s_gpu_vu_lifecycle_overlay_known{0};
	static std::atomic<u64> s_gpu_vu_lifecycle_overlay_enter_events{0};
	static std::atomic<u64> s_gpu_vu_lifecycle_overlay_leave_events{0};
	// CPU1 can wait on a generated transaction before DrawGpuVu() arms the GXM
	// submission watchdog. Keep that earlier ownership interval independently
	// visible to the same process-lifetime monitor.
	static std::atomic<u32> s_gpu_vu_handoff_watchdog_next{0};
	static std::atomic<u32> s_gpu_vu_handoff_watchdog_active{0};
	static std::atomic<u32> s_gpu_vu_handoff_watchdog_stage{0};
	static std::atomic<u64> s_gpu_vu_handoff_watchdog_sequence{0};
	static std::atomic<Common::Timer::Value>
		s_gpu_vu_handoff_watchdog_started{0};
	static constexpr double GpuVuNotificationNoProgressTimeoutSeconds = 2.0;
	static constexpr double GpuVuGxmCallJournalSeconds = 0.5;
#endif
	static std::atomic_bool s_gpu_vu_compiler_result_pending{false};
	static std::atomic_bool s_gpu_vu_input_retirement_pending{false};
	static std::atomic<uptr> s_gpu_vu_input_retirement_owner{0};
	static std::atomic<u32> s_gpu_vu_input_retirement_slot{0};
	static std::atomic<u32> s_gpu_vu_input_retirement_generation{0};
	static bool s_native_presenter_enabled = false;

		struct MtvuPath1Completion
		{
			VitaGpuVu::GpuVuDraw* first_draw = nullptr;
			u32 reservation_count = 0;
			VitaGpuVu::UniversalGpuVuEpoch* universal_epoch = nullptr;
			// A private-attestation draw executes only vertex-stage BUFFER2
			// writes. Its CPU oracle packet remains the visible PATH1 owner at
			// the same logical reservation instead of being discarded.
			bool cpu_path1_after_draws = false;
			// Set only for a real architectural visibility/resource-pressure
			// boundary. Ordinary VSync publication leaves generated draws in the
			// current scene and uses its EndScene vertex notification.
			bool submit_generated_transactions_after_draws = false;
			// Unlike a null first_draw CPU completion, this has no GIF packet.
			bool no_output_only = false;
			Common::Timer::Value published_at = 0;
		};
#if defined(__vita__)
	static_assert(sizeof(MtvuPath1Completion) == 24,
		"empty completion tag must reuse padding in the Vita queue");
#endif

	// PCSX2's EE thread reserves one logical PATH1 ordering point for every VU1
	// dispatch. Consecutive GPU executions have no intervening CPU PATH1
	// packet, so the single MTVU producer links them and publishes one run
	// completion. The physical MTGS command independently coalesces adjacent
	// reservations; ConsumePrefix() reconciles either run boundary without
	// weakening a single guest ordering point.
	class MtvuPath1CompletionQueue
	{
	public:
		bool Push(const MtvuPath1Completion& completion)
		{
			const u32 write = m_write.load(std::memory_order_relaxed);
			const u32 next = (write + 1) & RingBufferMask;
			if (next == m_read.load(std::memory_order_acquire))
				return false;
			m_completions[write] = completion;
			m_write.store(next, std::memory_order_release);
#if defined(__vita__)
			VitaGpuVu::HealthJournal::AddHandoffCounter(
				VitaGpuVu::HealthJournal::HandoffCounter::Completed,
				completion.reservation_count);
#endif
			return true;
		}

		bool Pop(MtvuPath1Completion* completion)
		{
			const u32 read = m_read.load(std::memory_order_relaxed);
			if (read == m_write.load(std::memory_order_acquire))
				return false;
			*completion = m_completions[read];
			m_read.store((read + 1) & RingBufferMask,
				std::memory_order_release);
			return true;
		}

		bool Peek(MtvuPath1Completion* completion) const
		{
			const u32 read = m_read.load(std::memory_order_relaxed);
			if (read == m_write.load(std::memory_order_acquire))
				return false;
			*completion = m_completions[read];
			return true;
		}

		void ConsumePrefix(u32 count,
			VitaGpuVu::GpuVuDraw* remaining_first_draw)
		{
			const u32 read = m_read.load(std::memory_order_relaxed);
			pxAssertRel(read != m_write.load(std::memory_order_acquire),
				"consumed an empty MTVU PATH1 completion queue");
			MtvuPath1Completion& completion = m_completions[read];
			pxAssertRel(count != 0 && count <= completion.reservation_count,
				"consumed an invalid MTVU PATH1 completion prefix");
				if (count == 0 || count > completion.reservation_count)
					return;
				if (count == completion.reservation_count &&
					completion.published_at != 0 &&
					VitaPerformanceTelemetry::IsEnabled())
				{
					const u64 age_us = static_cast<u64>(
						Common::Timer::ConvertValueToSeconds(
							Common::Timer::GetCurrentValue() -
							completion.published_at) * 1000000.0);
					VitaGpuVu::RecordUniversalGpuVuMtvuPath1QueueAge(age_us);
				}
			pxAssertRel(!completion.universal_epoch ||
				(count == completion.reservation_count &&
				 !remaining_first_draw),
				"partially consumed a universal GPU-VU ordering point");

			completion.first_draw = remaining_first_draw;
			completion.reservation_count -= count;
#if defined(__vita__)
			VitaGpuVu::HealthJournal::AddHandoffCounter(
				VitaGpuVu::HealthJournal::HandoffCounter::Consumed, count);
#endif
			if (completion.reservation_count != 0)
				return;

			pxAssertRel(!remaining_first_draw,
				"completed MTVU PATH1 run retained a direct draw");
			m_read.store((read + 1) & RingBufferMask,
				std::memory_order_release);
		}

		u32 ReadIndex() const { return m_read.load(std::memory_order_relaxed); }
		u32 WriteIndex() const { return m_write.load(std::memory_order_relaxed); }
		u32 PendingCount() const
		{
			return (WriteIndex() - ReadIndex()) & RingBufferMask;
		}

		void ResetAndDiscard()
		{
			MtvuPath1Completion completion;
			while (Pop(&completion))
			{
				if (completion.universal_epoch)
					completion.universal_epoch->Cancel();
				VitaGpuVu::GpuVuDraw* draw = completion.first_draw;
				for (u32 index = 0;
					index < completion.reservation_count && draw; index++)
				{
					VitaGpuVu::GpuVuDraw* const next = draw->path1_next;
					draw->path1_next = nullptr;
					VitaGpuVu::RecordGpuVuDrawRejected();
					delete draw;
					draw = next;
				}
				pxAssertRel(!draw,
					"MTVU PATH1 run exceeded its reservation count");
			}
			m_read.store(0, std::memory_order_relaxed);
			m_write.store(0, std::memory_order_relaxed);
		}

	private:
		alignas(__cachelinesize)
			std::array<MtvuPath1Completion, RingBufferSize> m_completions{};
		alignas(__cachelinesize) std::atomic<u32> m_read{0};
		alignas(__cachelinesize) std::atomic<u32> m_write{0};
	};

	static MtvuPath1CompletionQueue s_mtvu_path1_completions;
	// These fields have exactly one owner: the MTVU worker. CPU-produced PATH1
	// output and explicit drains publish the pending run before their own
	// completion, so no lock or cross-core refcount is needed here.
	static VitaGpuVu::GpuVuDraw* s_pending_mtvu_direct_head = nullptr;
	static VitaGpuVu::GpuVuDraw* s_pending_mtvu_direct_tail = nullptr;
	static u32 s_pending_mtvu_direct_count = 0;
	static u32 s_pending_mtvu_direct_reservations = 0;
	static u32 s_pending_mtvu_no_output_count = 0;
	static bool s_pending_mtvu_generated_transaction = false;
	static constexpr u32 MaximumPendingMtvuDirectRun = 256;
	static std::atomic_bool s_mtvu_path1_completion_waiting{false};
	static std::atomic_bool s_mtvu_path1_drain_waiter{false};
	static Threading::UserspaceSemaphore s_mtvu_path1_drain_sema;
	static Threading::SingleWaiterProgressEvent
		s_mtvu_path1_buffer_progress;
	// On Vita g_gs_renderer owns this instance, matching PCSX2 GS.cpp. QEMU's
	// software-only GSState keeps its existing mailbox-local owner instead.
	static VitaGxmGsState* s_gs = nullptr;

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
	static std::unique_ptr<VitaGxmGsState> s_qemu_gs;
#endif

#if defined(__vita__)
	struct HardwareVsyncProfile
	{
		u32 boundaries = 0;
		Common::Timer::Value wall_start = 0;
		u64 cpu_start = 0;
		u64 vu_cpu_start = 0;
		VU_Thread::ProducerProfileStats vu_start{};
	};
	static HardwareVsyncProfile s_producer_profile;
	static HardwareVsyncProfile s_worker_profile;
	static std::atomic<u32> s_profile_ring_stalls{0};
	static std::atomic<u32> s_profile_vsync_waits{0};
	static std::atomic<u32> s_profile_mtvu_path1_completion_deferrals{0};
	static std::atomic<int> s_profile_max_queued_frames{0};

	struct GsProducerPerformanceTotals
	{
		u64 submissions = 0;
		u64 ring_words = 0;
		u64 gs_packets = 0;
		u64 gs_packet_bytes = 0;
		u64 mtvu_packets = 0;
		u64 wait_calls = 0;
		u64 wait_spins = 0;
		u64 ring_spins = 0;
		u64 mtvu_path1_completion_ring_waits = 0;
		u64 vsync_wait_calls = 0;
		u64 vsync_wait_wall_us = 0;
	};

	struct GsWorkerPerformanceTotals
	{
		u64 commands = 0;
		u64 gs_packets = 0;
		u64 gs_packet_bytes = 0;
		u64 mtvu_packets = 0;
		u64 mtvu_packet_bytes = 0;
		u64 mtvu_path1_completion_deferrals = 0;
		u64 transfer_calls = 0;
		u64 transfer_wall_us = 0;
		u64 vsync_calls = 0;
		u64 vsync_wall_us = 0;
		u64 completed_vsyncs = 0;
	};

	static GsProducerPerformanceTotals s_gs_producer_performance;
	static GsWorkerPerformanceTotals s_gs_worker_performance;
	static std::atomic<u64> s_gs_published_commands{0};
	static std::atomic<u64> s_gs_published_packets{0};
	static std::atomic<u64> s_gs_published_packet_bytes{0};
	static std::atomic<u64> s_gs_published_mtvu_packets{0};
	static std::atomic<u64> s_gs_published_mtvu_packet_bytes{0};
	static std::atomic<u64> s_gs_published_mtvu_path1_completion_deferrals{0};
	static std::atomic<u64> s_gs_published_transfer_calls{0};
	static std::atomic<u64> s_gs_published_transfer_wall_us{0};
	static std::atomic<u64> s_gs_published_vsync_calls{0};
	static std::atomic<u64> s_gs_published_vsync_wall_us{0};
	static std::atomic<u64> s_gs_published_completed_vsyncs{0};

	static void TransferGsPacket(const u8* data, u32 qwords)
	{
		if (!s_gs || qwords == 0)
			return;
		const bool profile = VitaPerformanceTelemetry::IsEnabled();
		const Common::Timer::Value started = profile ?
			Common::Timer::GetCurrentValue() : 0;
		s_gs->Transfer<3>(data, qwords);
		if (!profile)
			return;
		const u64 elapsed_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(
				Common::Timer::GetCurrentValue() - started) * 1000000.0);
		s_gs_worker_performance.transfer_calls++;
		s_gs_worker_performance.transfer_wall_us += elapsed_us;
	}

	static void PublishGsWorkerPerformance()
	{
		s_gs_published_commands.store(s_gs_worker_performance.commands,
			std::memory_order_relaxed);
		s_gs_published_packets.store(s_gs_worker_performance.gs_packets,
			std::memory_order_relaxed);
		s_gs_published_packet_bytes.store(s_gs_worker_performance.gs_packet_bytes,
			std::memory_order_relaxed);
		s_gs_published_mtvu_packets.store(s_gs_worker_performance.mtvu_packets,
			std::memory_order_relaxed);
		s_gs_published_mtvu_packet_bytes.store(
			s_gs_worker_performance.mtvu_packet_bytes, std::memory_order_relaxed);
		s_gs_published_mtvu_path1_completion_deferrals.store(
			s_gs_worker_performance.mtvu_path1_completion_deferrals,
			std::memory_order_relaxed);
		s_gs_published_transfer_calls.store(
			s_gs_worker_performance.transfer_calls, std::memory_order_relaxed);
		s_gs_published_transfer_wall_us.store(
			s_gs_worker_performance.transfer_wall_us, std::memory_order_relaxed);
		s_gs_published_vsync_calls.store(
			s_gs_worker_performance.vsync_calls, std::memory_order_relaxed);
		s_gs_published_vsync_wall_us.store(
			s_gs_worker_performance.vsync_wall_us, std::memory_order_relaxed);
		s_gs_published_completed_vsyncs.store(
			s_gs_worker_performance.completed_vsyncs, std::memory_order_release);
	}

	static void RecordHardwareVsyncProfile(HardwareVsyncProfile& profile,
		const char* owner)
	{
		constexpr u32 WARMUP_BOUNDARIES = 60;
		constexpr u32 PROFILE_INTERVALS = 120;
		if (!VitaPerformanceTelemetry::IsEnabled() ||
			!s_native_presenter_enabled ||
			profile.boundaries > WARMUP_BOUNDARIES + PROFILE_INTERVALS)
			return;

		const u32 boundary = profile.boundaries++;
		// Keep the first guest-VSync handoff visible even for boot paths which
		// never reach the warm-up interval. This distinguishes a stalled EE from
		// a lost MTGS command or a presenter which is only drawing BGCOLOR.
		const u32 boundary_count = boundary + 1;
		if (boundary < 4 ||
			(boundary_count <= 64 &&
			 (boundary_count & (boundary_count - 1)) == 0))
		{
			Console.WriteLn("Vita GS flow: %s_vsync=%u queued=%d ring_read=%u ring_write=%u",
				owner, boundary_count,
				s_queued_frame_count.load(std::memory_order_relaxed),
				s_read_pos.load(std::memory_order_relaxed),
				s_write_pos.load(std::memory_order_relaxed));
		}
		if (boundary == WARMUP_BOUNDARIES)
		{
			profile.wall_start = Common::Timer::GetCurrentValue();
			profile.cpu_start = Threading::GetThreadCpuTime();
			if (&profile == &s_producer_profile && THREAD_VU1 &&
				vu1Thread.IsOpen())
			{
				profile.vu_cpu_start = vu1Thread.GetThreadHandle().GetCPUTime();
				profile.vu_start = vu1Thread.GetProducerProfileStats();
			}
			return;
		}
		if (boundary != WARMUP_BOUNDARIES + PROFILE_INTERVALS)
			return;

		const Common::Timer::Value wall_now = Common::Timer::GetCurrentValue();
		const u64 cpu_now = Threading::GetThreadCpuTime();
		const u64 wall_us = static_cast<u64>(Common::Timer::ConvertValueToSeconds(
			wall_now - profile.wall_start) * 1000000.0);
		const u64 cpu_us = cpu_now - profile.cpu_start;
		const double utilization = wall_us ?
			(static_cast<double>(cpu_us) * 100.0 / static_cast<double>(wall_us)) : 0.0;
		Console.WriteLn("Vita MTGS %s profile: warmup_vsyncs=60 interval_vsyncs=120 wall_us=%llu cpu_us=%llu cpu_util=%.1f%% ring_stalls=%u vsync_waits=%u mtvu_path1_completion_deferrals=%u max_queued=%d",
			owner,
			static_cast<unsigned long long>(wall_us),
			static_cast<unsigned long long>(cpu_us),
			utilization,
			s_profile_ring_stalls.load(std::memory_order_relaxed),
			s_profile_vsync_waits.load(std::memory_order_relaxed),
			s_profile_mtvu_path1_completion_deferrals.load(
				std::memory_order_relaxed),
			s_profile_max_queued_frames.load(std::memory_order_relaxed));
		if (&profile == &s_producer_profile && THREAD_VU1 &&
			vu1Thread.IsOpen())
		{
			const u64 vu_cpu_us = vu1Thread.GetThreadHandle().GetCPUTime() -
				profile.vu_cpu_start;
			const VU_Thread::ProducerProfileStats vu_now =
				vu1Thread.GetProducerProfileStats();
			const double vu_utilization = wall_us ?
				(static_cast<double>(vu_cpu_us) * 100.0 /
					static_cast<double>(wall_us)) : 0.0;
			Console.WriteLn("Vita MTVU profile: wall_us=%llu cpu_us=%llu cpu_util=%.1f%% programs=%llu waits=%llu ring_waits=%llu compile_barriers=%llu",
				static_cast<unsigned long long>(wall_us),
				static_cast<unsigned long long>(vu_cpu_us),
				vu_utilization,
				static_cast<unsigned long long>(vu_now.execute_enqueues -
					profile.vu_start.execute_enqueues),
				static_cast<unsigned long long>(vu_now.wait_calls -
					profile.vu_start.wait_calls),
				static_cast<unsigned long long>(vu_now.ring_waits -
					profile.vu_start.ring_waits),
				static_cast<unsigned long long>(vu_now.compile_barriers -
					profile.vu_start.compile_barriers));
			const VitaGpuVu::DrawStatistics draw =
				VitaGpuVu::GetGpuVuDrawStatistics();
			const VitaGpuVu::DirectProgramStatistics direct =
				VitaGpuVu::GetDirectProgramStatistics();
			const VitaGpuVu::InputRingStatistics input =
				VitaGpuVu::GetInputRingStatistics();
			Console.WriteLn(
				"GPU-VU hot profile: queued=%llu cpu_vu1=%llu "
				"continuation_shared=%llu continuation_general=%llu "
				"descriptor_size=%u descriptor_waits=%llu "
				"uniform_size=%u uniform_waits=%llu "
				"input_ring_waits=%llu encoded=%llu",
				static_cast<unsigned long long>(draw.queued),
				static_cast<unsigned long long>(draw.cpu_vu1_executions),
				static_cast<unsigned long long>(
					direct.shared_continuation_builds),
				static_cast<unsigned long long>(
					direct.general_continuation_builds),
				draw.descriptor_size,
				static_cast<unsigned long long>(
					draw.descriptor_pool_waits),
				draw.uniform_block_size,
				static_cast<unsigned long long>(draw.uniform_pool_waits),
				static_cast<unsigned long long>(input.ring_waits),
				static_cast<unsigned long long>(draw.encoded_objects));
			const VitaGpuVu::UniversalGpuVuEpochStatistics universal =
				VitaGpuVu::GetUniversalGpuVuEpochStatistics();
			Console.WriteLn(
				"GPU-VU universal profile: prepared=%llu preflight_accepted=%llu "
					"submitted=%llu continuation_groups=%llu "
					"continuation_core_submissions=%llu accepted=%llu accepted_pairs=%llu "
					"async_queued=%llu async_state_acquired=%llu async_pending_max=%llu "
					"cpu_fallbacks=%llu fallback_pairs=%llu cpu_vu_calls=%llu "
				"output=raw-path1",
				static_cast<unsigned long long>(universal.prepared),
				static_cast<unsigned long long>(universal.preflight_accepted),
				static_cast<unsigned long long>(universal.submitted),
				static_cast<unsigned long long>(universal.continuation_groups),
				static_cast<unsigned long long>(universal.continuation_submissions),
					static_cast<unsigned long long>(universal.accepted),
					static_cast<unsigned long long>(universal.accepted_pairs),
					static_cast<unsigned long long>(universal.async_epochs_queued),
					static_cast<unsigned long long>(universal.async_state_acquired),
					static_cast<unsigned long long>(universal.async_pending_max),
				static_cast<unsigned long long>(universal.cpu_fallbacks),
				static_cast<unsigned long long>(universal.rejected_pairs),
				static_cast<unsigned long long>(universal.cpu_vu_calls));
			Console.WriteLn(
				"GPU-VU pipeline profile: preflight_us=%llu program_prepare_us=%llu "
				"static_lookup_us=%llu epoch_allocation_us=%llu "
				"control_analysis_us=%llu pair_validation_us=%llu payload_encode_us=%llu "
				"static_cache_hits=%llu static_cache_misses=%llu "
				"mailbox_wait_us=%llu notification_wait_us=%llu retirement_wait_us=%llu "
				"cpu_fallback_us=%llu cpu_materialize_us=%llu "
				"cpu_unpack_replay_us=%llu cpu_path1_finish_us=%llu "
				"cpu_completion_publish_us=%llu worker_attempt_us=%llu "
				"path1_retirement_us=%llu",
				static_cast<unsigned long long>(universal.preflight_wall_us),
				static_cast<unsigned long long>(universal.program_prepare_wall_us),
				static_cast<unsigned long long>(
					universal.static_preflight_lookup_wall_us),
				static_cast<unsigned long long>(universal.epoch_allocation_wall_us),
				static_cast<unsigned long long>(universal.control_analysis_wall_us),
				static_cast<unsigned long long>(universal.pair_validation_wall_us),
				static_cast<unsigned long long>(universal.payload_encode_wall_us),
				static_cast<unsigned long long>(universal.static_preflight_cache_hits),
				static_cast<unsigned long long>(universal.static_preflight_cache_misses),
				static_cast<unsigned long long>(universal.mailbox_wait_wall_us),
				static_cast<unsigned long long>(universal.notification_wait_wall_us),
				static_cast<unsigned long long>(universal.retirement_wait_wall_us),
				static_cast<unsigned long long>(universal.cpu_fallback_wall_us),
				static_cast<unsigned long long>(universal.cpu_materialize_wall_us),
				static_cast<unsigned long long>(universal.cpu_unpack_replay_wall_us),
				static_cast<unsigned long long>(universal.cpu_path1_finish_wall_us),
				static_cast<unsigned long long>(
					universal.cpu_completion_publish_wall_us),
				static_cast<unsigned long long>(universal.worker_attempt_wall_us),
				static_cast<unsigned long long>(universal.path1_retirement_wall_us));
			const u64 execute_queue_average_us =
				universal.mtvu_execute_queue_samples ?
					universal.mtvu_execute_queue_age_us /
						universal.mtvu_execute_queue_samples : 0;
			const u64 path1_queue_average_us =
				universal.mtvu_path1_queue_samples ?
					universal.mtvu_path1_queue_age_us /
						universal.mtvu_path1_queue_samples : 0;
			Console.WriteLn(
				"GPU-VU backlog profile: execute_samples=%llu execute_age_avg_us=%llu "
					"execute_age_max_us=%llu execute_outstanding_max=%llu "
					"mtvu_queue_words_max=%llu async_pending_max=%llu "
					"multi_execute_gather_suppressed=%llu "
				"dispatch_cache_hits=%llu dispatch_cache_misses=%llu "
				"product_hot_hits=%llu product_hot_misses=%llu "
				"path1_samples=%llu path1_age_avg_us=%llu path1_age_max_us=%llu "
				"cpu0_mtvu_wait_us=%llu cpu0_ring_wait_us=%llu "
				"cpu0_execute_budget_waits=%llu cpu0_execute_budget_wait_us=%llu",
				static_cast<unsigned long long>(universal.mtvu_execute_queue_samples),
				static_cast<unsigned long long>(execute_queue_average_us),
				static_cast<unsigned long long>(universal.mtvu_execute_queue_age_max_us),
				static_cast<unsigned long long>(universal.mtvu_execute_outstanding_max),
					static_cast<unsigned long long>(universal.mtvu_queue_used_words_max),
					static_cast<unsigned long long>(universal.async_pending_max),
				static_cast<unsigned long long>(
					universal.mtvu_multi_execute_gather_suppressed),
				static_cast<unsigned long long>(universal.mtvu_dispatch_cache_hits),
				static_cast<unsigned long long>(universal.mtvu_dispatch_cache_misses),
				static_cast<unsigned long long>(
					universal.generated_product_hot_dispatch_hits),
				static_cast<unsigned long long>(
					universal.generated_product_hot_dispatch_misses),
				static_cast<unsigned long long>(universal.mtvu_path1_queue_samples),
				static_cast<unsigned long long>(path1_queue_average_us),
				static_cast<unsigned long long>(universal.mtvu_path1_queue_age_max_us),
				static_cast<unsigned long long>(universal.cpu0_mtvu_wait_wall_us),
				static_cast<unsigned long long>(universal.cpu0_mtvu_ring_wait_wall_us),
				static_cast<unsigned long long>(universal.cpu0_execute_budget_waits),
				static_cast<unsigned long long>(
					universal.cpu0_execute_budget_wait_wall_us));
		}
	}

	template <typename T>
	static u64 CounterDelta(T current, T previous)
	{
		return current >= previous ? static_cast<u64>(current - previous) :
			static_cast<u64>(current);
	}

	class CorrelatedPerformanceBatch
	{
	public:
		void Clear()
		{
			m_size = 0u;
			m_truncated = false;
		}

		void WriteLn(const char* format, ...)
		{
			char local[2048];
			std::va_list arguments;
			std::va_list retry_arguments;
			va_start(arguments, format);
			va_copy(retry_arguments, arguments);
			const int length = std::vsnprintf(
				local, sizeof(local), format, arguments);
			va_end(arguments);
			if (length <= 0)
			{
				va_end(retry_arguments);
				return;
			}

			const size_t separator = m_size != 0u ? 1u : 0u;
			const size_t formatted = static_cast<size_t>(length);
			if (separator + formatted >= m_lines.size() - m_size)
			{
				m_truncated = true;
				va_end(retry_arguments);
				return;
			}
			if (separator != 0u)
				m_lines[m_size++] = '\n';
			if (formatted < sizeof(local))
				std::memcpy(m_lines.data() + m_size, local, formatted);
			else
				std::vsnprintf(m_lines.data() + m_size, formatted + 1,
					format, retry_arguments);
			m_size += formatted;
			va_end(retry_arguments);
		}

		void Flush() const
		{
			if (m_size != 0u)
			{
				Log::WriteMultilineBatch(
					LOGLEVEL_INFO, Color_Default,
					std::string_view(m_lines.data(), m_size));
			}
			if (m_truncated)
				Console.Warning(
					"Vita correlated profile exceeded its fixed batch buffer.");
		}

		void FlushAndClear()
		{
			Flush();
			Clear();
		}

	private:
		static constexpr size_t Capacity = 128u * 1024u;
		std::array<char, Capacity + 1u> m_lines{};
		size_t m_size = 0u;
		bool m_truncated = false;
	};

	struct CorrelatedPerformanceSnapshot
	{
		Common::Timer::Value wall = 0;
		u64 ee_cpu_us = 0;
		u64 vu_cpu_us = 0;
		u64 gs_cpu_us = 0;
		u64 producer_vsyncs = 0;
		u64 completed_vsyncs = 0;
		u64 ee_cycle = 0;
		u64 iop_cycle = 0;
		u32 ee_pc = 0;
		u32 iop_pc = 0;
		VitaPerformanceTelemetry::CpuStageProfilerSnapshot cpu_stage_profiler;
#if defined(VITASX2_VIF_EPOCH_CENSUS)
		VitaVifEpochCensus::Snapshot vif_epoch;
#endif
		VitaA32EeProviderStats ee;
#if defined(VITASX2_CPU_PROFILER)
		VitaA32IopProviderStats iop;
#endif
		VitaVU::Vu0TelemetryStats vu0;
		VitaVU::Vu1TelemetryStats vu1;
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
		VitaGpuVuOpportunityCensus::Snapshot gpu_vu_census;
#endif
		VU_Thread::ProducerProfileStats mtvu{};
		GsProducerPerformanceTotals gs_producer;
		GsWorkerPerformanceTotals gs_worker;
		VitaGxmPerformanceCounters gxm;
		VitaGpuVu::DirectProgramStatistics gpu_vu_direct;
		VitaGpuVu::ProgramRegistryStatistics gpu_vu_programs;
		VitaGpuVu::GeneratedUniversalRegionBundleStatistics
			gpu_vu_region_bundles;
		VitaGpuVu::InputRingStatistics gpu_vu_input;
		VitaGpuVu::DrawStatistics gpu_vu_draw;
	};

	struct CorrelatedPerformanceProfile
	{
		enum class Origin : u8
		{
			Boot,
			Elf,
			WorkloadReplay,
		};

		u64 producer_vsyncs = 0;
		u64 producer_vsync_origin = 0;
		u32 sampling_boundaries = 0;
		u32 boundaries_at_start = 0;
		u64 window = 0;
		bool started = false;
		Origin origin_kind = Origin::Boot;
		CorrelatedPerformanceSnapshot origin;
		CorrelatedPerformanceSnapshot start;
	};
	static CorrelatedPerformanceProfile s_correlated_profile;

	struct LivePerformanceSnapshot
	{
		struct GxmHostCall
		{
			u64 samples = 0;
			u64 total_us = 0;
			u64 max_us = 0;
		};
		std::array<GxmHostCall,
			static_cast<size_t>(VitaGS::GpuVuGxmCallKind::Count)> gxm_host_calls{};
		Common::Timer::Value wall = 0;
		u64 ee_cpu_us = 0;
		u64 vu_cpu_us = 0;
		u64 gs_cpu_us = 0;
		u64 gpu_vu_notification_cpu_us = 0;
		u64 gpu_vu_notification_arms = 0;
		u64 gpu_vu_notification_polls = 0;
		u64 gpu_vu_notification_wakes = 0;
		u64 gpu_vu_notification_deferrals = 0;
		u64 gpu_vu_notification_pending_queues = 0;
		u64 gpu_vu_notification_pending_replacements = 0;
		u64 gpu_vu_notification_pending_chains = 0;
		u64 gpu_vu_notification_sequence = 0;
		u64 gpu_vu_notification_pending_sequence = 0;
		uptr gpu_vu_notification_address = 0;
		uptr gpu_vu_notification_pending_address = 0;
		u32 gpu_vu_notification_request = 0;
		u32 gpu_vu_notification_completed = 0;
		u32 gpu_vu_notification_value = 0;
		u32 gpu_vu_notification_observed = 0;
		u32 gpu_vu_notification_pending_state = 0;
		u32 gpu_vu_notification_pending_value = 0;
		u32 gpu_vu_submission_watchdog_ticket = 0;
		u32 gpu_vu_submission_watchdog_stage = 0;
		u64 gpu_vu_submission_watchdog_sequence = 0;
		u64 gpu_vu_submission_watchdog_scene = 0;
		uptr gpu_vu_submission_watchdog_input_owner = 0;
		uptr gpu_vu_submission_watchdog_output_address = 0;
		u32 gpu_vu_submission_watchdog_input_slot = 0;
		u32 gpu_vu_submission_watchdog_input_generation = 0;
		u32 gpu_vu_submission_watchdog_retirement_slot = 0;
		u32 gpu_vu_handoff_watchdog_ticket = 0;
		u32 gpu_vu_handoff_watchdog_stage = 0;
		u64 gpu_vu_handoff_watchdog_sequence = 0;
		u64 producer_vsyncs = 0;
		u64 completed_vsyncs = 0;
		GsWorkerPerformanceTotals gs_worker;
		VitaGxmPerformanceCounters gxm;
		VitaGpuVu::UniversalGpuVuEpochStatistics gpu_vu;
	};

	struct LivePerformanceProfile
	{
		bool started = false;
		u64 window = 0;
		LivePerformanceSnapshot start;
	};
	static LivePerformanceProfile s_live_performance;

	static const char* CorrelatedPerformanceOriginName(
		CorrelatedPerformanceProfile::Origin origin)
	{
		switch (origin)
		{
			case CorrelatedPerformanceProfile::Origin::Elf:
				return "elf";
			case CorrelatedPerformanceProfile::Origin::WorkloadReplay:
				return "workload-replay";
			default:
				return "boot";
		}
	}

	static GsWorkerPerformanceTotals GetPublishedGsWorkerPerformance()
	{
		GsWorkerPerformanceTotals stats;
		stats.completed_vsyncs =
			s_gs_published_completed_vsyncs.load(std::memory_order_acquire);
		stats.commands = s_gs_published_commands.load(std::memory_order_relaxed);
		stats.gs_packets = s_gs_published_packets.load(std::memory_order_relaxed);
		stats.gs_packet_bytes =
			s_gs_published_packet_bytes.load(std::memory_order_relaxed);
		stats.mtvu_packets =
			s_gs_published_mtvu_packets.load(std::memory_order_relaxed);
		stats.mtvu_packet_bytes =
			s_gs_published_mtvu_packet_bytes.load(std::memory_order_relaxed);
		stats.mtvu_path1_completion_deferrals =
			s_gs_published_mtvu_path1_completion_deferrals.load(
				std::memory_order_relaxed);
		stats.transfer_calls =
			s_gs_published_transfer_calls.load(std::memory_order_relaxed);
		stats.transfer_wall_us =
			s_gs_published_transfer_wall_us.load(std::memory_order_relaxed);
		stats.vsync_calls =
			s_gs_published_vsync_calls.load(std::memory_order_relaxed);
		stats.vsync_wall_us =
			s_gs_published_vsync_wall_us.load(std::memory_order_relaxed);
		return stats;
	}

	static CorrelatedPerformanceSnapshot CaptureCorrelatedPerformanceSnapshot(
		u64 producer_vsyncs)
	{
		CorrelatedPerformanceSnapshot snapshot;
		snapshot.producer_vsyncs = producer_vsyncs;
		snapshot.wall = Common::Timer::GetCurrentValue();
		snapshot.ee_cpu_us = Threading::GetThreadCpuTime();
		snapshot.vu_cpu_us = THREAD_VU1 && vu1Thread.IsOpen() ?
			vu1Thread.GetThreadHandle().GetCPUTime() : 0;
		snapshot.gs_cpu_us = s_thread.Joinable() ? s_thread.GetCPUTime() : 0;
		snapshot.ee_cycle = cpuRegs.cycle;
		snapshot.iop_cycle = psxRegs.cycle;
		snapshot.ee_pc = cpuRegs.pc;
		snapshot.iop_pc = psxRegs.pc;
		snapshot.cpu_stage_profiler =
			VitaPerformanceTelemetry::GetCpuStageProfilerSnapshot();
#if defined(VITASX2_VIF_EPOCH_CENSUS)
		snapshot.vif_epoch = VitaVifEpochCensus::GetSnapshot();
#endif
		snapshot.ee = VitaGetA32EeProviderStats();
#if defined(VITASX2_CPU_PROFILER)
		snapshot.iop = VitaGetA32IopProviderStats();
#endif
		snapshot.vu0 = VitaVU::GetVu0TelemetryStats();
		snapshot.vu1 = VitaVU::GetVu1TelemetryStats();
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
		snapshot.gpu_vu_census =
			VitaGpuVuOpportunityCensus::GetSnapshot();
#endif
		snapshot.mtvu = vu1Thread.GetProducerProfileStats();
		snapshot.gs_producer = s_gs_producer_performance;
		snapshot.gs_worker = GetPublishedGsWorkerPerformance();
		snapshot.completed_vsyncs = snapshot.gs_worker.completed_vsyncs;
		snapshot.gxm = VitaGxmGetPublishedPerformanceCounters();
		snapshot.gpu_vu_direct = VitaGpuVu::GetDirectProgramStatistics();
		snapshot.gpu_vu_programs =
			VitaGpuVu::GetGeneratedProgramRegistryStatistics();
		snapshot.gpu_vu_region_bundles =
			VitaGpuVu::GetGeneratedUniversalRegionBundleStatistics();
		snapshot.gpu_vu_input = VitaGpuVu::GetInputRingStatistics();
		snapshot.gpu_vu_draw = VitaGpuVu::GetGpuVuDrawStatistics();
		return snapshot;
	}

	static LivePerformanceSnapshot CaptureLivePerformanceSnapshot(
		u64 producer_vsyncs)
	{
		LivePerformanceSnapshot snapshot;
		snapshot.wall = Common::Timer::GetCurrentValue();
		snapshot.ee_cpu_us = Threading::GetThreadCpuTime();
		snapshot.vu_cpu_us = THREAD_VU1 && vu1Thread.IsOpen() ?
			vu1Thread.GetThreadHandle().GetCPUTime() : 0;
		snapshot.gs_cpu_us = s_thread.Joinable() ? s_thread.GetCPUTime() : 0;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		// CompleteGpuVuGxmCall already records these measurements around the
		// real API call, outside the journal publisher. Expose the existing
		// counters without adding timers, GXM calls, waits or hot-path atomics.
		for (u32 kind = 1u; kind < GpuVuGxmCallKindCount; ++kind)
		{
			auto& call = snapshot.gxm_host_calls[kind];
			call.samples = s_gpu_vu_gxm_call_samples[kind].load(
				std::memory_order_relaxed);
			call.total_us = s_gpu_vu_gxm_call_total_us[kind].load(
				std::memory_order_relaxed);
			call.max_us = s_gpu_vu_gxm_call_max_us[kind].load(
				std::memory_order_relaxed);
		}
		snapshot.gpu_vu_notification_cpu_us =
			s_gpu_vu_notification_thread.Joinable() ?
				s_gpu_vu_notification_thread.GetCPUTime() : 0;
		snapshot.gpu_vu_notification_arms =
			s_gpu_vu_notification_arms.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_polls =
			s_gpu_vu_notification_polls.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_wakes =
			s_gpu_vu_notification_wakes.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_deferrals =
			s_gpu_vu_notification_deferrals.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_pending_queues =
			s_gpu_vu_notification_pending_queues.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_pending_replacements =
			s_gpu_vu_notification_pending_replacements.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_notification_pending_chains =
			s_gpu_vu_notification_pending_chains.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_sequence =
			s_gpu_vu_notification_sequence.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_pending_sequence =
			s_gpu_vu_notification_pending_sequence.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_notification_address =
			s_gpu_vu_notification_address.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_pending_address =
			s_gpu_vu_notification_pending_address.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_notification_request =
			s_gpu_vu_notification_request.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_completed =
			s_gpu_vu_notification_completed.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_value =
			s_gpu_vu_notification_value.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_observed =
			s_gpu_vu_notification_observed.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_pending_state =
			s_gpu_vu_notification_pending_state.load(std::memory_order_relaxed);
		snapshot.gpu_vu_notification_pending_value =
			s_gpu_vu_notification_pending_value.load(std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_ticket =
			s_gpu_vu_submission_watchdog_active.load(std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_stage =
			s_gpu_vu_submission_watchdog_stage.load(std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_sequence =
			s_gpu_vu_submission_watchdog_sequence.load(std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_scene =
			s_gpu_vu_submission_watchdog_scene.load(std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_input_owner =
			s_gpu_vu_submission_watchdog_input_owner.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_output_address =
			s_gpu_vu_submission_watchdog_output_address.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_input_slot =
			s_gpu_vu_submission_watchdog_input_slot.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_input_generation =
			s_gpu_vu_submission_watchdog_input_generation.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_submission_watchdog_retirement_slot =
			s_gpu_vu_submission_watchdog_retirement_slot.load(
				std::memory_order_relaxed);
		snapshot.gpu_vu_handoff_watchdog_ticket =
			s_gpu_vu_handoff_watchdog_active.load(std::memory_order_relaxed);
		snapshot.gpu_vu_handoff_watchdog_stage =
			s_gpu_vu_handoff_watchdog_stage.load(std::memory_order_relaxed);
		snapshot.gpu_vu_handoff_watchdog_sequence =
			s_gpu_vu_handoff_watchdog_sequence.load(std::memory_order_relaxed);
#endif
		snapshot.producer_vsyncs = producer_vsyncs;
		snapshot.completed_vsyncs =
			s_gs_published_completed_vsyncs.load(std::memory_order_acquire);
		snapshot.gs_worker = GetPublishedGsWorkerPerformance();
		snapshot.gxm = VitaGxmGetPublishedPerformanceCounters();
		snapshot.gpu_vu = VitaGpuVu::GetUniversalGpuVuEpochStatistics();
		return snapshot;
	}

	static void RecordLivePerformanceProfile(u64 producer_vsyncs)
	{
		constexpr u64 MINIMUM_WINDOW_US = 2000000;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (!s_live_performance.started)
		{
			s_live_performance.start =
				CaptureLivePerformanceSnapshot(producer_vsyncs);
			s_live_performance.started = true;
			return;
		}
		const u64 elapsed_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(
				now - s_live_performance.start.wall) * 1000000.0);
		if (elapsed_us < MINIMUM_WINDOW_US)
			return;

		const LivePerformanceSnapshot end =
			CaptureLivePerformanceSnapshot(producer_vsyncs);
		const LivePerformanceSnapshot& start = s_live_performance.start;
		const u64 wall_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(end.wall - start.wall) *
			1000000.0);
		if (wall_us == 0)
			return;
		const auto delta = [](u64 current, u64 previous) {
			return current >= previous ? current - previous : current;
		};
		const auto rate = [wall_us](u64 count) {
			return static_cast<double>(count) * 1000000.0 /
				static_cast<double>(wall_us);
		};
		const auto utilization = [wall_us](u64 cpu_us) {
			return static_cast<double>(cpu_us) * 100.0 /
				static_cast<double>(wall_us);
		};
		const u64 guest_vsyncs = delta(
			end.producer_vsyncs, start.producer_vsyncs);
		const u64 completed_vsyncs = delta(
			end.completed_vsyncs, start.completed_vsyncs);
		const u64 presents = delta(
			end.gxm.present_calls, start.gxm.present_calls);
		const u64 gpu_vu_gxm_draws = delta(
			end.gxm.gpu_vu_draw_calls, start.gxm.gpu_vu_draw_calls);
		const u64 gpu_vu_gxm_objects = delta(
			end.gxm.gpu_vu_descriptor_objects,
			start.gxm.gpu_vu_descriptor_objects);
		const u64 fixed_epochs = delta(
			end.gpu_vu.universal_provider_epochs,
			start.gpu_vu.universal_provider_epochs);
		const u64 fixed_jobs = delta(
			end.gpu_vu.universal_provider_jobs,
			start.gpu_vu.universal_provider_jobs);
		const u64 generated_epochs = delta(
			end.gpu_vu.generated_provider_epochs,
			start.gpu_vu.generated_provider_epochs);
		const u64 generated_jobs = delta(
			end.gpu_vu.generated_provider_jobs,
			start.gpu_vu.generated_provider_jobs);
		const u64 accepted_pairs = delta(
			end.gpu_vu.accepted_pairs, start.gpu_vu.accepted_pairs);
		const u64 path1_packets = delta(
			end.gpu_vu.accepted_path1_packets,
			start.gpu_vu.accepted_path1_packets);
		const u64 path1_qwords = delta(
			end.gpu_vu.accepted_path1_qwords,
			start.gpu_vu.accepted_path1_qwords);
		const u64 residency_samples = delta(
			end.gpu_vu.gpu_residency_samples,
			start.gpu_vu.gpu_residency_samples);
		const u64 residency_us = delta(
			end.gpu_vu.gpu_residency_wall_us,
			start.gpu_vu.gpu_residency_wall_us);
		const u64 ee_cpu_us = delta(end.ee_cpu_us, start.ee_cpu_us);
		const u64 vu_cpu_us = delta(end.vu_cpu_us, start.vu_cpu_us);
		const u64 gs_cpu_us = delta(end.gs_cpu_us, start.gs_cpu_us);
		const u64 gs_transfer_calls = delta(end.gs_worker.transfer_calls,
			start.gs_worker.transfer_calls);
		const u64 gs_transfer_wall_us = delta(end.gs_worker.transfer_wall_us,
			start.gs_worker.transfer_wall_us);
		const u64 gs_vsync_calls = delta(end.gs_worker.vsync_calls,
			start.gs_worker.vsync_calls);
		const u64 gs_vsync_wall_us = delta(end.gs_worker.vsync_wall_us,
			start.gs_worker.vsync_wall_us);
		const u64 renderer_draw_calls = delta(end.gxm.renderer_draw_calls,
			start.gxm.renderer_draw_calls);
		const u64 renderer_draw_wall_us = delta(end.gxm.renderer_draw_wall_us,
			start.gxm.renderer_draw_wall_us);
		const u64 device_render_calls = delta(end.gxm.device_render_calls,
			start.gxm.device_render_calls);
		const u64 device_render_wall_us = delta(end.gxm.device_render_wall_us,
			start.gxm.device_render_wall_us);
		const u64 renderer_analysis_wall_us =
			renderer_draw_wall_us >= device_render_wall_us ?
				renderer_draw_wall_us - device_render_wall_us : 0;
		u64 gxm_host_wall_us = 0;
		for (size_t i = 0; i < end.gxm.host_calls.size(); i++)
		{
			gxm_host_wall_us += delta(end.gxm.host_calls[i].wall_us,
				start.gxm.host_calls[i].wall_us);
		}
		const u64 gpu_vu_notification_cpu_us = delta(
			end.gpu_vu_notification_cpu_us,
			start.gpu_vu_notification_cpu_us);
		const u64 gpu_vu_notification_arms = delta(
			end.gpu_vu_notification_arms,
			start.gpu_vu_notification_arms);
		const u64 gpu_vu_notification_polls = delta(
			end.gpu_vu_notification_polls,
			start.gpu_vu_notification_polls);
		const u64 gpu_vu_notification_wakes = delta(
			end.gpu_vu_notification_wakes,
			start.gpu_vu_notification_wakes);
		const u64 gpu_vu_notification_deferrals = delta(
			end.gpu_vu_notification_deferrals,
			start.gpu_vu_notification_deferrals);
		const u64 gpu_vu_notification_pending_queues = delta(
			end.gpu_vu_notification_pending_queues,
			start.gpu_vu_notification_pending_queues);
		const u64 gpu_vu_notification_pending_replacements = delta(
			end.gpu_vu_notification_pending_replacements,
			start.gpu_vu_notification_pending_replacements);
		const u64 gpu_vu_notification_pending_chains = delta(
			end.gpu_vu_notification_pending_chains,
			start.gpu_vu_notification_pending_chains);
		const double guest_frame_ms = guest_vsyncs ?
			static_cast<double>(wall_us) / 1000.0 /
				static_cast<double>(guest_vsyncs) : 0.0;
		const u64 window = ++s_live_performance.window;

#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
		// The 120-VSync report can never describe allocation failures which
		// happen before its first boundary. Sample at this existing two-second
		// telemetry boundary, not per draw/Execute or in an allocator callback.
		// Native cache counters are CPU0-owned here; they are cumulative work,
		// not an assertion about which owner retains every heap byte.
		const struct mallinfo heap = mallinfo();
		const u32 heap_arena = static_cast<u32>(heap.arena);
		const u32 heap_remaining = _newlib_heap_size_user > heap_arena ?
			_newlib_heap_size_user - heap_arena : 0u;
		const u64 heap_headroom = static_cast<u64>(heap_remaining) +
			static_cast<u32>(heap.fordblks);
		const auto ee_cache = VitaGetA32EeProviderStats();
		const auto vu_cache = VitaVU::GetVu1TelemetryStats();
		Console.WriteLn(
			"Vita perf v=1 window=%llu kind=memory_live heap_limit=%u "
			"heap_arena=%u heap_used=%u heap_free=%u heap_top=%u "
			"heap_chunks=%u heap_headroom=%llu heap_pressure=%u "
			"ee_cache_used=%llu ee_cache_resets=%u ee_generated_blocks=%llu "
			"vu1_generated_blocks=%llu vu1_generated_pairs=%llu "
			"vu1_versions_created=%llu",
			static_cast<unsigned long long>(window), _newlib_heap_size_user,
			heap_arena, static_cast<u32>(heap.uordblks),
			static_cast<u32>(heap.fordblks), static_cast<u32>(heap.keepcost),
			static_cast<u32>(heap.ordblks),
			static_cast<unsigned long long>(heap_headroom),
			heap_headroom < 4u * 1024u * 1024u ? 1u : 0u,
			static_cast<unsigned long long>(ee_cache.code_cache_used),
			ee_cache.code_cache_resets,
			static_cast<unsigned long long>(ee_cache.generated_blocks),
			static_cast<unsigned long long>(vu_cache.generated_blocks),
			static_cast<unsigned long long>(vu_cache.generated_pairs),
			static_cast<unsigned long long>(vu_cache.program_versions_created));
#endif

		// CPU0/1/2 are the pinned EE, MTVU and MTGS owners on Vita. GXM exposes
		// no public in-process SGX utilization counter, so publish exact work and
		// notification-latency proxies with sgx_hw_util_available=0.
		Console.WriteLn(
			"Vita perf v=2 window=%llu kind=frame_busy wall_us=%llu "
			"guest_vsyncs=%llu guest_vsync_fps=%.2f guest_frame_ms=%.2f "
			"gs_completed=%llu gs_completed_fps=%.2f presents=%llu "
			"present_fps=%.2f cpu0_ee_util=%.1f cpu1_mtvu_util=%.1f "
			"cpu2_mtgs_util=%.1f gpu_vu_notify_util=%.1f "
			"gpu_vu_notify_arms=%llu gpu_vu_notify_polls=%llu "
			"gpu_vu_notify_wakes=%llu gpu_vu_notify_deferrals=%llu "
			"universal_epochs=%llu generated_epochs=%llu "
			"gpu_vu_jobs=%llu gpu_vu_jobs_per_s=%.2f accepted_pairs=%llu "
			"pairs_per_s=%.2f path1_packets=%llu path1_qwords=%llu "
			"gxm_draws_per_s=%.2f gxm_indices_per_s=%.2f "
			"gpu_vu_gxm_draws=%llu gpu_vu_gxm_draws_per_s=%.2f "
			"gpu_vu_objects=%llu gpu_vu_objects_per_draw=%.2f "
			"gpu_residency_samples=%llu gpu_residency_avg_us=%llu "
			"gpu_residency_max_lifetime_us=%llu sgx_hw_util_available=0",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(wall_us),
			static_cast<unsigned long long>(guest_vsyncs), rate(guest_vsyncs),
			guest_frame_ms,
			static_cast<unsigned long long>(completed_vsyncs),
			rate(completed_vsyncs),
			static_cast<unsigned long long>(presents), rate(presents),
			utilization(ee_cpu_us), utilization(vu_cpu_us),
			utilization(gs_cpu_us),
			utilization(gpu_vu_notification_cpu_us),
			static_cast<unsigned long long>(gpu_vu_notification_arms),
			static_cast<unsigned long long>(gpu_vu_notification_polls),
			static_cast<unsigned long long>(gpu_vu_notification_wakes),
			static_cast<unsigned long long>(gpu_vu_notification_deferrals),
			static_cast<unsigned long long>(fixed_epochs),
			static_cast<unsigned long long>(generated_epochs),
			static_cast<unsigned long long>(fixed_jobs + generated_jobs),
			rate(fixed_jobs + generated_jobs),
			static_cast<unsigned long long>(accepted_pairs),
			rate(accepted_pairs),
			static_cast<unsigned long long>(path1_packets),
			static_cast<unsigned long long>(path1_qwords),
			rate(delta(end.gxm.draw_calls, start.gxm.draw_calls)),
			rate(delta(end.gxm.draw_indices, start.gxm.draw_indices)),
			static_cast<unsigned long long>(gpu_vu_gxm_draws),
			rate(gpu_vu_gxm_draws),
			static_cast<unsigned long long>(gpu_vu_gxm_objects),
			gpu_vu_gxm_draws ?
				static_cast<double>(gpu_vu_gxm_objects) /
					static_cast<double>(gpu_vu_gxm_draws) : 0.0,
			static_cast<unsigned long long>(residency_samples),
			static_cast<unsigned long long>(residency_samples ?
				residency_us / residency_samples : 0),
			static_cast<unsigned long long>(
				end.gpu_vu.gpu_residency_wall_us_max));
		Console.WriteLn(
			"Vita perf v=1 window=%llu kind=gs_cpu2_stage guest_frames=%llu "
			"cpu_us=%llu transfer_calls=%llu transfer_wall_us=%llu "
			"renderer_draw_calls=%llu renderer_draw_wall_us=%llu "
			"renderer_analysis_wall_us=%llu device_render_calls=%llu "
			"device_render_wall_us=%llu gxm_host_wall_us=%llu "
			"texture_uploads=%llu texture_upload_bytes=%llu "
			"texture_readbacks=%llu texture_readback_bytes=%llu "
			"vsync_calls=%llu vsync_wall_us=%llu wall_intervals_overlap=1",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(completed_vsyncs),
			static_cast<unsigned long long>(gs_cpu_us),
			static_cast<unsigned long long>(gs_transfer_calls),
			static_cast<unsigned long long>(gs_transfer_wall_us),
			static_cast<unsigned long long>(renderer_draw_calls),
			static_cast<unsigned long long>(renderer_draw_wall_us),
			static_cast<unsigned long long>(renderer_analysis_wall_us),
			static_cast<unsigned long long>(device_render_calls),
			static_cast<unsigned long long>(device_render_wall_us),
			static_cast<unsigned long long>(gxm_host_wall_us),
			static_cast<unsigned long long>(delta(end.gxm.texture_uploads,
				start.gxm.texture_uploads)),
			static_cast<unsigned long long>(delta(end.gxm.texture_upload_bytes,
				start.gxm.texture_upload_bytes)),
			static_cast<unsigned long long>(delta(end.gxm.texture_readbacks,
				start.gxm.texture_readbacks)),
			static_cast<unsigned long long>(delta(end.gxm.texture_readback_bytes,
				start.gxm.texture_readback_bytes)),
			static_cast<unsigned long long>(gs_vsync_calls),
			static_cast<unsigned long long>(gs_vsync_wall_us));
		Console.WriteLn(
			"Vita perf v=1 window=%llu kind=gs_cpu2_renderer_stage "
			"source_calls=%llu source_wall_us=%llu depth_source_calls=%llu depth_source_wall_us=%llu "
			"target_calls=%llu target_wall_us=%llu create_calls=%llu create_wall_us=%llu "
			"draw_prims_calls=%llu draw_prims_wall_us=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureSourceLookup)].calls,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureSourceLookup)].calls)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureSourceLookup)].wall_us,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureSourceLookup)].wall_us)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureDepthSourceLookup)].calls,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureDepthSourceLookup)].calls)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureDepthSourceLookup)].wall_us,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureDepthSourceLookup)].wall_us)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetLookup)].calls,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetLookup)].calls)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetLookup)].wall_us,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetLookup)].wall_us)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetCreate)].calls,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetCreate)].calls)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetCreate)].wall_us,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::TextureTargetCreate)].wall_us)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::DrawPrims)].calls,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::DrawPrims)].calls)),
			static_cast<unsigned long long>(delta(
				end.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::DrawPrims)].wall_us,
				start.gxm.renderer_stages[static_cast<size_t>(VitaGxmRendererStage::DrawPrims)].wall_us)));
		for (size_t i = static_cast<size_t>(VitaGxmRendererStage::SourceMapFind);
			i < static_cast<size_t>(VitaGxmRendererStage::Count); i++)
		{
			const auto& stage_end = end.gxm.renderer_stages[i];
			const auto& stage_start = start.gxm.renderer_stages[i];
			Console.WriteLn(
				"Vita perf v=1 window=%llu kind=gs_cpu2_renderer_substage "
				"stage=%s calls=%llu wall_us=%llu lifetime_max_us=%llu",
				static_cast<unsigned long long>(window), VitaGxmRendererStageNames[i],
				static_cast<unsigned long long>(delta(stage_end.calls, stage_start.calls)),
				static_cast<unsigned long long>(delta(stage_end.wall_us, stage_start.wall_us)),
				static_cast<unsigned long long>(stage_end.max_lifetime_us));
		}
		for (size_t i = 0; i < end.gxm.host_calls.size(); i++)
		{
			const u64 calls = delta(end.gxm.host_calls[i].calls,
				start.gxm.host_calls[i].calls);
			const u64 call_wall_us = delta(end.gxm.host_calls[i].wall_us,
				start.gxm.host_calls[i].wall_us);
			if (calls == 0 && call_wall_us == 0)
				continue;
			Console.WriteLn(
				"Vita perf v=1 window=%llu kind=gs_gxm_host call=%s "
				"guest_frames=%llu calls=%llu wall_us=%llu avg_us=%llu "
				"max_lifetime_us=%llu gpu_timer=0",
				static_cast<unsigned long long>(window), VitaGxmHostCallNames[i],
				static_cast<unsigned long long>(completed_vsyncs),
				static_cast<unsigned long long>(calls),
				static_cast<unsigned long long>(call_wall_us),
				static_cast<unsigned long long>(calls ? call_wall_us / calls : 0),
				static_cast<unsigned long long>(
					end.gxm.host_calls[i].max_lifetime_us));
		}
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		for (u32 kind = 1u; kind < GpuVuGxmCallKindCount; ++kind)
		{
			const auto& call = end.gxm_host_calls[kind];
			const auto& previous = start.gxm_host_calls[kind];
			const u64 samples = delta(call.samples, previous.samples);
			const u64 call_us = delta(call.total_us, previous.total_us);
			if (samples == 0u && call_us == 0u)
				continue;
			// These are host wall-time intervals: driver CPU work, scheduling
			// and any wait inside libGXM are included. They are not GPU execution
			// time or SGX utilization. Max is explicitly lifetime, not window.
			// Relaxed cross-thread counters can straddle a call at the boundary.
			Console.WriteLn(
				"GPU-VU GXM host v=1 window=%llu kind=%s guest_frames=%llu "
				"calls=%llu wall_us=%llu avg_us=%llu ms_per_frame=%.3f "
				"max_lifetime_us=%llu snapshot=relaxed gpu_timer=0",
				static_cast<unsigned long long>(window),
				GpuVuGxmCallKindName(kind),
				static_cast<unsigned long long>(guest_vsyncs),
				static_cast<unsigned long long>(samples),
				static_cast<unsigned long long>(call_us),
				static_cast<unsigned long long>(samples ? call_us / samples : 0u),
				guest_vsyncs ? static_cast<double>(call_us) /
					(1000.0 * static_cast<double>(guest_vsyncs)) : 0.0,
				static_cast<unsigned long long>(call.max_us));
		}
#endif
		Console.WriteLn(
			"GPU-VU fence v=1 window=%llu request=%u completed=%u active=%u "
			"sequence=%llu address=%08x required=%u observed=%u "
			"pending_state=%u pending_sequence=%llu pending_address=%08x "
			"pending_required=%u queues=%llu replacements=%llu chains=%llu "
			"submission_ticket=%u submission_stage=%u submission_sequence=%llu "
			"submission_scene=%llu submission_input=%08x:%u:%u "
			"submission_output=%08x submission_retirement_slot=%u "
			"handoff_ticket=%u handoff_stage=%u handoff_sequence=%llu.",
			static_cast<unsigned long long>(window),
			end.gpu_vu_notification_request,
			end.gpu_vu_notification_completed,
			end.gpu_vu_notification_request !=
				end.gpu_vu_notification_completed ? 1u : 0u,
			static_cast<unsigned long long>(
				end.gpu_vu_notification_sequence),
			static_cast<u32>(end.gpu_vu_notification_address),
			end.gpu_vu_notification_value,
			end.gpu_vu_notification_observed,
			end.gpu_vu_notification_pending_state,
			static_cast<unsigned long long>(
				end.gpu_vu_notification_pending_sequence),
			static_cast<u32>(end.gpu_vu_notification_pending_address),
			end.gpu_vu_notification_pending_value,
			static_cast<unsigned long long>(
				gpu_vu_notification_pending_queues),
			static_cast<unsigned long long>(
				gpu_vu_notification_pending_replacements),
			static_cast<unsigned long long>(
				gpu_vu_notification_pending_chains),
			end.gpu_vu_submission_watchdog_ticket,
			end.gpu_vu_submission_watchdog_stage,
			static_cast<unsigned long long>(
				end.gpu_vu_submission_watchdog_sequence),
			static_cast<unsigned long long>(
				end.gpu_vu_submission_watchdog_scene),
			static_cast<u32>(end.gpu_vu_submission_watchdog_input_owner),
			end.gpu_vu_submission_watchdog_input_slot,
			end.gpu_vu_submission_watchdog_input_generation,
			static_cast<u32>(end.gpu_vu_submission_watchdog_output_address),
			end.gpu_vu_submission_watchdog_retirement_slot,
			end.gpu_vu_handoff_watchdog_ticket,
			end.gpu_vu_handoff_watchdog_stage,
			static_cast<unsigned long long>(
				end.gpu_vu_handoff_watchdog_sequence));

		const auto per_guest_frame_ms = [guest_vsyncs](u64 microseconds) {
			return guest_vsyncs ?
				static_cast<double>(microseconds) / 1000.0 /
					static_cast<double>(guest_vsyncs) : 0.0;
		};
		const u64 live_resolutions = delta(
			end.gpu_vu.generated_live_contract_resolutions,
			start.gpu_vu.generated_live_contract_resolutions);
		const u64 live_resolution_us = delta(
			end.gpu_vu.generated_live_contract_resolution_wall_us,
			start.gpu_vu.generated_live_contract_resolution_wall_us);
		const u64 live_control_cache_hits = delta(
			end.gpu_vu.generated_live_control_cache_hits,
			start.gpu_vu.generated_live_control_cache_hits);
		const u64 live_control_cache_misses = delta(
			end.gpu_vu.generated_live_control_cache_misses,
			start.gpu_vu.generated_live_control_cache_misses);
		const u64 live_control_evaluations =
			live_control_cache_hits + live_control_cache_misses;
		const u64 descriptor_builds = delta(
			end.gpu_vu.generated_descriptor_builds,
			start.gpu_vu.generated_descriptor_builds);
		const u64 descriptor_us = delta(
			end.gpu_vu.generated_descriptor_build_wall_us,
			start.gpu_vu.generated_descriptor_build_wall_us);
		const u64 descriptor_cache_us = delta(
			end.gpu_vu.generated_descriptor_cache_wall_us,
			start.gpu_vu.generated_descriptor_cache_wall_us);
		const u64 descriptor_proof_us = delta(
			end.gpu_vu.generated_descriptor_runtime_proof_wall_us,
			start.gpu_vu.generated_descriptor_runtime_proof_wall_us);
		const u64 descriptor_input_us = delta(
			end.gpu_vu.generated_descriptor_input_pack_wall_us,
			start.gpu_vu.generated_descriptor_input_pack_wall_us);
		const u64 descriptor_layout_us = delta(
			end.gpu_vu.generated_descriptor_store_layout_wall_us,
			start.gpu_vu.generated_descriptor_store_layout_wall_us);
		const u64 descriptor_final_us = delta(
			end.gpu_vu.generated_descriptor_final_state_wall_us,
			start.gpu_vu.generated_descriptor_final_state_wall_us);
		const u64 descriptor_transaction_us = delta(
			end.gpu_vu.generated_descriptor_transaction_wall_us,
			start.gpu_vu.generated_descriptor_transaction_wall_us);
		const u64 descriptor_transaction_acquire_us = delta(
			end.gpu_vu.generated_descriptor_transaction_acquire_wall_us,
			start.gpu_vu.generated_descriptor_transaction_acquire_wall_us);
		const u64 descriptor_transaction_capture_us = delta(
			end.gpu_vu.generated_descriptor_transaction_capture_wall_us,
			start.gpu_vu.generated_descriptor_transaction_capture_wall_us);
		const u64 descriptor_transaction_configure_us = delta(
			end.gpu_vu.generated_descriptor_transaction_configure_wall_us,
			start.gpu_vu.generated_descriptor_transaction_configure_wall_us);
		const u64 private_advances = delta(
			end.gpu_vu.generated_private_state_advances,
			start.gpu_vu.generated_private_state_advances);
		const u64 private_advance_us = delta(
			end.gpu_vu.generated_private_state_advance_wall_us,
			start.gpu_vu.generated_private_state_advance_wall_us);
		const u64 private_same_layout_replacements = delta(
			end.gpu_vu.generated_private_same_layout_replacements,
			start.gpu_vu.generated_private_same_layout_replacements);
		const u64 private_covered_layout_replacements = delta(
			end.gpu_vu.generated_private_covered_layout_replacements,
			start.gpu_vu.generated_private_covered_layout_replacements);
		const u64 private_covered_owner_slots = delta(
			end.gpu_vu.generated_private_covered_owner_slots,
			start.gpu_vu.generated_private_covered_owner_slots);
		const u64 private_register_owner_replacements = delta(
			end.gpu_vu.generated_private_register_owner_replacements,
			start.gpu_vu.generated_private_register_owner_replacements);
		const u64 private_register_owner_replacement_slots = delta(
			end.gpu_vu.generated_private_register_owner_replacement_slots,
			start.gpu_vu.generated_private_register_owner_replacement_slots);
		const u64 private_register_owner_remaps = delta(
			end.gpu_vu.generated_private_register_owner_remaps,
			start.gpu_vu.generated_private_register_owner_remaps);
		const u64 bridge_calls = delta(
			end.gpu_vu.generated_private_bridge_calls,
			start.gpu_vu.generated_private_bridge_calls);
		const u64 bridge_pairs = delta(
			end.gpu_vu.generated_private_bridge_pairs,
			start.gpu_vu.generated_private_bridge_pairs);
			const u64 bridge_us = delta(
				end.gpu_vu.generated_private_bridge_wall_us,
				start.gpu_vu.generated_private_bridge_wall_us);
			const u64 formula_calls = delta(
				end.gpu_vu.generated_state_formula_calls,
				start.gpu_vu.generated_state_formula_calls);
			const u64 formula_pairs = delta(
				end.gpu_vu.generated_state_formula_logical_pairs,
				start.gpu_vu.generated_state_formula_logical_pairs);
			const u64 formula_operations = delta(
				end.gpu_vu.generated_state_formula_operations,
				start.gpu_vu.generated_state_formula_operations);
			const u64 formula_us = delta(
				end.gpu_vu.generated_state_formula_wall_us,
				start.gpu_vu.generated_state_formula_wall_us);
		const u64 batch_commits = delta(
			end.gpu_vu.generated_batch_commits,
			start.gpu_vu.generated_batch_commits);
		const u64 batch_commit_us = delta(
			end.gpu_vu.generated_batch_commit_wall_us,
			start.gpu_vu.generated_batch_commit_wall_us);
		const u64 drain_waits = delta(
			end.gpu_vu.generated_batch_drain_waits,
			start.gpu_vu.generated_batch_drain_waits);
		const u64 drain_wait_us = delta(
			end.gpu_vu.generated_batch_drain_wait_wall_us,
			start.gpu_vu.generated_batch_drain_wait_wall_us);
		const u64 drain_polls = delta(
			end.gpu_vu.generated_batch_drain_polls,
			start.gpu_vu.generated_batch_drain_polls);
		const u64 hot_calls = delta(
			end.gpu_vu.generated_hot_execute_calls,
			start.gpu_vu.generated_hot_execute_calls);
		const u64 hot_us = delta(
			end.gpu_vu.generated_hot_execute_wall_us,
			start.gpu_vu.generated_hot_execute_wall_us);
		const u64 queue_calls = delta(
			end.gpu_vu.generated_queue_calls,
			start.gpu_vu.generated_queue_calls);
		const u64 queue_us = delta(
			end.gpu_vu.generated_queue_wall_us,
			start.gpu_vu.generated_queue_wall_us);
		const u64 gather_calls = delta(
			end.gpu_vu.generated_gather_calls,
			start.gpu_vu.generated_gather_calls);
		const u64 gather_executes = delta(
			end.gpu_vu.generated_gather_executes,
			start.gpu_vu.generated_gather_executes);
		const u64 gather_us = delta(
			end.gpu_vu.generated_gather_wall_us,
			start.gpu_vu.generated_gather_wall_us);
		const u64 publication_calls = delta(
			end.gpu_vu.generated_publication_calls,
			start.gpu_vu.generated_publication_calls);
		const u64 publication_draws = delta(
			end.gpu_vu.generated_publication_draws,
			start.gpu_vu.generated_publication_draws);
		const u64 publication_us = delta(
			end.gpu_vu.generated_publication_wall_us,
			start.gpu_vu.generated_publication_wall_us);
		const u64 retirement_polls = delta(
			end.gpu_vu.generated_retirement_polls,
			start.gpu_vu.generated_retirement_polls);
		const u64 retirement_poll_us = delta(
			end.gpu_vu.generated_retirement_poll_wall_us,
			start.gpu_vu.generated_retirement_poll_wall_us);
		const u64 execute_records = delta(
			end.gpu_vu.generated_mtvu_execute_records,
			start.gpu_vu.generated_mtvu_execute_records);
		const u64 execute_record_us = delta(
			end.gpu_vu.generated_mtvu_execute_record_wall_us,
			start.gpu_vu.generated_mtvu_execute_record_wall_us);
		const u64 vif_records = delta(
			end.gpu_vu.generated_mtvu_vif_records,
			start.gpu_vu.generated_mtvu_vif_records);
		const u64 vif_record_us = delta(
			end.gpu_vu.generated_mtvu_vif_record_wall_us,
			start.gpu_vu.generated_mtvu_vif_record_wall_us);
		const u64 other_records = delta(
			end.gpu_vu.generated_mtvu_other_records,
			start.gpu_vu.generated_mtvu_other_records);
		const u64 other_record_us = delta(
			end.gpu_vu.generated_mtvu_other_record_wall_us,
			start.gpu_vu.generated_mtvu_other_record_wall_us);
		const u64 housekeeping_calls = delta(
			end.gpu_vu.generated_mtvu_housekeeping_calls,
			start.gpu_vu.generated_mtvu_housekeeping_calls);
		const u64 housekeeping_us = delta(
			end.gpu_vu.generated_mtvu_housekeeping_wall_us,
			start.gpu_vu.generated_mtvu_housekeeping_wall_us);
		Console.WriteLn(
				"GPU-VU CPU1 profile v=8 window=%llu guest_frames=%llu "
			"cpu1_us=%llu cpu1_ms_per_frame=%.2f "
			"live_resolutions=%llu live_resolution_us=%llu "
			"live_resolution_ms_per_frame=%.2f "
			"live_control_cache_hits=%llu live_control_cache_misses=%llu "
			"live_control_evaluations=%llu "
			"live_control_evals_per_resolution=%.2f "
			"descriptor_builds=%llu descriptor_us=%llu "
			"descriptor_ms_per_frame=%.2f "
			"descriptor_cache_ms_per_frame=%.2f "
			"descriptor_proof_ms_per_frame=%.2f "
			"descriptor_input_ms_per_frame=%.2f "
			"descriptor_layout_ms_per_frame=%.2f "
			"descriptor_final_ms_per_frame=%.2f "
			"descriptor_transaction_ms_per_frame=%.2f "
			"descriptor_transaction_acquire_ms_per_frame=%.2f "
			"descriptor_transaction_capture_ms_per_frame=%.2f "
			"descriptor_transaction_configure_ms_per_frame=%.2f "
			"private_advances=%llu "
			"private_advance_us=%llu private_advance_ms_per_frame=%.2f "
			"private_same_layout_replacements=%llu "
			"private_covered_layout_replacements=%llu "
			"private_covered_owner_slots=%llu "
			"private_register_owner_replacements=%llu "
			"private_register_owner_replacement_slots=%llu "
			"private_register_owner_remaps=%llu "
				"bridge_calls=%llu bridge_pairs=%llu bridge_us=%llu "
				"bridge_ms_per_frame=%.2f formula_calls=%llu "
				"formula_pairs=%llu formula_ops=%llu formula_us=%llu "
				"formula_ms_per_frame=%.2f cpu_semantic_pairs=%llu "
				"batch_commits=%llu "
			"batch_commit_us=%llu batch_commit_ms_per_frame=%.2f "
			"drain_waits=%llu drain_wait_us=%llu drain_polls=%llu "
			"drain_wait_ms_per_frame=%.2f cpu_vu_calls=%llu "
			"cpu_fallbacks=%llu fallback_pairs=%llu.",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(guest_vsyncs),
			static_cast<unsigned long long>(vu_cpu_us),
			per_guest_frame_ms(vu_cpu_us),
			static_cast<unsigned long long>(live_resolutions),
			static_cast<unsigned long long>(live_resolution_us),
			per_guest_frame_ms(live_resolution_us),
			static_cast<unsigned long long>(live_control_cache_hits),
			static_cast<unsigned long long>(live_control_cache_misses),
			static_cast<unsigned long long>(live_control_evaluations),
			live_resolutions ?
				static_cast<double>(live_control_evaluations) /
					static_cast<double>(live_resolutions) : 0.0,
			static_cast<unsigned long long>(descriptor_builds),
			static_cast<unsigned long long>(descriptor_us),
			per_guest_frame_ms(descriptor_us),
			per_guest_frame_ms(descriptor_cache_us),
			per_guest_frame_ms(descriptor_proof_us),
			per_guest_frame_ms(descriptor_input_us),
			per_guest_frame_ms(descriptor_layout_us),
			per_guest_frame_ms(descriptor_final_us),
			per_guest_frame_ms(descriptor_transaction_us),
			per_guest_frame_ms(descriptor_transaction_acquire_us),
			per_guest_frame_ms(descriptor_transaction_capture_us),
			per_guest_frame_ms(descriptor_transaction_configure_us),
			static_cast<unsigned long long>(private_advances),
			static_cast<unsigned long long>(private_advance_us),
			per_guest_frame_ms(private_advance_us),
			static_cast<unsigned long long>(
				private_same_layout_replacements),
			static_cast<unsigned long long>(
				private_covered_layout_replacements),
			static_cast<unsigned long long>(
				private_covered_owner_slots),
			static_cast<unsigned long long>(
				private_register_owner_replacements),
			static_cast<unsigned long long>(
				private_register_owner_replacement_slots),
			static_cast<unsigned long long>(private_register_owner_remaps),
			static_cast<unsigned long long>(bridge_calls),
			static_cast<unsigned long long>(bridge_pairs),
				static_cast<unsigned long long>(bridge_us),
				per_guest_frame_ms(bridge_us),
				static_cast<unsigned long long>(formula_calls),
				static_cast<unsigned long long>(formula_pairs),
				static_cast<unsigned long long>(formula_operations),
				static_cast<unsigned long long>(formula_us),
				per_guest_frame_ms(formula_us),
				static_cast<unsigned long long>(bridge_pairs),
				static_cast<unsigned long long>(batch_commits),
			static_cast<unsigned long long>(batch_commit_us),
			per_guest_frame_ms(batch_commit_us),
			static_cast<unsigned long long>(drain_waits),
			static_cast<unsigned long long>(drain_wait_us),
			static_cast<unsigned long long>(drain_polls),
			per_guest_frame_ms(drain_wait_us),
			static_cast<unsigned long long>(delta(
				end.gpu_vu.cpu_vu_calls, start.gpu_vu.cpu_vu_calls)),
			static_cast<unsigned long long>(delta(
				end.gpu_vu.cpu_fallbacks, start.gpu_vu.cpu_fallbacks)),
			static_cast<unsigned long long>(delta(
				end.gpu_vu.rejected_pairs, start.gpu_vu.rejected_pairs)));
		// vu_cpu_us is scheduler CPU time. Do not subtract drain_wait_us here:
		// that counter is wall time around a sleeping coarse observer wait.
		const u64 attributed_us = hot_us + formula_us + gather_us +
			publication_us + retirement_poll_us + batch_commit_us;
		const u64 unattributed_us = vu_cpu_us > attributed_us ?
			vu_cpu_us - attributed_us : 0u;
		Console.WriteLn(
			"GPU-VU CPU1 phases v=1 window=%llu guest_frames=%llu "
			"hot_calls=%llu hot_us=%llu hot_ms_per_frame=%.2f "
			"queue_calls=%llu queue_us=%llu queue_ms_per_frame=%.2f "
			"gather_calls=%llu gather_executes=%llu gather_us=%llu "
			"gather_ms_per_frame=%.2f publication_calls=%llu "
			"publication_draws=%llu publication_us=%llu "
			"publication_ms_per_frame=%.2f retirement_polls=%llu "
			"retirement_poll_us=%llu retirement_ms_per_frame=%.2f "
			"attributed_us=%llu unattributed_us=%llu "
			"unattributed_ms_per_frame=%.2f.",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(guest_vsyncs),
			static_cast<unsigned long long>(hot_calls),
			static_cast<unsigned long long>(hot_us),
			per_guest_frame_ms(hot_us),
			static_cast<unsigned long long>(queue_calls),
			static_cast<unsigned long long>(queue_us),
			per_guest_frame_ms(queue_us),
			static_cast<unsigned long long>(gather_calls),
			static_cast<unsigned long long>(gather_executes),
			static_cast<unsigned long long>(gather_us),
			per_guest_frame_ms(gather_us),
			static_cast<unsigned long long>(publication_calls),
			static_cast<unsigned long long>(publication_draws),
			static_cast<unsigned long long>(publication_us),
			per_guest_frame_ms(publication_us),
			static_cast<unsigned long long>(retirement_polls),
			static_cast<unsigned long long>(retirement_poll_us),
			per_guest_frame_ms(retirement_poll_us),
			static_cast<unsigned long long>(attributed_us),
			static_cast<unsigned long long>(unattributed_us),
			per_guest_frame_ms(unattributed_us));
		Console.WriteLn(
			"GPU-VU CPU1 records v=1 window=%llu guest_frames=%llu "
			"execute_records=%llu execute_us=%llu execute_ms_per_frame=%.2f "
			"vif_records=%llu vif_us=%llu vif_ms_per_frame=%.2f "
			"other_records=%llu other_us=%llu other_ms_per_frame=%.2f "
			"housekeeping_calls=%llu housekeeping_us=%llu "
			"housekeeping_ms_per_frame=%.2f record_total_ms_per_frame=%.2f.",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(guest_vsyncs),
			static_cast<unsigned long long>(execute_records),
			static_cast<unsigned long long>(execute_record_us),
			per_guest_frame_ms(execute_record_us),
			static_cast<unsigned long long>(vif_records),
			static_cast<unsigned long long>(vif_record_us),
			per_guest_frame_ms(vif_record_us),
			static_cast<unsigned long long>(other_records),
			static_cast<unsigned long long>(other_record_us),
			per_guest_frame_ms(other_record_us),
			static_cast<unsigned long long>(housekeeping_calls),
			static_cast<unsigned long long>(housekeeping_us),
			per_guest_frame_ms(housekeeping_us),
			per_guest_frame_ms(execute_record_us + vif_record_us +
				other_record_us + housekeeping_us));
		s_live_performance.start = end;
	}

	static void RecordCorrelatedPerformanceProfile()
	{
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
		// The census disables native VU1 block links so each returned block can
		// be attributed to its PCSX2-derived PairPlan. VU-heavy retail can be
		// more than an order of magnitude slower in this measurement-only build;
		// publish a complete early gameplay excerpt rather than holding the
		// personal Vita in an extended SGX-heavy diagnostic run.
		constexpr u32 WARMUP_VSYNCS = 1;
		constexpr u32 WINDOW_VSYNCS = 7;
#elif defined(VITASX2_SHORT_CORRELATED_PROFILE_WINDOW)
		constexpr u32 WARMUP_VSYNCS = 8;
		constexpr u32 WINDOW_VSYNCS = 32;
#else
		constexpr u32 WARMUP_VSYNCS = 60;
		constexpr u32 WINDOW_VSYNCS = 120;
#endif
		if (!s_native_presenter_enabled ||
			!VitaPerformanceTelemetry::IsEnabled())
			return;

		const u64 producer_vsync = ++s_correlated_profile.producer_vsyncs;
		const u32 boundary = ++s_correlated_profile.sampling_boundaries;
		if (!s_correlated_profile.started)
		{
			if (boundary < WARMUP_VSYNCS)
				return;
			s_correlated_profile.start =
				CaptureCorrelatedPerformanceSnapshot(producer_vsync);
			VitaBeginA32EeRegionStatisticsWindow();
			s_correlated_profile.boundaries_at_start = boundary;
			s_correlated_profile.started = true;
			return;
		}
		if (boundary - s_correlated_profile.boundaries_at_start < WINDOW_VSYNCS)
			return;

		const VitaPerformanceTelemetry::ScopedCpuStage diagnostic_stage(
			VitaPerformanceTelemetry::CpuStage::Diagnostics);
		const CorrelatedPerformanceSnapshot end =
			CaptureCorrelatedPerformanceSnapshot(producer_vsync);
		const CorrelatedPerformanceSnapshot& start = s_correlated_profile.start;
		const CorrelatedPerformanceSnapshot& origin = s_correlated_profile.origin;
		const u64 window = ++s_correlated_profile.window;
		const u64 wall_us = static_cast<u64>(Common::Timer::ConvertValueToSeconds(
			end.wall - start.wall) * 1000000.0);
		const u64 ee_cpu_us = CounterDelta(end.ee_cpu_us, start.ee_cpu_us);
		const u64 vu_cpu_us = CounterDelta(end.vu_cpu_us, start.vu_cpu_us);
		const u64 gs_cpu_us = CounterDelta(end.gs_cpu_us, start.gs_cpu_us);
		const auto utilization = [wall_us](u64 cpu_us) {
			return wall_us ? static_cast<double>(cpu_us) * 100.0 /
				static_cast<double>(wall_us) : 0.0;
		};
		const auto ee_fallbacks = [](const VitaA32EeProviderStats& stats) {
			return static_cast<u64>(stats.scan_unsupported_fallbacks) +
				stats.scan_boundary_fallbacks +
				stats.exact_trace_branch_likely_fallbacks +
				stats.execute_failed_fallbacks + stats.interpreter_path_fallbacks;
		};
		static CorrelatedPerformanceBatch output;
		output.Clear();

		output.WriteLn(
			"Vita perf v=1 window=%llu kind=anchor origin=%s origin_vsync=%llu "
			"producer_vsync_start=%llu "
			"producer_vsync_end=%llu completed_vsync_start=%llu completed_vsync_end=%llu "
			"ee_cycle_start=%llu ee_cycle_end=%llu ee_pc_start=%08x ee_pc_end=%08x "
			"iop_cycle_start=%llu iop_cycle_end=%llu iop_pc_start=%08x iop_pc_end=%08x",
			static_cast<unsigned long long>(window),
			CorrelatedPerformanceOriginName(s_correlated_profile.origin_kind),
			static_cast<unsigned long long>(
				s_correlated_profile.producer_vsync_origin),
			static_cast<unsigned long long>(start.producer_vsyncs),
			static_cast<unsigned long long>(end.producer_vsyncs),
			static_cast<unsigned long long>(start.completed_vsyncs),
			static_cast<unsigned long long>(end.completed_vsyncs),
			static_cast<unsigned long long>(start.ee_cycle),
			static_cast<unsigned long long>(end.ee_cycle), start.ee_pc, end.ee_pc,
			static_cast<unsigned long long>(start.iop_cycle),
			static_cast<unsigned long long>(end.iop_cycle), start.iop_pc, end.iop_pc);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=threads wall_us=%llu ee_cpu_us=%llu "
			"ee_util=%.1f vu_cpu_us=%llu vu_util=%.1f gs_cpu_us=%llu gs_util=%.1f",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(wall_us),
			static_cast<unsigned long long>(ee_cpu_us), utilization(ee_cpu_us),
			static_cast<unsigned long long>(vu_cpu_us), utilization(vu_cpu_us),
			static_cast<unsigned long long>(gs_cpu_us), utilization(gs_cpu_us));
		if (start.cpu_stage_profiler.valid &&
			end.cpu_stage_profiler.valid)
		{
			const u64 scheduler_entries = CounterDelta(
				end.cpu_stage_profiler.scheduler_entries,
				start.cpu_stage_profiler.scheduler_entries);
			const u64 ee_full_scheduler_entries = CounterDelta(
				end.cpu_stage_profiler.ee_full_scheduler_entries,
				start.cpu_stage_profiler.ee_full_scheduler_entries);
			const u64 ee_iop_only_scheduler_entries = CounterDelta(
				end.cpu_stage_profiler.ee_iop_only_scheduler_entries,
				start.cpu_stage_profiler.ee_iop_only_scheduler_entries);
			const u64 iop_retained_wait_scheduler_entries = CounterDelta(
				end.cpu_stage_profiler.iop_retained_wait_scheduler_entries,
				start.cpu_stage_profiler.iop_retained_wait_scheduler_entries);
			const u64 stage_samples = CounterDelta(
				end.cpu_stage_profiler.stage_samples,
				start.cpu_stage_profiler.stage_samples);
			const u64 unbalanced_samples = CounterDelta(
				end.cpu_stage_profiler.unbalanced_samples,
				start.cpu_stage_profiler.unbalanced_samples);
			const u64 interval_records = CounterDelta(
				end.cpu_stage_profiler.interval_records,
				start.cpu_stage_profiler.interval_records);
			const u64 overwritten_interval_records = CounterDelta(
				end.cpu_stage_profiler.overwritten_interval_records,
				start.cpu_stage_profiler.overwritten_interval_records);
			const u64 iop_deadline_gate_checks = CounterDelta(
				end.cpu_stage_profiler.iop_deadline_gate_checks,
				start.cpu_stage_profiler.iop_deadline_gate_checks);
			const u64 iop_deadline_gate_skips = CounterDelta(
				end.cpu_stage_profiler.iop_deadline_gate_skips,
				start.cpu_stage_profiler.iop_deadline_gate_skips);
			const u64 iop_deadline_gate_dispatches = CounterDelta(
				end.cpu_stage_profiler.iop_deadline_gate_dispatches,
				start.cpu_stage_profiler.iop_deadline_gate_dispatches);
			const u64 iop_deadline_shadow_late = CounterDelta(
				end.cpu_stage_profiler.iop_deadline_shadow_late,
				start.cpu_stage_profiler.iop_deadline_shadow_late);
			const u64 iop_deadline_due = CounterDelta(
				end.cpu_stage_profiler.iop_deadline_due,
				start.cpu_stage_profiler.iop_deadline_due);
			const u64 iop_counter_due = CounterDelta(
				end.cpu_stage_profiler.iop_counter_due,
				start.cpu_stage_profiler.iop_counter_due);
			const u64 iop_intc_visible = CounterDelta(
				end.cpu_stage_profiler.iop_intc_visible,
				start.cpu_stage_profiler.iop_intc_visible);
			const u64 iop_callback_due = CounterDelta(
				end.cpu_stage_profiler.iop_callback_due,
				start.cpu_stage_profiler.iop_callback_due);
			const u64 iop_manufactured_only = CounterDelta(
				end.cpu_stage_profiler.iop_manufactured_only,
				start.cpu_stage_profiler.iop_manufactured_only);
			const u64 ee_deadline_shadow_entries = CounterDelta(
				end.cpu_stage_profiler.ee_deadline_shadow_entries,
				start.cpu_stage_profiler.ee_deadline_shadow_entries);
#define EE_DEADLINE_DELTA(name) \
			const u64 name = CounterDelta( \
				end.cpu_stage_profiler.name, start.cpu_stage_profiler.name)
			EE_DEADLINE_DELTA(ee_deadline_owner_iop);
			EE_DEADLINE_DELTA(ee_deadline_owner_counter);
			EE_DEADLINE_DELTA(ee_deadline_owner_event);
			EE_DEADLINE_DELTA(ee_deadline_owner_none);
#if defined(VITASX2_CPU_PROFILER)
			std::array<u64,
				VitaPerformanceTelemetry::EE_DEADLINE_EVENT_SLOT_COUNT>
				ee_deadline_event_owners{};
			for (size_t event = 0;
				event < ee_deadline_event_owners.size(); event++)
			{
				ee_deadline_event_owners[event] = CounterDelta(
					end.cpu_stage_profiler
						.ee_deadline_event_owners[event],
					start.cpu_stage_profiler
						.ee_deadline_event_owners[event]);
			}
#endif
			EE_DEADLINE_DELTA(ee_deadline_horizon_le_3072);
			EE_DEADLINE_DELTA(ee_deadline_horizon_gt_3072);
			EE_DEADLINE_DELTA(ee_deadline_horizon_gt_6144);
			EE_DEADLINE_DELTA(ee_deadline_horizon_gt_12288);
			EE_DEADLINE_DELTA(ee_deadline_timer_enabled);
			EE_DEADLINE_DELTA(ee_deadline_timer_within_3072);
			EE_DEADLINE_DELTA(ee_deadline_owner_beyond_3072_timer_off);
			EE_DEADLINE_DELTA(ee_deadline_iop_rapid);
			EE_DEADLINE_DELTA(ee_iop_balance_positive);
			EE_DEADLINE_DELTA(ee_iop_balance_nonpositive);
			EE_DEADLINE_DELTA(ee_iop_ahead_gt_3072);
			EE_DEADLINE_DELTA(ee_wait_shadow_entries);
			EE_DEADLINE_DELTA(ee_wait_generic_ram);
			EE_DEADLINE_DELTA(ee_wait_poll_call_ram);
			EE_DEADLINE_DELTA(ee_wait_two_predicate_ram);
			EE_DEADLINE_DELTA(ee_wait_retained_unconditional);
			EE_DEADLINE_DELTA(ee_wait_gs_csr_vsint);
			EE_DEADLINE_DELTA(ee_wait_dmac_chcr_str);
			EE_DEADLINE_DELTA(ee_wait_intc_vblank_start_and_ram);
			EE_DEADLINE_DELTA(joint_wait_shadow_entries);
			EE_DEADLINE_DELTA(joint_wait_unknown_writer);
			EE_DEADLINE_DELTA(joint_wait_blocked);
			EE_DEADLINE_DELTA(joint_wait_qualified);
			EE_DEADLINE_DELTA(joint_wait_horizon_gt_6144);
			EE_DEADLINE_DELTA(joint_wait_horizon_gt_12288);
			EE_DEADLINE_DELTA(joint_wait_horizon_gt_24576);
			EE_DEADLINE_DELTA(joint_wait_ram_certified);
			EE_DEADLINE_DELTA(joint_wait_ram_write_overlaps);
			EE_DEADLINE_DELTA(
				joint_wait_ram_write_overlaps_outside_scheduler);
			EE_DEADLINE_DELTA(joint_wait_activations);
			EE_DEADLINE_DELTA(joint_wait_scheduled_ee_cycles);
			EE_DEADLINE_DELTA(silent_hsync_fold_attempts);
			EE_DEADLINE_DELTA(silent_hsync_fold_activations);
			EE_DEADLINE_DELTA(silent_hsync_folded_edges);
			EE_DEADLINE_DELTA(silent_hsync_fold_blocked_control);
			EE_DEADLINE_DELTA(silent_hsync_fold_blocked_horizon_due);
			EE_DEADLINE_DELTA(silent_hsync_fold_blocked_ee_counter);
			EE_DEADLINE_DELTA(silent_hsync_fold_blocked_iop_counter);
			EE_DEADLINE_DELTA(silent_hsync_fold_blocked_hsync_due);
			EE_DEADLINE_DELTA(silent_hsync_fold_blocked_limit);
			EE_DEADLINE_DELTA(silent_hsync_fold_blocked_hsint);
#if defined(VITASX2_CPU_PROFILER)
			EE_DEADLINE_DELTA(ipu_epoch_from_ipu_wait_entries);
			EE_DEADLINE_DELTA(ipu_epoch_candidate_entries);
			EE_DEADLINE_DELTA(ipu_epoch_candidate_chains);
			EE_DEADLINE_DELTA(ipu_epoch_candidate_continuations);
			EE_DEADLINE_DELTA(ipu_epoch_due_from_ipu);
			EE_DEADLINE_DELTA(ipu_epoch_due_to_ipu);
			EE_DEADLINE_DELTA(ipu_epoch_due_process);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_iop_active);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_no_due_ipu);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_due_non_ipu);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_ee_counter);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_cp0_timer);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_visible_exception);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_vu);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_dmac_suspended);
			EE_DEADLINE_DELTA(ipu_epoch_blocked_instant_dma);
			EE_DEADLINE_DELTA(ipu_epoch_chain_length_1);
			EE_DEADLINE_DELTA(ipu_epoch_chain_length_2_3);
			EE_DEADLINE_DELTA(ipu_epoch_chain_length_4_7);
			EE_DEADLINE_DELTA(ipu_epoch_chain_length_8_15);
			EE_DEADLINE_DELTA(ipu_epoch_chain_length_16_31);
			EE_DEADLINE_DELTA(ipu_epoch_chain_length_32_63);
			EE_DEADLINE_DELTA(ipu_epoch_chain_length_64_plus);
#endif
			EE_DEADLINE_DELTA(spu2_time_update_calls);
			EE_DEADLINE_DELTA(spu2_time_update_samples);
			EE_DEADLINE_DELTA(spu2_time_update_zero_samples);
			EE_DEADLINE_DELTA(spu2_time_update_one_sample);
			EE_DEADLINE_DELTA(spu2_time_update_2_to_15_samples);
			EE_DEADLINE_DELTA(spu2_time_update_16_to_63_samples);
			EE_DEADLINE_DELTA(spu2_time_update_64_plus_samples);
			EE_DEADLINE_DELTA(spu2_sync_periodic);
			EE_DEADLINE_DELTA(spu2_sync_register_reads);
			EE_DEADLINE_DELTA(spu2_sync_register_writes);
			EE_DEADLINE_DELTA(spu2_sync_dma);
			EE_DEADLINE_DELTA(spu2_sync_observers);
			EE_DEADLINE_DELTA(spu2_mixer_probes);
			EE_DEADLINE_DELTA(spu2_mixer_active_voices);
			EE_DEADLINE_DELTA(spu2_mixer_stopped_voices);
			EE_DEADLINE_DELTA(spu2_mixer_sliding_voices);
			EE_DEADLINE_DELTA(spu2_mixer_noise_voices);
			EE_DEADLINE_DELTA(spu2_mixer_modulated_voices);
			EE_DEADLINE_DELTA(spu2_mixer_fx_enabled_cores);
			EE_DEADLINE_DELTA(spu2_mixer_irq_enabled_cores);
			EE_DEADLINE_DELTA(spu2_mixer_reverb_range_cores);
			EE_DEADLINE_DELTA(spu2_mixer_auto_dma_cores);
			EE_DEADLINE_DELTA(spu2_mixer_equivalent_stopped_cores);
#if defined(VITASX2_CPU_PROFILER)
			EE_DEADLINE_DELTA(spu2_mixer_silent_reverb_samples);
			EE_DEADLINE_DELTA(
				spu2_mixer_silent_reverb_input_rejects);
			EE_DEADLINE_DELTA(
				spu2_mixer_silent_reverb_irq_rejects);
			EE_DEADLINE_DELTA(
				spu2_mixer_silent_reverb_range_rejects);
			EE_DEADLINE_DELTA(
				spu2_mixer_silent_reverb_state_rejects);
			EE_DEADLINE_DELTA(spu2_stopped_voice_batch_calls);
			EE_DEADLINE_DELTA(spu2_stopped_voice_batch_samples);
			EE_DEADLINE_DELTA(
				spu2_stopped_voice_bulk_voice_samples);
			EE_DEADLINE_DELTA(
				spu2_zero_input_reverb_batch_calls);
			EE_DEADLINE_DELTA(
				spu2_zero_input_reverb_batch_core_samples);
#endif
#undef EE_DEADLINE_DELTA
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_summary "
				"sample_period=%u scheduler_entries=%llu samples=%llu "
				"unbalanced=%llu interval_records=%llu interval_overwrites=%llu",
				static_cast<unsigned long long>(window),
				VitaPerformanceTelemetry::CPU_STAGE_SAMPLE_PERIOD,
				static_cast<unsigned long long>(scheduler_entries),
				static_cast<unsigned long long>(stage_samples),
				static_cast<unsigned long long>(unbalanced_samples),
				static_cast<unsigned long long>(interval_records),
				static_cast<unsigned long long>(overwritten_interval_records));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=ee_scheduler_split "
				"full=%llu iop_only=%llu iop_retained_wait=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(ee_full_scheduler_entries),
				static_cast<unsigned long long>(
					ee_iop_only_scheduler_entries),
				static_cast<unsigned long long>(
					iop_retained_wait_scheduler_entries));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_deadline_shadow "
				"iop_checks=%llu iop_skips=%llu iop_dispatches=%llu "
				"iop_late=%llu deadline_due=%llu counter_due=%llu "
				"intc_visible=%llu callback_due=%llu "
				"manufactured_only=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(iop_deadline_gate_checks),
				static_cast<unsigned long long>(iop_deadline_gate_skips),
				static_cast<unsigned long long>(iop_deadline_gate_dispatches),
				static_cast<unsigned long long>(iop_deadline_shadow_late),
				static_cast<unsigned long long>(iop_deadline_due),
				static_cast<unsigned long long>(iop_counter_due),
				static_cast<unsigned long long>(iop_intc_visible),
				static_cast<unsigned long long>(iop_callback_due),
				static_cast<unsigned long long>(iop_manufactured_only));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=iop_counter_split "
				"full=%llu spu2_only=%llu unconstrained=%llu "
				"irq_limited=%llu dma_limited=%llu "
				"auto_dma_active=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.iop_counter_full_updates,
					start.cpu_stage_profiler.iop_counter_full_updates)),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.iop_counter_spu2_only_updates,
					start.cpu_stage_profiler.iop_counter_spu2_only_updates)),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.iop_spu2_unconstrained_updates,
					start.cpu_stage_profiler.iop_spu2_unconstrained_updates)),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.iop_spu2_irq_limited_updates,
					start.cpu_stage_profiler.iop_spu2_irq_limited_updates)),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.iop_spu2_dma_limited_updates,
					start.cpu_stage_profiler.iop_spu2_dma_limited_updates)),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.iop_spu2_auto_dma_active_updates,
					start.cpu_stage_profiler.iop_spu2_auto_dma_active_updates)));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=ee_deadline_shadow "
				"entries=%llu owner_iop=%llu owner_counter=%llu "
				"owner_event=%llu owner_none=%llu horizon_le_3072=%llu "
				"horizon_gt_3072=%llu horizon_gt_6144=%llu "
				"horizon_gt_12288=%llu timer_enabled=%llu "
				"timer_within_3072=%llu owner_beyond_3072_timer_off=%llu "
				"iop_rapid=%llu balance_positive=%llu "
				"balance_nonpositive=%llu iop_ahead_gt_3072=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(ee_deadline_shadow_entries),
				static_cast<unsigned long long>(ee_deadline_owner_iop),
				static_cast<unsigned long long>(ee_deadline_owner_counter),
				static_cast<unsigned long long>(ee_deadline_owner_event),
				static_cast<unsigned long long>(ee_deadline_owner_none),
				static_cast<unsigned long long>(ee_deadline_horizon_le_3072),
				static_cast<unsigned long long>(ee_deadline_horizon_gt_3072),
				static_cast<unsigned long long>(ee_deadline_horizon_gt_6144),
				static_cast<unsigned long long>(ee_deadline_horizon_gt_12288),
				static_cast<unsigned long long>(ee_deadline_timer_enabled),
				static_cast<unsigned long long>(ee_deadline_timer_within_3072),
				static_cast<unsigned long long>(
					ee_deadline_owner_beyond_3072_timer_off),
				static_cast<unsigned long long>(ee_deadline_iop_rapid),
				static_cast<unsigned long long>(ee_iop_balance_positive),
				static_cast<unsigned long long>(ee_iop_balance_nonpositive),
				static_cast<unsigned long long>(ee_iop_ahead_gt_3072));
#if defined(VITASX2_CPU_PROFILER)
			output.WriteLn(
				"Vita perf v=1 window=%llu "
				"kind=ee_deadline_event_owners "
				"vif0=%llu vif1=%llu gif=%llu from_ipu=%llu "
				"to_ipu=%llu sif0=%llu sif1=%llu from_spr=%llu "
				"to_spr=%llu mfifo_vif=%llu mfifo_gif=%llu "
				"vu0_finish=%llu vu1_finish=%llu ipu_process=%llu "
				"mtvu_busy=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_VIF0]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_VIF1]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_GIF]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_FROM_IPU]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_TO_IPU]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_SIF0]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_SIF1]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_FROM_SPR]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_TO_SPR]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_MFIFO_VIF]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[DMAC_MFIFO_GIF]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[VIF_VU0_FINISH]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[VIF_VU1_FINISH]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[IPU_PROCESS]),
				static_cast<unsigned long long>(
					ee_deadline_event_owners[VU_MTVU_BUSY]));
#endif
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=joint_wait_shadow "
				"ee_wait=%llu generic_ram=%llu poll_call_ram=%llu "
				"two_predicate_ram=%llu unconditional=%llu gs_csr=%llu "
				"dmac_chcr_str=%llu intc_vblank_start_ram=%llu "
				"joint=%llu unknown_writer=%llu blocked=%llu qualified=%llu "
				"horizon_gt_6144=%llu horizon_gt_12288=%llu "
				"horizon_gt_24576=%llu ram_certified=%llu "
				"ram_write_overlaps=%llu ram_write_outside=%llu "
				"activations=%llu scheduled_ee_cycles=%llu "
				"hsync_fold_attempts=%llu hsync_fold_activations=%llu "
				"hsync_folded_edges=%llu "
				"hsync_block_control=%llu hsync_block_horizon=%llu "
				"hsync_block_ee_counter=%llu hsync_block_iop_counter=%llu "
				"hsync_block_due=%llu hsync_block_limit=%llu "
				"hsync_block_hsint=%llu hsync_stop_detail=0x%08x "
				"last_ram_offset=0x%08x last_ram_size=%u",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(ee_wait_shadow_entries),
				static_cast<unsigned long long>(ee_wait_generic_ram),
				static_cast<unsigned long long>(ee_wait_poll_call_ram),
				static_cast<unsigned long long>(
					ee_wait_two_predicate_ram),
				static_cast<unsigned long long>(
					ee_wait_retained_unconditional),
				static_cast<unsigned long long>(ee_wait_gs_csr_vsint),
				static_cast<unsigned long long>(
					ee_wait_dmac_chcr_str),
				static_cast<unsigned long long>(
					ee_wait_intc_vblank_start_and_ram),
				static_cast<unsigned long long>(joint_wait_shadow_entries),
				static_cast<unsigned long long>(joint_wait_unknown_writer),
				static_cast<unsigned long long>(joint_wait_blocked),
				static_cast<unsigned long long>(joint_wait_qualified),
				static_cast<unsigned long long>(
					joint_wait_horizon_gt_6144),
				static_cast<unsigned long long>(
					joint_wait_horizon_gt_12288),
				static_cast<unsigned long long>(
					joint_wait_horizon_gt_24576),
				static_cast<unsigned long long>(
					joint_wait_ram_certified),
				static_cast<unsigned long long>(
					joint_wait_ram_write_overlaps),
				static_cast<unsigned long long>(
					joint_wait_ram_write_overlaps_outside_scheduler),
				static_cast<unsigned long long>(joint_wait_activations),
				static_cast<unsigned long long>(
					joint_wait_scheduled_ee_cycles),
				static_cast<unsigned long long>(
					silent_hsync_fold_attempts),
				static_cast<unsigned long long>(
					silent_hsync_fold_activations),
				static_cast<unsigned long long>(
					silent_hsync_folded_edges),
				static_cast<unsigned long long>(
					silent_hsync_fold_blocked_control),
				static_cast<unsigned long long>(
					silent_hsync_fold_blocked_horizon_due),
				static_cast<unsigned long long>(
					silent_hsync_fold_blocked_ee_counter),
				static_cast<unsigned long long>(
					silent_hsync_fold_blocked_iop_counter),
				static_cast<unsigned long long>(
					silent_hsync_fold_blocked_hsync_due),
				static_cast<unsigned long long>(
					silent_hsync_fold_blocked_limit),
				static_cast<unsigned long long>(
					silent_hsync_fold_blocked_hsint),
				end.cpu_stage_profiler
					.silent_hsync_fold_last_stop_detail,
				end.cpu_stage_profiler.joint_wait_last_ram_offset,
				end.cpu_stage_profiler.joint_wait_last_ram_size);
#if defined(VITASX2_CPU_PROFILER)
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=ipu_epoch_opportunity "
				"from_ipu_wait=%llu candidates=%llu chains=%llu "
				"continuations=%llu due_from=%llu due_to=%llu "
				"due_process=%llu blocked_iop_active=%llu "
				"blocked_no_due_ipu=%llu blocked_due_non_ipu=%llu "
				"blocked_ee_counter=%llu blocked_cp0_timer=%llu "
				"blocked_visible_exception=%llu blocked_vu=%llu "
				"blocked_dmac_suspended=%llu blocked_instant_dma=%llu "
				"chains_1=%llu chains_2_3=%llu chains_4_7=%llu "
				"chains_8_15=%llu chains_16_31=%llu chains_32_63=%llu "
				"chains_64_plus=%llu longest=%u",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(
					ipu_epoch_from_ipu_wait_entries),
				static_cast<unsigned long long>(
					ipu_epoch_candidate_entries),
				static_cast<unsigned long long>(
					ipu_epoch_candidate_chains),
				static_cast<unsigned long long>(
					ipu_epoch_candidate_continuations),
				static_cast<unsigned long long>(ipu_epoch_due_from_ipu),
				static_cast<unsigned long long>(ipu_epoch_due_to_ipu),
				static_cast<unsigned long long>(ipu_epoch_due_process),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_iop_active),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_no_due_ipu),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_due_non_ipu),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_ee_counter),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_cp0_timer),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_visible_exception),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_vu),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_dmac_suspended),
				static_cast<unsigned long long>(
					ipu_epoch_blocked_instant_dma),
				static_cast<unsigned long long>(
					ipu_epoch_chain_length_1),
				static_cast<unsigned long long>(
					ipu_epoch_chain_length_2_3),
				static_cast<unsigned long long>(
					ipu_epoch_chain_length_4_7),
				static_cast<unsigned long long>(
					ipu_epoch_chain_length_8_15),
				static_cast<unsigned long long>(
					ipu_epoch_chain_length_16_31),
				static_cast<unsigned long long>(
					ipu_epoch_chain_length_32_63),
				static_cast<unsigned long long>(
					ipu_epoch_chain_length_64_plus),
				end.cpu_stage_profiler.ipu_epoch_longest_chain);
#endif
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=spu2_sync "
				"time_updates=%llu samples=%llu zero=%llu one=%llu "
				"samples_2_15=%llu samples_16_63=%llu samples_64_plus=%llu "
				"periodic=%llu reads=%llu writes=%llu dma=%llu observers=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(spu2_time_update_calls),
				static_cast<unsigned long long>(spu2_time_update_samples),
				static_cast<unsigned long long>(
					spu2_time_update_zero_samples),
				static_cast<unsigned long long>(
					spu2_time_update_one_sample),
				static_cast<unsigned long long>(
					spu2_time_update_2_to_15_samples),
				static_cast<unsigned long long>(
					spu2_time_update_16_to_63_samples),
				static_cast<unsigned long long>(
					spu2_time_update_64_plus_samples),
				static_cast<unsigned long long>(spu2_sync_periodic),
				static_cast<unsigned long long>(
					spu2_sync_register_reads),
				static_cast<unsigned long long>(
					spu2_sync_register_writes),
				static_cast<unsigned long long>(spu2_sync_dma),
				static_cast<unsigned long long>(spu2_sync_observers));
#if defined(VITASX2_CPU_PROFILER)
#define SPU2_SILENT_REVERB_FORMAT \
				" silent_reverb_samples=%llu" \
				" silent_reverb_input_rejects=%llu" \
				" silent_reverb_irq_rejects=%llu" \
				" silent_reverb_range_rejects=%llu" \
				" silent_reverb_state_rejects=%llu" \
				" stopped_voice_batch_calls=%llu" \
				" stopped_voice_batch_samples=%llu" \
				" stopped_voice_bulk_voice_samples=%llu" \
				" zero_input_reverb_batch_calls=%llu" \
				" zero_input_reverb_batch_core_samples=%llu"
#define SPU2_SILENT_REVERB_ARGUMENTS \
				, static_cast<unsigned long long>( \
					spu2_mixer_silent_reverb_samples) \
				, static_cast<unsigned long long>( \
					spu2_mixer_silent_reverb_input_rejects) \
				, static_cast<unsigned long long>( \
					spu2_mixer_silent_reverb_irq_rejects) \
				, static_cast<unsigned long long>( \
					spu2_mixer_silent_reverb_range_rejects) \
				, static_cast<unsigned long long>( \
					spu2_mixer_silent_reverb_state_rejects) \
				, static_cast<unsigned long long>( \
					spu2_stopped_voice_batch_calls) \
				, static_cast<unsigned long long>( \
					spu2_stopped_voice_batch_samples) \
				, static_cast<unsigned long long>( \
					spu2_stopped_voice_bulk_voice_samples) \
				, static_cast<unsigned long long>( \
					spu2_zero_input_reverb_batch_calls) \
				, static_cast<unsigned long long>( \
					spu2_zero_input_reverb_batch_core_samples)
#else
#define SPU2_SILENT_REVERB_FORMAT
#define SPU2_SILENT_REVERB_ARGUMENTS
#endif
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=spu2_mixer "
				"probes=%llu active_voices=%llu stopped_voices=%llu "
				"sliding_voices=%llu noise_voices=%llu "
				"modulated_voices=%llu fx_enabled_cores=%llu "
				"irq_enabled_cores=%llu reverb_range_cores=%llu "
				"auto_dma_cores=%llu equivalent_stopped_cores=%llu"
				SPU2_SILENT_REVERB_FORMAT,
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(spu2_mixer_probes),
				static_cast<unsigned long long>(
					spu2_mixer_active_voices),
				static_cast<unsigned long long>(
					spu2_mixer_stopped_voices),
				static_cast<unsigned long long>(
					spu2_mixer_sliding_voices),
				static_cast<unsigned long long>(
					spu2_mixer_noise_voices),
				static_cast<unsigned long long>(
					spu2_mixer_modulated_voices),
				static_cast<unsigned long long>(
					spu2_mixer_fx_enabled_cores),
				static_cast<unsigned long long>(
					spu2_mixer_irq_enabled_cores),
				static_cast<unsigned long long>(
					spu2_mixer_reverb_range_cores),
				static_cast<unsigned long long>(
					spu2_mixer_auto_dma_cores),
				static_cast<unsigned long long>(
					spu2_mixer_equivalent_stopped_cores)
				SPU2_SILENT_REVERB_ARGUMENTS);
#undef SPU2_SILENT_REVERB_ARGUMENTS
#undef SPU2_SILENT_REVERB_FORMAT
			std::array<u64,
				VitaPerformanceTelemetry::CPU_STAGE_COUNT> stage_time_us{};
			std::array<u64,
				VitaPerformanceTelemetry::CPU_STAGE_COUNT> stage_entries{};
			u64 sampled_time_us = 0;
			for (size_t i = 0; i < stage_time_us.size(); i++)
			{
				stage_time_us[i] = CounterDelta(
					end.cpu_stage_profiler.stage_time_us[i],
					start.cpu_stage_profiler.stage_time_us[i]);
				stage_entries[i] = CounterDelta(
					end.cpu_stage_profiler.stage_entries[i],
					start.cpu_stage_profiler.stage_entries[i]);
				sampled_time_us += stage_time_us[i];
			}
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage samples=%llu "
				"unbalanced=%llu sampled_time_us=%llu "
				"scheduler=%llu ee_exceptions=%llu iop_guest=%llu "
				"iop_event=%llu iop_counters=%llu iop_interrupts=%llu "
				"spu2=%llu dev9=%llu usb=%llu ee_counters=%llu "
				"ee_interrupts=%llu vu_sync=%llu deadline=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(stage_samples),
				static_cast<unsigned long long>(unbalanced_samples),
				static_cast<unsigned long long>(sampled_time_us),
				static_cast<unsigned long long>(stage_time_us[0]),
				static_cast<unsigned long long>(stage_time_us[1]),
				static_cast<unsigned long long>(stage_time_us[2]),
				static_cast<unsigned long long>(stage_time_us[3]),
				static_cast<unsigned long long>(stage_time_us[4]),
				static_cast<unsigned long long>(stage_time_us[5]),
				static_cast<unsigned long long>(stage_time_us[6]),
				static_cast<unsigned long long>(stage_time_us[7]),
				static_cast<unsigned long long>(stage_time_us[8]),
				static_cast<unsigned long long>(stage_time_us[9]),
				static_cast<unsigned long long>(stage_time_us[10]),
				static_cast<unsigned long long>(stage_time_us[11]),
				static_cast<unsigned long long>(stage_time_us[12]));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_entries "
				"scheduler=%llu ee_exceptions=%llu iop_guest=%llu "
				"iop_event=%llu iop_counters=%llu iop_interrupts=%llu "
				"spu2=%llu dev9=%llu usb=%llu ee_counters=%llu "
				"ee_interrupts=%llu vu_sync=%llu deadline=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(stage_entries[0]),
				static_cast<unsigned long long>(stage_entries[1]),
				static_cast<unsigned long long>(stage_entries[2]),
				static_cast<unsigned long long>(stage_entries[3]),
				static_cast<unsigned long long>(stage_entries[4]),
				static_cast<unsigned long long>(stage_entries[5]),
				static_cast<unsigned long long>(stage_entries[6]),
				static_cast<unsigned long long>(stage_entries[7]),
				static_cast<unsigned long long>(stage_entries[8]),
				static_cast<unsigned long long>(stage_entries[9]),
				static_cast<unsigned long long>(stage_entries[10]),
				static_cast<unsigned long long>(stage_entries[11]),
				static_cast<unsigned long long>(stage_entries[12]));
			const auto stage_time = [&stage_time_us](
				VitaPerformanceTelemetry::CpuStage stage) {
				return stage_time_us[static_cast<size_t>(stage)];
			};
			const auto stage_entry = [&stage_entries](
				VitaPerformanceTelemetry::CpuStage stage) {
				return stage_entries[static_cast<size_t>(stage)];
			};
			u64 vif_gif_time = stage_time(
				VitaPerformanceTelemetry::CpuStage::VifGif);
			u64 vif_gif_entries = stage_entry(
				VitaPerformanceTelemetry::CpuStage::VifGif);
#if defined(VITASX2_CPU_PROFILER)
			// VifDma remains the inclusive sparse exact owner. The detail stages
			// below are statistical-only so leaf process-time reads cannot perturb
			// VIF traffic and MTVU cannot publish CPU1 work as CPU0 time.
			const u64 vif_dma_time = stage_time(
				VitaPerformanceTelemetry::CpuStage::VifDma);
			const u64 vif_dma_entries = stage_entry(
				VitaPerformanceTelemetry::CpuStage::VifDma);
			vif_gif_time += vif_dma_time +
				stage_time(VitaPerformanceTelemetry::CpuStage::GifDma) +
				stage_time(VitaPerformanceTelemetry::CpuStage::VifVuFinish);
			vif_gif_entries += vif_dma_entries +
				stage_entry(VitaPerformanceTelemetry::CpuStage::GifDma) +
				stage_entry(VitaPerformanceTelemetry::CpuStage::VifVuFinish);
#endif
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_extended "
				"ee_generated=%llu ee_wait_resume=%llu "
				"ee_provider=%llu ee_compile=%llu "
				"ee_interpreter=%llu iop_interpreter=%llu "
				"cop1=%llu cop2_vu0=%llu ee_helper=%llu ee_memory_slow=%llu "
				"iop_helper=%llu iop_memory_slow=%llu ipu=%llu vif_gif=%llu "
				"sif=%llu cdvd=%llu dma=%llu other_device=%llu diagnostics=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::EeGenerated)),
#if defined(VITASX2_CPU_PROFILER)
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::EeWaitResume)),
#else
				0ull,
#endif
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::EeProvider)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::EeCompile)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::EeInterpreter)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopInterpreter)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Cop1)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Cop2Vu0)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::EeHelper)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::EeMemorySlowPath)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopHelper)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopMemorySlowPath)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Ipu)),
				static_cast<unsigned long long>(vif_gif_time),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Sif)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Cdvd)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Dma)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::OtherDevice)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Diagnostics)));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_extended_entries "
				"ee_generated=%llu ee_wait_resume=%llu "
				"ee_provider=%llu ee_compile=%llu "
				"ee_interpreter=%llu iop_interpreter=%llu "
				"cop1=%llu cop2_vu0=%llu ee_helper=%llu ee_memory_slow=%llu "
				"iop_helper=%llu iop_memory_slow=%llu ipu=%llu vif_gif=%llu "
				"sif=%llu cdvd=%llu dma=%llu other_device=%llu diagnostics=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::EeGenerated)),
#if defined(VITASX2_CPU_PROFILER)
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::EeWaitResume)),
#else
				0ull,
#endif
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::EeProvider)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::EeCompile)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::EeInterpreter)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::IopInterpreter)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::Cop1)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::Cop2Vu0)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::EeHelper)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::EeMemorySlowPath)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::IopHelper)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::IopMemorySlowPath)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::Ipu)),
				static_cast<unsigned long long>(vif_gif_entries),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::Sif)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::Cdvd)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::Dma)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::OtherDevice)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::Diagnostics)));
#if defined(VITASX2_CPU_PROFILER)
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_vif_gif "
				"vif_dma_us=%llu vif_dma_entries=%llu "
				"gif_dma_us=%llu gif_dma_entries=%llu "
				"vif_vu_finish_us=%llu vif_vu_finish_entries=%llu "
				"residual_us=%llu residual_entries=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(vif_dma_time),
				static_cast<unsigned long long>(vif_dma_entries),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::GifDma)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::GifDma)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::VifVuFinish)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::VifVuFinish)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::VifGif)),
				static_cast<unsigned long long>(stage_entry(
					VitaPerformanceTelemetry::CpuStage::VifGif)));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_iop "
				"generated=%llu provider=%llu compile=%llu "
				"interpreter=%llu helper=%llu memory_slow=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopGenerated)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopProvider)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopCompile)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopInterpreter)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopHelper)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IopMemorySlowPath)));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_ipu "
				"decode=%llu idct=%llu csc=%llu dma=%llu residual=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IpuDecode)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IpuIdct)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IpuCsc)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::IpuDma)),
				static_cast<unsigned long long>(stage_time(
					VitaPerformanceTelemetry::CpuStage::Ipu)));
			std::array<u64,
				VitaPerformanceTelemetry::CPU_STAGE_COUNT>
				statistical_stage_samples{};
			for (size_t i = 0;
				i < statistical_stage_samples.size(); i++)
			{
				statistical_stage_samples[i] = CounterDelta(
					end.cpu_stage_profiler.statistical_stage_samples[i],
					start.cpu_stage_profiler.statistical_stage_samples[i]);
			}
			const auto statistical_stage =
				[&statistical_stage_samples](
					VitaPerformanceTelemetry::CpuStage stage) {
					return statistical_stage_samples[
						static_cast<size_t>(stage)];
				};
			const u64 statistical_vif_unpack = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::VifUnpackHandoff) +
				statistical_stage(VitaPerformanceTelemetry::CpuStage::VifUnpackCopy) +
				statistical_stage(VitaPerformanceTelemetry::CpuStage::VifUnpackWiden) +
				statistical_stage(VitaPerformanceTelemetry::CpuStage::VifUnpackModeMask) +
				statistical_stage(VitaPerformanceTelemetry::CpuStage::VifUnpackCycle) +
				statistical_stage(VitaPerformanceTelemetry::CpuStage::VifUnpackGeneric);
			u64 statistical_vif_epoch_parse = 0;
			u64 statistical_vif_epoch_state = 0;
			u64 statistical_vif_epoch_state_payload = 0;
			u64 statistical_vif_epoch_unpack = 0;
			u64 statistical_vif_epoch_mpg = 0;
			u64 statistical_vif_epoch_direct = 0;
			u64 statistical_vif_epoch_synchronize = 0;
			u64 statistical_vif_epoch_error = 0;
			u64 statistical_vif_epoch_command_bookkeeping = 0;
			u64 statistical_vif_epoch_dma_bookkeeping = 0;
			u64 statistical_vif_epoch_stall = 0;
			u64 statistical_vif_epoch_queue_probe = 0;
			u64 statistical_vif_epoch_queue_observer = 0;
			u64 statistical_vif_epoch_queue_execution = 0;
			u64 statistical_vif_epoch_accounting = 0;
			u64 statistical_vif_epoch_samples = 0;
			u64 statistical_vif_epoch_unclassified = 0;
#if defined(VITASX2_VIF_EPOCH_CENSUS)
			const auto vif_epoch_stage = [&start, &end](
				VitaVifEpochCensus::StatisticalStage stage) {
				const size_t index = static_cast<size_t>(stage);
				return CounterDelta(
					end.vif_epoch.statistical_stage_samples[index],
					start.vif_epoch.statistical_stage_samples[index]);
			};
			statistical_vif_epoch_parse = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Parse);
			statistical_vif_epoch_state = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::State);
			statistical_vif_epoch_state_payload = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::StatePayload);
			statistical_vif_epoch_unpack = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Unpack);
			statistical_vif_epoch_mpg = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Mpg);
			statistical_vif_epoch_direct = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Direct);
			statistical_vif_epoch_synchronize = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Synchronize);
			statistical_vif_epoch_error = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Error);
			statistical_vif_epoch_command_bookkeeping = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::CommandBookkeeping);
			statistical_vif_epoch_dma_bookkeeping = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::DmaBookkeeping);
			statistical_vif_epoch_stall = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Stall);
			statistical_vif_epoch_queue_probe = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::QueueProbe);
			statistical_vif_epoch_queue_observer = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::QueueObserver);
			statistical_vif_epoch_queue_execution = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::QueueExecution);
			statistical_vif_epoch_accounting = vif_epoch_stage(
				VitaVifEpochCensus::StatisticalStage::Accounting);
			statistical_vif_epoch_samples = CounterDelta(
				end.vif_epoch.statistical_samples,
				start.vif_epoch.statistical_samples);
			statistical_vif_epoch_unclassified = CounterDelta(
				end.vif_epoch.statistical_unclassified_samples,
				start.vif_epoch.statistical_unclassified_samples);
#endif
			const u64 statistical_vif_dma = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::VifDma) +
				statistical_stage(VitaPerformanceTelemetry::CpuStage::VifTransfer) +
				statistical_vif_unpack;
			const u64 statistical_vif_gif = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::VifGif) +
				statistical_vif_dma +
				statistical_stage(VitaPerformanceTelemetry::CpuStage::GifDma) +
				statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifVuFinish);
			const u64 statistical_spu2_residual = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::Spu2);
			const u64 statistical_spu2_input = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::Spu2Input);
			const u64 statistical_spu2_voices = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::Spu2Voices);
			const u64 statistical_spu2_core = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::Spu2Core);
			const u64 statistical_spu2_reverb = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::Spu2Reverb);
			const u64 statistical_spu2_output = statistical_stage(
				VitaPerformanceTelemetry::CpuStage::Spu2Output);
			const u64 statistical_spu2_total =
				statistical_spu2_residual + statistical_spu2_input +
				statistical_spu2_voices + statistical_spu2_core +
				statistical_spu2_reverb + statistical_spu2_output;
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_statistical "
				"samples=%llu invalid=%llu sampler_cpu_us=%llu "
				"scheduler=%llu ee_exceptions=%llu "
				"iop_guest=%llu iop_event=%llu iop_counters=%llu "
				"iop_interrupts=%llu spu2=%llu dev9=%llu usb=%llu "
				"ee_counters=%llu ee_interrupts=%llu vu_sync=%llu "
				"deadline=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.statistical_samples,
					start.cpu_stage_profiler.statistical_samples)),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.statistical_invalid_samples,
					start.cpu_stage_profiler.statistical_invalid_samples)),
				static_cast<unsigned long long>(CounterDelta(
					end.cpu_stage_profiler.statistical_sampler_cpu_us,
					start.cpu_stage_profiler.statistical_sampler_cpu_us)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Scheduler)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeExceptions)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopGuest)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopEvent)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopCounters)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopInterrupts)),
				static_cast<unsigned long long>(statistical_spu2_total),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Dev9)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Usb)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeCounters)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeInterrupts)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VuSync)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Deadline)));
			output.WriteLn(
				"Vita perf v=1 window=%llu "
				"kind=cpu_stage_statistical_spu2 "
				"input=%llu voices=%llu core=%llu reverb=%llu "
				"output=%llu residual=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(statistical_spu2_input),
				static_cast<unsigned long long>(statistical_spu2_voices),
				static_cast<unsigned long long>(statistical_spu2_core),
				static_cast<unsigned long long>(statistical_spu2_reverb),
				static_cast<unsigned long long>(statistical_spu2_output),
				static_cast<unsigned long long>(statistical_spu2_residual));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_statistical_extended "
				"ee_generated=%llu ee_wait_resume=%llu "
				"ee_provider=%llu ee_compile=%llu "
				"ee_interpreter=%llu iop_interpreter=%llu cop1=%llu "
				"cop2_vu0=%llu ee_helper=%llu ee_memory_slow=%llu "
				"iop_helper=%llu iop_memory_slow=%llu ipu=%llu "
				"vif_gif=%llu sif=%llu cdvd=%llu dma=%llu "
				"other_device=%llu diagnostics=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeGenerated)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeWaitResume)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeProvider)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeCompile)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeInterpreter)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopInterpreter)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Cop1)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Cop2Vu0)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeHelper)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::EeMemorySlowPath)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopHelper)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopMemorySlowPath)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Ipu)),
				static_cast<unsigned long long>(statistical_vif_gif),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Sif)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Cdvd)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Dma)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::OtherDevice)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Diagnostics)));
			output.WriteLn(
				"Vita perf v=1 window=%llu "
				"kind=cpu_stage_statistical_vif_gif "
				"vif_dma=%llu gif_dma=%llu vif_vu_finish=%llu residual=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(statistical_vif_dma),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::GifDma)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifVuFinish)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifGif)));
			output.WriteLn(
				"Vita perf v=1 window=%llu "
				"kind=cpu_stage_statistical_vif_dma_detail "
				"control=%llu transfer=%llu handoff=%llu copy=%llu widen=%llu "
				"mode_mask=%llu cycle=%llu generic=%llu "
				"epoch_parse=%llu epoch_state=%llu epoch_state_payload=%llu "
				"epoch_unpack=%llu epoch_mpg=%llu epoch_direct=%llu "
				"epoch_synchronize=%llu epoch_error=%llu "
				"epoch_command_bookkeeping=%llu "
				"epoch_dma_bookkeeping=%llu epoch_stall=%llu "
				"epoch_queue_probe=%llu epoch_queue_observer=%llu "
				"epoch_queue_execution=%llu epoch_accounting=%llu "
				"epoch_samples=%llu epoch_unclassified=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifDma)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifTransfer)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifUnpackHandoff)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifUnpackCopy)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifUnpackWiden)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifUnpackModeMask)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifUnpackCycle)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::VifUnpackGeneric)),
				static_cast<unsigned long long>(statistical_vif_epoch_parse),
				static_cast<unsigned long long>(statistical_vif_epoch_state),
				static_cast<unsigned long long>(statistical_vif_epoch_state_payload),
				static_cast<unsigned long long>(statistical_vif_epoch_unpack),
				static_cast<unsigned long long>(statistical_vif_epoch_mpg),
				static_cast<unsigned long long>(statistical_vif_epoch_direct),
				static_cast<unsigned long long>(statistical_vif_epoch_synchronize),
				static_cast<unsigned long long>(statistical_vif_epoch_error),
				static_cast<unsigned long long>(
					statistical_vif_epoch_command_bookkeeping),
				static_cast<unsigned long long>(
					statistical_vif_epoch_dma_bookkeeping),
				static_cast<unsigned long long>(statistical_vif_epoch_stall),
				static_cast<unsigned long long>(
					statistical_vif_epoch_queue_probe),
				static_cast<unsigned long long>(
					statistical_vif_epoch_queue_observer),
				static_cast<unsigned long long>(
					statistical_vif_epoch_queue_execution),
				static_cast<unsigned long long>(
					statistical_vif_epoch_accounting),
				static_cast<unsigned long long>(statistical_vif_epoch_samples),
				static_cast<unsigned long long>(
					statistical_vif_epoch_unclassified));
#if defined(VITASX2_VIF_EPOCH_CENSUS)
			if (start.vif_epoch.valid && end.vif_epoch.valid)
			{
				constexpr std::array<const char*,
					VitaVifEpochCensus::COMMAND_CLASS_COUNT> command_class_names = {{
					"state", "state_payload", "unpack", "mpg", "direct",
					"synchronize", "error",
				}};
				constexpr std::array<const char*,
					VitaVifEpochCensus::BOUNDARY_REASON_COUNT> boundary_names = {{
					"input_complete", "partial_command", "irq", "timing_stall",
					"command_observer", "queue_execution", "no_progress",
				}};
				for (u32 unit = 0; unit < 2; unit++)
				{
					const VitaVifEpochCensus::UnitStatistics& first =
						start.vif_epoch.units[unit];
					const VitaVifEpochCensus::UnitStatistics& last =
						end.vif_epoch.units[unit];
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=vif_epoch_unit unit=%u "
						"transfer_calls=%llu input_words=%llu consumed_words=%llu "
						"tte_calls=%llu idle_entries=%llu continuation_entries=%llu "
						"commands_started=%llu commands_completed=%llu "
						"body_epochs=%llu body_commands=%llu body_words=%llu "
						"empty_boundaries=%llu unbalanced_transfers=%llu",
						static_cast<unsigned long long>(window), unit,
						static_cast<unsigned long long>(CounterDelta(
							last.transfer_calls, first.transfer_calls)),
						static_cast<unsigned long long>(CounterDelta(
							last.transfer_input_words, first.transfer_input_words)),
						static_cast<unsigned long long>(CounterDelta(
							last.transfer_consumed_words, first.transfer_consumed_words)),
						static_cast<unsigned long long>(CounterDelta(
							last.tte_calls, first.tte_calls)),
						static_cast<unsigned long long>(CounterDelta(
							last.idle_entries, first.idle_entries)),
						static_cast<unsigned long long>(CounterDelta(
							last.continuation_entries, first.continuation_entries)),
						static_cast<unsigned long long>(CounterDelta(
							last.commands_started, first.commands_started)),
						static_cast<unsigned long long>(CounterDelta(
							last.commands_completed, first.commands_completed)),
						static_cast<unsigned long long>(CounterDelta(
							last.body_epochs, first.body_epochs)),
						static_cast<unsigned long long>(CounterDelta(
							last.body_commands, first.body_commands)),
						static_cast<unsigned long long>(CounterDelta(
							last.body_words, first.body_words)),
						static_cast<unsigned long long>(CounterDelta(
							last.empty_boundaries, first.empty_boundaries)),
						static_cast<unsigned long long>(CounterDelta(
							last.unbalanced_transfers, first.unbalanced_transfers)));

					for (size_t command_class = 0;
						command_class < command_class_names.size(); command_class++)
					{
						output.WriteLn(
							"Vita perf v=1 window=%llu kind=vif_epoch_command "
							"unit=%u class=%s starts=%llu steps=%llu "
							"completions=%llu words=%llu",
							static_cast<unsigned long long>(window), unit,
							command_class_names[command_class],
							static_cast<unsigned long long>(CounterDelta(
								last.command_starts[command_class],
								first.command_starts[command_class])),
							static_cast<unsigned long long>(CounterDelta(
								last.command_steps[command_class],
								first.command_steps[command_class])),
							static_cast<unsigned long long>(CounterDelta(
								last.command_completions[command_class],
								first.command_completions[command_class])),
							static_cast<unsigned long long>(CounterDelta(
								last.command_words[command_class],
								first.command_words[command_class])));
					}

					for (size_t reason = 0; reason < boundary_names.size(); reason++)
					{
						output.WriteLn(
							"Vita perf v=1 window=%llu kind=vif_epoch_boundary "
							"unit=%u reason=%s count=%llu",
							static_cast<unsigned long long>(window), unit,
							boundary_names[reason],
							static_cast<unsigned long long>(CounterDelta(
								last.boundaries[reason], first.boundaries[reason])));
					}

					output.WriteLn(
						"Vita perf v=1 window=%llu kind=vif_epoch_queue unit=%u "
						"empty=%llu blocked_vu=%llu blocked_gif=%llu executed=%llu",
						static_cast<unsigned long long>(window), unit,
						static_cast<unsigned long long>(CounterDelta(
							last.queue_results[0], first.queue_results[0])),
						static_cast<unsigned long long>(CounterDelta(
							last.queue_results[1], first.queue_results[1])),
						static_cast<unsigned long long>(CounterDelta(
							last.queue_results[2], first.queue_results[2])),
						static_cast<unsigned long long>(CounterDelta(
							last.queue_results[3], first.queue_results[3])));

					const auto write_histogram = [&output, window, unit](
						const char* metric,
						const std::array<u64,
							VitaVifEpochCensus::SIZE_BUCKET_COUNT>& first_counts,
						const std::array<u64,
							VitaVifEpochCensus::SIZE_BUCKET_COUNT>& last_counts,
						const std::array<u64,
							VitaVifEpochCensus::SIZE_BUCKET_COUNT>& first_weights,
						const std::array<u64,
							VitaVifEpochCensus::SIZE_BUCKET_COUNT>& last_weights) {
						std::array<u64, VitaVifEpochCensus::SIZE_BUCKET_COUNT> counts{};
						std::array<u64, VitaVifEpochCensus::SIZE_BUCKET_COUNT> weights{};
						for (size_t i = 0; i < counts.size(); i++)
						{
							counts[i] = CounterDelta(last_counts[i], first_counts[i]);
							weights[i] = CounterDelta(last_weights[i], first_weights[i]);
						}
						output.WriteLn(
							"Vita perf v=1 window=%llu kind=vif_epoch_hist unit=%u "
							"metric=%s buckets=0,1,2-3,4-15,16-63,64-255,256+ "
							"counts=%llu,%llu,%llu,%llu,%llu,%llu,%llu "
							"weights=%llu,%llu,%llu,%llu,%llu,%llu,%llu",
							static_cast<unsigned long long>(window), unit, metric,
							static_cast<unsigned long long>(counts[0]),
							static_cast<unsigned long long>(counts[1]),
							static_cast<unsigned long long>(counts[2]),
							static_cast<unsigned long long>(counts[3]),
							static_cast<unsigned long long>(counts[4]),
							static_cast<unsigned long long>(counts[5]),
							static_cast<unsigned long long>(counts[6]),
							static_cast<unsigned long long>(weights[0]),
							static_cast<unsigned long long>(weights[1]),
							static_cast<unsigned long long>(weights[2]),
							static_cast<unsigned long long>(weights[3]),
							static_cast<unsigned long long>(weights[4]),
							static_cast<unsigned long long>(weights[5]),
							static_cast<unsigned long long>(weights[6]));
					};
					write_histogram("transfer_words", first.transfer_size_calls,
						last.transfer_size_calls, first.transfer_size_words,
						last.transfer_size_words);
					write_histogram("epoch_commands", first.epoch_command_counts,
						last.epoch_command_counts, first.epoch_command_weight,
						last.epoch_command_weight);
					write_histogram("epoch_words", first.epoch_word_counts,
						last.epoch_word_counts, first.epoch_word_weight,
						last.epoch_word_weight);
					write_histogram("command_step_words", first.command_step_counts,
						last.command_step_counts, first.command_step_words,
						last.command_step_words);
				}
			}
#endif
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_statistical_iop "
				"generated=%llu provider=%llu compile=%llu "
				"interpreter=%llu helper=%llu memory_slow=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopGenerated)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopProvider)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopCompile)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopInterpreter)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopHelper)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IopMemorySlowPath)));
			output.WriteLn(
				"Vita perf v=1 window=%llu "
				"kind=cpu_stage_statistical_ipu "
				"decode=%llu idct=%llu csc=%llu dma=%llu residual=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IpuDecode)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IpuIdct)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IpuCsc)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::IpuDma)),
				static_cast<unsigned long long>(statistical_stage(
					VitaPerformanceTelemetry::CpuStage::Ipu)));
			const u64 ee_compile_observations = CounterDelta(
				end.cpu_stage_profiler.ee_compile_observations,
				start.cpu_stage_profiler.ee_compile_observations);
			const u64 ee_compile_time_us = CounterDelta(
				end.cpu_stage_profiler.ee_compile_time_us,
				start.cpu_stage_profiler.ee_compile_time_us);
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=ee_compile_exact "
				"observations=%llu time_us=%llu average_ns=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(ee_compile_observations),
				static_cast<unsigned long long>(ee_compile_time_us),
				static_cast<unsigned long long>(ee_compile_observations ?
					(ee_compile_time_us * 1000u) /
						ee_compile_observations : 0));
			const u64 iop_compile_observations = CounterDelta(
				end.cpu_stage_profiler.iop_compile_observations,
				start.cpu_stage_profiler.iop_compile_observations);
			const u64 iop_compile_time_us = CounterDelta(
				end.cpu_stage_profiler.iop_compile_time_us,
				start.cpu_stage_profiler.iop_compile_time_us);
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=iop_compile_exact "
				"observations=%llu time_us=%llu average_ns=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(iop_compile_observations),
				static_cast<unsigned long long>(iop_compile_time_us),
				static_cast<unsigned long long>(iop_compile_observations ?
					(iop_compile_time_us * 1000u) /
						iop_compile_observations : 0));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=iop_cache "
				"resets_delta=%llu resets=%u used=%llu capacity=%llu "
				"block_records=%u cache_slots=%u descriptors=%u",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(CounterDelta(
					end.iop.code_cache_resets,
					start.iop.code_cache_resets)),
				end.iop.code_cache_resets,
				static_cast<unsigned long long>(end.iop.code_cache_used),
				static_cast<unsigned long long>(end.iop.code_cache_capacity),
				end.iop.code_cache_block_records,
				end.iop.code_cache_slots,
				end.iop.semantic_block_descriptors);
			constexpr size_t hot_guest_pc_pages =
				VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_CANDIDATE_COUNT /
				VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_COUNT;
			for (size_t page = 0; page < hot_guest_pc_pages; page++)
			{
				const u32 first_rank = static_cast<u32>(page *
					VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_COUNT);
				const VitaPerformanceTelemetry::CpuProfileHotPcSnapshot
					hot_ee_pcs =
						VitaPerformanceTelemetry::GetCpuProfileHotEePcSnapshot(
							start.cpu_stage_profiler.statistical_ee_pc_sequence + 1,
							end.cpu_stage_profiler.statistical_ee_pc_sequence + 1,
							first_rank);
				if (page == 0)
				{
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=cpu_hot_ee_pc_summary "
						"first_sequence=%llu next_sequence=%llu dropped=%llu "
						"invalid=%llu candidates=%u page_size=%u",
						static_cast<unsigned long long>(window),
						static_cast<unsigned long long>(hot_ee_pcs.first_sequence),
						static_cast<unsigned long long>(hot_ee_pcs.next_sequence),
						static_cast<unsigned long long>(hot_ee_pcs.dropped_samples),
						static_cast<unsigned long long>(hot_ee_pcs.invalid_samples),
						static_cast<unsigned>(
							VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_CANDIDATE_COUNT),
						static_cast<unsigned>(
							VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_COUNT));
				}
				size_t emitted = 0;
				for (size_t i = 0; i < hot_ee_pcs.pcs.size(); i++)
				{
					const VitaPerformanceTelemetry::CpuProfileHotPc& hot_pc =
						hot_ee_pcs.pcs[i];
					if (hot_pc.samples == 0)
						break;
#if defined(VITASX2_CPU_PROFILER)
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=cpu_hot_ee_pc "
						"rank=%u pc=0x%08x samples=%u words=%u "
						"code=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
						static_cast<unsigned long long>(window),
						static_cast<unsigned>(first_rank + i + 1), hot_pc.pc,
						hot_pc.samples, hot_pc.code_words,
						hot_pc.code[0], hot_pc.code[1],
						hot_pc.code[2], hot_pc.code[3],
						hot_pc.code[4], hot_pc.code[5],
						hot_pc.code[6], hot_pc.code[7]);
#else
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=cpu_hot_ee_pc "
						"rank=%u pc=0x%08x samples=%u",
						static_cast<unsigned long long>(window),
						static_cast<unsigned>(first_rank + i + 1), hot_pc.pc,
						hot_pc.samples);
#endif
					emitted++;
				}
				if (emitted != hot_ee_pcs.pcs.size())
					break;
			}

			// Preserve every retained generated-EE sampler observation from this
			// exact producer-VSync window. Unlike the ranked hot-PC report, this raw
			// sequence has no candidate truncation. Each sample carries the compiler
			// publication generation observed by the sampler, so SMC/cache churn
			// fails closed instead of borrowing later metadata for the same PC.
			// Everything above is the established fixed-size report. Flush it before
			// the potentially wider graph census, then keep the new records in small
			// batches. WriteMultilineBatch() constructs a second sink buffer, so one
			// multi-megabyte batch can exhaust the Vita's remaining user heap even
			// though the report is outside the measured endpoint.
			output.FlushAndClear();
			constexpr size_t EE_TRACE_GRAPH_BLOCK_LIMIT = 4096;
			std::vector<u64> root_keys;
			VitaPerformanceTelemetry::CpuProfileEeSampleSnapshot ee_sample_page;
			for (u32 first_sample = 0;;
				first_sample +=
					VitaPerformanceTelemetry::CPU_PROFILE_EE_SAMPLE_PAGE_SIZE)
			{
				ee_sample_page =
					VitaPerformanceTelemetry::GetCpuProfileEeSampleSnapshot(
						start.cpu_stage_profiler.statistical_ee_pc_sequence + 1,
						end.cpu_stage_profiler.statistical_ee_pc_sequence + 1,
						first_sample);
				if (first_sample == 0)
				{
					root_keys.reserve(std::min<size_t>(
						static_cast<size_t>(ee_sample_page.valid_samples),
						EE_TRACE_GRAPH_BLOCK_LIMIT));
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=cpu_ee_sample_summary "
						"first_sequence=%llu next_sequence=%llu dropped=%llu "
						"invalid=%llu valid=%llu page_size=%u",
						static_cast<unsigned long long>(window),
						static_cast<unsigned long long>(
							ee_sample_page.first_sequence),
						static_cast<unsigned long long>(
							ee_sample_page.next_sequence),
						static_cast<unsigned long long>(
							ee_sample_page.dropped_samples),
						static_cast<unsigned long long>(
							ee_sample_page.invalid_samples),
						static_cast<unsigned long long>(
							ee_sample_page.valid_samples),
						static_cast<unsigned>(
							VitaPerformanceTelemetry::CPU_PROFILE_EE_SAMPLE_PAGE_SIZE));
				}
				for (u32 i = 0; i < ee_sample_page.sample_count; i++)
				{
					const VitaPerformanceTelemetry::CpuProfileEeSample& sample =
						ee_sample_page.samples[i];
					if (root_keys.size() < EE_TRACE_GRAPH_BLOCK_LIMIT)
					{
						root_keys.push_back(
							(static_cast<u64>(sample.block_generation) << 32) |
							sample.pc);
					}
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=cpu_ee_sample "
						"sequence=%llu pc=0x%08x generation=%u",
						static_cast<unsigned long long>(window),
						static_cast<unsigned long long>(sample.sequence),
						sample.pc, sample.block_generation);
				}
				output.FlushAndClear();
				if (ee_sample_page.sample_count <
					VitaPerformanceTelemetry::CPU_PROFILE_EE_SAMPLE_PAGE_SIZE)
				{
					break;
				}
			}

			std::sort(root_keys.begin(), root_keys.end());
			root_keys.erase(std::unique(root_keys.begin(), root_keys.end()),
				root_keys.end());
			const u32 root_generation_count =
				static_cast<u32>(root_keys.size());
			std::vector<u64> pending_keys = root_keys;
			const size_t pending_root_count = pending_keys.size();
			u32 missing_roots = 0;
			std::vector<u32> visited_pcs;
			visited_pcs.reserve(EE_TRACE_GRAPH_BLOCK_LIMIT);
			for (const u64 key : root_keys)
			{
				visited_pcs.push_back(static_cast<u32>(key));
			}
			std::sort(visited_pcs.begin(), visited_pcs.end());
			visited_pcs.erase(std::unique(visited_pcs.begin(), visited_pcs.end()),
				visited_pcs.end());
			u32 missing_successors = 0;
			bool graph_truncated = false;
			u32 emitted_blocks = 0;
			for (size_t block_index = 0;
				block_index < pending_keys.size(); block_index++)
			{
				const u64 key = pending_keys[block_index];
				const u32 pc = static_cast<u32>(key);
				const u32 generation = static_cast<u32>(key >> 32);
				const bool is_root = block_index < pending_root_count;
				const VitaPerformanceTelemetry::CpuProfileEeTraceBlock entry =
					generation == 0 ?
						VitaPerformanceTelemetry::CpuProfileEeTraceBlock{} :
						VitaPerformanceTelemetry::GetCpuProfileEeTraceBlock(
							pc, generation);
				if (!entry.valid)
				{
					if (is_root)
						missing_roots++;
					else
						missing_successors++;
					continue;
				}

				const auto& source = entry.block;
				output.WriteLn(
					"Vita perf v=1 window=%llu kind=cpu_ee_trace_block "
					"pc=0x%08x generation=%u instructions=%u "
					"source_instructions=%u dependency_pc=0x%08x "
					"dependency_instructions=%u scaled_cycles=%u emitted_bytes=%u "
					"host_instructions=%u helper_calls=%u state_loads=%u "
					"state_stores=%u integer=%u branches=%u memory_loads=%u "
					"memory_stores=%u mmi=%u cop0=%u cop1=%u cop2=%u other=%u "
					"flags=%u successor_count=%u successor0=0x%08x "
					"successor1=0x%08x predecessor_count=%u "
					"predecessor0=0x%08x predecessor1=0x%08x "
					"predecessor2=0x%08x predecessor3=0x%08x "
					"predecessor_truncated=%u code_words=%u "
					"code=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
					static_cast<unsigned long long>(window),
					source.pc, entry.generation, source.instruction_count,
					source.source_instruction_count, source.dependency_start_pc,
					source.dependency_instruction_count, source.scaled_cycles,
					source.emitted_bytes, source.host_instructions,
					source.helper_calls, source.state_loads, source.state_stores,
					source.guest_integer, source.guest_branches,
					source.guest_memory_loads, source.guest_memory_stores,
					source.guest_mmi, source.guest_cop0, source.guest_cop1,
					source.guest_cop2, source.guest_other, source.flags,
					source.successor_count, source.successors[0],
					source.successors[1], entry.predecessor_count,
					entry.predecessors[0], entry.predecessors[1],
					entry.predecessors[2], entry.predecessors[3],
					entry.predecessor_truncated ? 1u : 0u, entry.code_words,
					entry.code[0], entry.code[1], entry.code[2], entry.code[3],
					entry.code[4], entry.code[5], entry.code[6], entry.code[7]);
				emitted_blocks++;
				if ((emitted_blocks & 15u) == 0)
					output.FlushAndClear();

				for (u32 successor_index = 0;
					successor_index < source.successor_count; successor_index++)
				{
					const u32 successor = source.successors[successor_index];
					const auto position = std::lower_bound(
						visited_pcs.begin(), visited_pcs.end(), successor);
					if (position != visited_pcs.end() && *position == successor)
						continue;
					if (pending_keys.size() >= EE_TRACE_GRAPH_BLOCK_LIMIT)
					{
						graph_truncated = true;
						continue;
					}
					const VitaPerformanceTelemetry::CpuProfileEeTraceBlock target =
						VitaPerformanceTelemetry::GetCpuProfileEeTraceBlock(successor);
					visited_pcs.insert(position, successor);
					if (!target.valid)
					{
						missing_successors++;
						continue;
					}
					pending_keys.push_back(
						(static_cast<u64>(target.generation) << 32) | successor);
				}
			}
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_ee_trace_graph "
				"root_generations=%u blocks=%u missing_roots=%u "
				"missing_successors=%u truncated=%u block_limit=%u",
				static_cast<unsigned long long>(window),
				root_generation_count, emitted_blocks,
				missing_roots, missing_successors, graph_truncated ? 1u : 0u,
				static_cast<unsigned>(EE_TRACE_GRAPH_BLOCK_LIMIT));
			output.FlushAndClear();
			for (size_t page = 0; page < hot_guest_pc_pages; page++)
			{
				const u32 first_rank = static_cast<u32>(page *
					VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_COUNT);
				const VitaPerformanceTelemetry::CpuProfileHotPcSnapshot
					hot_iop_pcs =
						VitaPerformanceTelemetry::GetCpuProfileHotIopPcSnapshot(
							start.cpu_stage_profiler.statistical_iop_pc_sequence + 1,
							end.cpu_stage_profiler.statistical_iop_pc_sequence + 1,
							first_rank);
				if (page == 0)
				{
					output.WriteLn(
#if defined(VITASX2_CPU_PROFILER)
						"Vita perf v=1 window=%llu kind=cpu_hot_iop_block_summary "
#else
						"Vita perf v=1 window=%llu kind=cpu_hot_iop_pc_summary "
#endif
						"first_sequence=%llu next_sequence=%llu dropped=%llu "
						"invalid=%llu candidates=%u page_size=%u",
						static_cast<unsigned long long>(window),
						static_cast<unsigned long long>(hot_iop_pcs.first_sequence),
						static_cast<unsigned long long>(hot_iop_pcs.next_sequence),
						static_cast<unsigned long long>(hot_iop_pcs.dropped_samples),
						static_cast<unsigned long long>(hot_iop_pcs.invalid_samples),
						static_cast<unsigned>(
							VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_CANDIDATE_COUNT),
						static_cast<unsigned>(
							VitaPerformanceTelemetry::CPU_PROFILE_HOT_GUEST_PC_COUNT));
				}
				size_t emitted = 0;
				for (size_t i = 0; i < hot_iop_pcs.pcs.size(); i++)
				{
					const VitaPerformanceTelemetry::CpuProfileHotPc& hot_pc =
						hot_iop_pcs.pcs[i];
					if (hot_pc.samples == 0)
						break;
#if defined(VITASX2_CPU_PROFILER)
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=cpu_hot_iop_block "
						"rank=%u pc=0x%08x samples=%u words=%u "
						"code=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
						static_cast<unsigned long long>(window),
						static_cast<unsigned>(first_rank + i + 1), hot_pc.pc,
						hot_pc.samples, hot_pc.code_words,
						hot_pc.code[0], hot_pc.code[1],
						hot_pc.code[2], hot_pc.code[3],
						hot_pc.code[4], hot_pc.code[5],
						hot_pc.code[6], hot_pc.code[7]);
#else
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=cpu_hot_iop_pc "
						"rank=%u pc=0x%08x samples=%u",
						static_cast<unsigned long long>(window),
						static_cast<unsigned>(first_rank + i + 1), hot_pc.pc,
						hot_pc.samples);
#endif
					emitted++;
				}
				if (emitted != hot_iop_pcs.pcs.size())
					break;
			}
			const VitaPerformanceTelemetry::CpuProfileHotEdgeSnapshot hot_edges =
				VitaPerformanceTelemetry::GetCpuProfileHotEdgeSnapshot(
					start.cpu_stage_profiler.interval_records + 1,
					end.cpu_stage_profiler.interval_records + 1);
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_hot_edge_summary "
				"first_sequence=%llu next_sequence=%llu dropped=%llu invalid=%llu",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(hot_edges.first_sequence),
				static_cast<unsigned long long>(hot_edges.next_sequence),
				static_cast<unsigned long long>(hot_edges.dropped_records),
				static_cast<unsigned long long>(hot_edges.invalid_records));
			for (size_t i = 0;
				i < VitaPerformanceTelemetry::CPU_PROFILE_HOT_EDGE_COUNT; i++)
			{
				const VitaPerformanceTelemetry::CpuProfileHotEdge& ee_edge =
					hot_edges.ee[i];
				const VitaPerformanceTelemetry::CpuProfileHotEdge& iop_edge =
					hot_edges.iop[i];
				if (ee_edge.samples == 0 && iop_edge.samples == 0)
					break;
				output.WriteLn(
					"Vita perf v=1 window=%llu kind=cpu_hot_edge rank=%u "
					"ee_start=0x%08x ee_end=0x%08x ee_samples=%u "
					"ee_us=%llu ee_cycles=%llu "
					"ee_code=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
					"iop_start=0x%08x iop_end=0x%08x iop_samples=%u "
					"iop_us=%llu iop_cycles=%llu "
					"iop_code=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
					static_cast<unsigned long long>(window),
					static_cast<unsigned>(i + 1),
					ee_edge.start_pc, ee_edge.end_pc, ee_edge.samples,
					static_cast<unsigned long long>(ee_edge.host_time_us),
					static_cast<unsigned long long>(ee_edge.guest_cycles),
					ee_edge.code_start[0], ee_edge.code_start[1],
					ee_edge.code_start[2], ee_edge.code_start[3],
					ee_edge.code_start[4], ee_edge.code_start[5],
					ee_edge.code_start[6], ee_edge.code_start[7],
					iop_edge.start_pc, iop_edge.end_pc, iop_edge.samples,
					static_cast<unsigned long long>(iop_edge.host_time_us),
					static_cast<unsigned long long>(iop_edge.guest_cycles),
					iop_edge.code_start[0], iop_edge.code_start[1],
					iop_edge.code_start[2], iop_edge.code_start[3],
					iop_edge.code_start[4], iop_edge.code_start[5],
					iop_edge.code_start[6], iop_edge.code_start[7]);
			}
#endif
		}
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee generated_blocks=%llu host_instructions=%llu "
			"host_loads=%llu host_stores=%llu helper_calls_generated=%llu "
			"register_loads_generated=%llu register_stores_generated=%llu "
			"guest_instructions_generated=%llu origin_blocks=%llu origin_host=%llu "
			"origin_helpers=%llu origin_register_loads=%llu origin_register_stores=%llu "
			"origin_guest=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_blocks,
				start.ee.generated_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_host_instructions,
				start.ee.generated_host_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_host_load_instructions,
				start.ee.generated_host_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_host_store_instructions,
				start.ee.generated_host_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_helper_call_instructions,
				start.ee.generated_helper_call_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_state_load_instructions,
				start.ee.generated_state_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_state_store_instructions,
				start.ee.generated_state_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_guest_instructions,
				start.ee.generated_guest_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_blocks,
				origin.ee.generated_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_host_instructions,
				origin.ee.generated_host_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_helper_call_instructions,
				origin.ee.generated_helper_call_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_state_load_instructions,
				origin.ee.generated_state_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_state_store_instructions,
				origin.ee.generated_state_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_guest_instructions,
				origin.ee.generated_guest_instructions)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_codegen integer=%llu branch=%llu "
			"gpr_load=%llu gpr_store=%llu mmi=%llu cop0=%llu cop1=%llu cop2=%llu "
			"other=%llu poll_call_wait=%llu two_predicate_wait=%llu "
			"largest_pc=0x%08x largest_guest=%u largest_host=%u "
			"largest_helpers=%u largest_state_loads=%u largest_state_stores=%u "
			"origin_integer=%llu origin_branch=%llu origin_gpr_load=%llu "
			"origin_gpr_store=%llu origin_mmi=%llu origin_cop0=%llu origin_cop1=%llu "
			"origin_cop2=%llu origin_other=%llu origin_poll_call_wait=%llu "
			"origin_two_predicate_wait=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_integer_instructions,
				start.ee.generated_integer_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_branch_instructions,
				start.ee.generated_branch_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_gpr_load_instructions,
				start.ee.generated_gpr_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_gpr_store_instructions,
				start.ee.generated_gpr_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_mmi_instructions,
				start.ee.generated_mmi_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_cop0_instructions,
				start.ee.generated_cop0_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_cop1_instructions,
				start.ee.generated_cop1_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_cop2_instructions,
				start.ee.generated_cop2_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.generated_other_instructions,
				start.ee.generated_other_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_poll_call_wait_blocks,
				start.ee.generated_poll_call_wait_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_two_predicate_wait_blocks,
				start.ee.generated_two_predicate_wait_blocks)),
			end.ee.largest_generated_block_pc,
			end.ee.largest_generated_block_guest_instructions,
			end.ee.largest_generated_block_host_instructions,
			end.ee.largest_generated_block_helper_calls,
			end.ee.largest_generated_block_state_loads,
			end.ee.largest_generated_block_state_stores,
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_integer_instructions,
				origin.ee.generated_integer_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_branch_instructions,
				origin.ee.generated_branch_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_gpr_load_instructions,
				origin.ee.generated_gpr_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_gpr_store_instructions,
				origin.ee.generated_gpr_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_mmi_instructions,
				origin.ee.generated_mmi_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_cop0_instructions,
				origin.ee.generated_cop0_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_cop1_instructions,
				origin.ee.generated_cop1_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_cop2_instructions,
				origin.ee.generated_cop2_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_other_instructions,
				origin.ee.generated_other_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_poll_call_wait_blocks,
				origin.ee.generated_poll_call_wait_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.generated_two_predicate_wait_blocks,
				origin.ee.generated_two_predicate_wait_blocks)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_dispatch provider_boundaries=%llu "
			"boundary_guest_instructions=%llu direct_exits=%llu event_exits=%llu "
			"cache_hits=%llu cache_misses=%llu lookup_hits=%llu fast_dispatch_hits=%llu "
			"event_tests=%llu event_resumes=%llu event_refusals=%llu retained_wait_events=%llu "
			"retained_dmac_chcr_poll_events=%llu "
			"retained_ram_wait_events=%llu retained_ram_wait_write_exits=%llu "
			"two_predicate_wait_ff=%llu invalidated_blocks=%llu failed_blocks=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.ee.compiled_blocks,
				start.ee.compiled_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.ee.compiled_instructions,
				start.ee.compiled_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.direct_exits,
				start.ee.direct_exits)),
			static_cast<unsigned long long>(CounterDelta(end.ee.event_exits,
				start.ee.event_exits)),
			static_cast<unsigned long long>(CounterDelta(end.ee.cache_hits,
				start.ee.cache_hits)),
			static_cast<unsigned long long>(CounterDelta(end.ee.cache_misses,
				start.ee.cache_misses)),
			static_cast<unsigned long long>(CounterDelta(end.ee.lookup_hits,
				start.ee.lookup_hits)),
			static_cast<unsigned long long>(CounterDelta(end.ee.fast_dispatch_hits,
				start.ee.fast_dispatch_hits)),
			static_cast<unsigned long long>(CounterDelta(end.ee.in_frame_event_tests,
				start.ee.in_frame_event_tests)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.in_frame_event_resume_candidates,
				start.ee.in_frame_event_resume_candidates)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.in_frame_event_resume_refusals,
				start.ee.in_frame_event_resume_refusals)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.retained_unconditional_wait_events,
				start.ee.retained_unconditional_wait_events)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.retained_dmac_chcr_poll_events,
				start.ee.retained_dmac_chcr_poll_events)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.retained_ram_wait_events,
				start.ee.retained_ram_wait_events)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.retained_ram_wait_write_exits,
				start.ee.retained_ram_wait_write_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.two_predicate_wait_fast_forwards,
				start.ee.two_predicate_wait_fast_forwards)),
			static_cast<unsigned long long>(CounterDelta(end.ee.invalidated_blocks,
				start.ee.invalidated_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.ee.failed_blocks,
				start.ee.failed_blocks)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_cache resets_delta=%llu resets=%u "
			"used=%llu capacity=%llu block_records=%u cache_slots=%u "
			"slot_metadata_bytes=%u",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.ee.code_cache_resets,
				start.ee.code_cache_resets)),
			end.ee.code_cache_resets,
			static_cast<unsigned long long>(end.ee.code_cache_used),
			static_cast<unsigned long long>(end.ee.code_cache_capacity),
			end.ee.code_cache_block_records, end.ee.code_cache_slots,
			end.ee.code_cache_slot_metadata_size);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region candidates=%llu attempts=%llu compiles=%llu "
			"compile_failures=%llu compile_wall_us=%llu budget_deferrals=%llu budget_refills=%llu "
			"prescreen_passes=%llu prescreen_rejections=%llu prescreen_unknowns=%llu "
			"heap_failures=%llu code_cache_failures=%llu "
			"source_deferrals=%llu source_resumes=%llu source_replacements=%llu "
			"source_capacity_rejections=%llu source_graph_attempts=%llu "
			"source_graph_formations=%llu "
			"forward_samples=%llu forward_collisions=%llu forward_requests=%llu "
			"forward_candidates=%llu forward_rejections=%llu "
			"probe_observations=%llu probe_event_max=%llu "
			"probe_sample_misses=%llu probe_sample_retries=%llu "
			"hot_promotions=%llu probe_evictions=%llu "
			"saturations=%llu admission_deferred=%llu admission_retried=%llu "
			"admission_rejected=%llu probe_directory_repairs=%llu "
			"evictions=%llu generation_resets=%llu "
			"executions=%llu boundary_exits=%llu event_exits=%llu "
			"profitability_fallbacks=%llu entry_state_fallbacks=%llu "
			"profitability_retirements=%llu memory_exits=%llu "
			"memory_alignment=%llu memory_handler=%llu memory_translation=%llu smc=%llu "
			"continuations=%llu continuation_nonzero_debt=%llu continuation_events=%llu "
			"continuation_scheduler_elided=%llu continuation_failures=%llu "
			"persistent_region_resumes=%llu required_outer_unwinds=%llu "
			"code_bytes=%llu active=%u probes=%u armed_probes=%u deferred=%u "
			"pending=%u publication=%u maintenance=%u budget_tokens=%u budget_wait_cycles=%llu "
			"total_candidates=%llu "
			"total_attempts=%llu total_compiles=%llu total_compile_failures=%llu "
			"total_source_deferrals=%llu total_source_resumes=%llu "
			"total_source_replacements=%llu total_saturations=%llu "
			"total_admission_deferred=%llu total_admission_retried=%llu "
			"total_admission_rejected=%llu total_probe_directory_repairs=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_candidates,
				start.ee.region_candidates)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_build_attempts,
				start.ee.region_build_attempts)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_compiles,
				start.ee.region_compiles)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_compile_failures,
				start.ee.region_compile_failures)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_compile_wall_us,
				start.ee.region_compile_wall_us)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_compile_budget_deferrals,
				start.ee.region_compile_budget_deferrals)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_compile_budget_refills,
				start.ee.region_compile_budget_refills)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_profitability_prescreen_passes,
				start.ee.region_profitability_prescreen_passes)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_profitability_prescreen_rejections,
				start.ee.region_profitability_prescreen_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_profitability_prescreen_unknowns,
				start.ee.region_profitability_prescreen_unknowns)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_compiler_heap_failures,
				start.ee.region_compiler_heap_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_code_cache_failures,
				start.ee.region_code_cache_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_source_contract_deferrals,
				start.ee.region_source_contract_deferrals)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_source_contract_resumes,
				start.ee.region_source_contract_resumes)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_source_contract_replacements,
				start.ee.region_source_contract_replacements)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_source_contract_capacity_rejections,
				start.ee.region_source_contract_capacity_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_source_graph_attempts,
				start.ee.region_source_graph_attempts)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_source_graph_formations,
				start.ee.region_source_graph_formations)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_forward_event_samples,
				start.ee.region_forward_event_samples)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_forward_sample_collisions,
				start.ee.region_forward_sample_collisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_forward_sample_requests,
				start.ee.region_forward_sample_requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_forward_sample_candidates,
				start.ee.region_forward_sample_candidates)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_forward_sample_rejections,
				start.ee.region_forward_sample_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_probe_observations,
				start.ee.region_probe_observations)),
			static_cast<unsigned long long>(
				end.ee.region_maximum_event_scoped_probe_observations),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_probe_sample_misses,
				start.ee.region_probe_sample_misses)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_probe_sample_retries,
				start.ee.region_probe_sample_retries)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_hot_promotions,
				start.ee.region_hot_promotions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_probe_evictions,
				start.ee.region_probe_evictions)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_admission_saturations,
				start.ee.region_admission_saturations)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_admission_capacity_deferrals,
				start.ee.region_admission_capacity_deferrals)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_admission_capacity_retries,
				start.ee.region_admission_capacity_retries)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_admission_capacity_rejections,
				start.ee.region_admission_capacity_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_probe_directory_repairs,
				start.ee.region_probe_directory_repairs)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_evictions,
				start.ee.region_evictions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_generation_resets,
				start.ee.region_generation_resets)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_executions,
				start.ee.region_executions)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_boundary_exits,
				start.ee.region_boundary_exits)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_event_exits,
				start.ee.region_event_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_profitability_fallbacks,
				start.ee.region_profitability_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_entry_state_fallbacks,
				start.ee.region_entry_state_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_profitability_retirements,
				start.ee.region_profitability_retirements)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_memory_exits,
				start.ee.region_memory_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_memory_alignment_exits,
				start.ee.region_memory_alignment_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_memory_handler_exits,
				start.ee.region_memory_handler_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_memory_translation_exits,
				start.ee.region_memory_translation_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_self_modifying_code_exits,
				start.ee.region_self_modifying_code_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_continuation_exits,
				start.ee.region_continuation_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_continuation_nonzero_debt_exits,
				start.ee.region_continuation_nonzero_debt_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_continuation_event_exits,
				start.ee.region_continuation_event_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_continuation_scheduler_elided_exits,
				start.ee.region_continuation_scheduler_elided_exits)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.region_continuation_failures,
				start.ee.region_continuation_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.persistent_region_resumes,
				start.ee.persistent_region_resumes)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.persistent_required_outer_unwinds,
				start.ee.persistent_required_outer_unwinds)),
			static_cast<unsigned long long>(CounterDelta(end.ee.region_code_bytes,
				start.ee.region_code_bytes)),
			end.ee.region_active, end.ee.region_active_probes,
			end.ee.region_armed_probes,
			end.ee.region_deferred,
			end.ee.region_pending_candidate,
			end.ee.region_publication_dirty,
			end.ee.region_maintenance_requested,
			end.ee.region_compile_budget_tokens,
			static_cast<unsigned long long>(
				end.ee.region_compile_budget_wait_cycles),
			static_cast<unsigned long long>(end.ee.region_candidates),
			static_cast<unsigned long long>(end.ee.region_build_attempts),
			static_cast<unsigned long long>(end.ee.region_compiles),
			static_cast<unsigned long long>(end.ee.region_compile_failures),
			static_cast<unsigned long long>(
				end.ee.region_source_contract_deferrals),
			static_cast<unsigned long long>(
				end.ee.region_source_contract_resumes),
			static_cast<unsigned long long>(
				end.ee.region_source_contract_replacements),
			static_cast<unsigned long long>(end.ee.region_admission_saturations),
			static_cast<unsigned long long>(
				end.ee.region_admission_capacity_deferrals),
			static_cast<unsigned long long>(
				end.ee.region_admission_capacity_retries),
			static_cast<unsigned long long>(
				end.ee.region_admission_capacity_rejections),
			static_cast<unsigned long long>(
				end.ee.region_probe_directory_repairs));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region_translation valid=%u "
			"start=%08x:%08x bound=%08x:%08x identity_limit=%08x stride=%u",
			static_cast<unsigned long long>(window),
			end.ee.region_translation_snapshot_valid,
			end.ee.region_translation_start_high,
			end.ee.region_translation_start_low,
			end.ee.region_translation_bound_high,
			end.ee.region_translation_bound_low,
			end.ee.region_translation_identity_limit,
			end.ee.region_translation_stride_bytes);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region_shape "
			"one_block=%llu multi_block=%llu preflight=%llu "
			"preflight_store=%llu spills=%llu weighted_host=%llu "
			"weighted_hot_bytes=%llu weighted_entry_words=%llu "
			"weighted_output_words=%llu semantic_runs=%llu "
			"semantic_iterations=%llu semantic_bytes=%llu "
			"semantic_kinds=%llu:%llu:%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(
				end.ee.region_one_block_executions),
			static_cast<unsigned long long>(
				end.ee.region_multi_block_executions),
			static_cast<unsigned long long>(
				end.ee.region_preflight_executions),
			static_cast<unsigned long long>(
				end.ee.region_preflight_store_executions),
			static_cast<unsigned long long>(end.ee.region_spill_executions),
			static_cast<unsigned long long>(
				end.ee.region_weighted_host_instructions),
			static_cast<unsigned long long>(
				end.ee.region_weighted_hot_code_bytes),
			static_cast<unsigned long long>(
				end.ee.region_weighted_entry_state_words),
			static_cast<unsigned long long>(
				end.ee.region_weighted_output_state_words),
			static_cast<unsigned long long>(
				end.ee.region_semantic_kernel_executions),
			static_cast<unsigned long long>(
				end.ee.region_semantic_kernel_iterations),
			static_cast<unsigned long long>(
				end.ee.region_semantic_kernel_bytes),
			static_cast<unsigned long long>(
				end.ee.region_semantic_fill_executions),
			static_cast<unsigned long long>(
				end.ee.region_semantic_copy_executions),
			static_cast<unsigned long long>(
				end.ee.region_semantic_unretained_executions));
		for (size_t rank = 0; rank < end.ee.region_entry_profile.size(); rank++)
		{
			const VitaA32EeRegionEntryProfile& entry =
				end.ee.region_entry_profile[rank];
			if (entry.executions == 0 && entry.profitability_fallbacks == 0 &&
				entry.entry_state_fallbacks == 0)
				break;
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=ee_region_entry rank=%u "
				"pc=%08x runs=%llu counted_iterations=%llu "
				"fallbacks=%llu state_fallbacks=%llu "
				"blocks=%u source_words=%u host=%u "
				"hot_host=%u hot_loads=%u hot_stores=%u state_loads=%u "
				"state_stores=%u frame=%u scratch=%u hot_bytes=%u cold_bytes=%u "
				"phases=%u:%u:%u:%u:%u block_io=%u:%u "
				"entry_words=%u output_words=%u "
				"core_peak=%u neon_peak=%u spilled=%u spill_bytes=%u "
					"edge_moves=%u preflight=%u preflight_stores=%u stride=%u "
					"minimum_iterations=%u state_leaves=%u low32_guards=%u "
					"semantic=%u:%u:%u:%u:%u",
				static_cast<unsigned long long>(window),
				static_cast<unsigned>(rank + 1), entry.pc,
				static_cast<unsigned long long>(entry.executions),
				static_cast<unsigned long long>(entry.counted_iterations),
				static_cast<unsigned long long>(entry.profitability_fallbacks),
				static_cast<unsigned long long>(entry.entry_state_fallbacks),
				entry.block_count, entry.source_words, entry.host_instructions,
				entry.hot_host_instructions, entry.hot_host_loads,
				entry.hot_host_stores, entry.hot_state_loads,
				entry.hot_state_stores, entry.frame_bytes,
				entry.work_scratch_bytes,
				entry.hot_code_bytes, entry.cold_code_bytes,
				entry.prologue_hot_bytes, entry.entry_event_hot_bytes,
				entry.entry_iteration_hot_bytes, entry.entry_memory_hot_bytes,
				entry.block_hot_bytes, entry.block_host_loads,
				entry.block_host_stores,
				entry.entry_state_words, entry.output_state_words,
				entry.core_peak_words, entry.neon_peak_q,
				entry.spilled_values, entry.spill_bytes, entry.edge_moves,
					entry.preflight_accesses, entry.preflight_store_accesses,
					entry.preflight_stride,
					entry.minimum_profitable_iterations,
					entry.pre_entry_state_leaves, entry.entry_low32_guards,
					entry.semantic_kernel_kind,
					entry.semantic_kernel_hot_bytes,
					entry.semantic_kernel_bytes_per_iteration,
					entry.semantic_kernel_minimum_profitable_bytes,
					entry.semantic_kernel_target_cost_valid);
		}
		output.WriteLn(
			"Vita perf v=2 window=%llu kind=ee_region_probes count=%u "
			"p0=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x "
			"p1=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x "
			"p2=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x "
			"p3=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x",
			static_cast<unsigned long long>(window),
			end.ee.region_probe_snapshot_count,
			end.ee.region_probe_entry_pc[0], end.ee.region_probe_backedge_pc[0],
			end.ee.region_probe_source_end_pc[0],
			end.ee.region_probe_snapshot_observations[0],
			end.ee.region_probe_maximum_observations[0], end.ee.region_probe_samples[0],
			end.ee.region_probe_required[0], end.ee.region_probe_internal_blocks[0],
			end.ee.region_probe_flags[0],
			end.ee.region_probe_entry_pc[1], end.ee.region_probe_backedge_pc[1],
			end.ee.region_probe_source_end_pc[1],
			end.ee.region_probe_snapshot_observations[1],
			end.ee.region_probe_maximum_observations[1], end.ee.region_probe_samples[1],
			end.ee.region_probe_required[1], end.ee.region_probe_internal_blocks[1],
			end.ee.region_probe_flags[1],
			end.ee.region_probe_entry_pc[2], end.ee.region_probe_backedge_pc[2],
			end.ee.region_probe_source_end_pc[2],
			end.ee.region_probe_snapshot_observations[2],
			end.ee.region_probe_maximum_observations[2], end.ee.region_probe_samples[2],
			end.ee.region_probe_required[2], end.ee.region_probe_internal_blocks[2],
			end.ee.region_probe_flags[2],
			end.ee.region_probe_entry_pc[3], end.ee.region_probe_backedge_pc[3],
			end.ee.region_probe_source_end_pc[3],
			end.ee.region_probe_snapshot_observations[3],
			end.ee.region_probe_maximum_observations[3], end.ee.region_probe_samples[3],
			end.ee.region_probe_required[3], end.ee.region_probe_internal_blocks[3],
			end.ee.region_probe_flags[3]);
		output.WriteLn(
			"Vita perf v=2 window=%llu kind=ee_region_probes_tail "
			"p4=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x "
			"p5=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x "
			"p6=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x "
			"p7=%08x:%08x:%08x:%u:%u:%u:%u:%u:%02x",
			static_cast<unsigned long long>(window),
			end.ee.region_probe_entry_pc[4], end.ee.region_probe_backedge_pc[4],
			end.ee.region_probe_source_end_pc[4],
			end.ee.region_probe_snapshot_observations[4],
			end.ee.region_probe_maximum_observations[4], end.ee.region_probe_samples[4],
			end.ee.region_probe_required[4], end.ee.region_probe_internal_blocks[4],
			end.ee.region_probe_flags[4],
			end.ee.region_probe_entry_pc[5], end.ee.region_probe_backedge_pc[5],
			end.ee.region_probe_source_end_pc[5],
			end.ee.region_probe_snapshot_observations[5],
			end.ee.region_probe_maximum_observations[5], end.ee.region_probe_samples[5],
			end.ee.region_probe_required[5], end.ee.region_probe_internal_blocks[5],
			end.ee.region_probe_flags[5],
			end.ee.region_probe_entry_pc[6], end.ee.region_probe_backedge_pc[6],
			end.ee.region_probe_source_end_pc[6],
			end.ee.region_probe_snapshot_observations[6],
			end.ee.region_probe_maximum_observations[6], end.ee.region_probe_samples[6],
			end.ee.region_probe_required[6], end.ee.region_probe_internal_blocks[6],
			end.ee.region_probe_flags[6],
			end.ee.region_probe_entry_pc[7], end.ee.region_probe_backedge_pc[7],
			end.ee.region_probe_source_end_pc[7],
			end.ee.region_probe_snapshot_observations[7],
			end.ee.region_probe_maximum_observations[7], end.ee.region_probe_samples[7],
			end.ee.region_probe_required[7], end.ee.region_probe_internal_blocks[7],
			end.ee.region_probe_flags[7]);
		output.WriteLn(
			"Vita perf v=2 window=%llu kind=ee_region_deferred count=%u "
			"d0=%08x:%08x:%08x:%08x:%02x d1=%08x:%08x:%08x:%08x:%02x "
			"d2=%08x:%08x:%08x:%08x:%02x d3=%08x:%08x:%08x:%08x:%02x",
			static_cast<unsigned long long>(window),
			end.ee.region_deferred_snapshot_count,
			end.ee.region_deferred_entry_pc[0], end.ee.region_deferred_backedge_pc[0],
			end.ee.region_deferred_source_end_pc[0],
			end.ee.region_deferred_missing_pc[0],
			end.ee.region_deferred_flags[0],
			end.ee.region_deferred_entry_pc[1], end.ee.region_deferred_backedge_pc[1],
			end.ee.region_deferred_source_end_pc[1],
			end.ee.region_deferred_missing_pc[1],
			end.ee.region_deferred_flags[1],
			end.ee.region_deferred_entry_pc[2], end.ee.region_deferred_backedge_pc[2],
			end.ee.region_deferred_source_end_pc[2],
			end.ee.region_deferred_missing_pc[2],
			end.ee.region_deferred_flags[2],
			end.ee.region_deferred_entry_pc[3], end.ee.region_deferred_backedge_pc[3],
			end.ee.region_deferred_source_end_pc[3],
			end.ee.region_deferred_missing_pc[3],
			end.ee.region_deferred_flags[3]);
		output.WriteLn(
			"Vita perf v=2 window=%llu kind=ee_region_deferred_tail "
			"d4=%08x:%08x:%08x:%08x:%02x d5=%08x:%08x:%08x:%08x:%02x "
			"d6=%08x:%08x:%08x:%08x:%02x d7=%08x:%08x:%08x:%08x:%02x",
			static_cast<unsigned long long>(window),
			end.ee.region_deferred_entry_pc[4], end.ee.region_deferred_backedge_pc[4],
			end.ee.region_deferred_source_end_pc[4],
			end.ee.region_deferred_missing_pc[4],
			end.ee.region_deferred_flags[4],
			end.ee.region_deferred_entry_pc[5], end.ee.region_deferred_backedge_pc[5],
			end.ee.region_deferred_source_end_pc[5],
			end.ee.region_deferred_missing_pc[5],
			end.ee.region_deferred_flags[5],
			end.ee.region_deferred_entry_pc[6], end.ee.region_deferred_backedge_pc[6],
			end.ee.region_deferred_source_end_pc[6],
			end.ee.region_deferred_missing_pc[6],
			end.ee.region_deferred_flags[6],
			end.ee.region_deferred_entry_pc[7], end.ee.region_deferred_backedge_pc[7],
			end.ee.region_deferred_source_end_pc[7],
			end.ee.region_deferred_missing_pc[7],
			end.ee.region_deferred_flags[7]);
		const auto write_repeated_candidate_snapshot =
			[&](const char* kind, u32 base) {
				output.WriteLn(
					"Vita perf v=3 window=%llu kind=%s count=%u base=%u "
					"r0=%08x:%08x:%08x:%u:%u:%u:%u:%u "
					"r1=%08x:%08x:%08x:%u:%u:%u:%u:%u "
					"r2=%08x:%08x:%08x:%u:%u:%u:%u:%u "
					"r3=%08x:%08x:%08x:%u:%u:%u:%u:%u",
					static_cast<unsigned long long>(window), kind,
					end.ee.region_repeated_candidate_snapshot_count, base,
					end.ee.region_repeated_candidate_entry_pc[base + 0],
					end.ee.region_repeated_candidate_source_end_pc[base + 0],
					end.ee.region_repeated_candidate_detail_pc[base + 0],
					end.ee.region_repeated_candidate_observations[base + 0],
					end.ee.region_repeated_candidate_blocks[base + 0],
					end.ee.region_repeated_candidate_outcome[base + 0],
					end.ee.region_repeated_candidate_failure_stage[base + 0],
					end.ee.region_repeated_candidate_failure_detail[base + 0],
					end.ee.region_repeated_candidate_entry_pc[base + 1],
					end.ee.region_repeated_candidate_source_end_pc[base + 1],
					end.ee.region_repeated_candidate_detail_pc[base + 1],
					end.ee.region_repeated_candidate_observations[base + 1],
					end.ee.region_repeated_candidate_blocks[base + 1],
					end.ee.region_repeated_candidate_outcome[base + 1],
					end.ee.region_repeated_candidate_failure_stage[base + 1],
					end.ee.region_repeated_candidate_failure_detail[base + 1],
					end.ee.region_repeated_candidate_entry_pc[base + 2],
					end.ee.region_repeated_candidate_source_end_pc[base + 2],
					end.ee.region_repeated_candidate_detail_pc[base + 2],
					end.ee.region_repeated_candidate_observations[base + 2],
					end.ee.region_repeated_candidate_blocks[base + 2],
					end.ee.region_repeated_candidate_outcome[base + 2],
					end.ee.region_repeated_candidate_failure_stage[base + 2],
					end.ee.region_repeated_candidate_failure_detail[base + 2],
					end.ee.region_repeated_candidate_entry_pc[base + 3],
					end.ee.region_repeated_candidate_source_end_pc[base + 3],
					end.ee.region_repeated_candidate_detail_pc[base + 3],
					end.ee.region_repeated_candidate_observations[base + 3],
					end.ee.region_repeated_candidate_blocks[base + 3],
					end.ee.region_repeated_candidate_outcome[base + 3],
					end.ee.region_repeated_candidate_failure_stage[base + 3],
					end.ee.region_repeated_candidate_failure_detail[base + 3]);
			};
		write_repeated_candidate_snapshot("ee_region_repeated", 0);
		write_repeated_candidate_snapshot("ee_region_repeated_1", 4);
		write_repeated_candidate_snapshot("ee_region_repeated_2", 8);
		write_repeated_candidate_snapshot("ee_region_repeated_tail", 12);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region_target_cost analyses=%llu "
			"valid=%u entry=%08x end=%08x diagnostics=%08x "
			"shape=%u:%u:%u code=%u:%u state=%u:%u exits=%u:%u "
			"exit_kinds=%016llx:%016llx control_targets=%016llx:%016llx "
			"compact=%u:%u:%u "
			"pressure=%u:%u:%u spill=%u:%u:%u:%u:%u "
			"edges=%u:%u:%u:%u:%u memory=%u:%u forward=%u "
			"forward_proof=%u:%u:%u:%u preflight=%u:%u aggregate=%u "
			"emitted=%u failure=%u:%u:%u:%04x:%08x:%08x:%u",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(end.ee.region_target_cost_analyses),
			end.ee.region_target_cost_valid,
			end.ee.region_target_cost_entry_pc,
			end.ee.region_target_cost_source_end_pc,
			end.ee.region_target_cost_semantic_diagnostics,
			end.ee.region_target_cost_blocks,
			end.ee.region_target_cost_source_words,
			end.ee.region_target_cost_direct_calls,
			end.ee.region_target_cost_host_instructions,
			end.ee.region_target_cost_hot_code_bytes,
			end.ee.region_target_cost_entry_state_words,
			end.ee.region_target_cost_output_state_words,
			end.ee.region_target_cost_exit_sites,
			end.ee.region_target_cost_exit_state_words,
			static_cast<unsigned long long>(
				end.ee.region_target_cost_exit_sites_by_kind),
			static_cast<unsigned long long>(
				end.ee.region_target_cost_exit_state_words_by_kind),
			static_cast<unsigned long long>(
				end.ee.region_target_cost_control_exit_sites_by_target),
			static_cast<unsigned long long>(
				end.ee.region_target_cost_control_exit_state_words_by_target),
			end.ee.region_target_cost_compact_exit_descriptors,
			end.ee.region_target_cost_compact_exit_words,
			end.ee.region_target_cost_compact_snapshot_bytes,
			end.ee.region_target_cost_core_peak_words,
			end.ee.region_target_cost_vfp_peak_s,
			end.ee.region_target_cost_neon_peak_q,
			end.ee.region_target_cost_spilled_values,
			end.ee.region_target_cost_spill_bytes,
			end.ee.region_target_cost_spilled_core_values,
			end.ee.region_target_cost_spilled_vfp_values,
			end.ee.region_target_cost_spilled_neon_values,
			end.ee.region_target_cost_edge_moves,
			end.ee.region_target_cost_edge_call_moves,
			end.ee.region_target_cost_edge_return_moves,
			end.ee.region_target_cost_edge_backedge_moves,
			end.ee.region_target_cost_edge_state_words,
			end.ee.region_target_cost_memory_loads,
			end.ee.region_target_cost_memory_stores,
			end.ee.region_target_cost_forwarded_memory_loads,
			end.ee.region_target_cost_memory_forward_candidates,
			end.ee.region_target_cost_memory_forward_reaching_stores,
			end.ee.region_target_cost_memory_forward_address_matches,
			end.ee.region_target_cost_memory_forward_state_matches,
			end.ee.region_target_cost_memory_preflight_ranges,
			end.ee.region_target_cost_memory_preflight_accesses,
			end.ee.region_target_cost_aggregate_cycle_plan_status,
			end.ee.region_target_cost_backend_emitted,
			end.ee.region_target_cost_failure_stage,
			end.ee.region_target_cost_backend_failure,
			end.ee.region_target_cost_failure_emission_step,
			end.ee.region_target_cost_failure_ir_opcode,
			end.ee.region_target_cost_failure_value,
			end.ee.region_target_cost_failure_pc,
			end.ee.region_target_cost_failure_detail);
		for (u32 index = 0;
			index < std::min<u32>(end.ee.region_target_cost_block_snapshot_count,
				end.ee.region_target_cost_block_snapshot.size()); index++)
		{
			const VitaA32EeRegionTargetCostBlock& block =
				end.ee.region_target_cost_block_snapshot[index];
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=ee_region_target_cost_block "
				"rank=%u pc=%08x source=%u code=%u io=%u:%u "
				"spill=%u:%u:%u:%u:%u edge=%u:%u exits=%u:%u roles=%u",
				static_cast<unsigned long long>(window), index, block.pc,
				block.source_instructions, block.hot_bytes, block.host_loads,
				block.host_stores, block.spilled_core_values,
				block.spilled_vfp_values, block.spilled_neon_values,
				block.spill_loads, block.spill_stores, block.edge_moves,
				block.edge_state_words, block.exit_sites,
				block.exit_state_words, block.direct_call_roles);
		}
		for (u32 index = 0;
			index < std::min<u32>(end.ee.region_target_cost_snapshot_count,
				end.ee.region_target_cost_snapshot.size()); index++)
		{
			const VitaA32EeRegionTargetCostProfile& cost =
				end.ee.region_target_cost_snapshot[index];
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=ee_region_target_cost_slot slot=%u "
				"valid=%u entry=%08x end=%08x diagnostics=%08x "
				"shape=%u:%u:%u code=%u:%u state=%u:%u exits=%u:%u "
				"exit_kinds=%016llx:%016llx control_targets=%016llx:%016llx "
				"compact=%u:%u:%u pressure=%u:%u:%u spill=%u:%u:%u:%u:%u "
				"edges=%u:%u:%u:%u:%u memory=%u:%u forward=%u "
				"forward_proof=%u:%u:%u:%u preflight=%u:%u aggregate=%u "
				"emitted=%u failure=%u:%u:%u:%04x:%08x:%08x:%u",
				static_cast<unsigned long long>(window), index, cost.valid,
				cost.entry_pc, cost.source_end_pc, cost.semantic_diagnostics,
				cost.blocks, cost.source_words, cost.direct_calls,
				cost.host_instructions, cost.hot_code_bytes,
				cost.entry_state_words, cost.output_state_words,
				cost.exit_sites, cost.exit_state_words,
				static_cast<unsigned long long>(cost.exit_sites_by_kind),
				static_cast<unsigned long long>(cost.exit_state_words_by_kind),
				static_cast<unsigned long long>(
					cost.control_exit_sites_by_target),
				static_cast<unsigned long long>(
					cost.control_exit_state_words_by_target),
				cost.compact_exit_descriptors, cost.compact_exit_words,
				cost.compact_snapshot_bytes, cost.core_peak_words,
				cost.vfp_peak_s, cost.neon_peak_q, cost.spilled_values,
				cost.spill_bytes, cost.spilled_core_values,
				cost.spilled_vfp_values, cost.spilled_neon_values,
				cost.edge_moves, cost.edge_call_moves, cost.edge_return_moves,
				cost.edge_backedge_moves, cost.edge_state_words,
				cost.memory_loads, cost.memory_stores,
				cost.forwarded_memory_loads,
				cost.memory_forward_candidates,
				cost.memory_forward_reaching_stores,
				cost.memory_forward_address_matches,
				cost.memory_forward_state_matches,
				cost.memory_preflight_ranges,
				cost.memory_preflight_accesses,
				cost.aggregate_cycle_plan_status, cost.backend_emitted,
				cost.failure_stage, cost.backend_failure,
				cost.failure_emission_step, cost.failure_ir_opcode,
				cost.failure_value, cost.failure_pc, cost.failure_detail);
		}
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region_failures count=%u "
			"e0=%08x:%u:%u:%08x:%08x e1=%08x:%u:%u:%08x:%08x "
			"e2=%08x:%u:%u:%08x:%08x e3=%08x:%u:%u:%08x:%08x",
			static_cast<unsigned long long>(window),
			end.ee.region_failure_snapshot_count,
			end.ee.region_failure_entry_pc[0], end.ee.region_failure_stage[0],
			end.ee.region_failure_backend[0], end.ee.region_failure_pc[0],
			end.ee.region_failure_opcode[0],
			end.ee.region_failure_entry_pc[1], end.ee.region_failure_stage[1],
			end.ee.region_failure_backend[1], end.ee.region_failure_pc[1],
			end.ee.region_failure_opcode[1],
			end.ee.region_failure_entry_pc[2], end.ee.region_failure_stage[2],
			end.ee.region_failure_backend[2], end.ee.region_failure_pc[2],
			end.ee.region_failure_opcode[2],
			end.ee.region_failure_entry_pc[3], end.ee.region_failure_stage[3],
			end.ee.region_failure_backend[3], end.ee.region_failure_pc[3],
			end.ee.region_failure_opcode[3]);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region_failures_tail "
			"e4=%08x:%u:%u:%08x:%08x e5=%08x:%u:%u:%08x:%08x "
			"e6=%08x:%u:%u:%08x:%08x e7=%08x:%u:%u:%08x:%08x",
			static_cast<unsigned long long>(window),
			end.ee.region_failure_entry_pc[4], end.ee.region_failure_stage[4],
			end.ee.region_failure_backend[4], end.ee.region_failure_pc[4],
			end.ee.region_failure_opcode[4],
			end.ee.region_failure_entry_pc[5], end.ee.region_failure_stage[5],
			end.ee.region_failure_backend[5], end.ee.region_failure_pc[5],
			end.ee.region_failure_opcode[5],
			end.ee.region_failure_entry_pc[6], end.ee.region_failure_stage[6],
			end.ee.region_failure_backend[6], end.ee.region_failure_pc[6],
			end.ee.region_failure_opcode[6],
			end.ee.region_failure_entry_pc[7], end.ee.region_failure_stage[7],
			end.ee.region_failure_backend[7], end.ee.region_failure_pc[7],
			end.ee.region_failure_opcode[7]);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region_failure_details "
			"e0=%u:%04x:%08x:%08x e1=%u:%04x:%08x:%08x "
			"e2=%u:%04x:%08x:%08x e3=%u:%04x:%08x:%08x",
			static_cast<unsigned long long>(window),
			end.ee.region_failure_emission_step[0],
			end.ee.region_failure_ir_opcode[0], end.ee.region_failure_value[0],
			end.ee.region_failure_detail[0],
			end.ee.region_failure_emission_step[1],
			end.ee.region_failure_ir_opcode[1], end.ee.region_failure_value[1],
			end.ee.region_failure_detail[1],
			end.ee.region_failure_emission_step[2],
			end.ee.region_failure_ir_opcode[2], end.ee.region_failure_value[2],
			end.ee.region_failure_detail[2],
			end.ee.region_failure_emission_step[3],
			end.ee.region_failure_ir_opcode[3], end.ee.region_failure_value[3],
			end.ee.region_failure_detail[3]);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_region_failure_details_tail "
			"e4=%u:%04x:%08x:%08x e5=%u:%04x:%08x:%08x "
			"e6=%u:%04x:%08x:%08x e7=%u:%04x:%08x:%08x",
			static_cast<unsigned long long>(window),
			end.ee.region_failure_emission_step[4],
			end.ee.region_failure_ir_opcode[4], end.ee.region_failure_value[4],
			end.ee.region_failure_detail[4],
			end.ee.region_failure_emission_step[5],
			end.ee.region_failure_ir_opcode[5], end.ee.region_failure_value[5],
			end.ee.region_failure_detail[5],
			end.ee.region_failure_emission_step[6],
			end.ee.region_failure_ir_opcode[6], end.ee.region_failure_value[6],
			end.ee.region_failure_detail[6],
			end.ee.region_failure_emission_step[7],
			end.ee.region_failure_ir_opcode[7], end.ee.region_failure_value[7],
			end.ee.region_failure_detail[7]);
#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
		// Sample the process allocator only at the existing cold 120-VSync
		// reporting boundary. This is diagnostic-only telemetry: it must never
		// add mallinfo()/sysmem calls to the normal CPU0 or GS hot paths.
		const struct mallinfo heap = mallinfo();
		const u32 heap_limit = _newlib_heap_size_user;
		const u32 heap_arena = static_cast<u32>(heap.arena);
		const u32 heap_used = static_cast<u32>(heap.uordblks);
		const u32 heap_free = static_cast<u32>(heap.fordblks);
		const u32 heap_sbrk_remaining =
			heap_limit > heap_arena ? heap_limit - heap_arena : 0;
		SceKernelFreeMemorySizeInfo free_memory{};
		free_memory.size = sizeof(free_memory);
		const s32 free_memory_result =
			sceKernelGetFreeMemorySize(&free_memory);
		constexpr s32 MaximumCredibleLpddrBytes = 512 * 1024 * 1024;
		const bool free_memory_valid = free_memory_result >= 0 &&
			free_memory.size_user >= 0 &&
			free_memory.size_user <= MaximumCredibleLpddrBytes;
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=memory heap_limit=%u "
			"heap_arena=%u heap_used=%u heap_free=%u heap_headroom=%u "
			"lpddr_free=%u lpddr_valid=%u lpddr_result=%08x",
			static_cast<unsigned long long>(window), heap_limit, heap_arena,
			heap_used, heap_free, heap_sbrk_remaining + heap_free,
			free_memory_valid ?
				static_cast<u32>(free_memory.size_user) : 0,
			free_memory_valid ? 1u : 0u,
			static_cast<u32>(free_memory_result));
#endif
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=ee_fallback interpreter_steps=%llu "
			"fallbacks=%llu unsupported=%llu scan_boundary=%llu branch_likely=%llu "
			"execute_failed=%llu interpreter_path=%llu last_pc=0x%08x "
			"last_opcode=0x%08x last_reason=%u",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.ee.interpreter_steps,
				start.ee.interpreter_steps)),
			static_cast<unsigned long long>(CounterDelta(ee_fallbacks(end.ee),
				ee_fallbacks(start.ee))),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.scan_unsupported_fallbacks,
				start.ee.scan_unsupported_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(end.ee.scan_boundary_fallbacks,
				start.ee.scan_boundary_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.exact_trace_branch_likely_fallbacks,
				start.ee.exact_trace_branch_likely_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(end.ee.execute_failed_fallbacks,
				start.ee.execute_failed_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.ee.interpreter_path_fallbacks,
				start.ee.interpreter_path_fallbacks)),
			end.ee.last_interpreter_pc, end.ee.last_interpreter_opcode,
			end.ee.last_interpreter_reason);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=vu0 execute_calls=%llu executed_blocks=%llu "
			"executed_pairs=%llu interpreter_steps=%llu generated_blocks=%llu "
			"generated_pairs=%llu host_instructions=%llu host_loads=%llu host_stores=%llu "
			"helper_calls_generated=%llu "
			"register_loads_generated=%llu register_stores_generated=%llu "
			"content_hits=%llu invalidations=%llu scan_rejects=%llu compile_failures=%llu "
			"origin_execute_calls=%llu origin_executed_pairs=%llu "
			"origin_interpreter_steps=%llu origin_generated_blocks=%llu "
			"origin_generated_pairs=%llu origin_compile_failures=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.vu0.execute_calls,
				start.vu0.execute_calls)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.executed_blocks,
				start.vu0.executed_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.executed_pairs,
				start.vu0.executed_pairs)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.interpreter_steps,
				start.vu0.interpreter_steps)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.generated_blocks,
				start.vu0.generated_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.generated_pairs,
				start.vu0.generated_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu0.generated_host_instructions,
				start.vu0.generated_host_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu0.generated_host_load_instructions,
				start.vu0.generated_host_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu0.generated_host_store_instructions,
				start.vu0.generated_host_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu0.generated_helper_call_instructions,
				start.vu0.generated_helper_call_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu0.generated_state_load_instructions,
				start.vu0.generated_state_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu0.generated_state_store_instructions,
				start.vu0.generated_state_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.content_cache_hits,
				start.vu0.content_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.invalidations,
				start.vu0.invalidations)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.scan_rejects,
				start.vu0.scan_rejects)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.compile_failures,
				start.vu0.compile_failures)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.execute_calls,
				origin.vu0.execute_calls)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.executed_pairs,
				origin.vu0.executed_pairs)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.interpreter_steps,
				origin.vu0.interpreter_steps)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.generated_blocks,
				origin.vu0.generated_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.generated_pairs,
				origin.vu0.generated_pairs)),
			static_cast<unsigned long long>(CounterDelta(end.vu0.compile_failures,
				origin.vu0.compile_failures)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=vu1 programs=%llu executed_blocks=%llu "
			"executed_pairs=%llu interpreter_steps=%llu generated_blocks=%llu "
			"generated_pairs=%llu host_instructions=%llu host_loads=%llu host_stores=%llu "
			"helper_calls_generated=%llu register_loads_generated=%llu "
			"register_stores_generated=%llu working_flag_blocks=%llu "
			"working_flag_producers=%llu working_fdiv_barriers=%llu "
			"working_state_stores_removed=%llu "
			"nearest_neon_fmac_ops=%llu nearest_neon_scalar_ops_removed=%llu "
			"nearest_neon_conversion_ops=%llu "
			"nearest_neon_conversion_scalar_ops_removed=%llu "
			"nearest_neon_half_ops=%llu "
			"nearest_neon_efu_ops=%llu nearest_neon_efu_scalar_ops_removed=%llu "
			"approximate_q_ops=%llu approximate_p_ops=%llu "
			"neon_clip_pairs=%llu "
			"mac_classification_elisions=%llu mac_classification_min_instructions_removed=%llu "
			"mvu_flag_hack_blocks=%llu status_classification_elisions=%llu "
			"complete_flag_classification_elisions=%llu "
			"scheduled_upper_tests_elided=%llu scheduled_lower_tests_elided=%llu "
			"scheduled_ialu_producers_elided=%llu scheduled_vi_backup_writes_elided=%llu "
			"scheduled_fmac_hazard_metadata_pairs=%llu "
			"scheduled_local_fmac_warmup_pairs_elided=%llu "
			"scheduled_local_fmac_relative_cycle_pairs=%llu "
			"empty_entry_blocks=%llu empty_entry_pairs=%llu "
			"empty_entry_local_blocks=%llu empty_entry_local_pairs=%llu "
			"empty_entry_pipe_test_elisions=%llu empty_entry_executions=%llu "
			"origin_programs=%llu "
			"origin_executed_pairs=%llu origin_interpreter_steps=%llu "
			"origin_generated_blocks=%llu origin_generated_pairs=%llu "
			"origin_working_flag_blocks=%llu origin_working_flag_producers=%llu "
			"origin_working_fdiv_barriers=%llu origin_working_state_stores_removed=%llu "
			"origin_nearest_neon_fmac_ops=%llu origin_nearest_neon_scalar_ops_removed=%llu "
			"origin_nearest_neon_conversion_ops=%llu "
			"origin_nearest_neon_conversion_scalar_ops_removed=%llu "
			"origin_nearest_neon_half_ops=%llu "
			"origin_nearest_neon_efu_ops=%llu "
			"origin_nearest_neon_efu_scalar_ops_removed=%llu "
			"origin_approximate_q_ops=%llu origin_approximate_p_ops=%llu "
			"origin_neon_clip_pairs=%llu "
			"origin_mac_classification_elisions=%llu "
			"origin_mac_classification_min_instructions_removed=%llu "
			"origin_mvu_flag_hack_blocks=%llu origin_status_classification_elisions=%llu "
			"origin_complete_flag_classification_elisions=%llu "
			"origin_scheduled_upper_tests_elided=%llu "
			"origin_scheduled_lower_tests_elided=%llu "
			"origin_scheduled_ialu_producers_elided=%llu "
			"origin_scheduled_vi_backup_writes_elided=%llu "
			"origin_scheduled_fmac_hazard_metadata_pairs=%llu "
			"origin_scheduled_local_fmac_warmup_pairs_elided=%llu "
			"origin_scheduled_local_fmac_relative_cycle_pairs=%llu "
			"origin_empty_entry_blocks=%llu origin_empty_entry_pairs=%llu "
			"origin_empty_entry_local_blocks=%llu origin_empty_entry_local_pairs=%llu "
			"origin_empty_entry_pipe_test_elisions=%llu origin_empty_entry_executions=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.vu1.completed_programs,
				start.vu1.completed_programs)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.executed_blocks,
				start.vu1.executed_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.executed_pairs,
				start.vu1.executed_pairs)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.interpreter_steps,
				start.vu1.interpreter_steps)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.generated_blocks,
				start.vu1.generated_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.generated_pairs,
				start.vu1.generated_pairs)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.generated_host_instructions,
				start.vu1.generated_host_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.generated_host_load_instructions,
				start.vu1.generated_host_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.generated_host_store_instructions,
				start.vu1.generated_host_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.generated_helper_call_instructions,
				start.vu1.generated_helper_call_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.generated_state_load_instructions,
				start.vu1.generated_state_load_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.generated_state_store_instructions,
				start.vu1.generated_state_store_instructions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_flag_blocks,
				start.vu1.resident_working_fmac_flag_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_flag_producers,
				start.vu1.resident_working_fmac_flag_producers)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_fdiv_barriers,
				start.vu1.resident_working_fmac_fdiv_barriers)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_state_stores_removed,
				start.vu1.resident_working_fmac_state_stores_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_fmac_ops,
				start.vu1.nearest_neon_fmac_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_scalar_ops_removed,
				start.vu1.nearest_neon_scalar_ops_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_conversion_ops,
				start.vu1.nearest_neon_conversion_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_conversion_scalar_ops_removed,
				start.vu1.nearest_neon_conversion_scalar_ops_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_half_ops,
				start.vu1.nearest_neon_half_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_efu_ops,
				start.vu1.nearest_neon_efu_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_efu_scalar_ops_removed,
				start.vu1.nearest_neon_efu_scalar_ops_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.approximate_q_ops, start.vu1.approximate_q_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.approximate_p_ops, start.vu1.approximate_p_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.neon_clip_pairs, start.vu1.neon_clip_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.mac_flag_classification_elisions,
				start.vu1.mac_flag_classification_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.mac_flag_classification_minimum_instructions_removed,
				start.vu1.mac_flag_classification_minimum_instructions_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.mvu_flag_hack_blocks, start.vu1.mvu_flag_hack_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.status_flag_classification_elisions,
				start.vu1.status_flag_classification_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.complete_flag_classification_elisions,
				start.vu1.complete_flag_classification_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_upper_stall_tests_elided,
				start.vu1.scheduled_upper_stall_tests_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_lower_stall_tests_elided,
				start.vu1.scheduled_lower_stall_tests_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_ialu_producers_elided,
				start.vu1.scheduled_ialu_producers_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_vi_backup_writes_elided,
				start.vu1.scheduled_vi_backup_writes_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_fmac_hazard_metadata_pairs,
				start.vu1.scheduled_fmac_hazard_metadata_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_local_fmac_warmup_pairs_elided,
				start.vu1.scheduled_local_fmac_warmup_pairs_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_local_fmac_relative_cycle_pairs,
				start.vu1.scheduled_local_fmac_relative_cycle_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_entry_blocks,
				start.vu1.empty_pipeline_entry_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_entry_pairs,
				start.vu1.empty_pipeline_entry_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_local_fmac_blocks,
				start.vu1.empty_pipeline_local_fmac_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_local_fmac_pairs,
				start.vu1.empty_pipeline_local_fmac_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_test_pipes_elisions,
				start.vu1.empty_pipeline_test_pipes_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_entry_executions,
				start.vu1.empty_pipeline_entry_executions)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.completed_programs,
				origin.vu1.completed_programs)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.executed_pairs,
				origin.vu1.executed_pairs)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.interpreter_steps,
				origin.vu1.interpreter_steps)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.generated_blocks,
				origin.vu1.generated_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.generated_pairs,
				origin.vu1.generated_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_flag_blocks,
				origin.vu1.resident_working_fmac_flag_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_flag_producers,
				origin.vu1.resident_working_fmac_flag_producers)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_fdiv_barriers,
				origin.vu1.resident_working_fmac_fdiv_barriers)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.resident_working_fmac_state_stores_removed,
				origin.vu1.resident_working_fmac_state_stores_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_fmac_ops,
				origin.vu1.nearest_neon_fmac_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_scalar_ops_removed,
				origin.vu1.nearest_neon_scalar_ops_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_conversion_ops,
				origin.vu1.nearest_neon_conversion_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_conversion_scalar_ops_removed,
				origin.vu1.nearest_neon_conversion_scalar_ops_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_half_ops,
				origin.vu1.nearest_neon_half_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_efu_ops,
				origin.vu1.nearest_neon_efu_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.nearest_neon_efu_scalar_ops_removed,
				origin.vu1.nearest_neon_efu_scalar_ops_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.approximate_q_ops, origin.vu1.approximate_q_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.approximate_p_ops, origin.vu1.approximate_p_ops)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.neon_clip_pairs, origin.vu1.neon_clip_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.mac_flag_classification_elisions,
				origin.vu1.mac_flag_classification_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.mac_flag_classification_minimum_instructions_removed,
				origin.vu1.mac_flag_classification_minimum_instructions_removed)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.mvu_flag_hack_blocks, origin.vu1.mvu_flag_hack_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.status_flag_classification_elisions,
				origin.vu1.status_flag_classification_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.complete_flag_classification_elisions,
				origin.vu1.complete_flag_classification_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_upper_stall_tests_elided,
				origin.vu1.scheduled_upper_stall_tests_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_lower_stall_tests_elided,
				origin.vu1.scheduled_lower_stall_tests_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_ialu_producers_elided,
				origin.vu1.scheduled_ialu_producers_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_vi_backup_writes_elided,
				origin.vu1.scheduled_vi_backup_writes_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_fmac_hazard_metadata_pairs,
				origin.vu1.scheduled_fmac_hazard_metadata_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_local_fmac_warmup_pairs_elided,
				origin.vu1.scheduled_local_fmac_warmup_pairs_elided)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.scheduled_local_fmac_relative_cycle_pairs,
				origin.vu1.scheduled_local_fmac_relative_cycle_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_entry_blocks,
				origin.vu1.empty_pipeline_entry_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_entry_pairs,
				origin.vu1.empty_pipeline_entry_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_local_fmac_blocks,
				origin.vu1.empty_pipeline_local_fmac_blocks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_local_fmac_pairs,
				origin.vu1.empty_pipeline_local_fmac_pairs)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_test_pipes_elisions,
				origin.vu1.empty_pipeline_test_pipes_elisions)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.empty_pipeline_entry_executions,
				origin.vu1.empty_pipeline_entry_executions)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=vu1_cache prepare_checks=%llu prepare_calls=%llu "
			"quick_hits=%llu program_hits=%llu maps_reused=%llu versions_created=%llu "
			"content_hits=%llu compile_requests=%llu invalidations=%llu compile_failures=%llu "
			"origin_prepare_checks=%llu origin_prepare_calls=%llu origin_quick_hits=%llu "
			"origin_program_hits=%llu origin_maps_reused=%llu origin_versions_created=%llu "
			"origin_content_hits=%llu origin_compile_requests=%llu origin_invalidations=%llu "
			"origin_compile_failures=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.vu1.program_prepare_checks,
				start.vu1.program_prepare_checks)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.program_prepare_calls,
				start.vu1.program_prepare_calls)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.program_quick_cache_hits,
				start.vu1.program_quick_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_version_cache_hits,
				start.vu1.program_version_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_block_maps_reused,
				start.vu1.program_block_maps_reused)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_versions_created,
				start.vu1.program_versions_created)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.content_cache_hits,
				start.vu1.content_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.program_compile_requests,
				start.vu1.program_compile_requests)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.invalidations,
				start.vu1.invalidations)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.compile_failures,
				start.vu1.compile_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_prepare_checks,
				origin.vu1.program_prepare_checks)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_prepare_calls,
				origin.vu1.program_prepare_calls)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_quick_cache_hits,
				origin.vu1.program_quick_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_version_cache_hits,
				origin.vu1.program_version_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_block_maps_reused,
				origin.vu1.program_block_maps_reused)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_versions_created,
				origin.vu1.program_versions_created)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.content_cache_hits,
				origin.vu1.content_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.vu1.program_compile_requests,
				origin.vu1.program_compile_requests)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.invalidations,
				origin.vu1.invalidations)),
			static_cast<unsigned long long>(CounterDelta(end.vu1.compile_failures,
				origin.vu1.compile_failures)));
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
		if (start.gpu_vu_census.valid && end.gpu_vu_census.valid)
		{
			const auto& first = start.gpu_vu_census;
			const auto& last = end.gpu_vu_census;
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=gpu_vu_census jobs=%llu "
				"explicit=%llu resume=%llu completed=%llu still_active=%llu "
				"diagnostic_cpu_us=%llu vu_cycles=%llu blocks=%llu pairs=%llu "
				"pairplan_pairs=%llu invalid_pairplan=%llu "
				"current_fixed_body_pairs=%llu observation_free_pairs=%llu "
				"fully_fixed_body_jobs=%llu fully_observation_free_jobs=%llu "
				"interpreter_pairs=%llu malformed=%llu branches=%llu "
				"indirect_branches=%llu xgkick=%llu ebit=%llu mbit=%llu "
				"enabled_dbit=%llu enabled_tbit=%llu clip=%llu fdiv=%llu efu=%llu "
				"vif_epochs=%llu unpacks=%llu unpack_vectors=%llu "
				"unpack_bytes=%llu serializable_unpacks=%llu fixed_v4_32_unpacks=%llu "
				"serializable_epochs=%llu fixed_v4_32_epochs=%llu "
				"row_updates=%llu column_updates=%llu micro_writes=%llu "
				"micro_write_bytes=%llu data_writes=%llu data_write_bytes=%llu "
				"vi_writes=%llu vf_writes=%llu path1_jobs=%llu path1_bytes=%llu "
				"preflight_candidate_jobs=%llu preflight_candidate_pairs=%llu "
				"program_keys_end=%u dropped_program_keys=%llu "
				"dropped_program_pair_weight=%llu link_mode=natural_block_returns "
				"fixed_semantics=unproven "
				"preflight_role=current_fixed_gxp_replay_candidate_not_admission",
				static_cast<unsigned long long>(window),
				static_cast<unsigned long long>(CounterDelta(
					last.execute_jobs, first.execute_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.explicit_jobs, first.explicit_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.resume_jobs, first.resume_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.completed_jobs, first.completed_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.still_active_jobs, first.still_active_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.cpu_us, first.cpu_us)),
				static_cast<unsigned long long>(CounterDelta(
					last.vu_cycles, first.vu_cycles)),
				static_cast<unsigned long long>(CounterDelta(
					last.executed_blocks, first.executed_blocks)),
				static_cast<unsigned long long>(CounterDelta(
					last.executed_pairs, first.executed_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.pairplan_pairs, first.pairplan_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.invalid_pairplan_pairs, first.invalid_pairplan_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.fixed_shader_body_pairs,
					first.fixed_shader_body_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.observation_free_pairs,
					first.observation_free_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.fully_fixed_body_jobs,
					first.fully_fixed_body_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.fully_observation_free_jobs,
					first.fully_observation_free_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.interpreter_pairs, first.interpreter_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.malformed_block_reports,
					first.malformed_block_reports)),
				static_cast<unsigned long long>(CounterDelta(
					last.branch_pairs, first.branch_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.indirect_branch_pairs,
					first.indirect_branch_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.xgkick_pairs, first.xgkick_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.ebit_pairs, first.ebit_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.mbit_pairs, first.mbit_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.enabled_dbit_pairs, first.enabled_dbit_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.enabled_tbit_pairs, first.enabled_tbit_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.clip_pairs, first.clip_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.fdiv_pairs, first.fdiv_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.efu_pairs, first.efu_pairs)),
				static_cast<unsigned long long>(CounterDelta(
					last.vif_epochs, first.vif_epochs)),
				static_cast<unsigned long long>(CounterDelta(
					last.vif_unpack_commands, first.vif_unpack_commands)),
				static_cast<unsigned long long>(CounterDelta(
					last.vif_unpack_vectors, first.vif_unpack_vectors)),
				static_cast<unsigned long long>(CounterDelta(
					last.vif_unpack_payload_bytes,
					first.vif_unpack_payload_bytes)),
				static_cast<unsigned long long>(CounterDelta(
					last.serializable_unpack_commands,
					first.serializable_unpack_commands)),
				static_cast<unsigned long long>(CounterDelta(
					last.fixed_shader_unpack_commands,
					first.fixed_shader_unpack_commands)),
				static_cast<unsigned long long>(CounterDelta(
					last.fully_serializable_vif_epochs,
					first.fully_serializable_vif_epochs)),
				static_cast<unsigned long long>(CounterDelta(
					last.fully_fixed_shader_vif_epochs,
					first.fully_fixed_shader_vif_epochs)),
				static_cast<unsigned long long>(CounterDelta(
					last.row_state_updates, first.row_state_updates)),
				static_cast<unsigned long long>(CounterDelta(
					last.column_state_updates,
					first.column_state_updates)),
				static_cast<unsigned long long>(CounterDelta(
					last.micro_writes, first.micro_writes)),
				static_cast<unsigned long long>(CounterDelta(
					last.micro_write_bytes, first.micro_write_bytes)),
				static_cast<unsigned long long>(CounterDelta(
					last.data_writes, first.data_writes)),
				static_cast<unsigned long long>(CounterDelta(
					last.data_write_bytes, first.data_write_bytes)),
				static_cast<unsigned long long>(CounterDelta(
					last.vi_state_writes, first.vi_state_writes)),
				static_cast<unsigned long long>(CounterDelta(
					last.vf_state_writes, first.vf_state_writes)),
				static_cast<unsigned long long>(CounterDelta(
					last.path1_jobs, first.path1_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.path1_bytes, first.path1_bytes)),
				static_cast<unsigned long long>(CounterDelta(
					last.preflight_candidate_jobs,
					first.preflight_candidate_jobs)),
				static_cast<unsigned long long>(CounterDelta(
					last.preflight_candidate_pairs,
					first.preflight_candidate_pairs)),
				last.program_count,
				static_cast<unsigned long long>(CounterDelta(
					last.dropped_program_keys, first.dropped_program_keys)),
				static_cast<unsigned long long>(CounterDelta(
					last.dropped_program_pair_weight,
					first.dropped_program_pair_weight)));

			for (size_t reason = 0;
				reason < VitaGpuVuOpportunityCensus::UniversalPreflightRejectionCount;
				reason++)
			{
				const u64 jobs = CounterDelta(
					last.preflight_rejection_jobs[reason],
					first.preflight_rejection_jobs[reason]);
				const u64 pair_weight = CounterDelta(
					last.preflight_rejection_pair_weight[reason],
					first.preflight_rejection_pair_weight[reason]);
				if (jobs == 0 && pair_weight == 0)
					continue;
				output.WriteLn(
					"Vita perf v=1 window=%llu kind=gpu_vu_census_reject "
					"reason=%s jobs=%llu pair_weight=%llu "
					"role=current_fixed_gxp_preflight_not_admission",
					static_cast<unsigned long long>(window),
					VitaGpuVuOpportunityCensus::UniversalPreflightRejectionName(
						static_cast<VitaGpuVuOpportunityCensus::UniversalPreflightRejection>(
							reason)),
					static_cast<unsigned long long>(jobs),
					static_cast<unsigned long long>(pair_weight));
			}

			for (u32 kind = 0;
				kind < VitaGpuVuOpportunityCensus::PairKindCount; kind++)
			{
				const u64 upper = CounterDelta(last.upper_kind_pairs[kind],
					first.upper_kind_pairs[kind]);
				if (upper != 0)
				{
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=gpu_vu_census_upper "
						"op=%u name=%s pairs=%llu",
						static_cast<unsigned long long>(window), kind,
						VitaGpuVuOpportunityCensus::UpperKindName(kind),
						static_cast<unsigned long long>(upper));
				}
				const u64 lower = CounterDelta(last.lower_kind_pairs[kind],
					first.lower_kind_pairs[kind]);
				if (lower != 0)
				{
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=gpu_vu_census_lower "
						"op=%u name=%s pairs=%llu",
						static_cast<unsigned long long>(window), kind,
						VitaGpuVuOpportunityCensus::LowerKindName(kind),
						static_cast<unsigned long long>(lower));
				}
			}

			constexpr std::array<const char*, 3> histogram_names = {{
				"job_pairs", "epoch_unpacks", "epoch_payload_bytes",
			}};
			for (u32 histogram = 0; histogram < histogram_names.size(); histogram++)
			{
				for (u32 bucket = 0;
					bucket < VitaGpuVuOpportunityCensus::SizeBucketCount;
					bucket++)
				{
					const auto& first_counts = histogram == 0 ?
						first.job_pair_counts : histogram == 1 ?
							first.epoch_unpack_counts : first.epoch_payload_counts;
					const auto& last_counts = histogram == 0 ?
						last.job_pair_counts : histogram == 1 ?
							last.epoch_unpack_counts : last.epoch_payload_counts;
					const auto& first_weight = histogram == 0 ?
						first.job_pair_weight : histogram == 1 ?
							first.epoch_unpack_weight : first.epoch_payload_weight;
					const auto& last_weight = histogram == 0 ?
						last.job_pair_weight : histogram == 1 ?
							last.epoch_unpack_weight : last.epoch_payload_weight;
					output.WriteLn(
						"Vita perf v=1 window=%llu kind=gpu_vu_census_hist "
						"metric=%s bucket=%u count=%llu weight=%llu",
						static_cast<unsigned long long>(window),
						histogram_names[histogram], bucket,
						static_cast<unsigned long long>(CounterDelta(
							last_counts[bucket], first_counts[bucket])),
						static_cast<unsigned long long>(CounterDelta(
							last_weight[bucket], first_weight[bucket])));
				}
			}

			struct ProgramDelta
			{
				const VitaGpuVuOpportunityCensus::ProgramStatistics* program;
				VitaGpuVuOpportunityCensus::ProgramStatistics delta;
			};
			std::vector<ProgramDelta> program_deltas;
			for (u32 index = 0; index < last.program_count; index++)
			{
				const auto& current = last.programs[index];
				const VitaGpuVuOpportunityCensus::ProgramStatistics* previous = nullptr;
				for (u32 first_index = 0;
					first_index < first.program_count; first_index++)
				{
					const auto& candidate = first.programs[first_index];
					if (candidate.executed_slice_hash ==
							current.executed_slice_hash &&
						candidate.start_pc == current.start_pc &&
						candidate.resume == current.resume)
					{
						previous = &candidate;
						break;
					}
				}
				const VitaGpuVuOpportunityCensus::ProgramStatistics zero{};
				const auto& before = previous ? *previous : zero;
				ProgramDelta entry{&current, {}};
				entry.delta.jobs = CounterDelta(current.jobs, before.jobs);
				entry.delta.completed_jobs = CounterDelta(
					current.completed_jobs, before.completed_jobs);
				entry.delta.cpu_us = CounterDelta(current.cpu_us, before.cpu_us);
				entry.delta.vu_cycles = CounterDelta(
					current.vu_cycles, before.vu_cycles);
				entry.delta.executed_pairs = CounterDelta(
					current.executed_pairs, before.executed_pairs);
				entry.delta.pairplan_pairs = CounterDelta(
					current.pairplan_pairs, before.pairplan_pairs);
				entry.delta.fixed_shader_body_pairs = CounterDelta(
					current.fixed_shader_body_pairs,
					before.fixed_shader_body_pairs);
				entry.delta.observation_free_pairs = CounterDelta(
					current.observation_free_pairs,
					before.observation_free_pairs);
				entry.delta.xgkick_pairs = CounterDelta(
					current.xgkick_pairs, before.xgkick_pairs);
				entry.delta.path1_bytes = CounterDelta(
					current.path1_bytes, before.path1_bytes);
				entry.delta.unpack_commands = CounterDelta(
					current.unpack_commands, before.unpack_commands);
				entry.delta.unpack_vectors = CounterDelta(
					current.unpack_vectors, before.unpack_vectors);
				entry.delta.unpack_payload_bytes = CounterDelta(
					current.unpack_payload_bytes,
					before.unpack_payload_bytes);
				entry.delta.preflight_candidate_jobs = CounterDelta(
					current.preflight_candidate_jobs,
					before.preflight_candidate_jobs);
				entry.delta.preflight_candidate_pairs = CounterDelta(
					current.preflight_candidate_pairs,
					before.preflight_candidate_pairs);
				if (entry.delta.jobs != 0 || entry.delta.executed_pairs != 0)
					program_deltas.push_back(entry);
			}
			std::sort(program_deltas.begin(), program_deltas.end(),
				[](const ProgramDelta& left, const ProgramDelta& right) {
					return left.delta.executed_pairs > right.delta.executed_pairs;
				});
			const u32 program_output_count = static_cast<u32>(
				std::min<size_t>(program_deltas.size(), 16));
			for (u32 rank = 0; rank < program_output_count; rank++)
			{
				const ProgramDelta& entry = program_deltas[rank];
				output.WriteLn(
					"Vita perf v=1 window=%llu kind=gpu_vu_census_program "
					"rank=%u slice_hash=0x%016llx start_pc=0x%04x resume=%u "
					"jobs=%llu completed=%llu diagnostic_cpu_us=%llu "
					"vu_cycles=%llu pairs=%llu pairplan_pairs=%llu "
					"current_fixed_body_pairs=%llu observation_free_pairs=%llu "
					"distinct_pairs=%llu xgkick=%llu path1_bytes=%llu "
					"unpacks=%llu unpack_vectors=%llu unpack_bytes=%llu "
					"preflight_candidate_jobs=%llu preflight_candidate_pairs=%llu "
					"preflight_rejection_mask=0x%016llx "
					"identity_role=diagnostic_only",
					static_cast<unsigned long long>(window), rank + 1,
					static_cast<unsigned long long>(
						entry.program->executed_slice_hash),
					entry.program->start_pc, entry.program->resume ? 1u : 0u,
					static_cast<unsigned long long>(entry.delta.jobs),
					static_cast<unsigned long long>(entry.delta.completed_jobs),
					static_cast<unsigned long long>(entry.delta.cpu_us),
					static_cast<unsigned long long>(entry.delta.vu_cycles),
					static_cast<unsigned long long>(entry.delta.executed_pairs),
					static_cast<unsigned long long>(entry.delta.pairplan_pairs),
					static_cast<unsigned long long>(
						entry.delta.fixed_shader_body_pairs),
					static_cast<unsigned long long>(
						entry.delta.observation_free_pairs),
					static_cast<unsigned long long>(
						entry.program->distinct_pairs),
					static_cast<unsigned long long>(entry.delta.xgkick_pairs),
					static_cast<unsigned long long>(entry.delta.path1_bytes),
					static_cast<unsigned long long>(entry.delta.unpack_commands),
					static_cast<unsigned long long>(entry.delta.unpack_vectors),
					static_cast<unsigned long long>(
						entry.delta.unpack_payload_bytes),
					static_cast<unsigned long long>(
						entry.delta.preflight_candidate_jobs),
					static_cast<unsigned long long>(
						entry.delta.preflight_candidate_pairs),
					static_cast<unsigned long long>(
						entry.program->preflight_rejection_mask));
			}
		}
#endif
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=mtvu submissions=%llu queue_words=%llu "
			"execute_jobs=%llu waits=%llu ring_waits=%llu ring_spins=%llu compile_barriers=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.mtvu.queue_submissions,
				start.mtvu.queue_submissions)),
			static_cast<unsigned long long>(CounterDelta(end.mtvu.queue_words,
				start.mtvu.queue_words)),
			static_cast<unsigned long long>(CounterDelta(end.mtvu.execute_enqueues,
				start.mtvu.execute_enqueues)),
			static_cast<unsigned long long>(CounterDelta(end.mtvu.wait_calls,
				start.mtvu.wait_calls)),
			static_cast<unsigned long long>(CounterDelta(end.mtvu.ring_waits,
				start.mtvu.ring_waits)),
			static_cast<unsigned long long>(CounterDelta(end.mtvu.ring_wait_spins,
				start.mtvu.ring_wait_spins)),
			static_cast<unsigned long long>(CounterDelta(end.mtvu.compile_barriers,
				start.mtvu.compile_barriers)));
		output.WriteLn(
				"Vita perf v=4 window=%llu kind=gpu_vu_input captures=%llu bytes=%llu "
				"derived_captures=%llu derived_bytes=%llu "
			"publish_batches=%llu published_bytes=%llu "
			"fetch_guard_scans=%llu fetch_guard_failures=%llu "
			"logical_high_water_bytes=%llu "
			"capture_bypasses=%llu capture_bypass_bytes=%llu "
			"fallbacks=%llu slot_reuses=%llu ring_waits=%llu ring_spins=%llu "
			"deferred=%llu affine_merges=%llu replayed=%llu "
			"retain_generation_rollbacks=%llu retain_range_rollbacks=%llu "
			"retain_overflow_rejections=%llu "
			"release_generation_mismatches=%llu "
			"release_underflow_rejections=%llu generation_exhaustions=%llu "
			"live_start=%llu live_end=%llu peak_live=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.captures, start.gpu_vu_input.captures)),
				static_cast<unsigned long long>(CounterDelta(
					end.gpu_vu_input.captured_bytes,
					start.gpu_vu_input.captured_bytes)),
				static_cast<unsigned long long>(CounterDelta(
					end.gpu_vu_input.derived_captures,
					start.gpu_vu_input.derived_captures)),
				static_cast<unsigned long long>(CounterDelta(
					end.gpu_vu_input.derived_bytes,
					start.gpu_vu_input.derived_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.publication_batches,
				start.gpu_vu_input.publication_batches)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.published_bytes,
				start.gpu_vu_input.published_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.fetch_guard_scans,
				start.gpu_vu_input.fetch_guard_scans)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.fetch_guard_failures,
				start.gpu_vu_input.fetch_guard_failures)),
			static_cast<unsigned long long>(
				end.gpu_vu_input.logical_high_water_bytes),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.capture_bypasses,
				start.gpu_vu_input.capture_bypasses)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.capture_bypass_bytes,
				start.gpu_vu_input.capture_bypass_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.capture_fallbacks,
				start.gpu_vu_input.capture_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.slot_reuses,
				start.gpu_vu_input.slot_reuses)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.ring_waits,
				start.gpu_vu_input.ring_waits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.ring_wait_spins,
				start.gpu_vu_input.ring_wait_spins)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.deferred_unpacks,
				start.gpu_vu_input.deferred_unpacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.affine_span_merges,
				start.gpu_vu_input.affine_span_merges)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.replayed_unpacks,
				start.gpu_vu_input.replayed_unpacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.retain_generation_rollbacks,
				start.gpu_vu_input.retain_generation_rollbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.retain_range_rollbacks,
				start.gpu_vu_input.retain_range_rollbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.retain_overflow_rejections,
				start.gpu_vu_input.retain_overflow_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.release_generation_mismatches,
				start.gpu_vu_input.release_generation_mismatches)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.release_underflow_rejections,
				start.gpu_vu_input.release_underflow_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.generation_exhaustions,
				start.gpu_vu_input.generation_exhaustions)),
			static_cast<unsigned long long>(
				start.gpu_vu_input.live_references),
			static_cast<unsigned long long>(
				end.gpu_vu_input.live_references),
			static_cast<unsigned long long>(
				end.gpu_vu_input.peak_live_references));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=gpu_vu_input_binding "
				"hybrid_attempts=%llu hybrid_hits=%llu hybrid_fallbacks=%llu "
					"raw_bytes_retained=%llu derived_bytes=%llu copy_bytes_avoided=%llu "
					"canonical_bindings=%llu canonical_bytes=%llu "
					"persistent_raw_bindings=%llu persistent_raw_bytes=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.hybrid_input_attempts,
				start.gpu_vu_direct.hybrid_input_attempts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.hybrid_input_hits,
				start.gpu_vu_direct.hybrid_input_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.hybrid_input_fallbacks,
				start.gpu_vu_direct.hybrid_input_fallbacks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.hybrid_raw_bytes_retained,
				start.gpu_vu_direct.hybrid_raw_bytes_retained)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.hybrid_derived_bytes,
				start.gpu_vu_direct.hybrid_derived_bytes)),
				static_cast<unsigned long long>(CounterDelta(
					end.gpu_vu_direct.hybrid_copy_bytes_avoided,
					start.gpu_vu_direct.hybrid_copy_bytes_avoided)),
				static_cast<unsigned long long>(CounterDelta(
					end.gpu_vu_direct.canonical_input_bindings,
					start.gpu_vu_direct.canonical_input_bindings)),
					static_cast<unsigned long long>(CounterDelta(
						end.gpu_vu_direct.canonical_input_bytes,
						start.gpu_vu_direct.canonical_input_bytes)),
					static_cast<unsigned long long>(CounterDelta(
						end.gpu_vu_direct.persistent_raw_input_bindings,
						start.gpu_vu_direct.persistent_raw_input_bindings)),
					static_cast<unsigned long long>(CounterDelta(
						end.gpu_vu_direct.persistent_raw_input_bytes,
						start.gpu_vu_direct.persistent_raw_input_bytes)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_analysis_cache "
			"prepare_requests=%llu prepare_requests_total=%llu "
			"prepared=%llu prepared_total=%llu "
			"prepare_hits=%llu prepare_hits_total=%llu "
			"prepare_us=%llu prepare_us_total=%llu prepare_max_us=%llu "
			"hash_bytes=%llu hash_bytes_total=%llu "
			"compare_bytes=%llu compare_bytes_total=%llu "
			"copy_bytes=%llu copy_bytes_total=%llu "
			"analysis_builds=%llu analysis_builds_total=%llu "
			"analysis_us=%llu analysis_us_total=%llu analysis_max_us=%llu "
			"proof_us=%llu proof_us_total=%llu proof_max_us=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.preparation_requests,
				start.gpu_vu_direct.preparation_requests)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.preparation_requests),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.prepared_programs,
				start.gpu_vu_direct.prepared_programs)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.prepared_programs),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.preparation_cache_hits,
				start.gpu_vu_direct.preparation_cache_hits)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.preparation_cache_hits),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.preparation_wall_us,
				start.gpu_vu_direct.preparation_wall_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.preparation_wall_us),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.preparation_wall_us_max),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.source_hash_bytes,
				start.gpu_vu_direct.source_hash_bytes)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.source_hash_bytes),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.source_compare_bytes,
				start.gpu_vu_direct.source_compare_bytes)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.source_compare_bytes),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.source_copy_bytes,
				start.gpu_vu_direct.source_copy_bytes)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.source_copy_bytes),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.analysis_builds,
				start.gpu_vu_direct.analysis_builds)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.analysis_builds),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.analysis_wall_us,
				start.gpu_vu_direct.analysis_wall_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.analysis_wall_us),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.analysis_wall_us_max),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.candidate_proof_wall_us,
				start.gpu_vu_direct.candidate_proof_wall_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.candidate_proof_wall_us),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.candidate_proof_wall_us_max));
		output.WriteLn(
			"Vita perf v=2 window=%llu kind=gpu_vu_program "
			"prepare_requests=%llu prepared=%llu prepare_hits=%llu "
			"evictions=%llu prepare_us=%llu prepare_max_us=%llu "
			"source_hash_bytes=%llu source_compare_bytes=%llu "
			"source_copy_bytes=%llu analysis_builds=%llu analysis_us=%llu "
			"analysis_max_us=%llu candidate_proof_us=%llu "
			"candidate_proof_max_us=%llu analysis_failures=%llu "
			"without_parallel=%llu candidates=%llu prime_attempts=%llu "
			"prime_hits=%llu stale_tokens=%llu gif_address_failures=%llu "
			"gif_contract_rejections=%llu generated_roots=%llu "
			"compiler_requests=%llu compiler_request_retries=%llu "
			"continuation_shared=%llu continuation_general=%llu "
			"registry_requests=%llu unavailable=%llu registry_hits=%llu "
			"registry_misses=%llu queue_retries=%llu compile_successes=%llu "
			"compile_failures=%llu ready=%llu failed=%llu "
			"compiler_safety_rejections=%llu "
			"compiler_submit_attempts=%llu compiler_submitted=%llu "
			"compiler_reject_state=%llu compiler_reject_source=%llu "
			"compiler_reject_capacity=%llu compiler_reject_duplicate=%llu "
			"compiler_dequeued=%llu compiler_starts=%llu "
			"compiler_completions=%llu compiler_successes=%llu "
			"compiler_failures=%llu compiler_polled=%llu "
			"compiler_dropped=%llu compiler_time_us=%llu "
			"compiler_longest_us=%llu compiler_worker_cpu_us=%llu "
			"compiler_priority_before=%08x compiler_priority_after=%08x "
			"compiler_priority_result=%08x compiler_affinity_result=%08x "
			"compiler_pending_end=%llu "
			"compiler_results_end=%llu compiler_in_flight_end=%llu "
			"compiler_active_end=%llu compiler_invalid_output=%llu "
			"compiler_resource_rejected=%llu "
			"compiler_diag_truncated=%llu compiler_arena_capacity=%llu "
			"compiler_arena_peak=%llu compiler_arena_current=%llu "
			"compiler_arena_resets=%llu "
			"compiler_arena_guard_failures=%llu "
			"planner_submit_attempts=%llu planner_submitted=%llu "
			"planner_reject_state=%llu planner_reject_capacity=%llu "
			"planner_coalesced=%llu planner_starts=%llu "
			"planner_completions=%llu planner_time_us=%llu "
			"planner_longest_us=%llu planner_pending_end=%llu "
			"planner_active_end=%llu "
			"compiler_cache_hits=%llu compiler_cache_misses=%llu "
			"compiler_cache_writes=%llu compiler_cache_write_failures=%llu "
			"compiler_cache_invalid=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.preparation_requests,
				start.gpu_vu_direct.preparation_requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.prepared_programs,
				start.gpu_vu_direct.prepared_programs)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.preparation_cache_hits,
				start.gpu_vu_direct.preparation_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.preparation_evictions,
				start.gpu_vu_direct.preparation_evictions)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.preparation_wall_us,
				start.gpu_vu_direct.preparation_wall_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.preparation_wall_us_max),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.source_hash_bytes,
				start.gpu_vu_direct.source_hash_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.source_compare_bytes,
				start.gpu_vu_direct.source_compare_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.source_copy_bytes,
				start.gpu_vu_direct.source_copy_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.analysis_builds,
				start.gpu_vu_direct.analysis_builds)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.analysis_wall_us,
				start.gpu_vu_direct.analysis_wall_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.analysis_wall_us_max),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.candidate_proof_wall_us,
				start.gpu_vu_direct.candidate_proof_wall_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_direct.candidate_proof_wall_us_max),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.analysis_failures,
				start.gpu_vu_direct.analysis_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.programs_without_parallel_candidate,
				start.gpu_vu_direct.programs_without_parallel_candidate)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.parallel_candidates,
				start.gpu_vu_direct.parallel_candidates)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.prime_attempts,
				start.gpu_vu_direct.prime_attempts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.prime_cache_hits,
				start.gpu_vu_direct.prime_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.stale_tokens,
				start.gpu_vu_direct.stale_tokens)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.gif_address_failures,
				start.gpu_vu_direct.gif_address_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.gif_contract_rejections,
				start.gpu_vu_direct.gif_contract_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.generated_roots,
				start.gpu_vu_direct.generated_roots)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.compiler_requests,
				start.gpu_vu_direct.compiler_requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.compiler_request_retries,
				start.gpu_vu_direct.compiler_request_retries)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.shared_continuation_builds,
				start.gpu_vu_direct.shared_continuation_builds)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_direct.general_continuation_builds,
				start.gpu_vu_direct.general_continuation_builds)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.requests,
				start.gpu_vu_programs.requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.unavailable_requests,
				start.gpu_vu_programs.unavailable_requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.cache_hits,
				start.gpu_vu_programs.cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.cache_misses,
				start.gpu_vu_programs.cache_misses)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler_queue_retries,
				start.gpu_vu_programs.compiler_queue_retries)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compile_successes,
				start.gpu_vu_programs.compile_successes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compile_failures,
				start.gpu_vu_programs.compile_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.ready_programs,
				start.gpu_vu_programs.ready_programs)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.failed_programs,
				start.gpu_vu_programs.failed_programs)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler_safety_rejections,
				start.gpu_vu_programs.compiler_safety_rejections)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.submission_attempts,
				start.gpu_vu_programs.compiler.submission_attempts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.accepted_submissions,
				start.gpu_vu_programs.compiler.accepted_submissions)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.rejected_state,
				start.gpu_vu_programs.compiler.rejected_state)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.rejected_source,
				start.gpu_vu_programs.compiler.rejected_source)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.rejected_capacity,
				start.gpu_vu_programs.compiler.rejected_capacity)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.rejected_duplicate,
				start.gpu_vu_programs.compiler.rejected_duplicate)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.dequeued_requests,
				start.gpu_vu_programs.compiler.dequeued_requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.compile_starts,
				start.gpu_vu_programs.compiler.compile_starts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.compile_completions,
				start.gpu_vu_programs.compiler.compile_completions)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.compile_successes,
				start.gpu_vu_programs.compiler.compile_successes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.compile_failures,
				start.gpu_vu_programs.compiler.compile_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.polled_results,
				start.gpu_vu_programs.compiler.polled_results)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.dropped_results,
				start.gpu_vu_programs.compiler.dropped_results)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.total_compile_us,
				start.gpu_vu_programs.compiler.total_compile_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.longest_compile_us),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.worker_cpu_us,
				start.gpu_vu_programs.compiler.worker_cpu_us)),
			static_cast<u32>(
				end.gpu_vu_programs.compiler.worker_priority_before),
			static_cast<u32>(
				end.gpu_vu_programs.compiler.worker_priority_after),
			static_cast<u32>(
				end.gpu_vu_programs.compiler.worker_priority_result),
			static_cast<u32>(
				end.gpu_vu_programs.compiler.worker_affinity_result),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.pending_requests),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.completed_results),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.in_flight_requests),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.active_compiles),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.invalid_outputs,
				start.gpu_vu_programs.compiler.invalid_outputs)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.resource_rejected_outputs,
				start.gpu_vu_programs.compiler.resource_rejected_outputs)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.truncated_diagnostics,
				start.gpu_vu_programs.compiler.truncated_diagnostics)),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.private_arena_capacity),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.private_arena_peak),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.private_arena_current),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.private_arena_resets,
				start.gpu_vu_programs.compiler.private_arena_resets)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.private_arena_guard_failures,
				start.gpu_vu_programs.compiler.private_arena_guard_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.planning_submission_attempts,
				start.gpu_vu_programs.compiler.planning_submission_attempts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.accepted_planning_tasks,
				start.gpu_vu_programs.compiler.accepted_planning_tasks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.rejected_planning_state,
				start.gpu_vu_programs.compiler.rejected_planning_state)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.rejected_planning_capacity,
				start.gpu_vu_programs.compiler.rejected_planning_capacity)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.coalesced_planning_tasks,
				start.gpu_vu_programs.compiler.coalesced_planning_tasks)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.planning_starts,
				start.gpu_vu_programs.compiler.planning_starts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.planning_completions,
				start.gpu_vu_programs.compiler.planning_completions)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.total_planning_us,
				start.gpu_vu_programs.compiler.total_planning_us)),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.longest_planning_us),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.pending_planning_tasks),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.active_planning_tasks),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.persistent_cache_hits,
				start.gpu_vu_programs.compiler.persistent_cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.persistent_cache_misses,
				start.gpu_vu_programs.compiler.persistent_cache_misses)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.persistent_cache_writes,
				start.gpu_vu_programs.compiler.persistent_cache_writes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.persistent_cache_write_failures,
				start.gpu_vu_programs.compiler.persistent_cache_write_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.persistent_cache_invalid,
				start.gpu_vu_programs.compiler.persistent_cache_invalid)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_compiler_recovery "
			"allocator_failures=%llu first_failed_bytes=%llu generations=%llu "
			"attempts=%llu ready=%llu unload_failures=%llu "
			"cancelled_requests=%llu cancelled_plans=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.allocator_failures,
				start.gpu_vu_programs.compiler.allocator_failures)),
			static_cast<unsigned long long>(end.gpu_vu_programs.compiler.last_failed_allocation_bytes),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.worker_generations,
				start.gpu_vu_programs.compiler.worker_generations)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.recovery_attempts,
				start.gpu_vu_programs.compiler.recovery_attempts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.recovery_successes,
				start.gpu_vu_programs.compiler.recovery_successes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.module_unload_failures,
				start.gpu_vu_programs.compiler.module_unload_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.cancelled_requests,
				start.gpu_vu_programs.compiler.cancelled_requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.cancelled_planning_tasks,
				start.gpu_vu_programs.compiler.cancelled_planning_tasks)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_region_bundle "
			"requests=%llu cache_hits=%llu generated_bundles=%llu "
			"generation_failures=%llu generated_modules=%llu "
			"submit_attempts=%llu submissions=%llu submit_retries=%llu "
			"compiler_safety_rejections=%llu "
			"cached_end=%u ready_end=%u failed_end=%u pending_end=%u "
			"pending_source_bytes_end=%u admission=closed",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.requests,
				start.gpu_vu_region_bundles.requests)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.cache_hits,
				start.gpu_vu_region_bundles.cache_hits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.generated_bundles,
				start.gpu_vu_region_bundles.generated_bundles)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.generation_failures,
				start.gpu_vu_region_bundles.generation_failures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.generated_modules,
				start.gpu_vu_region_bundles.generated_modules)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.module_submit_attempts,
				start.gpu_vu_region_bundles.module_submit_attempts)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.module_submissions,
				start.gpu_vu_region_bundles.module_submissions)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.module_submit_retries,
				start.gpu_vu_region_bundles.module_submit_retries)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_region_bundles.module_compiler_safety_rejections,
				start.gpu_vu_region_bundles.module_compiler_safety_rejections)),
			end.gpu_vu_region_bundles.cached_bundles,
			end.gpu_vu_region_bundles.ready_bundles,
			end.gpu_vu_region_bundles.failed_bundles,
			end.gpu_vu_region_bundles.pending_modules,
			end.gpu_vu_region_bundles.pending_source_bytes);
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_draw queued=%llu consumed=%llu "
			"rejected=%llu parallel=%llu serial=%llu interpreter=%llu "
			"fused_vertices=%llu fused_primitives=%llu tfx_exports=%llu "
			"raw_exports=%llu retirement_batches=%llu retired=%llu "
			"ring_waits=%llu notification_waits=%llu "
			"descriptor_pool_waits=%llu descriptor_pool_start=%llu "
			"descriptor_pool_end=%llu descriptor_pool_peak=%llu "
			"descriptor_pool_capacity=%u descriptor_size=%u "
			"uniform_pool_waits=%llu uniform_pool_start=%llu "
			"uniform_pool_end=%llu uniform_pool_peak=%llu "
			"uniform_pool_capacity=%u uniform_block_size=%u "
			"live_start=%llu live_end=%llu peak_live=%llu "
			"cpu_vu1=%llu cpu_path1_packets=%llu cpu_path1_bytes=%llu "
			"encoded_objects=%llu reject_disconnected=%llu "
			"reject_no_token=%llu reject_build=%llu reject_queue=%llu "
			"reject_spans=%llu reject_not_ready=%llu reject_tag=%llu "
			"reject_seed=%llu reject_geometry=%llu reject_input=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.queued, start.gpu_vu_draw.queued)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.consumed, start.gpu_vu_draw.consumed)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.rejected, start.gpu_vu_draw.rejected)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.generated_parallel_invocations,
				start.gpu_vu_draw.generated_parallel_invocations)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.generated_serial_invocations,
				start.gpu_vu_draw.generated_serial_invocations)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.interpreter_invocations,
				start.gpu_vu_draw.interpreter_invocations)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.fused_vertices,
				start.gpu_vu_draw.fused_vertices)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.fused_primitives,
				start.gpu_vu_draw.fused_primitives)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.tfx_vertex_exports,
				start.gpu_vu_draw.tfx_vertex_exports)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.raw_path1_exports,
				start.gpu_vu_draw.raw_path1_exports)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.retirement_batches,
				start.gpu_vu_draw.retirement_batches)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.retired_draws,
				start.gpu_vu_draw.retired_draws)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.retirement_ring_waits,
				start.gpu_vu_draw.retirement_ring_waits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.notification_waits,
				start.gpu_vu_draw.notification_waits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.descriptor_pool_waits,
				start.gpu_vu_draw.descriptor_pool_waits)),
			static_cast<unsigned long long>(
				start.gpu_vu_draw.descriptor_pool_in_use),
			static_cast<unsigned long long>(
				end.gpu_vu_draw.descriptor_pool_in_use),
			static_cast<unsigned long long>(
				end.gpu_vu_draw.peak_descriptor_pool_in_use),
			end.gpu_vu_draw.descriptor_pool_capacity,
			end.gpu_vu_draw.descriptor_size,
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.uniform_pool_waits,
				start.gpu_vu_draw.uniform_pool_waits)),
			static_cast<unsigned long long>(
				start.gpu_vu_draw.uniform_pool_in_use),
			static_cast<unsigned long long>(
				end.gpu_vu_draw.uniform_pool_in_use),
			static_cast<unsigned long long>(
				end.gpu_vu_draw.peak_uniform_pool_in_use),
			end.gpu_vu_draw.uniform_pool_capacity,
			end.gpu_vu_draw.uniform_block_size,
			static_cast<unsigned long long>(start.gpu_vu_draw.live_draws),
			static_cast<unsigned long long>(end.gpu_vu_draw.live_draws),
			static_cast<unsigned long long>(end.gpu_vu_draw.peak_live_draws),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.cpu_vu1_executions,
				start.gpu_vu_draw.cpu_vu1_executions)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.cpu_path1_packets,
				start.gpu_vu_draw.cpu_path1_packets)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.cpu_path1_bytes,
				start.gpu_vu_draw.cpu_path1_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.encoded_objects,
				start.gpu_vu_draw.encoded_objects)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::Disconnected)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::Disconnected)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::NoProgramToken)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::NoProgramToken)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::BuildFailed)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::BuildFailed)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::QueueRejected)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::QueueRejected)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::NoInputSpans)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::NoInputSpans)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::NoReadyCandidate)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::NoReadyCandidate)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::TagMismatch)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::TagMismatch)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::SeedUnstable)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::SeedUnstable)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::GeometryFailed)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::GeometryFailed)])),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::InputResolveFailed)],
				start.gpu_vu_draw.admission_failures[
					static_cast<size_t>(
						VitaGpuVu::AdmissionFailure::InputResolveFailed)])));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gs submitted=%llu submitted_words=%llu "
			"processed=%llu packets=%llu packet_bytes=%llu mtvu_packets=%llu "
			"mtvu_packet_bytes=%llu waits=%llu wait_spins=%llu ring_spins=%llu "
			"path1_completion_ring_waits=%llu path1_completion_deferrals=%llu "
			"vsync_wait_calls=%llu vsync_wait_wall_us=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.gs_producer.submissions,
				start.gs_producer.submissions)),
			static_cast<unsigned long long>(CounterDelta(end.gs_producer.ring_words,
				start.gs_producer.ring_words)),
			static_cast<unsigned long long>(CounterDelta(end.gs_worker.commands,
				start.gs_worker.commands)),
			static_cast<unsigned long long>(CounterDelta(end.gs_worker.gs_packets,
				start.gs_worker.gs_packets)),
			static_cast<unsigned long long>(CounterDelta(end.gs_worker.gs_packet_bytes,
				start.gs_worker.gs_packet_bytes)),
			static_cast<unsigned long long>(CounterDelta(end.gs_worker.mtvu_packets,
				start.gs_worker.mtvu_packets)),
			static_cast<unsigned long long>(CounterDelta(end.gs_worker.mtvu_packet_bytes,
				start.gs_worker.mtvu_packet_bytes)),
			static_cast<unsigned long long>(CounterDelta(end.gs_producer.wait_calls,
				start.gs_producer.wait_calls)),
			static_cast<unsigned long long>(CounterDelta(end.gs_producer.wait_spins,
				start.gs_producer.wait_spins)),
			static_cast<unsigned long long>(CounterDelta(end.gs_producer.ring_spins,
				start.gs_producer.ring_spins)),
			static_cast<unsigned long long>(CounterDelta(
				end.gs_producer.mtvu_path1_completion_ring_waits,
				start.gs_producer.mtvu_path1_completion_ring_waits)),
			static_cast<unsigned long long>(CounterDelta(
				end.gs_worker.mtvu_path1_completion_deferrals,
				start.gs_worker.mtvu_path1_completion_deferrals)),
			static_cast<unsigned long long>(CounterDelta(
				end.gs_producer.vsync_wait_calls, start.gs_producer.vsync_wait_calls)),
			static_cast<unsigned long long>(CounterDelta(
				end.gs_producer.vsync_wait_wall_us, start.gs_producer.vsync_wait_wall_us)));
		for (size_t i = 0; i < end.gxm.tfx_programs.size(); i++)
		{
			const auto& last = end.gxm.tfx_programs[i];
			const auto& first = start.gxm.tfx_programs[i];
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=gxm_tfx_program program=%s "
				"draws=%llu indices=%llu draw_rect_pixels=%llu "
				"no_tests_draws=%llu no_tests_rect_pixels=%llu gpu_time=0",
				static_cast<unsigned long long>(window), VitaGxmTfxProgramNames[i],
				static_cast<unsigned long long>(CounterDelta(last.draws, first.draws)),
				static_cast<unsigned long long>(CounterDelta(last.indices, first.indices)),
				static_cast<unsigned long long>(CounterDelta(last.draw_rect_pixels, first.draw_rect_pixels)),
				static_cast<unsigned long long>(CounterDelta(last.no_tests_draws, first.no_tests_draws)),
				static_cast<unsigned long long>(CounterDelta(last.no_tests_rect_pixels, first.no_tests_rect_pixels)));
		}
		for (size_t i = 0; i < end.gxm.scene_transitions.size(); i++)
		{
			const auto& last = end.gxm.scene_transitions[i];
			const auto& first = start.gxm.scene_transitions[i];
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=gxm_scene_transition bits=%u "
				"transitions=%llu clear_calls=%llu clear_followups=%llu clear_only_followups=%llu gpu_time=0",
				static_cast<unsigned long long>(window), static_cast<unsigned>(i),
				static_cast<unsigned long long>(CounterDelta(last.transitions, first.transitions)),
				static_cast<unsigned long long>(CounterDelta(last.clear_calls, first.clear_calls)),
				static_cast<unsigned long long>(CounterDelta(last.clear_followups, first.clear_followups)),
				static_cast<unsigned long long>(CounterDelta(last.clear_only_followups, first.clear_only_followups)));
		}
		for (size_t i = 0; i < end.gxm.scene_contents.size(); i++)
		{
			const auto& last = end.gxm.scene_contents[i];
			const auto& first = start.gxm.scene_contents[i];
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=gxm_scene_content bits=%u "
				"scenes=%llu end_scene_wall_us=%llu gpu_time=0",
				static_cast<unsigned long long>(window), static_cast<unsigned>(i),
				static_cast<unsigned long long>(CounterDelta(last.scenes, first.scenes)),
				static_cast<unsigned long long>(CounterDelta(last.end_scene_wall_us, first.end_scene_wall_us)));
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=gxm_unsupported_pass bits=%u draws=%llu",
				static_cast<unsigned long long>(window), static_cast<unsigned>(i),
				static_cast<unsigned long long>(CounterDelta(
					end.gxm.unsupported_passes[i], start.gxm.unsupported_passes[i])));
		}
		for (size_t i = 0; i < end.gxm.host_calls.size(); i++)
		{
			const auto& last = end.gxm.host_calls[i];
			const auto& first = start.gxm.host_calls[i];
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=gxm_host_call call=%s "
				"calls=%llu wall_us=%llu max_lifetime_us=%llu gpu_time=0",
				static_cast<unsigned long long>(window), VitaGxmHostCallNames[i],
				static_cast<unsigned long long>(CounterDelta(last.calls, first.calls)),
				static_cast<unsigned long long>(CounterDelta(last.wall_us, first.wall_us)),
				static_cast<unsigned long long>(last.max_lifetime_us));
		}
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gxm draws=%llu indices=%llu "
			"vertex_bytes=%llu index_bytes=%llu texture_uploads=%llu "
			"texture_upload_bytes=%llu readbacks=%llu readback_bytes=%llu "
			"store_acquires=%llu store_new=%llu store_switches=%llu "
			"store_scenes=%llu store_loads=%llu store_materializes=%llu "
			"store_failures=%llu "
			"psm24_draws=%llu "
			"rejected_tfx=%llu reject_features=0x%016llx "
			"reject_ps_lo=0x%016llx reject_ps_hi=0x%016llx "
			"reject_blend=0x%08x reject_vs=0x%02x reject_sampler=0x%02x "
			"reject_depth=0x%02x reject_colormask=0x%02x reject_topology=%u",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.gxm.draw_calls,
				start.gxm.draw_calls)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.draw_indices,
				start.gxm.draw_indices)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.vertex_upload_bytes,
				start.gxm.vertex_upload_bytes)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.index_upload_bytes,
				start.gxm.index_upload_bytes)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.texture_uploads,
				start.gxm.texture_uploads)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.texture_upload_bytes,
				start.gxm.texture_upload_bytes)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.texture_readbacks,
				start.gxm.texture_readbacks)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.texture_readback_bytes,
				start.gxm.texture_readback_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_store_acquisitions,
				start.gxm.render_store_acquisitions)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_store_new_residencies,
				start.gxm.render_store_new_residencies)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_store_resident_switches,
				start.gxm.render_store_resident_switches)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_store_physical_scenes,
				start.gxm.render_store_physical_scenes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_store_loads,
				start.gxm.render_store_loads)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_store_materializations,
				start.gxm.render_store_materializations)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_store_failures,
				start.gxm.render_store_failures)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.psm24_draws,
				start.gxm.psm24_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.rejected_tfx_draws,
				start.gxm.rejected_tfx_draws)),
			static_cast<unsigned long long>(
				end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
					end.gxm.last_rejected_tfx_features : 0),
			static_cast<unsigned long long>(
				end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
					end.gxm.last_rejected_tfx_ps_lo : 0),
			static_cast<unsigned long long>(
				end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
					end.gxm.last_rejected_tfx_ps_hi : 0),
			end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
				end.gxm.last_rejected_tfx_blend : 0,
			end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
				end.gxm.last_rejected_tfx_vs : 0,
			end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
				end.gxm.last_rejected_tfx_sampler : 0,
			end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
				end.gxm.last_rejected_tfx_depth : 0,
			end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
				end.gxm.last_rejected_tfx_colormask : 0,
			static_cast<u32>(
				end.gxm.rejected_tfx_draws != start.gxm.rejected_tfx_draws ?
					end.gxm.last_rejected_tfx_topology : 0));
		const bool feedback_seen =
			end.gxm.feedback_rt_draws != start.gxm.feedback_rt_draws ||
			end.gxm.feedback_depth_draws != start.gxm.feedback_depth_draws ||
			end.gxm.rt_hazard_draws != start.gxm.rt_hazard_draws ||
			end.gxm.depth_hazard_draws != start.gxm.depth_hazard_draws;
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gxm_semantics tfx=%llu textured=%llu "
			"rt_source=%llu depth_source=%llu rt_hazard=%llu depth_hazard=%llu "
			"feedback_rt=%llu feedback_depth=%llu snapshots=%llu snapshot_bytes=%llu "
			"sw_blend=%llu fixed_blend=%llu alpha_test=%llu partial_mask=%llu "
			"device_rejects=%llu reject_hash=0x%08x "
			"feedback_ps_lo=0x%016llx feedback_ps_hi=0x%016llx "
			"feedback_blend=0x%08x feedback_vs=0x%02x "
			"feedback_sampler=0x%02x feedback_depth_state=0x%02x "
			"feedback_colormask=0x%02x feedback_topology=%u feedback_hazard=%u",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.gxm.tfx_draws,
				start.gxm.tfx_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.textured_tfx_draws,
				start.gxm.textured_tfx_draws)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.render_target_source_draws,
				start.gxm.render_target_source_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.depth_source_draws,
				start.gxm.depth_source_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.rt_hazard_draws,
				start.gxm.rt_hazard_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.depth_hazard_draws,
				start.gxm.depth_hazard_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.feedback_rt_draws,
				start.gxm.feedback_rt_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.feedback_depth_draws,
				start.gxm.feedback_depth_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.feedback_snapshots,
				start.gxm.feedback_snapshots)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.feedback_snapshot_bytes,
				start.gxm.feedback_snapshot_bytes)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.software_blend_draws,
				start.gxm.software_blend_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.fixed_blend_draws,
				start.gxm.fixed_blend_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.alpha_test_draws,
				start.gxm.alpha_test_draws)),
			static_cast<unsigned long long>(CounterDelta(
				end.gxm.partial_color_mask_draws,
				start.gxm.partial_color_mask_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.device_rejects,
				start.gxm.device_rejects)),
			end.gxm.device_rejects != start.gxm.device_rejects ?
				end.gxm.last_device_reject_hash : 0,
			static_cast<unsigned long long>(feedback_seen ?
				end.gxm.last_feedback_ps_lo : 0),
			static_cast<unsigned long long>(feedback_seen ?
				end.gxm.last_feedback_ps_hi : 0),
			feedback_seen ? end.gxm.last_feedback_blend : 0,
			feedback_seen ? end.gxm.last_feedback_vs : 0,
			feedback_seen ? end.gxm.last_feedback_sampler : 0,
			feedback_seen ? end.gxm.last_feedback_depth : 0,
			feedback_seen ? end.gxm.last_feedback_colormask : 0,
			static_cast<u32>(feedback_seen ?
				end.gxm.last_feedback_topology : 0),
			static_cast<u32>(feedback_seen ?
				end.gxm.last_feedback_hazard : 0));
		const bool merge_seen = end.gxm.merge_calls != start.gxm.merge_calls;
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gxm_output merges=%llu rc1_draws=%llu "
			"rc2_draws=%llu presents=%llu interlace=%llu pmode=0x%016llx "
			"extbuf=0x%016llx background=0x%08x source_mask=0x%02x "
			"source_states=0x%02x source1_size=0x%08x source2_size=0x%08x "
			"source1_id=%u source2_id=%u trace_circuit=%u writer_kind=%u "
			"writer_tfx=%llu writer_source_id=%u writer_source_size=0x%08x "
			"writer_ps_lo=0x%016llx writer_ps_hi=0x%016llx "
			"writer_blend=0x%08x writer_keys=0x%08x writer_topology=%u "
			"writer_draw_area=0x%016llx writer_sample_area=0x%016llx "
			"parent_kind=%u parent_tfx=%llu parent_textured=%llu "
			"parent_untextured=%llu parent_rt_source=%llu "
			"parent_full_mask=%llu parent_rgb_only=%llu "
			"parent_alpha_only=%llu parent_other_mask=%llu "
			"parent_source_id=%u "
			"parent_source_size=0x%08x parent_ps_lo=0x%016llx "
			"parent_ps_hi=0x%016llx parent_blend=0x%08x "
			"parent_keys=0x%08x parent_topology=%u parent_colormask=0x%02x "
			"parent_draw_area=0x%016llx parent_sample_area=0x%016llx "
			"parent_rgb_source_id=%u parent_rgb_source_size=0x%08x "
			"parent_rgb_ps_lo=0x%016llx parent_rgb_ps_hi=0x%016llx "
			"parent_rgb_blend=0x%08x parent_rgb_keys=0x%08x "
			"parent_rgb_topology=%u parent_rgb_colormask=0x%02x "
			"parent_rgb_draw_area=0x%016llx parent_rgb_sample_area=0x%016llx",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(end.gxm.merge_calls,
				start.gxm.merge_calls)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.merge_rc1_draws,
				start.gxm.merge_rc1_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.merge_rc2_draws,
				start.gxm.merge_rc2_draws)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.present_calls,
				start.gxm.present_calls)),
			static_cast<unsigned long long>(CounterDelta(end.gxm.interlace_calls,
				start.gxm.interlace_calls)),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_pmode : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_extbuf : 0),
			merge_seen ? end.gxm.last_merge_background : 0,
			merge_seen ? end.gxm.last_merge_source_mask : 0,
			merge_seen ? end.gxm.last_merge_source_states : 0,
			merge_seen ? end.gxm.last_merge_source_sizes[0] : 0,
			merge_seen ? end.gxm.last_merge_source_sizes[1] : 0,
			merge_seen ? end.gxm.last_merge_source_ids[0] : 0,
			merge_seen ? end.gxm.last_merge_source_ids[1] : 0,
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_trace_circuit : 0),
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_writer_kind : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_writer_tfx_writes : 0),
			merge_seen ? end.gxm.last_merge_writer_source_id : 0,
			merge_seen ? end.gxm.last_merge_writer_source_size : 0,
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_writer_ps_lo : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_writer_ps_hi : 0),
			merge_seen ? end.gxm.last_merge_writer_blend : 0,
			merge_seen ? end.gxm.last_merge_writer_selector_keys : 0,
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_writer_topology : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_writer_draw_area : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_writer_sample_area : 0),
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_parent_kind : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_tfx_writes : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_textured_tfx_writes : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_untextured_tfx_writes : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_render_target_source_tfx_writes : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_full_mask_tfx_writes : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_rgb_only_tfx_writes : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_alpha_only_tfx_writes : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_other_mask_tfx_writes : 0),
			merge_seen ? end.gxm.last_merge_parent_source_id : 0,
			merge_seen ? end.gxm.last_merge_parent_source_size : 0,
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_ps_lo : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_ps_hi : 0),
			merge_seen ? end.gxm.last_merge_parent_blend : 0,
			merge_seen ? end.gxm.last_merge_parent_selector_keys : 0,
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_parent_topology : 0),
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_parent_color_mask : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_draw_area : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_sample_area : 0),
			merge_seen ? end.gxm.last_merge_parent_last_rgb_source_id : 0,
			merge_seen ? end.gxm.last_merge_parent_last_rgb_source_size : 0,
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_last_rgb_ps_lo : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_last_rgb_ps_hi : 0),
			merge_seen ? end.gxm.last_merge_parent_last_rgb_blend : 0,
			merge_seen ? end.gxm.last_merge_parent_last_rgb_selector_keys : 0,
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_parent_last_rgb_topology : 0),
			static_cast<u32>(merge_seen ?
				end.gxm.last_merge_parent_last_rgb_color_mask : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_last_rgb_draw_area : 0),
			static_cast<unsigned long long>(merge_seen ?
				end.gxm.last_merge_parent_last_rgb_sample_area : 0));

		const VitaGpuVu::UniversalGpuVuEpochStatistics universal =
			VitaGpuVu::GetUniversalGpuVuEpochStatistics();
		const u64 execute_queue_average_us =
			universal.mtvu_execute_queue_samples ?
				universal.mtvu_execute_queue_age_us /
					universal.mtvu_execute_queue_samples : 0;
		const u64 path1_queue_average_us =
			universal.mtvu_path1_queue_samples ?
				universal.mtvu_path1_queue_age_us /
					universal.mtvu_path1_queue_samples : 0;
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_universal_cumulative "
			"prepared=%llu preflight_accepted=%llu submitted=%llu "
			"continuation_groups=%llu continuation_core_submissions=%llu "
				"accepted=%llu "
				"accepted_pairs=%llu path1_packets=%llu path1_qwords=%llu "
				"async_queued=%llu async_state_acquired=%llu async_pending_max=%llu "
			"universal_epochs=%llu universal_jobs=%llu generated_epochs=%llu "
			"generated_jobs=%llu gpu_residency_samples=%llu "
			"gpu_residency_us=%llu gpu_residency_max_us=%llu "
			"cpu_fallbacks=%llu fallback_pairs=%llu cpu_vu_calls=%llu "
			"output=raw-path1",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(universal.prepared),
			static_cast<unsigned long long>(universal.preflight_accepted),
			static_cast<unsigned long long>(universal.submitted),
			static_cast<unsigned long long>(universal.continuation_groups),
			static_cast<unsigned long long>(universal.continuation_submissions),
			static_cast<unsigned long long>(universal.accepted),
			static_cast<unsigned long long>(universal.accepted_pairs),
				static_cast<unsigned long long>(universal.accepted_path1_packets),
				static_cast<unsigned long long>(universal.accepted_path1_qwords),
				static_cast<unsigned long long>(universal.async_epochs_queued),
				static_cast<unsigned long long>(universal.async_state_acquired),
				static_cast<unsigned long long>(universal.async_pending_max),
			static_cast<unsigned long long>(universal.universal_provider_epochs),
			static_cast<unsigned long long>(universal.universal_provider_jobs),
			static_cast<unsigned long long>(universal.generated_provider_epochs),
			static_cast<unsigned long long>(universal.generated_provider_jobs),
			static_cast<unsigned long long>(universal.gpu_residency_samples),
			static_cast<unsigned long long>(universal.gpu_residency_wall_us),
			static_cast<unsigned long long>(universal.gpu_residency_wall_us_max),
			static_cast<unsigned long long>(universal.cpu_fallbacks),
			static_cast<unsigned long long>(universal.rejected_pairs),
			static_cast<unsigned long long>(universal.cpu_vu_calls));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_pipeline_cumulative "
			"preflight_us=%llu program_prepare_us=%llu static_lookup_us=%llu "
			"epoch_allocation_us=%llu control_analysis_us=%llu "
			"pair_validation_us=%llu payload_encode_us=%llu "
			"static_cache_hits=%llu static_cache_misses=%llu mailbox_wait_us=%llu "
			"notification_wait_us=%llu retirement_wait_us=%llu "
			"cpu_fallback_us=%llu worker_attempt_us=%llu path1_retirement_us=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(universal.preflight_wall_us),
			static_cast<unsigned long long>(universal.program_prepare_wall_us),
			static_cast<unsigned long long>(
				universal.static_preflight_lookup_wall_us),
			static_cast<unsigned long long>(universal.epoch_allocation_wall_us),
			static_cast<unsigned long long>(universal.control_analysis_wall_us),
			static_cast<unsigned long long>(universal.pair_validation_wall_us),
			static_cast<unsigned long long>(universal.payload_encode_wall_us),
			static_cast<unsigned long long>(universal.static_preflight_cache_hits),
			static_cast<unsigned long long>(universal.static_preflight_cache_misses),
			static_cast<unsigned long long>(universal.mailbox_wait_wall_us),
			static_cast<unsigned long long>(universal.notification_wait_wall_us),
			static_cast<unsigned long long>(universal.retirement_wait_wall_us),
			static_cast<unsigned long long>(universal.cpu_fallback_wall_us),
			static_cast<unsigned long long>(universal.worker_attempt_wall_us),
			static_cast<unsigned long long>(universal.path1_retirement_wall_us));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_backlog_cumulative "
			"execute_samples=%llu execute_age_avg_us=%llu execute_age_max_us=%llu "
				"execute_outstanding_max=%llu mtvu_queue_words_max=%llu "
				"async_pending_max=%llu "
				"multi_execute_gather_suppressed=%llu "
				"dispatch_cache_hits=%llu dispatch_cache_misses=%llu "
				"product_hot_hits=%llu product_hot_misses=%llu "
				"path1_samples=%llu path1_age_avg_us=%llu "
			"path1_age_max_us=%llu cpu0_mtvu_wait_us=%llu cpu0_ring_wait_us=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(universal.mtvu_execute_queue_samples),
			static_cast<unsigned long long>(execute_queue_average_us),
			static_cast<unsigned long long>(universal.mtvu_execute_queue_age_max_us),
			static_cast<unsigned long long>(universal.mtvu_execute_outstanding_max),
				static_cast<unsigned long long>(universal.mtvu_queue_used_words_max),
				static_cast<unsigned long long>(universal.async_pending_max),
			static_cast<unsigned long long>(
				universal.mtvu_multi_execute_gather_suppressed),
				static_cast<unsigned long long>(universal.mtvu_dispatch_cache_hits),
				static_cast<unsigned long long>(universal.mtvu_dispatch_cache_misses),
				static_cast<unsigned long long>(
					universal.generated_product_hot_dispatch_hits),
				static_cast<unsigned long long>(
					universal.generated_product_hot_dispatch_misses),
			static_cast<unsigned long long>(universal.mtvu_path1_queue_samples),
			static_cast<unsigned long long>(path1_queue_average_us),
			static_cast<unsigned long long>(universal.mtvu_path1_queue_age_max_us),
			static_cast<unsigned long long>(universal.cpu0_mtvu_wait_wall_us),
			static_cast<unsigned long long>(universal.cpu0_mtvu_ring_wait_wall_us));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_backpressure_cumulative "
			"execute_budget_waits=%llu execute_budget_wait_us=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(universal.cpu0_execute_budget_waits),
			static_cast<unsigned long long>(
				universal.cpu0_execute_budget_wait_wall_us));

		output.Flush();
		s_correlated_profile.start = end;
		VitaBeginA32EeRegionStatisticsWindow();
		s_correlated_profile.boundaries_at_start = boundary;
	}
#endif

	static void SetEvent();
	static void MainLoop();

	enum class UniversalGpuVuNotificationArmResult : u8
	{
		Armed,
		Queued,
		Failed,
	};

#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	static bool UniversalGpuVuNotificationReached(
		const VitaGpuVu::UniversalGpuVuEpoch& epoch)
	{
		const uptr address = epoch.SubmissionNotificationAddress();
		const u32 value = epoch.SubmissionNotificationValue();
		return address != 0 && value != 0 &&
			VitaGpuVu::HasCompletedNotificationValue(
				*reinterpret_cast<volatile u32*>(address), value);
	}

	[[maybe_unused]] static const char* GpuVuGxmSubmissionStageName(u32 stage)
	{
		switch (static_cast<VitaGS::GpuVuGxmSubmissionStage>(stage))
		{
			case VitaGS::GpuVuGxmSubmissionStage::DrawEncoding:
				return "draw-encoding";
			case VitaGS::GpuVuGxmSubmissionStage::VertexProgramBinding:
				return "vertex-program-binding";
			case VitaGS::GpuVuGxmSubmissionStage::FragmentProgramBinding:
				return "fragment-program-binding";
			case VitaGS::GpuVuGxmSubmissionStage::TextureBinding:
				return "texture-binding";
			case VitaGS::GpuVuGxmSubmissionStage::RasterStateBinding:
				return "raster-state-binding";
			case VitaGS::GpuVuGxmSubmissionStage::FinalRasterBinding:
				return "final-raster-binding";
			case VitaGS::GpuVuGxmSubmissionStage::VertexDefaultUniformReservation:
				return "vertex-default-uniform-reservation";
			case VitaGS::GpuVuGxmSubmissionStage::VertexDefaultUniformUpload:
				return "vertex-default-uniform-upload";
			case VitaGS::GpuVuGxmSubmissionStage::FragmentDefaultUniformReservation:
				return "fragment-default-uniform-reservation";
			case VitaGS::GpuVuGxmSubmissionStage::FragmentDefaultUniformUpload:
				return "fragment-default-uniform-upload";
			case VitaGS::GpuVuGxmSubmissionStage::AuxiliaryUniformBinding:
				return "auxiliary-uniform-binding";
			case VitaGS::GpuVuGxmSubmissionStage::PrecomputeProgramBinding:
				return "precompute-program-binding";
			case VitaGS::GpuVuGxmSubmissionStage::FinalProgramRestore:
				return "final-program-restore";
			case VitaGS::GpuVuGxmSubmissionStage::DrawBinding:
				return "draw-binding";
			case VitaGS::GpuVuGxmSubmissionStage::DrawCall:
				return "draw-call";
			case VitaGS::GpuVuGxmSubmissionStage::DrawReturned:
				return "draw-returned";
			case VitaGS::GpuVuGxmSubmissionStage::TransactionClaim:
				return "transaction-claim";
			case VitaGS::GpuVuGxmSubmissionStage::ResourceOwnershipTransfer:
				return "resource-ownership-transfer";
			case VitaGS::GpuVuGxmSubmissionStage::ResourcesRetained:
				return "resources-retained";
			case VitaGS::GpuVuGxmSubmissionStage::RenderReturnedToDevice:
				return "render-returned-to-device";
			case VitaGS::GpuVuGxmSubmissionStage::SceneDrawAccounting:
				return "scene-draw-accounting";
			case VitaGS::GpuVuGxmSubmissionStage::SceneInputOwnership:
				return "scene-input-ownership";
			case VitaGS::GpuVuGxmSubmissionStage::DescriptorInputRelease:
				return "descriptor-input-release";
			case VitaGS::GpuVuGxmSubmissionStage::DescriptorRelease:
				return "descriptor-release";
			case VitaGS::GpuVuGxmSubmissionStage::DrawsRetainedForScene:
				return "draws-retained-for-scene";
			case VitaGS::GpuVuGxmSubmissionStage::DeviceReturn:
				return "device-return";
			case VitaGS::GpuVuGxmSubmissionStage::GsStateGroupBegin:
				return "gs-state-group-begin";
			case VitaGS::GpuVuGxmSubmissionStage::GsStateGroupReturned:
				return "gs-state-group-returned";
			case VitaGS::GpuVuGxmSubmissionStage::GsStateGroupAdvanced:
				return "gs-state-group-advanced";
			case VitaGS::GpuVuGxmSubmissionStage::GsStateContractAdvanced:
				return "gs-state-contract-advanced";
			case VitaGS::GpuVuGxmSubmissionStage::GsStateConsumeReturned:
				return "gs-state-consume-returned";
			case VitaGS::GpuVuGxmSubmissionStage::MtgsConsumeReturned:
				return "mtgs-consume-returned";
			case VitaGS::GpuVuGxmSubmissionStage::MtgsCpuPath1Replay:
				return "mtgs-cpu-path1-replay";
			case VitaGS::GpuVuGxmSubmissionStage::MtgsCpuPath1ReplayReturned:
				return "mtgs-cpu-path1-replay-returned";
			case VitaGS::GpuVuGxmSubmissionStage::MtgsSubmissionMarkerCheck:
				return "mtgs-submission-marker-check";
			case VitaGS::GpuVuGxmSubmissionStage::MtgsCommandReturn:
				return "mtgs-command-return";
			case VitaGS::GpuVuGxmSubmissionStage::MidSceneFlush:
				return "mid-scene-flush";
			case VitaGS::GpuVuGxmSubmissionStage::EndScene:
				return "end-scene";
			case VitaGS::GpuVuGxmSubmissionStage::SceneTransition:
				return "scene-transition";
			case VitaGS::GpuVuGxmSubmissionStage::BeginScene:
				return "begin-scene";
			case VitaGS::GpuVuGxmSubmissionStage::FinishDrain:
				return "finish-drain";
			default:
				return "unknown";
		}
	}

	[[maybe_unused]] static const char* GpuVuGxmOwnershipActionName(u32 action)
	{
		switch (static_cast<VitaGS::GpuVuGxmOwnershipAction>(action))
		{
			case VitaGS::GpuVuGxmOwnershipAction::None:
				return "none";
			case VitaGS::GpuVuGxmOwnershipAction::DrawAccounting:
				return "draw-accounting";
			case VitaGS::GpuVuGxmOwnershipAction::InputInspect:
				return "input-inspect";
			case VitaGS::GpuVuGxmOwnershipAction::InputAlreadyRetained:
				return "input-already-retained";
			case VitaGS::GpuVuGxmOwnershipAction::InputTransferredToScene:
				return "input-transferred-to-scene";
			case VitaGS::GpuVuGxmOwnershipAction::DescriptorInputReleaseBefore:
				return "descriptor-input-release-before";
			case VitaGS::GpuVuGxmOwnershipAction::DescriptorInputReleaseAfter:
				return "descriptor-input-release-after";
			case VitaGS::GpuVuGxmOwnershipAction::DescriptorDestroyBefore:
				return "descriptor-destroy-before";
			case VitaGS::GpuVuGxmOwnershipAction::DescriptorDestroyAfter:
				return "descriptor-destroy-after";
			case VitaGS::GpuVuGxmOwnershipAction::Complete:
				return "complete";
			default:
				return "unknown";
		}
	}

	static void UpdateGpuVuLatencyMaximum(std::atomic<u64>* maximum,
		u64 elapsed_us)
	{
		if (!maximum)
			return;
		u64 observed = maximum->load(std::memory_order_relaxed);
		while (observed < elapsed_us &&
			!maximum->compare_exchange_weak(observed, elapsed_us,
				std::memory_order_relaxed, std::memory_order_relaxed))
		{
		}
	}

	static void RecordGpuVuSubmissionStageLatency(u32 stage, u64 elapsed_us)
	{
		if (stage == 0u || stage >= GpuVuGxmSubmissionStageCount)
			return;
		s_gpu_vu_submission_stage_samples[stage].fetch_add(
			1u, std::memory_order_relaxed);
		s_gpu_vu_submission_stage_total_us[stage].fetch_add(
			elapsed_us, std::memory_order_relaxed);
		UpdateGpuVuLatencyMaximum(
			&s_gpu_vu_submission_stage_max_us[stage], elapsed_us);
		s_gpu_vu_submission_last_stage.store(stage, std::memory_order_relaxed);
		s_gpu_vu_submission_last_stage_us.store(
			elapsed_us, std::memory_order_relaxed);
		u64 maximum = s_gpu_vu_submission_slowest_stage_us.load(
			std::memory_order_relaxed);
		while (maximum < elapsed_us &&
			!s_gpu_vu_submission_slowest_stage_us.compare_exchange_weak(
				maximum, elapsed_us, std::memory_order_relaxed,
				std::memory_order_relaxed))
		{
		}
		if (maximum < elapsed_us)
		{
			s_gpu_vu_submission_slowest_stage.store(
				stage, std::memory_order_relaxed);
		}
	}

	static void RecordCurrentGpuVuSubmissionStageLatency(
		Common::Timer::Value now)
	{
		const Common::Timer::Value started =
			s_gpu_vu_submission_watchdog_stage_started.load(
				std::memory_order_relaxed);
		if (started == 0u || now < started)
			return;
		const u32 stage = s_gpu_vu_submission_watchdog_stage.load(
			std::memory_order_relaxed);
		const u64 elapsed_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(now - started) * 1000000.0);
		RecordGpuVuSubmissionStageLatency(stage, elapsed_us);
	}

	static void RecordGpuVuOwnershipActionLatency(u32 action, u64 elapsed_us)
	{
		if (action == 0u || action >= GpuVuGxmOwnershipActionCount)
			return;
		s_gpu_vu_ownership_action_samples[action].fetch_add(
			1u, std::memory_order_relaxed);
		s_gpu_vu_ownership_action_total_us[action].fetch_add(
			elapsed_us, std::memory_order_relaxed);
		UpdateGpuVuLatencyMaximum(
			&s_gpu_vu_ownership_action_max_us[action], elapsed_us);
		s_gpu_vu_ownership_last_action.store(action, std::memory_order_relaxed);
		s_gpu_vu_ownership_last_action_us.store(
			elapsed_us, std::memory_order_relaxed);
		u64 maximum = s_gpu_vu_ownership_slowest_action_us.load(
			std::memory_order_relaxed);
		while (maximum < elapsed_us &&
			!s_gpu_vu_ownership_slowest_action_us.compare_exchange_weak(
				maximum, elapsed_us, std::memory_order_relaxed,
				std::memory_order_relaxed))
		{
		}
		if (maximum < elapsed_us)
		{
			s_gpu_vu_ownership_slowest_action.store(
				action, std::memory_order_relaxed);
		}
	}

	static void RecordCurrentGpuVuOwnershipActionLatency(
		Common::Timer::Value now)
	{
		const Common::Timer::Value started =
			s_gpu_vu_submission_ownership_action_started.load(
				std::memory_order_relaxed);
		if (started == 0u || now < started)
			return;
		const u32 action = s_gpu_vu_submission_ownership_action.load(
			std::memory_order_relaxed);
		const u64 elapsed_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(now - started) * 1000000.0);
		RecordGpuVuOwnershipActionLatency(action, elapsed_us);
	}

	static bool ServiceGpuVuGxmSubmissionWatchdog()
	{
		const u32 ticket = s_gpu_vu_submission_watchdog_active.load(
			std::memory_order_acquire);
		if (ticket == 0u || ticket == std::numeric_limits<u32>::max())
			return false;

		const Common::Timer::Value started =
			s_gpu_vu_submission_watchdog_started.load(
				std::memory_order_relaxed);
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (started == 0u ||
			s_gpu_vu_submission_watchdog_active.load(
				std::memory_order_acquire) != ticket ||
			Common::Timer::ConvertValueToSeconds(now - started) <
				GpuVuNotificationNoProgressTimeoutSeconds)
		{
			return false;
		}

		// The fixed journal already owns the exact MTGS/GXM stage and mapped
		// resource identities. Never put Console's mutexed, flushed text sink
		// between the watchdog decision and title-local fail-stop.
		s_gpu_vu_notification_available.store(false, std::memory_order_release);
		(void)VitaGpuVu::HealthJournal::FlushForFatal();
		sceKernelExitProcess(1);
		return true;
	}

	[[maybe_unused]] static const char* GpuVuGxmCallKindName(u32 kind)
	{
		switch (static_cast<VitaGS::GpuVuGxmCallKind>(kind))
		{
			case VitaGS::GpuVuGxmCallKind::GeneratedPrecomputeDraw:
				return "generated-precompute-draw";
			case VitaGS::GpuVuGxmCallKind::GeneratedBatchDraw:
				return "generated-batch-draw";
			case VitaGS::GpuVuGxmCallKind::GeneratedDirectDraw:
				return "generated-direct-draw";
			case VitaGS::GpuVuGxmCallKind::OrdinaryTfxDraw:
				return "ordinary-tfx-draw";
			case VitaGS::GpuVuGxmCallKind::ClearDraw:
				return "clear-draw";
			case VitaGS::GpuVuGxmCallKind::ScissorDraw:
				return "scissor-draw";
			case VitaGS::GpuVuGxmCallKind::PresentationDraw:
				return "presentation-draw";
			case VitaGS::GpuVuGxmCallKind::MidSceneFlush:
				return "mid-scene-flush";
			case VitaGS::GpuVuGxmCallKind::EndScene:
				return "end-scene";
			case VitaGS::GpuVuGxmCallKind::BeginScene:
				return "begin-scene";
			case VitaGS::GpuVuGxmCallKind::Finish:
				return "finish";
			default:
				return "unknown";
		}
	}

	static void RecordGpuVuGxmCallLatency(u32 kind, u64 elapsed_us)
	{
		if (kind == 0u || kind >= GpuVuGxmCallKindCount)
			return;
		s_gpu_vu_gxm_call_samples[kind].fetch_add(
			1u, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_total_us[kind].fetch_add(
			elapsed_us, std::memory_order_relaxed);
		UpdateGpuVuLatencyMaximum(&s_gpu_vu_gxm_call_max_us[kind], elapsed_us);
		u64 maximum = s_gpu_vu_gxm_call_slowest_us.load(
			std::memory_order_relaxed);
		while (maximum < elapsed_us &&
			!s_gpu_vu_gxm_call_slowest_us.compare_exchange_weak(
				maximum, elapsed_us, std::memory_order_relaxed,
				std::memory_order_relaxed))
		{
		}
		if (maximum < elapsed_us)
		{
			s_gpu_vu_gxm_call_slowest_kind.store(kind, std::memory_order_relaxed);
		}
	}

	enum class GpuVuHealthLifecycleStage : u32
	{
		Polled = 1u,
		StateQueryFailed,
	};

	static void PublishGpuVuLifecycleHealth(
		GpuVuHealthLifecycleStage stage, Common::Timer::Value now,
		u32 app_state = 0u)
	{
		VitaGpuVu::HealthJournal::LifecycleState health;
		health.update_time_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(now) * 1000000.0);
		// AppMgr's queued events belong to the application lifecycle owner. Keep
		// both event counters at zero rather than consuming or inventing events.
		health.overlay_enter_count =
			s_gpu_vu_lifecycle_overlay_enter_events.load(
				std::memory_order_relaxed);
		health.overlay_leave_count =
			s_gpu_vu_lifecycle_overlay_leave_events.load(
				std::memory_order_relaxed);
		health.stage = static_cast<u32>(stage);
		health.system_ui_overlaid =
			s_gpu_vu_lifecycle_system_ui_overlaid.load(
				std::memory_order_relaxed);
		health.app_state = app_state;
		health.flags =
			(s_gpu_vu_lifecycle_poll_errors.load(std::memory_order_relaxed) != 0u ?
				1u : 0u) |
			(1u << 2u); // Non-destructive AppMgr state sampling only.
		VitaGpuVu::HealthJournal::PublishLifecycle(health);
	}

	static void ServiceGpuVuAppLifecycle()
	{
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		Common::Timer::Value previous = s_gpu_vu_lifecycle_last_poll.load(
			std::memory_order_relaxed);
		if (previous != 0u && now >= previous &&
			Common::Timer::ConvertValueToSeconds(now - previous) < 0.1)
		{
			return;
		}
		if (!s_gpu_vu_lifecycle_last_poll.compare_exchange_strong(
				previous, now, std::memory_order_relaxed,
				std::memory_order_relaxed))
		{
			return;
		}

		const u64 poll_gap_us = previous != 0u && now >= previous ?
			static_cast<u64>(Common::Timer::ConvertValueToSeconds(
				now - previous) * 1000000.0) : 0u;
		s_gpu_vu_lifecycle_last_poll_gap_us.store(
			poll_gap_us, std::memory_order_relaxed);
		SceAppMgrAppState state{};
		const int state_result = _sceAppMgrGetAppState(
			&state, sizeof(state), PSP2_SDK_VERSION & 0xffff0000u);
		if (state_result < 0)
		{
			s_gpu_vu_lifecycle_poll_errors.fetch_add(
				1u, std::memory_order_relaxed);
			PublishGpuVuLifecycleHealth(
				GpuVuHealthLifecycleStage::StateQueryFailed, now);
			return;
		}

		const u32 overlaid = state.isSystemUiOverlaid ? 1u : 0u;
		const u32 previous_overlaid =
			s_gpu_vu_lifecycle_system_ui_overlaid.exchange(
				overlaid, std::memory_order_relaxed);
		if (s_gpu_vu_lifecycle_overlay_known.exchange(
				1u, std::memory_order_relaxed) != 0u &&
			previous_overlaid != overlaid)
		{
			(overlaid != 0u ? s_gpu_vu_lifecycle_overlay_enter_events :
				s_gpu_vu_lifecycle_overlay_leave_events).fetch_add(
				1u, std::memory_order_relaxed);
		}

		const u32 app_state =
			(state.systemEventNum & 0xffffu) |
			((state.appEventNum & 0xffffu) << 16u);
		PublishGpuVuLifecycleHealth(
			GpuVuHealthLifecycleStage::Polled, now, app_state);
	}

	static bool ServiceGpuVuGxmCallHealth()
	{
		const u32 token = s_gpu_vu_gxm_call_active.load(
			std::memory_order_acquire);
		if (token == 0u || token == std::numeric_limits<u32>::max())
			return false;
		const Common::Timer::Value started = s_gpu_vu_gxm_call_started.load(
			std::memory_order_relaxed);
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (started == 0u || s_gpu_vu_gxm_call_active.load(
				std::memory_order_acquire) != token)
		{
			return false;
		}
		const double elapsed = Common::Timer::ConvertValueToSeconds(
			now - started);
		if (elapsed < GpuVuNotificationNoProgressTimeoutSeconds)
			return false;
		(void)VitaGpuVu::HealthJournal::FlushForFatal();
		sceKernelExitProcess(1);
		return true;
	}

	[[maybe_unused]] static const char* GeneratedGpuVuHandoffStageName(u32 stage)
	{
		switch (static_cast<VitaGS::GeneratedGpuVuHandoffStage>(stage))
		{
			case VitaGS::GeneratedGpuVuHandoffStage::CompletionPublish:
				return "completion-publish";
			case VitaGS::GeneratedGpuVuHandoffStage::CompletionPublished:
				return "completion-published";
			case VitaGS::GeneratedGpuVuHandoffStage::MtgsConsumed:
				return "mtgs-consumed";
			case VitaGS::GeneratedGpuVuHandoffStage::DrawsEncoded:
				return "draws-encoded";
			case VitaGS::GeneratedGpuVuHandoffStage::SubmissionRequested:
				return "submission-requested";
			default:
				return "unknown";
		}
	}

	static bool ServiceGeneratedGpuVuHandoffWatchdog()
	{
		const u32 ticket = s_gpu_vu_handoff_watchdog_active.load(
			std::memory_order_acquire);
		if (ticket == 0u || ticket == std::numeric_limits<u32>::max())
			return false;

		const Common::Timer::Value started =
			s_gpu_vu_handoff_watchdog_started.load(std::memory_order_relaxed);
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (started == 0u ||
			s_gpu_vu_handoff_watchdog_active.load(
				std::memory_order_acquire) != ticket ||
			Common::Timer::ConvertValueToSeconds(now - started) <
				GpuVuNotificationNoProgressTimeoutSeconds)
		{
			return false;
		}

		// CPU1 and MTGS publish their independently owned stages to the durable
		// journal. Avoid a synchronous text sink after liveness has failed.
		s_gpu_vu_notification_available.store(false, std::memory_order_release);
		(void)VitaGpuVu::HealthJournal::FlushForFatal();
		sceKernelExitProcess(1);
		return true;
	}

	struct GpuVuNotificationTarget
	{
		uptr address = 0;
		u32 value = 0;
		u32 start_value = 0;
		u32 job_base = 0;
		u32 job_count = 0;
		u64 sequence = 0;
	};

	static bool QueueGpuVuNotificationTarget(
		const GpuVuNotificationTarget& target)
	{
		// The GXM context is ordered, so its newest vertex-completion target also
		// proves every earlier target.  Keep exactly that newest target while the
		// notification owner is observing the current one.  State ownership keeps
		// the pointer-free payload coherent without taking an MTGS-side lock.
		for (u32 attempt = 0; attempt < 64u; attempt++)
		{
			u32 state = s_gpu_vu_notification_pending_state.load(
				std::memory_order_acquire);
			if (state != 0u && state != 2u)
				continue;
			const bool replacement = state == 2u;
			if (!s_gpu_vu_notification_pending_state.compare_exchange_weak(
					state, 1u, std::memory_order_acq_rel,
					std::memory_order_acquire))
			{
				continue;
			}

			s_gpu_vu_notification_pending_address.store(
				target.address, std::memory_order_relaxed);
			s_gpu_vu_notification_pending_value.store(
				target.value, std::memory_order_relaxed);
			s_gpu_vu_notification_pending_start_value.store(
				target.start_value, std::memory_order_relaxed);
			s_gpu_vu_notification_pending_job_base.store(
				target.job_base, std::memory_order_relaxed);
			s_gpu_vu_notification_pending_job_count.store(
				target.job_count, std::memory_order_relaxed);
			s_gpu_vu_notification_pending_sequence.store(
				target.sequence, std::memory_order_relaxed);
			s_gpu_vu_notification_pending_state.store(
				2u, std::memory_order_release);
			s_gpu_vu_notification_pending_queues.fetch_add(
				1u, std::memory_order_relaxed);
			if (replacement)
			{
				s_gpu_vu_notification_pending_replacements.fetch_add(
					1u, std::memory_order_relaxed);
			}
			if (!replacement)
				s_gpu_vu_notification_request_sema.Post();
			return true;
		}
		return false;
	}

	static bool TakeGpuVuNotificationTarget(GpuVuNotificationTarget* target)
	{
		if (!target)
			return false;
		u32 state = 2u;
		if (!s_gpu_vu_notification_pending_state.compare_exchange_strong(
				state, 3u, std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
			return false;
		}
		GpuVuNotificationTarget resolved;
		resolved.address = s_gpu_vu_notification_pending_address.load(
			std::memory_order_relaxed);
		resolved.value = s_gpu_vu_notification_pending_value.load(
			std::memory_order_relaxed);
		resolved.start_value =
			s_gpu_vu_notification_pending_start_value.load(
				std::memory_order_relaxed);
		resolved.job_base = s_gpu_vu_notification_pending_job_base.load(
			std::memory_order_relaxed);
		resolved.job_count = s_gpu_vu_notification_pending_job_count.load(
			std::memory_order_relaxed);
		resolved.sequence = s_gpu_vu_notification_pending_sequence.load(
			std::memory_order_relaxed);
		s_gpu_vu_notification_pending_state.store(
			0u, std::memory_order_release);
		*target = resolved;
		return resolved.address != 0u && resolved.value != 0u;
	}

	static u32 PublishGpuVuNotificationTarget(
		const GpuVuNotificationTarget& target)
	{
		u32 next = s_gpu_vu_notification_request.load(
			std::memory_order_relaxed) + 1u;
		if (next == 0u)
			next = 1u;
		s_gpu_vu_notification_address.store(
			target.address, std::memory_order_relaxed);
		s_gpu_vu_notification_value.store(
			target.value, std::memory_order_relaxed);
		s_gpu_vu_notification_start_value.store(
			target.start_value, std::memory_order_relaxed);
		s_gpu_vu_notification_job_base.store(
			target.job_base, std::memory_order_relaxed);
		s_gpu_vu_notification_job_count.store(
			target.job_count, std::memory_order_relaxed);
		s_gpu_vu_notification_sequence.store(
			target.sequence, std::memory_order_relaxed);
		s_gpu_vu_notification_request.store(next, std::memory_order_release);
		s_gpu_vu_notification_arms.fetch_add(1u, std::memory_order_relaxed);
		return next;
	}

	enum class GpuVuHealthNotificationStage : u32
	{
		Idle = 1u,
		TargetClaimed,
		Polling,
		Reached,
		Timeout,
	};

	static void PublishGpuVuNotificationHealth(
		GpuVuHealthNotificationStage stage, u32 observed)
	{
		VitaGpuVu::HealthJournal::NotificationState health;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		health.update_time_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(now) * 1000000.0);
		health.active_address = s_gpu_vu_notification_address.load(
			std::memory_order_relaxed);
		health.active_sequence = s_gpu_vu_notification_sequence.load(
			std::memory_order_relaxed);
		health.pending_address = s_gpu_vu_notification_pending_address.load(
			std::memory_order_relaxed);
		health.pending_sequence = s_gpu_vu_notification_pending_sequence.load(
			std::memory_order_relaxed);
		health.arm_count = s_gpu_vu_notification_arms.load(
			std::memory_order_relaxed);
		health.poll_count = s_gpu_vu_notification_polls.load(
			std::memory_order_relaxed);
		health.wake_count = s_gpu_vu_notification_wakes.load(
			std::memory_order_relaxed);
		health.deferral_count = s_gpu_vu_notification_deferrals.load(
			std::memory_order_relaxed);
		health.stage = static_cast<u32>(stage);
		health.request = s_gpu_vu_notification_request.load(
			std::memory_order_relaxed);
		health.completed = s_gpu_vu_notification_completed.load(
			std::memory_order_relaxed);
		health.active_required = s_gpu_vu_notification_value.load(
			std::memory_order_relaxed);
		health.active_observed = observed;
		health.pending_required = s_gpu_vu_notification_pending_value.load(
			std::memory_order_relaxed);
		health.pending_observed = health.pending_address != 0u ?
			*reinterpret_cast<volatile u32*>(health.pending_address) : 0u;
		health.flags =
			s_gpu_vu_notification_pending_state.load(std::memory_order_relaxed) |
			(s_gpu_vu_submission_watchdog_active.load(std::memory_order_relaxed) != 0u ?
				(1u << 2u) : 0u) |
			(s_gpu_vu_handoff_watchdog_active.load(std::memory_order_relaxed) != 0u ?
				(1u << 3u) : 0u) |
			(s_gpu_vu_gxm_call_active.load(std::memory_order_relaxed) != 0u ?
				(1u << 4u) : 0u) |
			(s_gpu_vu_notification_timeout_request.load(std::memory_order_relaxed) !=
				0u ? (1u << 5u) : 0u);
		VitaGpuVu::HealthJournal::PublishNotification(health);
	}

	static void UniversalGpuVuNotificationThreadEntryPoint()
	{
		Threading::SetNameOfCurrentThread("GPU-VU notify");
		const Threading::ThreadHandle self =
			Threading::ThreadHandle::GetForCallingThread();
		const bool affinity_set = self.SetAffinity(1u << 2);
		// This thread is the only process-local observer which can terminate the
		// title while MTGS is trapped in a libGXM user-space wait. SetAffinity()
		// first moves this USER_2-only thread into Sony's 64..127 individual ready
		// queue. Keep the sleeping watchdog one step above the journal writer and
		// two above MTGS so neither a GXM spin nor a filesystem write can postpone
		// a two-second safety decision for hundreds of seconds.
		const int pinned_priority = sceKernelGetThreadCurrentPriority();
		const int watchdog_priority = pinned_priority > 65 ?
			pinned_priority - 2 : pinned_priority;
		const int priority_result = affinity_set && pinned_priority >= 64 &&
			pinned_priority <= 127 ? sceKernelChangeThreadPriority(
				sceKernelGetThreadId(), watchdog_priority) : -1;
		const int effective_priority = sceKernelGetThreadCurrentPriority();
		const bool scheduling_ready = affinity_set && priority_result >= 0 &&
			pinned_priority >= 64 && pinned_priority <= 127 &&
			effective_priority >= 64 && effective_priority <= 127;
		s_gpu_vu_notification_startup_pinned_priority.store(
			pinned_priority, std::memory_order_relaxed);
		s_gpu_vu_notification_startup_effective_priority.store(
			effective_priority, std::memory_order_relaxed);
		s_gpu_vu_notification_startup_priority_result.store(
			priority_result, std::memory_order_relaxed);
		s_gpu_vu_notification_startup_result.store(
			scheduling_ready ? 1u : 2u, std::memory_order_release);
		s_gpu_vu_notification_startup_sema.Post();
		if (!scheduling_ready)
			return;
		for (;;)
		{
			if (s_gpu_vu_gxm_call_health_enabled.load(
					std::memory_order_acquire))
			{
				if (!s_gpu_vu_notification_request_sema.TryWait())
					Threading::Sleep(1);
			}
			else
			{
				s_gpu_vu_notification_request_sema.Wait();
			}
			if (s_gpu_vu_notification_shutdown.load(std::memory_order_acquire))
				break;
			u32 request =
				s_gpu_vu_notification_request.load(std::memory_order_acquire);
			const u32 completed =
				s_gpu_vu_notification_completed.load(std::memory_order_acquire);
			if (request == completed)
			{
				GpuVuNotificationTarget pending;
				if (TakeGpuVuNotificationTarget(&pending))
				{
					if (request != 0u)
					{
						s_gpu_vu_notification_pending_chains.fetch_add(
							1u, std::memory_order_relaxed);
					}
					request = PublishGpuVuNotificationTarget(pending);
					PublishGpuVuNotificationHealth(
						GpuVuHealthNotificationStage::TargetClaimed,
						s_gpu_vu_notification_observed.load(
							std::memory_order_relaxed));
				}
				else
				{
					// Semaphore posts also service the pre-notification and handoff
					// watchdogs.  Do not re-observe the preceding completed target
					// merely because one of those owners woke this thread.
					request = 0u;
				}
			}
			const uptr address =
				s_gpu_vu_notification_address.load(std::memory_order_relaxed);
			const u32 value =
				s_gpu_vu_notification_value.load(std::memory_order_relaxed);
			if (request == 0 || address == 0 || value == 0)
			{
				PublishGpuVuNotificationHealth(
					GpuVuHealthNotificationStage::Idle,
					s_gpu_vu_notification_observed.load(
						std::memory_order_relaxed));
				// No completion target is currently owned.  Only this branch may
				// perform lifecycle/watchdog diagnostics before another poll; an
				// active or newly chained target is always sampled first.
				// AppMgr state sampling is observational and may enter a synchronous
				// system call. Keep it off every notification/GXM/watchdog critical
				// path; lifecycle evidence is collected only while this owner is truly
				// idle.
				if (s_gpu_vu_gxm_call_active.load(std::memory_order_acquire) == 0u &&
					s_gpu_vu_submission_watchdog_active.load(
						std::memory_order_acquire) == 0u &&
					s_gpu_vu_handoff_watchdog_active.load(
						std::memory_order_acquire) == 0u)
				{
					ServiceGpuVuAppLifecycle();
				}
				if (ServiceGpuVuGxmCallHealth())
					return;
				while (!s_gpu_vu_notification_shutdown.load(
						std::memory_order_acquire) &&
					s_gpu_vu_notification_pending_state.load(
						std::memory_order_acquire) != 2u &&
					(s_gpu_vu_submission_watchdog_active.load(
						 std::memory_order_acquire) != 0u ||
					 s_gpu_vu_handoff_watchdog_active.load(
						 std::memory_order_acquire) != 0u))
				{
					if (ServiceGpuVuGxmCallHealth())
						return;
					if (ServiceGpuVuGxmSubmissionWatchdog())
						return;
					if (ServiceGeneratedGpuVuHandoffWatchdog())
						return;
					Threading::Sleep(1);
				}
				if (request != 0u)
				{
					s_gpu_vu_notification_completed.store(
						request, std::memory_order_release);
					s_work_sema.NotifyOfWork();
				}
				continue;
			}

			volatile u32* const notification =
				reinterpret_cast<volatile u32*>(address);
			u32 last_observed = *notification;
			s_gpu_vu_notification_observed.store(
				last_observed, std::memory_order_relaxed);
			PublishGpuVuNotificationHealth(
				GpuVuHealthNotificationStage::Polling, last_observed);
			const Common::Timer::Value wait_started =
				Common::Timer::GetCurrentValue();
			Common::Timer::Value last_progress = wait_started;
			while (!s_gpu_vu_notification_shutdown.load(
						std::memory_order_acquire) &&
				!VitaGpuVu::HasCompletedNotificationValue(
					*notification, value))
			{
				if (ServiceGpuVuGxmCallHealth())
					return;
				if (ServiceGpuVuGxmSubmissionWatchdog())
					return;
				if (ServiceGeneratedGpuVuHandoffWatchdog())
					return;
				const u32 observed = *notification;
				s_gpu_vu_notification_observed.store(
					observed, std::memory_order_relaxed);
				const bool notification_reached =
					VitaGpuVu::HasCompletedNotificationValue(observed, value);
				if (!notification_reached && observed != last_observed)
				{
					last_observed = observed;
					last_progress = Common::Timer::GetCurrentValue();
					PublishGpuVuNotificationHealth(
						GpuVuHealthNotificationStage::Polling, observed);
				}
				const Common::Timer::Value now =
					Common::Timer::GetCurrentValue();
				if (!notification_reached &&
					Common::Timer::ConvertValueToSeconds(now - last_progress) >=
					GpuVuNotificationNoProgressTimeoutSeconds)
				{
					const u64 elapsed_us = static_cast<u64>(
						Common::Timer::ConvertValueToSeconds(
							now - wait_started) * 1000000.0);
					s_gpu_vu_notification_available.store(
						false, std::memory_order_release);
					s_gpu_vu_notification_timeout_observed.store(
						observed, std::memory_order_relaxed);
					s_gpu_vu_notification_timeout_elapsed_us.store(
						elapsed_us, std::memory_order_relaxed);
					s_gpu_vu_notification_timeout_request.store(
						request, std::memory_order_release);
					PublishGpuVuNotificationHealth(
						GpuVuHealthNotificationStage::Timeout, observed);
					s_work_sema.NotifyOfWork();
					// Sony's process manager defines sceKernelExitProcess() as
					// deleting only the calling title.  The PSP2 PowerVR client uses
					// the same operation for PVRSRV_CLIENT_RESET_ON_HWTIMEOUT.  Do
					// not rely on MTGS to service the request above: it may be the
					// thread blocked inside the GXM submission which stopped this
					// notification.  Once two seconds of mapped-notification
					// no-progress proves ownership indeterminate, terminating
					// VitaSX2 is safer than leaving its GXM context able to hang the
					// user's shell or a later reboot.
					if (!s_gpu_vu_notification_shutdown.load(
							std::memory_order_acquire))
					{
						(void)VitaGpuVu::HealthJournal::FlushForFatal();
						sceKernelExitProcess(1);
					}
					return;
				}
				// PhyreRenderInterfaceGXM uses notification polling plus a 1 ms
				// sleep when a non-blocking owner cannot use the previous-frame
				// sceGxmNotificationWait path. The delay keeps this bridge off the
				// throughput cores while retaining bounded GPU-fault shutdown.
				s_gpu_vu_notification_polls.fetch_add(
					1, std::memory_order_relaxed);
				Threading::Sleep(1);
			}
			if (s_gpu_vu_notification_shutdown.load(std::memory_order_acquire))
				break;

			s_gpu_vu_notification_completed.store(
				request, std::memory_order_release);
			s_gpu_vu_notification_wakes.fetch_add(
				1, std::memory_order_relaxed);
			PublishGpuVuNotificationHealth(
				GpuVuHealthNotificationStage::Reached, *notification);
			s_work_sema.NotifyOfWork();
		}
	}

	static UniversalGpuVuNotificationArmResult ArmGpuVuNotificationWake(
		uptr address, u32 value, u32 start_value, u32 job_base,
		u32 job_count, u64 sequence)
	{
		if (!s_gpu_vu_notification_available.load(std::memory_order_acquire))
			return UniversalGpuVuNotificationArmResult::Failed;
		if (address == 0 || value == 0)
			return UniversalGpuVuNotificationArmResult::Failed;
		if (VitaGpuVu::HasCompletedNotificationValue(
				*reinterpret_cast<volatile u32*>(address), value))
		{
			// Completion can race the check in MainLoop. Preserve that wake so
			// the retained ring head is revisited without an external command.
			s_gpu_vu_notification_wakes.fetch_add(
				1, std::memory_order_relaxed);
			s_work_sema.NotifyOfWork();
			return UniversalGpuVuNotificationArmResult::Armed;
		}

		const u32 requested =
			s_gpu_vu_notification_request.load(std::memory_order_acquire);
		const u32 completed =
			s_gpu_vu_notification_completed.load(std::memory_order_acquire);
		if (requested != completed)
		{
			s_gpu_vu_notification_deferrals.fetch_add(
				1u, std::memory_order_relaxed);
		}
		const GpuVuNotificationTarget target{
			address, value, start_value, job_base, job_count, sequence};
		if (!QueueGpuVuNotificationTarget(target))
			return UniversalGpuVuNotificationArmResult::Failed;
		return UniversalGpuVuNotificationArmResult::Queued;
	}

	static UniversalGpuVuNotificationArmResult
	ArmUniversalGpuVuNotificationWake(
		const VitaGpuVu::UniversalGpuVuEpoch& epoch)
	{
		return ArmGpuVuNotificationWake(
			epoch.SubmissionNotificationAddress(),
			epoch.SubmissionNotificationValue(),
			epoch.SubmissionNotificationStartValue(),
			epoch.SubmissionJobBase(), epoch.SubmissionJobCount(),
			epoch.Sequence());
	}

	static void StartUniversalGpuVuNotificationThread()
	{
		if (s_gpu_vu_notification_thread.Joinable())
			return;
		if (!VitaGpuVu::HealthJournal::Initialize())
		{
			Console.Error(
				"GPU-VU: durable health journal writer could not establish its "
				"scheduling contract; generated admission remains disabled.");
			return;
		}
		s_gpu_vu_notification_shutdown.store(false, std::memory_order_relaxed);
		s_gpu_vu_notification_available.store(false, std::memory_order_relaxed);
		s_gpu_vu_notification_startup_result.store(0u, std::memory_order_relaxed);
		s_gpu_vu_notification_startup_pinned_priority.store(
			-1, std::memory_order_relaxed);
		s_gpu_vu_notification_startup_effective_priority.store(
			-1, std::memory_order_relaxed);
		s_gpu_vu_notification_startup_priority_result.store(
			-1, std::memory_order_relaxed);
		s_gpu_vu_notification_address.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_value.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_start_value.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_job_base.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_job_count.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_sequence.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_observed.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_request.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_completed.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_arms.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_polls.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_wakes.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_deferrals.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_state.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_address.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_value.store(0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_start_value.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_job_base.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_job_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_sequence.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_queues.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_replacements.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_pending_chains.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_timeout_request.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_timeout_observed.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_timeout_elapsed_us.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_next.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_active.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_stage.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_sequence.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_scene.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_input_owner.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_output_address.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_input_slot.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_input_generation.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_input_payload_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_stream_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_object_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_private_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_output_bytes.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_retirement_slot.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_updates.store(0, std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_total_draws.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_state_first.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_state_end.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_group_first.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_group_end.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_group_index.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_consumer_flags.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_pending_direct_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_pending_generated.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_last_publication_kind.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_last_publication_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_reservations_remaining.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_last_completion_count.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_last_completion_prefix.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_last_completion_submit.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_path1_last_ring_advance.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_started.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_stage_started.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_watchdog_last_report.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_ownership_action_started.store(
			0, std::memory_order_relaxed);
		for (u32 index = 0u; index < GpuVuGxmSubmissionStageCount; index++)
		{
			s_gpu_vu_submission_stage_samples[index].store(
				0, std::memory_order_relaxed);
			s_gpu_vu_submission_stage_total_us[index].store(
				0, std::memory_order_relaxed);
			s_gpu_vu_submission_stage_max_us[index].store(
				0, std::memory_order_relaxed);
		}
		s_gpu_vu_submission_last_stage.store(0, std::memory_order_relaxed);
		s_gpu_vu_submission_last_stage_us.store(0, std::memory_order_relaxed);
		s_gpu_vu_submission_slowest_stage.store(0, std::memory_order_relaxed);
		s_gpu_vu_submission_slowest_stage_us.store(0, std::memory_order_relaxed);
		s_gpu_vu_submission_last_completed_ticket.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_submission_last_completed_us.store(0, std::memory_order_relaxed);
		s_gpu_vu_submission_max_completed_us.store(0, std::memory_order_relaxed);
		for (u32 index = 0u; index < GpuVuGxmOwnershipActionCount; index++)
		{
			s_gpu_vu_ownership_action_samples[index].store(
				0, std::memory_order_relaxed);
			s_gpu_vu_ownership_action_total_us[index].store(
				0, std::memory_order_relaxed);
			s_gpu_vu_ownership_action_max_us[index].store(
				0, std::memory_order_relaxed);
		}
		s_gpu_vu_ownership_last_action.store(0, std::memory_order_relaxed);
		s_gpu_vu_ownership_last_action_us.store(0, std::memory_order_relaxed);
		s_gpu_vu_ownership_slowest_action.store(0, std::memory_order_relaxed);
		s_gpu_vu_ownership_slowest_action_us.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_health_enabled.store(
			false, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_next.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_active.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_kind.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_sequence.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_scene.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_started.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_journal_time.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_journal_token.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_completed.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_completed_kind.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_completed_sequence.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_completed_scene.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_completed_result.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_completed_elapsed_us.store(
			0, std::memory_order_relaxed);
		for (u32 index = 0u; index < GpuVuGxmCallKindCount; index++)
		{
			s_gpu_vu_gxm_call_samples[index].store(0, std::memory_order_relaxed);
			s_gpu_vu_gxm_call_total_us[index].store(0, std::memory_order_relaxed);
			s_gpu_vu_gxm_call_max_us[index].store(0, std::memory_order_relaxed);
		}
		s_gpu_vu_gxm_call_slowest_kind.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_slowest_us.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_sequence_end.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_input_owner.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_output_address.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_input_slot.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_gxm_call_input_generation.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_retirement_slot.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_gxm_call_phase_index.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_gxm_call_module_index.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_gxm_call_object_index.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_gxm_call_group_index.store(
			std::numeric_limits<u32>::max(), std::memory_order_relaxed);
		s_gpu_vu_gxm_call_index_count.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_object_count.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_private_count.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_output_bytes.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_vertex_program.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_fragment_program.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_object.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_descriptor.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_data.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_ps_selector_low.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_ps_selector_high.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_primitive_type.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_topology.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_sampler_key.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_blend_key.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_color_mask_key.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_depth_key.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_type.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_format.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_width.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_height.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_stride.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_mipmap_count.store(0, std::memory_order_relaxed);
		s_gpu_vu_gxm_call_texture_sampler_state.store(0, std::memory_order_relaxed);
		s_gpu_vu_lifecycle_last_poll.store(0, std::memory_order_relaxed);
		s_gpu_vu_lifecycle_poll_errors.store(0, std::memory_order_relaxed);
		s_gpu_vu_lifecycle_last_poll_gap_us.store(0, std::memory_order_relaxed);
		s_gpu_vu_lifecycle_system_ui_overlaid.store(0, std::memory_order_relaxed);
		s_gpu_vu_lifecycle_overlay_known.store(0, std::memory_order_relaxed);
		s_gpu_vu_lifecycle_overlay_enter_events.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_lifecycle_overlay_leave_events.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_handoff_watchdog_next.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_handoff_watchdog_active.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_handoff_watchdog_stage.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_handoff_watchdog_sequence.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_handoff_watchdog_started.store(
			0, std::memory_order_relaxed);
		s_gpu_vu_notification_thread.SetStackSize(32 * 1024);
		if (!s_gpu_vu_notification_thread.Start(
				&UniversalGpuVuNotificationThreadEntryPoint))
		{
			VitaGpuVu::HealthJournal::Shutdown();
			Console.Error(
				"GPU-VU: asynchronous notification bridge could not start; "
				"universal admission will remain disabled.");
			return;
		}
		s_gpu_vu_notification_startup_sema.Wait();
		const u32 scheduling_result =
			s_gpu_vu_notification_startup_result.load(std::memory_order_acquire);
		Console.WriteLn(
			"GPU-VU: notification watchdog scheduling priority_pinned=%08x "
			"priority_after=%08x priority_result=%08x ready=%u.",
			static_cast<u32>(s_gpu_vu_notification_startup_pinned_priority.load(
				std::memory_order_relaxed)),
			static_cast<u32>(s_gpu_vu_notification_startup_effective_priority.load(
				std::memory_order_relaxed)),
			static_cast<u32>(s_gpu_vu_notification_startup_priority_result.load(
				std::memory_order_relaxed)),
			scheduling_result == 1u ? 1u : 0u);
		if (scheduling_result != 1u)
		{
			s_gpu_vu_notification_thread.Join();
			VitaGpuVu::HealthJournal::Shutdown();
			Console.Error(
				"GPU-VU: notification watchdog could not establish Sony's "
				"individual-queue scheduling contract; generated admission remains "
				"disabled.");
			return;
		}
		s_gpu_vu_notification_available.store(true, std::memory_order_release);
	}

	static void StopUniversalGpuVuNotificationThread()
	{
		if (!s_gpu_vu_notification_thread.Joinable())
		{
			VitaGpuVu::HealthJournal::Shutdown();
			return;
		}
		s_gpu_vu_notification_available.store(false, std::memory_order_release);
		s_gpu_vu_notification_shutdown.store(true, std::memory_order_release);
		s_gpu_vu_submission_watchdog_active.store(
			0, std::memory_order_release);
		s_gpu_vu_gxm_call_health_enabled.store(
			false, std::memory_order_release);
		s_gpu_vu_gxm_call_active.store(0, std::memory_order_release);
		s_gpu_vu_handoff_watchdog_active.store(
			0, std::memory_order_release);
		s_gpu_vu_notification_request_sema.Post();
		s_gpu_vu_notification_thread.Join();
		VitaGpuVu::HealthJournal::Shutdown();
	}
#else
	static bool UniversalGpuVuNotificationReached(
		const VitaGpuVu::UniversalGpuVuEpoch&)
	{
		return false;
	}

	static UniversalGpuVuNotificationArmResult
	ArmUniversalGpuVuNotificationWake(
		const VitaGpuVu::UniversalGpuVuEpoch&)
	{
		return UniversalGpuVuNotificationArmResult::Failed;
	}

	static UniversalGpuVuNotificationArmResult ArmGpuVuNotificationWake(
		uptr, u32, u32, u32, u32, u64)
	{
		return UniversalGpuVuNotificationArmResult::Failed;
	}
#endif

	static void PublishMtvuPath1Completion(
		MtvuPath1Completion completion)
		{
		pxAssertRel(completion.reservation_count != 0,
			"empty MTVU PATH1 completion run");
			if (VitaPerformanceTelemetry::IsEnabled())
				completion.published_at = Common::Timer::GetCurrentValue();
			bool waited = false;
		while (!s_mtvu_path1_completions.Push(completion))
		{
			waited = true;
			s_work_sema.NotifyOfWork();
			Threading::SpinWait();
		}
#if defined(__vita__)
		if (waited && VitaPerformanceTelemetry::IsEnabled())
			s_gs_producer_performance.mtvu_path1_completion_ring_waits++;
#endif
		// Publication normally races ahead of the GS reservation and needs no
		// kernel wake. Notify only when the owner has actually observed a hole.
		if (s_mtvu_path1_completion_waiting.exchange(
				false, std::memory_order_acq_rel))
		{
			s_work_sema.NotifyOfWork();
		}
		if (s_mtvu_path1_drain_waiter.exchange(
				false, std::memory_order_acq_rel))
		{
			s_mtvu_path1_drain_sema.Post();
		}
	}

	static void FailPendingMtvuDirectInputPublication(const char* scope)
	{
		VitaGpuVu::GpuVuDraw* draw = s_pending_mtvu_direct_head;
		const u32 expected_count = s_pending_mtvu_direct_count;
		s_pending_mtvu_direct_head = nullptr;
		s_pending_mtvu_direct_tail = nullptr;
		s_pending_mtvu_direct_count = 0;
		s_pending_mtvu_direct_reservations = 0;
		s_pending_mtvu_no_output_count = 0;
		s_pending_mtvu_generated_transaction = false;
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		s_gpu_vu_path1_pending_direct_count.store(0, std::memory_order_relaxed);
		s_gpu_vu_path1_pending_generated.store(0, std::memory_order_release);
#endif

		u32 discarded = 0;
		while (draw && discarded < expected_count)
		{
			VitaGpuVu::GpuVuDraw* const next = draw->path1_next;
			draw->path1_next = nullptr;
			VitaGpuVu::RecordGpuVuDrawRejected();
			delete draw;
			draw = next;
			discarded++;
		}
		Console.Error(
			"GPU-VU FATAL input_publication=failed scope=%s "
			"expected_draws=%u discarded_draws=%u trailing_draw=%u "
			"pre_effect=1 gpu_submission=0 cpu_replay=0 process_exit=1.",
			scope ? scope : "unknown", expected_count, discarded,
			draw ? 1u : 0u);
#if defined(__vita__)
		// The corresponding MTGS reservation is already visible and cannot be
		// repaired here without replaying an accepted generated transaction.
		// Terminate only VitaSX2; never expose an unpublished mapped generation
		// to GXM and never leave MTGS sleeping forever on the reservation.
		(void)VitaGpuVu::HealthJournal::FlushForFatal();
		sceKernelExitProcess(1);
#endif
	}

	static void PublishPendingMtvuNoOutput()
	{
		// Only trailing credits are held here. The preceding draw run must have
		// been published first; no following CPU packet or GS command may pass.
		pxAssertRel(!s_pending_mtvu_direct_head,
			"empty MTVU completions passed an unpublished draw");
		if (s_pending_mtvu_no_output_count == 0)
			return;
		PublishMtvuPath1Completion({nullptr, s_pending_mtvu_no_output_count,
			nullptr, false, false, true});
		s_pending_mtvu_no_output_count = 0;
	}

	static void FlushPendingMtvuDirectRun(bool require_vertex_visibility)
	{
		if (!s_pending_mtvu_direct_head)
		{
			PublishPendingMtvuNoOutput();
			return;
		}
		if (!s_pending_mtvu_direct_tail ||
			s_pending_mtvu_direct_count == 0)
		{
			pxAssertRel(false, "incomplete pending MTVU direct run");
			return;
		}
		// The EE producer writes UNPACK payloads directly into a cacheable,
		// GPU-coherent GXM mapping. Publish all committed prefixes once per
		// descriptor run before its release makes any draw visible. This is
		// an ownership boundary, not another payload copy.
		if (!VitaGpuVu::PublishPendingRawVifPayloads(
				s_pending_mtvu_direct_head, s_pending_mtvu_direct_count))
		{
			FailPendingMtvuDirectInputPublication("complete-run");
			return;
		}
		const bool cpu_path1_after_draws =
			s_pending_mtvu_direct_head->HasPrivateStoreComparison();
		const bool submit_generated_transactions =
			require_vertex_visibility &&
			s_pending_mtvu_generated_transaction;
		const u64 generated_handoff_sequence =
			submit_generated_transactions ?
				s_pending_mtvu_direct_tail->ordering_sequence : 0u;
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		const u32 published_count = s_pending_mtvu_direct_count;
#endif
		PublishMtvuPath1Completion({
			s_pending_mtvu_direct_head,
			s_pending_mtvu_direct_reservations,
			nullptr,
			cpu_path1_after_draws,
			submit_generated_transactions,
		});
		if (submit_generated_transactions)
		{
			VitaGS::UpdateGeneratedGpuVuHandoffWatchdog(
				VitaGS::GeneratedGpuVuHandoffStage::CompletionPublished,
				generated_handoff_sequence);
		}
		s_pending_mtvu_direct_head = nullptr;
		s_pending_mtvu_direct_tail = nullptr;
		s_pending_mtvu_direct_count = 0;
		s_pending_mtvu_direct_reservations = 0;
		s_pending_mtvu_generated_transaction = false;
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		s_gpu_vu_path1_pending_direct_count.store(0, std::memory_order_relaxed);
		s_gpu_vu_path1_pending_generated.store(0, std::memory_order_relaxed);
		s_gpu_vu_path1_last_publication_kind.store(
			require_vertex_visibility ? 2u : 3u, std::memory_order_relaxed);
		s_gpu_vu_path1_last_publication_count.store(
			published_count, std::memory_order_release);
#endif
		PublishPendingMtvuNoOutput();
	}

	static u32 PublishPendingMtvuDirectPrefix()
	{
		if (!s_pending_mtvu_direct_head)
			PublishPendingMtvuNoOutput();
		if (!s_pending_mtvu_direct_head ||
			!s_pending_mtvu_generated_transaction ||
			s_pending_mtvu_direct_count <= 1u)
		{
			return 0u;
		}

		// A generated transaction is submitted only after MTGS consumes a
		// completion carrying submit_generated_transactions_after_draws.  An
		// asynchronous prefix must therefore retain the final generated draw in
		// the MTVU-owned tail.  A later strong flush consumes that tail and places
		// the one ordered submission marker after every already-published prefix.
		// Publishing the complete run here would leave no descriptor on which the
		// pressure drain could place that marker, and CPU1 would wait forever for
		// prepared transactions which never reached sceGxmEndScene/a vertex job.
		VitaGpuVu::GpuVuDraw* previous = nullptr;
		VitaGpuVu::GpuVuDraw* last_generated = nullptr;
		VitaGpuVu::GpuVuDraw* before_last_generated = nullptr;
		u32 last_generated_index = 0u;
		u32 prefix_reservations = 0u;
		u32 scanned_reservations = 0u;
		u32 index = 0u;
		for (VitaGpuVu::GpuVuDraw* draw = s_pending_mtvu_direct_head;
			draw; draw = draw->path1_next, index++)
		{
			if (draw->HasGeneratedLoopKernelTransaction())
			{
				last_generated = draw;
				before_last_generated = previous;
				last_generated_index = index;
				prefix_reservations = scanned_reservations;
			}
			scanned_reservations += draw->path1_leading_no_output_count + 1u;
			previous = draw;
		}

		if (!last_generated || !before_last_generated ||
			last_generated_index == 0u)
		{
			return 0u;
		}

		VitaGpuVu::GpuVuDraw* const prefix_head =
			s_pending_mtvu_direct_head;
		const u32 prefix_count = last_generated_index;
		if (!VitaGpuVu::PublishPendingRawVifPayloads(
				prefix_head, prefix_count))
		{
			// Keep the list intact until publication succeeds so the fail-stop can
			// release the complete pending owner in one bounded pass.
			FailPendingMtvuDirectInputPublication("asynchronous-prefix");
			return 0u;
		}
		before_last_generated->path1_next = nullptr;
		PublishMtvuPath1Completion({
			prefix_head,
			prefix_reservations,
			nullptr,
			prefix_head->HasPrivateStoreComparison(),
			false,
		});

		s_pending_mtvu_direct_head = last_generated;
		s_pending_mtvu_direct_count -= prefix_count;
		s_pending_mtvu_direct_reservations -= prefix_reservations;
		// last_generated itself proves the retained suffix still owns a
		// generated transaction and therefore requires the final submit marker.
		s_pending_mtvu_generated_transaction = true;
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		s_gpu_vu_path1_pending_direct_count.store(
			s_pending_mtvu_direct_count, std::memory_order_relaxed);
		s_gpu_vu_path1_pending_generated.store(1, std::memory_order_relaxed);
		s_gpu_vu_path1_last_publication_kind.store(1, std::memory_order_relaxed);
		s_gpu_vu_path1_last_publication_count.store(
			prefix_count, std::memory_order_release);
#endif
		return prefix_count;
	}

	static void AppendPendingMtvuDirectDraw(VitaGpuVu::GpuVuDraw* draw)
	{
		pxAssertRel(draw && !draw->path1_next &&
			!draw->path1_leading_no_output_count,
			"invalid direct draw appended to MTVU PATH1 run");
		if (!draw)
			return;
		// Never mix visible direct output and invisible private attestation in
		// one completion run. The latter has one accompanying CPU PATH1 packet
		// per real draw; interleaved no-output reservations have no CPU packet.
		if (s_pending_mtvu_direct_tail &&
			s_pending_mtvu_direct_tail->HasPrivateStoreComparison() !=
				draw->HasPrivateStoreComparison())
		{
			FlushPendingMtvuDirectRun(true);
		}
		draw->path1_leading_no_output_count = s_pending_mtvu_no_output_count;
		s_pending_mtvu_direct_reservations += s_pending_mtvu_no_output_count + 1u;
		s_pending_mtvu_no_output_count = 0;
		if (s_pending_mtvu_direct_tail)
			s_pending_mtvu_direct_tail->path1_next = draw;
		else
			s_pending_mtvu_direct_head = draw;
		s_pending_mtvu_direct_tail = draw;
		s_pending_mtvu_direct_count++;
		s_pending_mtvu_generated_transaction |=
			draw->HasGeneratedLoopKernelTransaction();
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		s_gpu_vu_path1_pending_direct_count.store(
			s_pending_mtvu_direct_count, std::memory_order_relaxed);
		s_gpu_vu_path1_pending_generated.store(
			s_pending_mtvu_generated_transaction ? 1u : 0u,
			std::memory_order_release);
#endif
		if (s_pending_mtvu_direct_reservations >= MaximumPendingMtvuDirectRun)
			FlushPendingMtvuDirectRun(true);
	}

	static void AppendPendingMtvuNoOutput()
	{
		// PCSX2 MTVU.cpp::ExecuteRingBuffer / Gif_Path::FinishGSPacketMTVU
		// publish one completion even with gsPack.size == 0. Coalesce that credit
		// into the following draw, keeping the current generated batch intact.
		++s_pending_mtvu_no_output_count;
		if (s_pending_mtvu_direct_reservations + s_pending_mtvu_no_output_count >=
			MaximumPendingMtvuDirectRun)
		{
			FlushPendingMtvuDirectRun(true);
		}
	}

	static VitaGpuVu::GpuVuDraw* TakeMtvuDirectPrefix(
		VitaGpuVu::GpuVuDraw* draw, u32 reservations,
		std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>>& draws)
	{
		while (draw && reservations != 0)
		{
			const u32 empty_prefix = std::min(reservations,
				draw->path1_leading_no_output_count);
			draw->path1_leading_no_output_count -= empty_prefix;
			reservations -= empty_prefix;
			if (reservations == 0)
				break;
			--reservations;
			VitaGpuVu::GpuVuDraw* const next = draw->path1_next;
			draw->path1_next = nullptr;
			draws.emplace_back(draw);
			draw = next;
		}
		pxAssertRel(reservations == 0,
			"MTVU PATH1 direct prefix is shorter than its logical reservations");
		return draw;
	}

	static bool TryTakeMtvuPath1Completion(
		MtvuPath1Completion* completion)
	{
		if (s_mtvu_path1_completions.Peek(completion))
			return true;

		// Publish the sleeping intent before retrying so a producer cannot place
		// a completion between the empty observation and the sleep without also
		// waking this worker.
		s_mtvu_path1_completion_waiting.store(
			true, std::memory_order_release);
		if (s_mtvu_path1_completions.Peek(completion))
		{
			s_mtvu_path1_completion_waiting.store(
				false, std::memory_order_release);
			return true;
		}
#if defined(__vita__)
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			s_profile_mtvu_path1_completion_deferrals.fetch_add(
				1, std::memory_order_relaxed);
			s_gs_worker_performance.mtvu_path1_completion_deferrals++;
		}
#endif
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		s_mtvu_path1_completion_deferrals_for_validation.fetch_add(
			1, std::memory_order_relaxed);
#endif
		return false;
	}

	static void ApplyVitaGsSettings(Pcsx2Config::GSOptions& options)
	{
#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
	#if defined(VITASX2_VITA_SOFTWARE_GS_CONTROL) && \
		VITASX2_VITA_SOFTWARE_GS_CONTROL
		// PCSX2 owner: GS.cpp::OpenGSRenderer() passes the configured worker
		// count to makeGSRendererSW(). Keep EE on USER_0 and split scanline bands
		// over USER_1/USER_2; CPU3 remains reserved for the shell and plugins.
		options.Renderer = GSRendererType::SW;
		options.SWExtraThreads = 2;
	#else
		options.Renderer = GSRendererType::Auto;
	#endif
		options.UpscaleMultiplier = 1.0f;
		options.DumpReplaceableTextures = false;
		options.LoadTextureReplacements = false;
		options.GPUPaletteConversion = false;
		// PCSX2 owner: GSRendererHW::PossibleCLUTDraw() and the
		// UserHacks_CPUCLUTRender fallback. The GXM backend cannot yet consume a
		// palette drawn into a host render target (GSClut's GPU-target branch is
		// deliberately disabled on Vita), so conservatively render only draws
		// which PCSX2 identifies as CLUT updates into GS local memory.
		if (options.UserHacks_CPUCLUTRender == 0)
			options.UserHacks_CPUCLUTRender = 1;
#endif
	}

	static u8 GsTraceSourceForGifPath(GIF_PATH path)
	{
		switch (path)
		{
			case GIF_PATH_1: return Pcsx2Trace::GsTraceSourcePath1;
			case GIF_PATH_2: return Pcsx2Trace::GsTraceSourcePath2;
			case GIF_PATH_3: return Pcsx2Trace::GsTraceSourcePath3;
			default: return Pcsx2Trace::GsTraceSourcePath3;
		}
	}

	static void CloseGsOnWorker()
	{
		// PCSX2 owners: GS.cpp::CloseGSRenderer() and CloseGSDevice(). The
		// texture cache must release every device object while GXM is still live.
		s_gs = nullptr;
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		s_qemu_gs.reset();
#else
		if (g_gs_device &&
			static_cast<GSDeviceGXM*>(g_gs_device.get())->HasFatalGpuFault())
		{
			// Do not destroy renderer textures before GSDeviceGXM gets a chance to
			// reject unsafe user-space teardown. Their mappings may still be visible
			// to the wedged firmware job. This terminates only VitaSX2, not the shell
			// or the system, and deliberately bypasses all C++ GPU resource owners.
			Console.Error(
				"GPU-VU FATAL close_intercept=1; exiting VitaSX2 before "
				"renderer or GXM resource teardown.");
			(void)VitaGpuVu::HealthJournal::FlushForFatal();
			sceKernelExitProcess(1);
			return;
		}
		GSTextureReplacements::Shutdown();
		if (g_gs_renderer)
		{
			g_gs_renderer->Destroy();
			g_gs_renderer.reset();
		}
		if (g_gs_device)
		{
			g_gs_device->Destroy();
			g_gs_device.reset();
		}
#endif
	}

	static bool OpenGsOnWorker()
	{
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		pxAssert(!s_qemu_gs && !s_gs);
#else
		pxAssert(!g_gs_renderer && !g_gs_device && !s_gs);
#endif
		GSConfig = EmuConfig.GS;
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		GSConfig.Renderer = GSRendererType::SW;
		GSConfig.SWExtraThreads = 0;
#else
		// Apply the same Vita constraints before renderer construction and during
		// later settings updates. GSRendererHW snapshots several settings while it
		// builds its caches; deferring these until ApplySettings() is too late.
		ApplyVitaGsSettings(GSConfig);
#endif

		// PCSX2 owner: GS/GS.cpp::OpenGSRenderer(). The software vertex
		// conversion table is process-global and must be populated before the
		// first decoded draw, including GSRendererHW's owned CPU fallbacks.
		GSVertexSW::InitStatic();

#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
		// GSLocalMemory is constructed after GSDeviceGXM by PCSX2's renderer
		// lifecycle, but its canonical 4 MiB store is mandatory. Reserve that
		// store first so GXM's optional GPU-VU ring cannot consume or fragment
		// the final range it needs.
		if (!VitaGS::ReserveCanonicalLocalMemory())
			return false;

		// PCSX2 owner: GS.cpp::OpenGSDevice(). GSDeviceGXM owns the process's
		// only immediate GXM context and must precede GSRendererHW/GSTextureCache.
		g_gs_device = std::make_unique<GSDeviceGXM>();
		if (!g_gs_device->Create(VMManager::GetEffectiveVSyncMode(),
			VMManager::ShouldAllowPresentThrottle()))
		{
			g_gs_device->Destroy();
			g_gs_device.reset();
			VitaGS::ReleaseUnclaimedCanonicalLocalMemory();
			return false;
		}
#endif

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		s_qemu_gs = std::make_unique<VitaGxmGsState>(false);
		s_gs = s_qemu_gs.get();
#else
		auto renderer = std::make_unique<VitaGxmGsState>(
			s_native_presenter_enabled);
		s_gs = renderer.get();
		g_gs_renderer = std::move(renderer);
		if (!s_gs->IsNativePresenterReady())
		{
			CloseGsOnWorker();
			VitaGS::ReleaseUnclaimedCanonicalLocalMemory();
			return false;
		}
#endif
		s_gs->SetRegsMem(s_ring.regs);
		s_gs->ResetPCRTC();
#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
		s_gs->UpdateRenderFixes();
#endif
		// PCSX2 owner: GS/GS.cpp::OpenGSRenderer(). Construction establishes
		// GSState; the MTGS::ResetGS() caller applies the requested hardware or
		// soft reset. Repeating a hardware reset here was both redundant and
		// observably different from the renderer lifecycle owner.
		g_perfmon.Reset();
		return true;
	}

	static void ProcessVSync(u32 field, bool registers_written)
	{
		if (!s_gs)
			return;
#if defined(__vita__)
		const bool profile_vsync = VitaPerformanceTelemetry::IsEnabled();
		const Common::Timer::Value vsync_started = profile_vsync ?
			Common::Timer::GetCurrentValue() : 0;
#endif

		s_gs->PCRTCDisplays.SetVideoMode(s_gs->GetVideoMode());
		s_gs->PCRTCDisplays.EnableDisplays(s_gs->m_regs->PMODE,
			s_gs->m_regs->SMODE2, s_gs->isReallyInterlaced());
		s_gs->PCRTCDisplays.SetRects(0, s_gs->m_regs->DISP[0].DISPLAY,
			s_gs->m_regs->DISP[0].DISPFB);
		s_gs->PCRTCDisplays.SetRects(1, s_gs->m_regs->DISP[1].DISPLAY,
			s_gs->m_regs->DISP[1].DISPFB);
		s_gs->PCRTCDisplays.CheckSameSource();
		s_gs->PCRTCDisplays.CalculateDisplayOffset(s_gs->m_scanmask_used);
		s_gs->PCRTCDisplays.CalculateFramebufferOffset(s_gs->m_scanmask_used,
			s_gs->m_regs->DISP[0].DISPFB, s_gs->m_regs->DISP[1].DISPFB);
		s_gs->Flush(GSState::VSYNC);
		const bool idle_frame = s_gs->IsIdleFrame();
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		const bool framebuffer_sprite_frame =
			g_perfmon.GetDisplayFramebufferSpriteBlits() > 0;
		s_gs->VSync(field);
		g_perfmon.EndFrame(idle_frame);
		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();
		PerformanceMetrics::Update(registers_written,
			framebuffer_sprite_frame, false);
#else
		// PCSX2 owner: GS.cpp::GSvsync(). GSRendererHW::VSync() owns texture-cache
		// aging, PCRTC merge, presentation, perfmon and frame metrics.
		s_gs->VSync(field, registers_written, idle_frame);
#endif
#if defined(__vita__)
		if (profile_vsync)
		{
			const u64 elapsed_us = static_cast<u64>(
				Common::Timer::ConvertValueToSeconds(
					Common::Timer::GetCurrentValue() - vsync_started) * 1000000.0);
			s_gs_worker_performance.vsync_calls++;
			s_gs_worker_performance.vsync_wall_us += elapsed_us;
		}
		// Close this guest frame's direct descriptor epoch before any later
		// command can extend the scene which owns their inputs.
		if (g_gs_device)
			static_cast<GSDeviceGXM*>(g_gs_device.get())->EndGpuVuEpoch();
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			VitaGxmPublishPerformanceCounters();
			s_gs_worker_performance.completed_vsyncs++;
			PublishGsWorkerPerformance();
			RecordHardwareVsyncProfile(s_worker_profile, "worker");
		}
#endif
		// PCSX2 owner: GS.cpp::GSvsync() snapshots after Flush() and VSync().
		s_gs->TraceGsStateSnapshot(Pcsx2Trace::GsTraceStateTriggerVSyncStart);
	}

	static void ThreadEntryPoint()
	{
		Threading::SetNameOfCurrentThread("GS");
		FPControlRegister::SetCurrent(FPControlRegister::GetDefault());

		for (;;)
		{
			while (!s_open_flag.load(std::memory_order_acquire))
			{
				if (s_shutdown_flag.load(std::memory_order_acquire))
				{
					s_work_sema.Kill();
					return;
				}
				s_work_sema.WaitForWork();
			}

			std::memcpy(s_ring.regs, g_RealGSMem, sizeof(s_ring.regs));
			const bool opened = OpenGsOnWorker();
			s_open_flag.store(opened, std::memory_order_release);
			s_open_or_close_done.Post();
			if (!opened)
				continue;

			MainLoop();
			pxAssertRel(!s_open_flag.load(std::memory_order_relaxed),
				"GS worker returned while still open");
			s_mtvu_path1_completions.ResetAndDiscard();
			s_mtvu_path1_completion_waiting.store(
				false, std::memory_order_relaxed);
			CloseGsOnWorker();
			// MainLoop kills WorkSema to release any waiter. Reset it before the
			// close acknowledgement so an immediate reopen cannot lose its wakeup.
			s_work_sema.Reset();
			s_open_or_close_done.Post();
		}
	}

	const Threading::ThreadHandle& GetThreadHandle()
	{
		return s_thread;
	}

	bool IsOpen()
	{
		return s_open_flag.load(std::memory_order_acquire);
	}

	void StartThread()
	{
		if (s_thread.Joinable())
			return;

#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		StartUniversalGpuVuNotificationThread();
#endif

		pxAssertRel(!IsOpen(), "GS worker should be closed when starting");
		s_read_pos.store(0, std::memory_order_relaxed);
		s_write_pos.store(0, std::memory_order_relaxed);
		for (std::atomic<u32>& run : s_mtvu_reservation_runs)
			run.store(0, std::memory_order_relaxed);
		s_open_mtvu_reservation_run = NoOpenMtvuReservationRun;
		s_mtvu_reservations_remaining = 0;
		s_mtvu_path1_completions.ResetAndDiscard();
		s_pending_mtvu_direct_head = nullptr;
		s_pending_mtvu_direct_tail = nullptr;
		s_pending_mtvu_direct_count = 0;
		s_pending_mtvu_direct_reservations = 0;
		s_pending_mtvu_no_output_count = 0;
		s_pending_mtvu_generated_transaction = false;
		s_mtvu_path1_completion_waiting.store(
			false, std::memory_order_relaxed);
		s_mtvu_path1_drain_waiter.store(false, std::memory_order_relaxed);
		pxAssertRel(!s_mtvu_path1_buffer_progress.IsWaitingForValidation(),
			"MTVU PATH1 buffer waiter survived GS worker shutdown");
		s_gpu_vu_compiler_result_pending.store(false,
			std::memory_order_relaxed);
		s_gpu_vu_input_retirement_pending.store(false,
			std::memory_order_relaxed);
		s_gpu_vu_input_retirement_owner.store(0,
			std::memory_order_relaxed);
		s_gpu_vu_input_retirement_slot.store(0,
			std::memory_order_relaxed);
		s_gpu_vu_input_retirement_generation.store(0,
			std::memory_order_relaxed);
		s_work_sema.Reset();
		s_shutdown_flag.store(false, std::memory_order_release);
		s_thread.SetStackSize(256 * 1024);
		if (!s_thread.Start(&ThreadEntryPoint))
		{
			Console.Error("Failed to start the Vita GS worker.");
			return;
		}
#if defined(__vita__)
		// PCSX2 owner: VMManager::SetEmuThreadAffinities(). Share USER_1 with
		// GS only when MTVU is disabled; otherwise reserve USER_1 for VU1 and
		// move GS to USER_2. A rejected affinity is non-fatal.
		const u64 gs_affinity = 1u << (THREAD_VU1 ? 2 : 1);
		if (!s_thread.SetAffinity(gs_affinity))
			Console.Warning("Vita GS worker affinity was rejected; using the scheduler default.");
#endif
	}

	void ShutdownThread()
	{
		if (s_thread.Joinable())
		{
			s_shutdown_flag.store(true, std::memory_order_release);
			if (IsOpen())
				WaitForClose();
			s_work_sema.NotifyOfWork();
			s_thread.Join();
		}
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		StopUniversalGpuVuNotificationThread();
#endif
	}

	static void SetEvent()
	{
		s_work_sema.NotifyOfWork();
		s_copy_data_tally = 0;
	}

#if defined(__vita__)
	static void PollGpuVuProgramsOnOwner(bool force)
	{
		if (!g_gs_device)
			return;
		// The relaxed load is only a cheap hint on ordinary command boundaries;
		// the exchange acquires the compiler's publication before Poll().
		if (!force &&
			!s_gpu_vu_compiler_result_pending.load(std::memory_order_relaxed))
			return;
		s_gpu_vu_compiler_result_pending.exchange(false,
			std::memory_order_acquire);
		static_cast<GSDeviceGXM*>(g_gs_device.get())->PollGpuVuPrograms();
	}

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	static bool ServiceGpuVuNotificationTimeoutOnOwner()
	{
		const u32 request = s_gpu_vu_notification_timeout_request.exchange(
			0, std::memory_order_acquire);
		if (request == 0)
			return false;

		const uptr address = s_gpu_vu_notification_address.load(
			std::memory_order_relaxed);
		const u32 required_value = s_gpu_vu_notification_value.load(
			std::memory_order_relaxed);
		const u32 observed_value = s_gpu_vu_notification_timeout_observed.load(
			std::memory_order_relaxed);
		const u64 sequence = s_gpu_vu_notification_sequence.load(
			std::memory_order_relaxed);
		const u64 elapsed_us = s_gpu_vu_notification_timeout_elapsed_us.load(
			std::memory_order_relaxed);
		if (g_gs_device)
		{
			static_cast<GSDeviceGXM*>(g_gs_device.get())->
				HandleGpuVuNotificationTimeout(address, required_value,
					observed_value, sequence, elapsed_us);
		}
		return true;
	}
#endif

	static void ServiceGpuVuInputRetirementOnOwner()
	{
		if (!s_gpu_vu_input_retirement_pending.exchange(
				false, std::memory_order_acquire))
		{
			return;
		}
		VitaGpuVu::RawVifPayloadRef blocked_generation;
		blocked_generation.owner =
			s_gpu_vu_input_retirement_owner.load(
				std::memory_order_relaxed);
		blocked_generation.slot =
			s_gpu_vu_input_retirement_slot.load(
				std::memory_order_relaxed);
		blocked_generation.generation =
			s_gpu_vu_input_retirement_generation.load(
				std::memory_order_relaxed);
		blocked_generation.size = 1;
		const bool handled = g_gs_device &&
			static_cast<GSDeviceGXM*>(g_gs_device.get())->
				WaitForGpuVuInputRetirement(blocked_generation);
		const u32 remaining =
			VitaGpuVu::GetRawVifPayloadGenerationReferenceCount(
				blocked_generation);
		if (remaining != 0)
		{
			// Ordered work not yet consumed by this owner can still hold the
			// requested generation. Keep the request armed; its completion
			// publication wakes this worker, which retries after draining the
			// newly visible descriptors. Never turn this into a polling loop.
			s_gpu_vu_input_retirement_pending.store(
				true, std::memory_order_release);
		}
		if (!handled && remaining != 0)
		{
			// This is expected only while a preceding MTVU completion is still
			// in flight. The retained request above follows that ordered work.
			return;
		}
	}
#endif

#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	enum class GpuVuHealthMtgsStage : u32
	{
		BeforeWait = 1u,
		AfterWake,
		OwnerPoll,
		OwnerPollReturned,
		Path1Dispatch,
		Path1CompletionMissing,
		Path1CompletionClaimed,
		Path1HeadRetained,
		Path1HeadAdvanced,
		Idle,
		Shutdown,
	};

	static void PublishGpuVuMtgsHealth(GpuVuHealthMtgsStage stage,
		u32 current_command = 0u, u64 sequence = 0u)
	{
		VitaGpuVu::HealthJournal::MtgsState health;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		health.update_time_us = static_cast<u64>(
			Common::Timer::ConvertValueToSeconds(now) * 1000000.0);
		health.stage_started_time_us = health.update_time_us;
		health.ticket = s_gpu_vu_submission_watchdog_active.load(
			std::memory_order_relaxed);
		health.sequence = sequence;
		health.scene = s_gpu_vu_gxm_call_scene.load(std::memory_order_relaxed);
		health.last_completion_sequence = sequence;
		health.stage = static_cast<u32>(stage);
		health.ring_read_position = s_read_pos.load(std::memory_order_relaxed);
		health.ring_write_position = s_write_pos.load(std::memory_order_relaxed);
		health.current_command = current_command;
		health.reservations_remaining = s_mtvu_reservations_remaining;
		health.completion_read_position = s_mtvu_path1_completions.ReadIndex();
		health.completion_write_position = s_mtvu_path1_completions.WriteIndex();
		health.completion_pending = s_mtvu_path1_completions.PendingCount();
		health.completion_logical_count =
			s_gpu_vu_path1_last_completion_count.load(std::memory_order_relaxed);
		health.completion_physical_count = s_pending_mtvu_direct_count;
		health.submission_marker_result =
			s_gpu_vu_path1_last_completion_submit.load(std::memory_order_relaxed);
		health.flags =
			(s_pending_mtvu_generated_transaction ? 1u : 0u) |
			(s_mtvu_path1_completion_waiting.load(std::memory_order_relaxed) ?
				(1u << 1u) : 0u);
		VitaGpuVu::HealthJournal::PublishMtgs(health);
	}
#endif

	static void MainLoop()
	{
#if defined(__vita__)
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();
#endif
		while (true)
		{
	#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
		VITASX2_GPU_VU_UNIVERSAL_VALIDATION
			PublishGpuVuMtgsHealth(GpuVuHealthMtgsStage::BeforeWait);
			VitaGpuVu::HealthJournal::AddHandoffCounter(
				VitaGpuVu::HealthJournal::HandoffCounter::OwnerPasses);
	#endif
			s_work_sema.WaitForWork();
	#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
		VITASX2_GPU_VU_UNIVERSAL_VALIDATION
			PublishGpuVuMtgsHealth(GpuVuHealthMtgsStage::AfterWake);
	#endif

			if (!s_open_flag.load(std::memory_order_acquire))
				break;

#if defined(__vita__)
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
			// The bridge owns no libGXM state. Transfer a no-progress report to
			// this context-owning thread, then leave the remaining mailbox work
			// untouched: the device is fail-stopped and no later GXM command may
			// observe or release its indeterminate in-flight resources.
			if (ServiceGpuVuNotificationTimeoutOnOwner())
				continue;
#endif
			// Completed ShaccCg output is registered only here, on the thread
			// which owns the immediate context and shader patcher. Compilation
			// itself never blocks this worker.
	#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
		VITASX2_GPU_VU_UNIVERSAL_VALIDATION
			PublishGpuVuMtgsHealth(GpuVuHealthMtgsStage::OwnerPoll);
	#endif
			PollGpuVuProgramsOnOwner(true);
	#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
		VITASX2_GPU_VU_UNIVERSAL_VALIDATION
			PublishGpuVuMtgsHealth(GpuVuHealthMtgsStage::OwnerPollReturned);
	#endif
#endif

			while (s_read_pos.load(std::memory_order_relaxed) !=
				s_write_pos.load(std::memory_order_acquire))
			{
				const u32 read_pos = s_read_pos.load(std::memory_order_relaxed);
				const PacketTag& tag = reinterpret_cast<const PacketTag&>(s_ring[read_pos]);
				u32 ring_advance = 1;
				bool command_progress = false;
#if defined(__vita__)
				const bool continuing_mtvu_reservation =
					static_cast<Command>(tag.command) ==
							Command::MTVUGSPacket &&
						s_mtvu_reservations_remaining != 0;
				if (performance_telemetry_enabled &&
					!continuing_mtvu_reservation)
					s_gs_worker_performance.commands++;
#endif

				switch (static_cast<Command>(tag.command))
				{
					case Command::GSPacket:
					{
						const GIF_PATH path_index = static_cast<GIF_PATH>(tag.data[2]);
						Gif_Path& path = gifUnit.gifPath[path_index];
						const u32 offset = tag.data[0];
						const u32 size = tag.data[1];
#if defined(__vita__)
						if (performance_telemetry_enabled)
						{
							s_gs_worker_performance.gs_packets++;
							s_gs_worker_performance.gs_packet_bytes += size;
						}
#endif
						if (s_gs && offset != ~0u)
						{
							const Pcsx2Trace::ScopedGsTraceSourceOverride trace_source(
								GsTraceSourceForGifPath(path_index));
						#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
							VitaGsDrawTraceRecordPacket(
								GsTraceSourceForGifPath(path_index), &path.buffer[offset],
								size, false);
						#endif
							TransferGsPacket(&path.buffer[offset], size / 16);
						}
						path.readAmount.fetch_sub(size, std::memory_order_acq_rel);
						break;
					}

					case Command::MTVUGSPacket:
					{
	#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
		VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						PublishGpuVuMtgsHealth(
							GpuVuHealthMtgsStage::Path1Dispatch, tag.command);
	#endif
						MtvuPath1Completion completion;
						if (!TryTakeMtvuPath1Completion(&completion))
						{
							// Retain the reservation at the head of the MTGS
							// ring. The ordinary worker semaphore sleeps until
							// the single MTVU producer publishes its immutable
							// completion; no per-dispatch semaphore resource is
							// consumed.
							ring_advance = 0;
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
							s_gpu_vu_path1_last_ring_advance.store(
								0, std::memory_order_release);
							PublishGpuVuMtgsHealth(
								GpuVuHealthMtgsStage::Path1CompletionMissing,
								tag.command);
#endif
							break;
						}
						pxAssertRel(completion.reservation_count != 0,
							"MTVU PATH1 completion has no EE reservation");
						if (completion.reservation_count == 0)
							break;
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						const u64 completion_sequence = completion.first_draw ?
							completion.first_draw->ordering_sequence :
							(completion.universal_epoch ?
								completion.universal_epoch->Sequence() : 0u);
						PublishGpuVuMtgsHealth(
							GpuVuHealthMtgsStage::Path1CompletionClaimed,
							tag.command, completion_sequence);
#endif
						if (completion.universal_epoch)
						{
							pxAssertRel(!completion.first_draw &&
								completion.reservation_count ==
									completion.universal_epoch->ExecuteCount(),
								"universal GPU-VU completion does not own every "
								"Execute ordering point");
								VitaGpuVu::UniversalGpuVuEpochStage stage =
									completion.universal_epoch->Stage();
							if ((stage ==
									 VitaGpuVu::UniversalGpuVuEpochStage::Prepared ||
								 stage ==
									 VitaGpuVu::UniversalGpuVuEpochStage::Submitted) &&
								s_gs)
							{
									s_gs->ServiceUniversalGpuVuEpoch(
										completion.universal_epoch);
									stage = completion.universal_epoch->Stage();
								}
									while (stage ==
										VitaGpuVu::UniversalGpuVuEpochStage::Submitted &&
										s_gs && UniversalGpuVuNotificationReached(
											*completion.universal_epoch))
									{
										// Consume only notifications which are already visible. A
										// continuation may publish a new value here; never wait for
										// it on MTGS.
										s_gs->ServiceUniversalGpuVuEpoch(
											completion.universal_epoch);
										stage = completion.universal_epoch->Stage();
									}
									const UniversalGpuVuNotificationArmResult arm_result =
										stage == VitaGpuVu::UniversalGpuVuEpochStage::Submitted ?
											ArmUniversalGpuVuNotificationWake(
												*completion.universal_epoch) :
											UniversalGpuVuNotificationArmResult::Armed;
									if (arm_result ==
										UniversalGpuVuNotificationArmResult::Failed)
									{
										// Admission is gated on the bridge before submission, so
										// this is an internal ownership failure after private GPU
										// work began. Preserve order and fail closed; do not replay
										// partially completed work on CPU.
										Console.Error(
											"GPU-VU seq=%llu could not arm asynchronous "
											"notification wake; retaining private transaction.",
											static_cast<unsigned long long>(
												completion.universal_epoch->Sequence()));
									}
									stage = completion.universal_epoch->Stage();
								if (stage ==
										VitaGpuVu::UniversalGpuVuEpochStage::Accepted ||
									stage ==
										VitaGpuVu::UniversalGpuVuEpochStage::GpuRejected)
								{
									vu1Thread.NotifyUniversalGpuVuProgress();
								}
								if (stage ==
										VitaGpuVu::UniversalGpuVuEpochStage::Accepted &&
									!completion.universal_epoch->AcceptedStateAcquired())
								{
									// CPU1 must adopt the mapped generation before PATH1
									// retirement can make its slot reusable.
									ring_advance = 0;
									break;
								}
								if (stage ==
									VitaGpuVu::UniversalGpuVuEpochStage::Prepared ||
								stage ==
									VitaGpuVu::UniversalGpuVuEpochStage::Completing ||
								stage ==
									VitaGpuVu::UniversalGpuVuEpochStage::Submitted ||
								stage ==
									VitaGpuVu::UniversalGpuVuEpochStage::GpuRejected)
							{
								// Prepared/Submitted waits for the persistent GXM owner;
								// GpuRejected waits for MTVU's retained-journal replay.
								// In all cases this logical PATH1 point remains the ring
								// head, so no later GS work can pass uncommitted output.
								ring_advance = 0;
								break;
							}
							if (stage ==
									VitaGpuVu::UniversalGpuVuEpochStage::Retired)
							{
								pxFailRel("retired universal GPU-VU epoch remained queued");
							}
						}

						// Claim the complete logical count once. If the MTVU
						// completion boundary falls inside this command, keep the
						// command at the ring head and consume the next completion
						// before advancing to any interleaved GS work.
						if (s_mtvu_reservations_remaining == 0)
						{
							const u32 claimed =
								s_mtvu_reservation_runs[read_pos].exchange(
									0, std::memory_order_acq_rel);
							s_mtvu_reservations_remaining =
								claimed & MtvuReservationRunCountMask;
							// A zero state can only come from an old diagnostic
							// producer or corrupt ring publication. Retain the
							// historical one-command/one-reservation behavior so
							// release builds fail closed instead of losing order.
							if (s_mtvu_reservations_remaining == 0)
								s_mtvu_reservations_remaining = 1;
						}
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						const u32 completion_count = completion.reservation_count;
#endif
						const u32 reservation_prefix = std::min(
							completion.reservation_count,
							s_mtvu_reservations_remaining);
						const bool submit_generated_transactions =
							completion.submit_generated_transactions_after_draws &&
							reservation_prefix == completion.reservation_count;
						pxAssertRel(reservation_prefix != 0,
							"MTVU PATH1 command has no logical reservation");
						if (reservation_prefix == 0)
							break;
						s_mtvu_reservations_remaining -= reservation_prefix;
						ring_advance =
							s_mtvu_reservations_remaining == 0 ? 1 : 0;
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
						s_gpu_vu_path1_reservations_remaining.store(
							s_mtvu_reservations_remaining, std::memory_order_relaxed);
						s_gpu_vu_path1_last_completion_count.store(
							completion_count, std::memory_order_relaxed);
						s_gpu_vu_path1_last_completion_prefix.store(
							reservation_prefix, std::memory_order_relaxed);
						s_gpu_vu_path1_last_completion_submit.store(
							submit_generated_transactions ? 1u : 0u,
							std::memory_order_relaxed);
						s_gpu_vu_path1_last_ring_advance.store(
							ring_advance, std::memory_order_release);
#endif
						command_progress = true;
#if defined(__vita__)
						if (performance_telemetry_enabled)
						{
							s_gs_worker_performance.mtvu_packets +=
								reservation_prefix;
						}
#endif
						if (completion.first_draw)
						{
							const bool cpu_path1_after_draws =
								completion.cpu_path1_after_draws;
							std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>>
								draws;
							draws.reserve(reservation_prefix);
							VitaGpuVu::GpuVuDraw* const direct_draw =
								TakeMtvuDirectPrefix(completion.first_draw,
									reservation_prefix, draws);
							for (const auto& draw : draws)
							{
								pxAssertRel(
									draw->HasPrivateStoreComparison() ==
										cpu_path1_after_draws,
									"mixed visible and private GPU-VU completion run");
								VitaGpuVu::RecordGpuVuDrawConsumed();
							}
							const u32 physical_draw_count = static_cast<u32>(draws.size());
							const u64 generated_handoff_sequence =
								submit_generated_transactions && !draws.empty() ?
									draws.back()->ordering_sequence : 0u;
							if (submit_generated_transactions)
							{
								VitaGS::UpdateGeneratedGpuVuHandoffWatchdog(
									VitaGS::GeneratedGpuVuHandoffStage::MtgsConsumed,
									generated_handoff_sequence);
							}
							s_mtvu_path1_completions.ConsumePrefix(
								reservation_prefix, direct_draw);
							// The prefix may end inside the empty gap before a draw.
							// It cannot carry the final marker: that stays on a real
							// generated descriptor retained by prefix publication.
							if (draws.empty())
							{
								pxAssertRel(!submit_generated_transactions,
									"empty prefix consumed a generated submission marker");
								break;
							}
							if (s_gs)
							{
								s_gs->ConsumeGpuVuDraws(std::move(draws));
							}
							else
							{
								for (const auto& draw : draws)
									VitaGpuVu::RecordGpuVuDrawRejected();
							}
							VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
								VitaGS::GpuVuGxmSubmissionStage::MtgsConsumeReturned);
							if (submit_generated_transactions)
							{
								VitaGS::UpdateGeneratedGpuVuHandoffWatchdog(
									VitaGS::GeneratedGpuVuHandoffStage::DrawsEncoded,
									generated_handoff_sequence);
							}
							if (cpu_path1_after_draws)
							{
								VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
									VitaGS::GpuVuGxmSubmissionStage::MtgsCpuPath1Replay);
								Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
								for (u32 packet_index = 0;
									packet_index < physical_draw_count; packet_index++)
								{
									GS_Packet packet;
									if (!path.TryGetGSPacketMTVU(packet))
									{
										pxFailRel(
											"private GPU-VU attestation had no CPU PATH1 packet");
										break;
									}
#if defined(__vita__)
									if (performance_telemetry_enabled)
									{
										s_gs_worker_performance.mtvu_packet_bytes +=
											packet.size;
									}
#endif
									if (s_gs && packet.size)
									{
										const Pcsx2Trace::ScopedGsTraceSourceOverride
											trace_source(Pcsx2Trace::GsTraceSourcePath1);
#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
										VitaGsDrawTraceRecordPacket(
											Pcsx2Trace::GsTraceSourcePath1,
											&path.buffer[packet.offset], packet.size, true);
#endif
										TransferGsPacket(&path.buffer[packet.offset],
											packet.size / 16);
									}
									path.readAmount.fetch_sub(
										packet.size + packet.readAmount,
										std::memory_order_acq_rel);
									path.PopGSPacketMTVU();
								}
								s_mtvu_path1_buffer_progress.NotifyOfProgress();
								VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
									VitaGS::GpuVuGxmSubmissionStage::
										MtgsCpuPath1ReplayReturned);
							}
							VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
								VitaGS::GpuVuGxmSubmissionStage::
									MtgsSubmissionMarkerCheck);
							#if !defined(VITASX2_QEMU_VALIDATION)
							if (submit_generated_transactions && g_gs_device)
							{
								VitaGS::UpdateGeneratedGpuVuHandoffWatchdog(
									VitaGS::GeneratedGpuVuHandoffStage::SubmissionRequested,
									generated_handoff_sequence);
								const bool submitted =
									static_cast<GSDeviceGXM*>(g_gs_device.get())->
										SubmitGeneratedGpuVuTransactions();
								pxAssertRel(submitted,
									"generated GPU-VU run could not submit its transaction");
								if (submitted)
								{
									VitaGS::CompleteGeneratedGpuVuHandoffWatchdog(
										generated_handoff_sequence);
								}
							}
							#else
							(void)submit_generated_transactions;
							#endif
							VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
								VitaGS::GpuVuGxmSubmissionStage::MtgsCommandReturn);
							break;
						}

						if (completion.no_output_only)
						{
							pxAssertRel(!completion.universal_epoch &&
								!completion.cpu_path1_after_draws &&
								!completion.submit_generated_transactions_after_draws,
								"invalid no-output MTVU completion");
							s_mtvu_path1_completions.ConsumePrefix(
								reservation_prefix, nullptr);
							break;
						}

							if (completion.universal_epoch &&
								completion.universal_epoch->Completion().stage ==
									VitaGpuVu::UniversalGpuVuEpochStage::Accepted)
							{
				const Common::Timer::Value retirement_start =
					VitaPerformanceTelemetry::IsEnabled() ?
										Common::Timer::GetCurrentValue() : 0;
							VitaGpuVu::UniversalGpuVuEpoch* const epoch =
								completion.universal_epoch;
							const VitaGpuVu::UniversalRawPath1Export* const output =
								epoch->RawPath1Output();
							pxAssertRel(output,
								"accepted universal GPU-VU epoch has no PATH1 output");
							if (output && s_gs)
							{
								const Pcsx2Trace::ScopedGsTraceSourceOverride trace_source(
									Pcsx2Trace::GsTraceSourcePath1);
								for (u32 packet_index = 0;
									packet_index < output->packet_count; packet_index++)
								{
									const auto& packet = output->packets[packet_index];
									const u8* const packet_data =
										reinterpret_cast<const u8*>(
											output->data.data() +
											packet.output_qword_offset);
								#if defined(VITASX2_GS_DRAW_TRACE) && \
									VITASX2_GS_DRAW_TRACE
									VitaGsDrawTraceRecordPacket(
										Pcsx2Trace::GsTraceSourcePath1, packet_data,
										packet.qword_count * 16u, true);
								#endif
									TransferGsPacket(packet_data, packet.qword_count);
								#if defined(__vita__)
									if (performance_telemetry_enabled)
										s_gs_worker_performance.mtvu_packet_bytes +=
											packet.qword_count * 16u;
								#endif
								}
							}
							pxAssertRel(reservation_prefix ==
								epoch->ExecuteCount(),
								"accepted universal GPU-VU epoch was split across "
								"PATH1 reservation runs");
							s_mtvu_path1_completions.ConsumePrefix(
								reservation_prefix, nullptr);
									pxAssertRel(epoch->MarkRetired(),
										"accepted universal GPU-VU epoch was not retired");
									vu1Thread.NotifyUniversalGpuVuProgress();
								if (retirement_start != 0)
								{
									const u64 retirement_us = static_cast<u64>(
										Common::Timer::ConvertValueToSeconds(
											Common::Timer::GetCurrentValue() -
											retirement_start) * 1000000.0);
									VitaGpuVu::RecordUniversalGpuVuPath1RetirementTime(
										retirement_us);
								}
							break;
						}
						if (completion.universal_epoch &&
							completion.universal_epoch->Completion().stage ==
								VitaGpuVu::UniversalGpuVuEpochStage::CpuFallback)
						{
							VitaGpuVu::UniversalGpuVuEpoch* const epoch =
								completion.universal_epoch;
							pxAssertRel(reservation_prefix == epoch->ExecuteCount(),
								"CPU-replayed universal GPU-VU epoch was split across "
								"PATH1 reservation runs");
							s_mtvu_path1_completions.ConsumePrefix(
								reservation_prefix, nullptr);
							Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
							for (u32 execute_index = 0;
								execute_index < reservation_prefix; execute_index++)
							{
								GS_Packet packet;
								if (!path.TryGetGSPacketMTVU(packet))
								{
									pxFailRel("multi-Execute CPU replay had no PATH1 packet");
									break;
								}
#if defined(__vita__)
								if (performance_telemetry_enabled)
									s_gs_worker_performance.mtvu_packet_bytes += packet.size;
#endif
								if (s_gs && packet.size)
								{
									const Pcsx2Trace::ScopedGsTraceSourceOverride
										trace_source(Pcsx2Trace::GsTraceSourcePath1);
#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
									VitaGsDrawTraceRecordPacket(
										Pcsx2Trace::GsTraceSourcePath1,
										&path.buffer[packet.offset], packet.size, true);
#endif
									TransferGsPacket(&path.buffer[packet.offset],
										packet.size / 16);
								}
								path.readAmount.fetch_sub(
									packet.size + packet.readAmount,
									std::memory_order_acq_rel);
								path.PopGSPacketMTVU();
							}
							s_mtvu_path1_buffer_progress.NotifyOfProgress();
								pxAssertRel(epoch->MarkRetired(),
									"CPU-replayed multi-Execute universal epoch was not retired");
								vu1Thread.NotifyUniversalGpuVuProgress();
							break;
						}

						pxAssertRel(completion.reservation_count == 1,
							"CPU PATH1 completion covered multiple dispatches");
						pxAssertRel(reservation_prefix == 1,
							"CPU PATH1 completion consumed multiple logical "
							"reservations");
						s_mtvu_path1_completions.ConsumePrefix(1, nullptr);
						Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
						GS_Packet packet;
						if (!path.TryGetGSPacketMTVU(packet))
						{
							pxFailRel(
								"MTVU PATH1 packet completion had no packet");
							break;
						}
#if defined(__vita__)
						if (performance_telemetry_enabled)
						{
							s_gs_worker_performance.mtvu_packet_bytes += packet.size;
						}
#endif
						if (s_gs && packet.size)
						{
							const Pcsx2Trace::ScopedGsTraceSourceOverride trace_source(
								Pcsx2Trace::GsTraceSourcePath1);
						#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
							VitaGsDrawTraceRecordPacket(
								Pcsx2Trace::GsTraceSourcePath1,
								&path.buffer[packet.offset], packet.size, true);
						#endif
							TransferGsPacket(&path.buffer[packet.offset], packet.size / 16);
						}
						path.readAmount.fetch_sub(packet.size + packet.readAmount,
							std::memory_order_acq_rel);
						path.PopGSPacketMTVU();
						s_mtvu_path1_buffer_progress.NotifyOfProgress();
						if (completion.universal_epoch)
						{
								pxAssertRel(completion.universal_epoch->MarkRetired(),
									"CPU-replayed universal GPU-VU epoch was not retired");
								vu1Thread.NotifyUniversalGpuVuProgress();
						}
						break;
					}

					case Command::GpuVuDraw:
					{
						std::unique_ptr<VitaGpuVu::GpuVuDraw> draw(
							reinterpret_cast<VitaGpuVu::GpuVuDraw*>(tag.pointer));
						if (!draw)
							break;
						VitaGpuVu::RecordGpuVuDrawConsumed();
						if (s_gs)
							s_gs->ConsumeGpuVuDraw(std::move(draw));
						else
							VitaGpuVu::RecordGpuVuDrawRejected();
						break;
					}

					case Command::VSync:
					{
						const u32 payload_size = tag.data[0];
						RingVSyncSnapshot snapshot = {};
						u32 payload_pos = (read_pos + 1) & RingBufferMask;
						MemCopy_WrappedSrc(s_ring.ring, payload_pos, RingBufferSize,
							reinterpret_cast<u128*>(&snapshot), payload_size);
						ring_advance += payload_size;
						std::memcpy(s_ring.regs, snapshot.regset1,
							sizeof(snapshot.regset1));
						std::memcpy(&s_ring.regs[0x1000], &snapshot.csr,
							sizeof(snapshot.csr));
						std::memcpy(&s_ring.regs[0x1010], &snapshot.imr,
							sizeof(snapshot.imr));
						std::memcpy(&s_ring.regs[0x1080], &snapshot.siglblid,
							sizeof(snapshot.siglblid));
						// PCSX2 MTGS.cpp owns FIELD derivation from CSR bit 0x2000.
						const u32 field = (snapshot.csr & 0x2000u) ? 0u : 1u;
						ProcessVSync(field, snapshot.registers_written != 0);
						s_queued_frame_count.fetch_sub(1, std::memory_order_acq_rel);
						if (s_vsync_signal_listener.exchange(false,
							std::memory_order_acq_rel))
						{
							s_vsync_sema.Post();
						}
						break;
					}

					case Command::AsyncCall:
					{
						auto* function = reinterpret_cast<AsyncCallType*>(tag.pointer);
						if (function)
						{
							(*function)();
							delete function;
						}
						break;
					}

					case Command::Freeze:
					{
						auto* data = reinterpret_cast<FreezeData*>(tag.pointer);
						if (!data || !s_gs)
						{
							if (data)
								data->retval = -1;
							break;
						}
						const FreezeAction mode = static_cast<FreezeAction>(tag.data[0]);
						if (mode == FreezeAction::Save)
							data->retval = s_gs->Freeze(data->fdata, false);
						else if (mode == FreezeAction::Size)
							data->retval = s_gs->Freeze(data->fdata, true);
						else
						{
							// PCSX2 owner: GS.cpp::GSfreeze(). Defrost replaces GS local
							// state, so no cached render target may survive it.
							if (g_gs_device)
								g_gs_device->ClearCurrent();
							data->retval = s_gs->Defrost(data->fdata);
						}
						break;
					}

					case Command::Reset:
						if (s_gs)
							s_gs->Reset(tag.data[0] != 0);
						break;

					case Command::InitAndReadFIFO:
						if (s_gs)
						{
							u8* const memory = reinterpret_cast<u8*>(tag.pointer);
							s_gs->InitReadFIFO(memory, tag.data[0]);
							s_gs->ReadFIFO(memory, tag.data[0]);
						}
						break;

					default:
						pxFailRel("Vita MTGS encountered an invalid command");
						break;
				}

				if (ring_advance == 0)
				{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
					PublishGpuVuMtgsHealth(
						GpuVuHealthMtgsStage::Path1HeadRetained, tag.command);
#endif
					// A counted PATH1 command can make useful progress while a
					// later completion for the same command is already queued.
					// Recheck immediately; if it is not ready, the ordinary
					// completion-wait path above leaves command_progress false
					// and sleeps without spinning.
					if (command_progress)
						continue;
					break;
				}

				const u32 new_read_pos = (read_pos + ring_advance) & RingBufferMask;
				s_read_pos.store(new_read_pos, std::memory_order_release);
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
				if (static_cast<Command>(tag.command) == Command::MTVUGSPacket)
				{
					PublishGpuVuMtgsHealth(
						GpuVuHealthMtgsStage::Path1HeadAdvanced, tag.command);
				}
#endif
				if (s_signal_ring_enabled.load(std::memory_order_acquire) &&
					s_signal_ring_position.fetch_sub(static_cast<int>(ring_advance),
						std::memory_order_acq_rel) <= 0)
				{
					s_signal_ring_enabled.store(false, std::memory_order_release);
					s_ring_reset_sema.Post();
				}
#if defined(__vita__)
				// A hot producer can keep this drain loop non-empty for seconds.
				// The release/acquire pending bit avoids a compiler mutex probe
				// on ordinary commands while still handing completed GXP to the
				// GXM owner at the next command boundary.
				PollGpuVuProgramsOnOwner(false);
#endif
			}

#if defined(__vita__)
			// Process all already-published ordered GS work first. If its
			// direct scene owns every immutable VIF slot, submit/retire that
			// scene on this GXM-owning thread before the producer can continue.
			ServiceGpuVuInputRetirementOnOwner();
#endif
			if (s_signal_ring_enabled.exchange(false, std::memory_order_acq_rel))
			{
				s_signal_ring_position.store(0, std::memory_order_release);
				s_ring_reset_sema.Post();
			}
			if (s_vsync_signal_listener.exchange(false, std::memory_order_acq_rel))
				s_vsync_sema.Post();
			// This release RMW pairs with a waiter which arms after the final
			// packet pop, so a drain-to-empty transition cannot lose its wake.
			s_mtvu_path1_buffer_progress.PublishQuiescence();
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
			PublishGpuVuMtgsHealth(GpuVuHealthMtgsStage::Idle);
#endif
		}

#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		PublishGpuVuMtgsHealth(GpuVuHealthMtgsStage::Shutdown);
#endif
		s_read_pos.store(s_write_pos.load(std::memory_order_acquire),
			std::memory_order_release);
		s_mtvu_path1_buffer_progress.PublishQuiescence();
		s_work_sema.Kill();
	}

	enum class Cpu0HealthStage : u32
	{
		Returned = 1, RingPublish, RingSemaphore, RingSpin,
		GsPublish, GsEmptyWait, GsCompletionWait, VsyncPublish,
		VsyncBudget, VsyncSemaphore
	};
	static void PublishCpu0Health(Cpu0HealthStage stage, u32 requested = 0)
	{
#if defined(__vita__)
		// Observes PCSX2 MTGS::GenericStall/WaitGS/PostVsyncStart boundaries.
		// No foreign private fields, allocation, formatting, or file I/O.
		static VitaGpuVu::HealthJournal::Cpu0State state;
		const u64 now = static_cast<u64>(Common::Timer::ConvertValueToSeconds(
			Common::Timer::GetCurrentValue()) * 1000000.0);
		if (state.stage != static_cast<u32>(stage))
			state.stage_started_time_us = now;
		state.update_time_us = now;
		state.stage = static_cast<u32>(stage);
		state.wait_visits++;
		state.ring_read = s_read_pos.load(std::memory_order_relaxed);
		state.ring_write = s_write_pos.load(std::memory_order_relaxed);
		state.requested_words = requested;
		state.queued_frames = static_cast<u32>(
			s_queued_frame_count.load(std::memory_order_relaxed));
		const auto vu = vu1Thread.GetProducerQueueHealth();
		state.execute_enqueued = vu.enqueued;
		state.execute_completed = vu.completed;
		state.vu_read = vu.read;
		state.vu_published_write = vu.published_write;
		state.vu_private_write = vu.private_write;
		state.pending_vif_batch = vu.pending_vif;
		VitaGpuVu::HealthJournal::PublishCpu0(state);
#endif
	}

	static void GenericStall(u32 size)
	{
#if defined(__vita__)
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();
#endif
		const u32 write_pos = s_write_pos.load(std::memory_order_relaxed);
		pxAssert(size < RingBufferSize);
		u32 read_pos = s_read_pos.load(std::memory_order_acquire);
		u32 free_room = write_pos < read_pos ? read_pos - write_pos :
			RingBufferSize - (write_pos - read_pos);
		if (free_room > size)
			return;
		PublishCpu0Health(Cpu0HealthStage::RingPublish, size);

		// The EE producer may reach this pressure wait from inside VIF1 before
		// VIF1transfer() publishes its complete private MTVU batch.  MTGS is
		// allowed to retain a counted PATH1 reservation until CPU1 publishes the
		// matching completion, so waiting for MTGS ring space first creates a
		// three-owner cycle: CPU0 waits for MTGS, MTGS waits for CPU1, and CPU1
		// cannot observe CPU0's private records.  Publish only on the slow path and
		// request the ordered vertex-visibility boundary before sleeping/spinning.
		if (THREAD_VU1)
		{
			vu1Thread.PublishPendingVifBatch();
			vu1Thread.RequestGpuVuPath1Flush();
		}

#if defined(__vita__)
		if (performance_telemetry_enabled && s_native_presenter_enabled)
			s_profile_ring_stalls.fetch_add(1, std::memory_order_relaxed);
#endif

		u32 completed = (RingBufferSize - free_room) / 4;
		if (completed < size + 1)
			completed = size + 1;
		if (completed > 0x80)
		{
			pxAssert(!s_signal_ring_enabled.load(std::memory_order_relaxed));
			s_signal_ring_position.store(static_cast<int>(completed),
				std::memory_order_release);
			while (true)
			{
				s_signal_ring_enabled.store(true, std::memory_order_release);
				SetEvent();
				PublishCpu0Health(Cpu0HealthStage::RingSemaphore, size);
				s_ring_reset_sema.Wait();
				read_pos = s_read_pos.load(std::memory_order_acquire);
				free_room = write_pos < read_pos ? read_pos - write_pos :
					RingBufferSize - (write_pos - read_pos);
				if (free_room > size)
					break;
			}
		}
		else
		{
			SetEvent();
			PublishCpu0Health(Cpu0HealthStage::RingSpin, size);
			while (true)
			{
				Threading::SpinWait();
#if defined(__vita__)
				if (performance_telemetry_enabled)
					s_gs_producer_performance.ring_spins++;
#endif
				read_pos = s_read_pos.load(std::memory_order_acquire);
				free_room = write_pos < read_pos ? read_pos - write_pos :
					RingBufferSize - (write_pos - read_pos);
				if (free_room > size)
					break;
			}
		}
		PublishCpu0Health(Cpu0HealthStage::Returned);
	}

	static void CloseOpenMtvuReservationRun()
	{
		const u32 slot = s_open_mtvu_reservation_run;
		if (slot == NoOpenMtvuReservationRun)
			return;

		u32 state =
			s_mtvu_reservation_runs[slot].load(std::memory_order_acquire);
		while ((state & MtvuReservationRunOpen) != 0 &&
			!s_mtvu_reservation_runs[slot].compare_exchange_weak(
				state, state & MtvuReservationRunCountMask,
				std::memory_order_release, std::memory_order_acquire))
		{
		}
		s_open_mtvu_reservation_run = NoOpenMtvuReservationRun;
	}

	static void PrepareDataPacket(Command command, u32 size)
	{
		// A data-packet tag is itself an ordered MTGS command. Prevent a later
		// VU dispatch from extending a PATH1 run across it.
		CloseOpenMtvuReservationRun();
		s_packet_size = size;
		GenericStall(size + 1);
		const u32 write_pos = s_write_pos.load(std::memory_order_relaxed);
		PacketTag& tag = reinterpret_cast<PacketTag&>(s_ring[write_pos]);
		tag.command = static_cast<u32>(command);
		tag.data[0] = size;
		s_packet_start_pos = write_pos;
		s_packet_write_pos = (write_pos + 1) & RingBufferMask;
	}

	static void SendDataPacket()
	{
		pxAssert(s_packet_size != 0);
		const u32 actual_size =
			((s_packet_write_pos - s_packet_start_pos) & RingBufferMask) - 1;
		pxAssert(actual_size <= s_packet_size);
		PacketTag& tag = reinterpret_cast<PacketTag&>(s_ring[s_packet_start_pos]);
		tag.data[0] = actual_size;
#if defined(__vita__)
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			s_gs_producer_performance.submissions++;
			s_gs_producer_performance.ring_words += actual_size + 1;
		}
#endif
		s_write_pos.store(s_packet_write_pos, std::memory_order_release);
		if (EmuConfig.GS.SynchronousMTGS)
			WaitGS();
		else
		{
			s_copy_data_tally += static_cast<int>(s_packet_size);
			if (s_copy_data_tally > 0x2000)
				SetEvent();
		}
		s_packet_size = 0;
	}

	static void FinishSimplePacket()
	{
		const u32 future_write_pos =
			(s_write_pos.load(std::memory_order_relaxed) + 1) & RingBufferMask;
		pxAssert(future_write_pos != s_read_pos.load(std::memory_order_acquire));
		s_write_pos.store(future_write_pos, std::memory_order_release);
		if (EmuConfig.GS.SynchronousMTGS)
			WaitGS();
		else
			++s_copy_data_tally;
	}

	static void QueueMtvuPath1Reservation()
	{
		u32 slot = s_open_mtvu_reservation_run;
		if (slot != NoOpenMtvuReservationRun)
		{
			u32 state =
				s_mtvu_reservation_runs[slot].load(std::memory_order_acquire);
			while ((state & MtvuReservationRunOpen) != 0 &&
				(state & MtvuReservationRunCountMask) <
					MaximumMtvuReservationRun)
			{
				const u32 next = state + 1;
				if (s_mtvu_reservation_runs[slot].compare_exchange_weak(
						state, next, std::memory_order_release,
						std::memory_order_acquire))
				{
#if defined(__vita__)
					if (VitaPerformanceTelemetry::IsEnabled())
						s_gs_producer_performance.mtvu_packets++;
#endif
					// Match the historical logical command tally so an idle GS
					// worker is woken at the same guest-work cadence.
					++s_copy_data_tally;
#if defined(__vita__)
					VitaGpuVu::HealthJournal::AddHandoffCounter(
						VitaGpuVu::HealthJournal::HandoffCounter::Reserved);
#endif
					return;
				}
			}
			s_open_mtvu_reservation_run = NoOpenMtvuReservationRun;
		}

		GenericStall(1);
		slot = s_write_pos.load(std::memory_order_relaxed);
		PacketTag& tag = reinterpret_cast<PacketTag&>(s_ring[slot]);
		tag.command = static_cast<u32>(Command::MTVUGSPacket);
		tag.data[0] = 1;
		tag.data[1] = 0;
		tag.data[2] = static_cast<u32>(GIF_PATH_1);
		s_mtvu_reservation_runs[slot].store(
			MtvuReservationRunOpen | 1u, std::memory_order_release);
		s_open_mtvu_reservation_run = slot;
#if defined(__vita__)
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			// submissions/ring_words are physical mailbox work; mtvu_packets
			// remains the architectural logical-reservation count.
			s_gs_producer_performance.submissions++;
			s_gs_producer_performance.ring_words++;
			s_gs_producer_performance.mtvu_packets++;
		}
#endif
		FinishSimplePacket();
#if defined(__vita__)
		VitaGpuVu::HealthJournal::AddHandoffCounter(
			VitaGpuVu::HealthJournal::HandoffCounter::Reserved);
#endif
	}

	static void SendSimplePacket(Command command, u32 data0, u32 data1, u32 data2)
	{
		if (command == Command::MTVUGSPacket)
		{
			QueueMtvuPath1Reservation();
			return;
		}
		CloseOpenMtvuReservationRun();
		GenericStall(1);
		PacketTag& tag = reinterpret_cast<PacketTag&>(
			s_ring[s_write_pos.load(std::memory_order_relaxed)]);
		tag.command = static_cast<u32>(command);
		tag.data[0] = data0;
		tag.data[1] = data1;
		tag.data[2] = data2;
#if defined(__vita__)
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			s_gs_producer_performance.submissions++;
			s_gs_producer_performance.ring_words++;
			if (command == Command::GSPacket)
			{
				s_gs_producer_performance.gs_packets++;
				s_gs_producer_performance.gs_packet_bytes += data1;
			}
		}
#endif
		FinishSimplePacket();
	}

	static void SendSimpleGSPacket(Command command, u32 offset, u32 size,
		GIF_PATH path)
	{
		SendSimplePacket(command, offset, size, static_cast<u32>(path));
		if (!EmuConfig.GS.SynchronousMTGS)
		{
			s_copy_data_tally += static_cast<int>(size / 16);
			if (s_copy_data_tally > 0x2000)
				SetEvent();
		}
	}

	static void SendPointerPacket(Command command, u32 data0, void* pointer)
	{
		CloseOpenMtvuReservationRun();
		GenericStall(1);
		PacketTag& tag = reinterpret_cast<PacketTag&>(
			s_ring[s_write_pos.load(std::memory_order_relaxed)]);
		tag.command = static_cast<u32>(command);
		tag.data[0] = data0;
		tag.pointer = reinterpret_cast<uptr>(pointer);
#if defined(__vita__)
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			s_gs_producer_performance.submissions++;
			s_gs_producer_performance.ring_words++;
		}
#endif
		FinishSimplePacket();
	}

	void PresentCurrentFrame()
	{
		if (!IsOpen())
			return;
		RunOnGSThread([]() {
			if (s_gs)
			{
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
				s_gs->Present();
#else
				// PCSX2 owner: GSRenderer::PresentCurrentFrame(). Reuse the
				// completed merge texture; do not replay GS work.
				s_gs->PresentCurrentFrame();
#endif
			}
		});
	}

	void WaitGS(bool sync_regs, bool weak_wait, bool is_mtvu)
	{
		if (!IsOpen())
			return;

		// CPU0 can enter a strong GS observation from within the outer VIF1
		// transfer (notably Gif_Path::mtgsReadWait() for DIRECT/DIRECTHL).  Make
		// every complete private MTVU record and its final generated PATH1 marker
		// visible before MTGS is allowed to wait on that work.  The CPU1 weak-wait
		// path must never publish CPU0-owned producer state.
		if (!is_mtvu && THREAD_VU1)
		{
			PublishCpu0Health(Cpu0HealthStage::GsPublish);
			vu1Thread.PublishPendingVifBatch();
			vu1Thread.RequestGpuVuPath1Flush();
		}

#if defined(__vita__)
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();
		if (performance_telemetry_enabled)
			s_gs_producer_performance.wait_calls++;
#endif

		if (weak_wait && is_mtvu)
		{
			SetEvent();
			Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
			if (path.GetPendingGSPackets() == 0)
				return;

			const size_t consumer_position =
				path.GetGSPacketConsumerPositionMTVU();
			s_mtvu_path1_buffer_progress.WaitForChange(
				consumer_position, [&path]() {
					return path.GetGSPacketConsumerPositionMTVU();
				});
		}
		else
		{
			for (;;)
			{
				SetEvent();
				if (!is_mtvu)
					PublishCpu0Health(Cpu0HealthStage::GsEmptyWait);
				if (!s_work_sema.WaitForEmpty())
				{
					pxFailRel(
						"Vita GS worker died while waiting for an empty queue");
					break;
				}
				if (s_read_pos.load(std::memory_order_acquire) ==
					s_write_pos.load(std::memory_order_acquire))
				{
					break;
				}

				// WorkSema sees a deliberately deferred PATH1 reservation as
				// idle. A strong lifecycle/observation wait sleeps for the next
				// completion publication and then rechecks the actual MTGS ring.
				s_mtvu_path1_drain_waiter.store(
					true, std::memory_order_release);
				if (!is_mtvu)
					PublishCpu0Health(Cpu0HealthStage::GsCompletionWait);
				if (s_mtvu_path1_completion_waiting.load(
						std::memory_order_acquire))
				{
					s_mtvu_path1_drain_sema.Wait();
				}
				else if (!s_mtvu_path1_drain_waiter.exchange(
							 false, std::memory_order_acq_rel))
				{
					// A producer claimed the waiter while the worker consumed
					// its completion. Drain the paired post before rechecking.
					s_mtvu_path1_drain_sema.Wait();
				}
			}
		}

		pxAssert(!(weak_wait && sync_regs));
		if (!is_mtvu)
			PublishCpu0Health(Cpu0HealthStage::Returned);
		if (sync_regs)
			std::memcpy(s_ring.regs, g_RealGSMem, sizeof(s_ring.regs));
	}

	void ResetGS(bool hardware_reset)
	{
		if (!IsOpen() && !WaitForOpen())
			return;
		if (hardware_reset)
		{
			pxAssertRel(s_read_pos.load(std::memory_order_acquire) ==
					s_write_pos.load(std::memory_order_acquire),
				"hardware GS reset requires a quiescent Vita MTGS ring");
			s_read_pos.store(s_write_pos.load(std::memory_order_acquire),
				std::memory_order_release);
			s_queued_frame_count.store(0, std::memory_order_release);
			s_vsync_signal_listener.store(false, std::memory_order_release);
		}
		SendSimplePacket(Command::Reset, hardware_reset ? 1u : 0u, 0, 0);
		if (hardware_reset)
			SetEvent();
	}

	bool WaitForOpen()
	{
		if (IsOpen())
			return true;
		StartThread();
		if (!s_thread.Joinable())
			return false;
		s_open_flag.store(true, std::memory_order_release);
		s_work_sema.NotifyOfWork();
		s_open_or_close_done.Wait();
		return IsOpen();
	}

	void WaitForClose()
	{
		if (!IsOpen())
			return;

		// Vita VM teardown can arrive while a low-tally command has not yet woken
		// the worker. Retire the queue before clearing the open request so borrowed
		// GIF storage, heap-owned AsyncCall objects, and frame accounting cannot be
		// abandoned. PCSX2 normally establishes this seam in VMManager before
		// WaitForClose(); keeping it here makes every Vita lifecycle exit safe.
		WaitGS(false);
		s_open_flag.store(false, std::memory_order_release);
		s_work_sema.NotifyOfWork();
		s_open_or_close_done.Wait();
	}

	void Freeze(FreezeAction mode, FreezeData& data)
	{
		if (!IsOpen() && !WaitForOpen())
		{
			data.retval = -1;
			return;
		}
		if (mode == FreezeAction::Load)
			WaitGS(true);
		SendPointerPacket(Command::Freeze, static_cast<u32>(mode), &data);
		WaitGS(false);
	}

	int GetCurrentVsyncQueueSize()
	{
		return s_queued_frame_count.load(std::memory_order_acquire);
	}

	void PostVsyncStart(bool registers_written)
	{
		if (!IsOpen() && !WaitForOpen())
			return;

		PublishCpu0Health(Cpu0HealthStage::VsyncPublish);
		// A VSync can make the EE wait for the GS worker. Publish any partial
		// direct PATH1 run first so a frame with fewer than the size threshold
		// cannot leave GS asleep at an earlier MTVUGSPacket reservation.
		vu1Thread.PublishPendingVifBatch();
		// The VSync itself is ordered behind any open counted MTVU reservation.
		// An asynchronous prefix deliberately retains its final generated draw,
		// so it cannot let that reservation advance. Close the tail with the one
		// submission marker before CPU0 is allowed to wait for this VSync; leaving
		// it for a future pressure drain forms a CPU0/CPU1/MTGS wait cycle.
		vu1Thread.RequestGpuVuPath1Flush();

#if defined(__vita__)
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();
		if (performance_telemetry_enabled)
		{
			RecordHardwareVsyncProfile(s_producer_profile, "producer");
			RecordCorrelatedPerformanceProfile();
			RecordLivePerformanceProfile(
				s_correlated_profile.producer_vsyncs);
		}
#endif

		RingVSyncSnapshot snapshot = {};
		std::memcpy(snapshot.regset1, g_RealGSMem, sizeof(snapshot.regset1));
		snapshot.csr = GSCSRr;
		snapshot.imr = GSIMR._u32;
		snapshot.siglblid = GSSIGLBLID;
		snapshot.registers_written = registers_written ? 1u : 0u;
		constexpr u32 payload_size = sizeof(snapshot) / sizeof(u128);
		PrepareDataPacket(Command::VSync, payload_size);
		MemCopy_WrappedDest(reinterpret_cast<const u128*>(&snapshot), s_ring.ring,
			s_packet_write_pos, RingBufferSize, payload_size);

		// Publish the frame-accounting state before the command itself. Otherwise
		// the GS worker can retire this VSync first, transiently making the count
		// negative and losing the wakeup for a producer which is about to sleep.
		// PCSX2 owner: MTGS.cpp::PostVsyncStart() queue limiting; the ordering is
		// made explicit here because Vita runs both producer and worker at 500 MHz.
		const int previous_queued_frames =
			s_queued_frame_count.fetch_add(1, std::memory_order_acq_rel);
#if defined(__vita__)
		if (performance_telemetry_enabled && s_native_presenter_enabled)
			s_profile_max_queued_frames.store(std::max(
				s_profile_max_queued_frames.load(std::memory_order_relaxed),
				previous_queued_frames + 1), std::memory_order_relaxed);
#endif
		const bool force_drain = EmuConfig.GS.VsyncQueueSize <= 0;
		const bool wait_for_frame = !force_drain &&
			previous_queued_frames >= EmuConfig.GS.VsyncQueueSize;
		if (wait_for_frame)
			s_vsync_signal_listener.store(true, std::memory_order_release);

		SendDataPacket();
		if (s_copy_data_tally != 0)
			SetEvent();
		PublishCpu0Health(Cpu0HealthStage::VsyncBudget);
		vu1Thread.BoundGpuVuExecutionLatencyAtFrameBoundary();
		PublishCpu0Health(Cpu0HealthStage::Returned);
		if (force_drain)
		{
#if defined(__vita__)
			if (performance_telemetry_enabled && s_native_presenter_enabled)
				s_profile_vsync_waits.fetch_add(1, std::memory_order_relaxed);
#endif
			// With no prior queued frame there is nothing which can safely satisfy
			// the listener before this command retires. An explicit drain avoids
			// accepting the worker's end-of-empty safety post as this VSync's wake.
			WaitGS(false);
			return;
		}
		if (!wait_for_frame)
			return;
#if defined(__vita__)
		if (performance_telemetry_enabled && s_native_presenter_enabled)
			s_profile_vsync_waits.fetch_add(1, std::memory_order_relaxed);
#endif
		PublishCpu0Health(Cpu0HealthStage::VsyncSemaphore);
#if defined(__vita__)
		const auto vsync_wait_start = performance_telemetry_enabled ?
			Common::Timer::GetCurrentValue() : 0;
#endif
		s_vsync_sema.Wait();
#if defined(__vita__)
		if (performance_telemetry_enabled)
		{
			s_gs_producer_performance.vsync_wait_calls++;
			s_gs_producer_performance.vsync_wait_wall_us += static_cast<u64>(
				Common::Timer::ConvertValueToSeconds(
					Common::Timer::GetCurrentValue() - vsync_wait_start) * 1000000.0);
		}
#endif
		PublishCpu0Health(Cpu0HealthStage::Returned);
	}

	void InitAndReadFIFO(u8* memory, u32 qwc)
	{
		if (!IsOpen() && !WaitForOpen())
		{
			std::memset(memory, 0, qwc * sizeof(u128));
			return;
		}
		SendPointerPacket(Command::InitAndReadFIFO, qwc, memory);
		WaitGS(false);
	}

	void RunOnGSThread(AsyncCallType function)
	{
		if (!function || (!IsOpen() && !WaitForOpen()))
			return;
		SendPointerPacket(Command::AsyncCall, 0,
			new AsyncCallType(std::move(function)));
		SetEvent();
	}

	void GameChanged()
	{
	}

	void ApplySettings()
	{
		Pcsx2Config::GSOptions options = EmuConfig.GS;
#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		options.Renderer = GSRendererType::SW;
		options.UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;
#else
		ApplyVitaGsSettings(options);
#endif
		RunOnGSThread([options = std::move(options)]() {
			Pcsx2Config::GSOptions old_options = std::move(GSConfig);
			GSConfig = options;
			if (s_gs)
				s_gs->UpdateSettings(old_options);
		});
	}

	void ResizeDisplayWindow(u32 width, u32 height, float scale)
	{
		if (!IsOpen())
			return;
		RunOnGSThread([width, height, scale]() {
			if (g_gs_device)
				g_gs_device->ResizeWindow(width, height, scale);
		});
	}

	void UpdateDisplayWindow()
	{
		if (!IsOpen())
			return;
		RunOnGSThread([]() {
			if (g_gs_device && !g_gs_device->UpdateWindow())
				Console.Error("Vita GXM display-window update failed.");
		});
	}

	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
	{
		if (!IsOpen())
			return;
		RunOnGSThread([mode, allow_present_throttle]() {
			if (g_gs_device)
				g_gs_device->SetVSyncMode(mode, allow_present_throttle);
		});
	}

	void UpdateVSyncMode()
	{
		SetVSyncMode(VMManager::GetEffectiveVSyncMode(),
			VMManager::ShouldAllowPresentThrottle());
	}

	void SetSoftwareRendering(bool software, GSInterlaceMode interlace,
		bool display_message)
	{
		(void)software;
		(void)interlace;
		(void)display_message;
	}

	void ToggleSoftwareRendering()
	{
	}

	bool SaveMemorySnapshot(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels)
	{
		return false;
	}

	void SetRunIdle(bool enabled)
	{
		(void)enabled;
	}
} // namespace MTGS

void VitaGS::SetNativePresenterEnabled(bool enabled)
{
	// Product/QEMU setup fixes this contract before VMManager opens the GS. The
	// Vita product always uses GSRendererHW/GSDeviceGXM; the flag remains the
	// boundary for hardware-only flow profiling and validation entry points.
	pxAssertRel(!MTGS::IsOpen(),
		"native GS presenter selection changed while open");
	MTGS::s_native_presenter_enabled = enabled;
}

bool VitaGS::IsNativePresenterEnabled()
{
	return MTGS::s_native_presenter_enabled;
}

bool VitaGS::QueueGpuVuDraw(
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	if (!draw || !MTGS::IsOpen())
		return false;
	if (draw->UsesQuarantinedSnapshotArchitecture())
	{
		Console.Error(
			"GPU-VU: refusing quarantined snapshot/precompute draw descriptor; "
			"CPU MTVU remains authoritative.");
		return false;
	}
	if (draw->HasGeneratedLoopKernelTransaction())
	{
		const auto transaction =
			draw->GeneratedLoopKernelTransactionOwner();
		if (!transaction || !transaction->HasReplayUnpackJournal())
		{
			Console.Error(
				"GPU-VU: refusing generated transaction without its immutable "
				"pre-effect replay journal.");
			return false;
		}
	}
	if (draw->ordering_sequence == 0)
		draw->ordering_sequence = VitaGpuVu::NextGpuVuOrderingSequence();
	std::string error;
	if (!draw->ValidateForQueue(&error))
	{
		Console.Error("GPU-VU: refusing malformed draw descriptor: %s",
			error.c_str());
		return false;
	}

	VitaGpuVu::RecordGpuVuDrawQueued();
	MTGS::AppendPendingMtvuDirectDraw(draw.release());
	return true;
}

bool VitaGS::QueueUniversalGpuVuEpoch(
	VitaGpuVu::UniversalGpuVuEpoch* epoch)
{
	if (!epoch || !MTGS::IsOpen() ||
		epoch->Stage() != VitaGpuVu::UniversalGpuVuEpochStage::Prepared)
	{
		return false;
	}
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (!MTGS::s_gpu_vu_notification_available.load(std::memory_order_acquire))
	{
		VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
		return false;
	}
#endif
	// A universal epoch is one complete logical VIF/VU transaction and cannot
	// join a generated direct-draw run. Close the preceding run first, publish
	// every retained input byte, then expose the raw pointer while MTVU keeps
	// unique ownership until the queue marks it Retired.
	MTGS::FlushPendingMtvuDirectRun(true);
	const auto& unpacks = epoch->Unpacks();
	if (!VitaGpuVu::PublishPendingRawVifPayloads(
			unpacks.data(), static_cast<u32>(unpacks.size())))
	{
		return false;
	}
	MTGS::PublishMtvuPath1Completion(
		{nullptr, epoch->ExecuteCount(), epoch});
	return true;
}

void VitaGS::NotifyUniversalGpuVuProgress()
{
	MTGS::s_work_sema.NotifyOfWork();
}

bool VitaGS::ArmGpuVuVertexNotification(
	uptr address, u32 value, u64 sequence)
{
	if (value == 0u)
		return false;
	const MTGS::UniversalGpuVuNotificationArmResult result =
		MTGS::ArmGpuVuNotificationWake(
			address, value, value - 1u, 0u, 1u, sequence);
	return result != MTGS::UniversalGpuVuNotificationArmResult::Failed;
}

void VitaGS::SetGpuVuFragmentCompletionIsolationEnabled(bool enabled)
{
	MTGS::s_gpu_vu_fragment_completion_isolation_enabled.store(
		enabled, std::memory_order_release);
}

bool VitaGS::IsGpuVuFragmentCompletionIsolationEnabled()
{
	return MTGS::s_gpu_vu_fragment_completion_isolation_enabled.load(
		std::memory_order_acquire);
}

u32 VitaGS::ArmGpuVuGxmSubmissionWatchdog(
	GpuVuGxmSubmissionStage stage, u64 sequence, u64 scene_serial)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	constexpr u32 PublishingTicket = std::numeric_limits<u32>::max();
	if (!MTGS::s_gpu_vu_notification_available.load(
			std::memory_order_acquire))
	{
		return 0u;
	}

	u32 expected = 0u;
	if (!MTGS::s_gpu_vu_submission_watchdog_active.compare_exchange_strong(
			expected, PublishingTicket, std::memory_order_acq_rel,
			std::memory_order_acquire))
	{
		return 0u;
	}

	u32 ticket = 0u;
	do
	{
		ticket = MTGS::s_gpu_vu_submission_watchdog_next.fetch_add(
			1u, std::memory_order_relaxed) + 1u;
	} while (ticket == 0u || ticket == PublishingTicket);
	MTGS::s_gpu_vu_submission_watchdog_stage.store(
		static_cast<u32>(stage), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_sequence.store(
		sequence, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_scene.store(
		scene_serial, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_input_owner.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_output_address.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_input_slot.store(
		std::numeric_limits<u32>::max(), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_input_generation.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_input_payload_count.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_stream_count.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_object_count.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_private_count.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_output_bytes.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_retirement_slot.store(
		std::numeric_limits<u32>::max(), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_updates.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_total_draws.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_state_first.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_state_end.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_group_first.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_group_end.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_group_index.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_flags.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_action.store(
		static_cast<u32>(GpuVuGxmOwnershipAction::None),
		std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_owner.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_slot.store(
		std::numeric_limits<u32>::max(), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_generation.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_offset.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_size.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_references.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_draw_index.store(
		std::numeric_limits<u32>::max(), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_index.store(
		std::numeric_limits<u32>::max(), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_retained_count.store(
		0, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_descriptor_count.store(
		0, std::memory_order_relaxed);
	const Common::Timer::Value started = Common::Timer::GetCurrentValue();
	MTGS::s_gpu_vu_submission_watchdog_started.store(
		started, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_stage_started.store(
		started, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_action_started.store(
		0u, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_last_report.store(
		started, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_active.store(
		ticket, std::memory_order_release);
	MTGS::s_gpu_vu_notification_request_sema.Post();
	return ticket;
#else
	(void)stage;
	(void)sequence;
	(void)scene_serial;
	return 0u;
#endif
}

void VitaGS::UpdateGpuVuGxmSubmissionWatchdog(
	u32 ticket, GpuVuGxmSubmissionStage stage, u64 sequence,
	u64 scene_serial)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (ticket == 0u ||
		MTGS::s_gpu_vu_submission_watchdog_active.load(
			std::memory_order_acquire) != ticket)
	{
		return;
	}
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	MTGS::RecordCurrentGpuVuSubmissionStageLatency(now);
	MTGS::s_gpu_vu_submission_watchdog_stage.store(
		static_cast<u32>(stage), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_sequence.store(
		sequence, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_scene.store(
		scene_serial, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_stage_started.store(
		now, std::memory_order_release);
#else
	(void)ticket;
	(void)stage;
	(void)sequence;
	(void)scene_serial;
#endif
}

void VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
	GpuVuGxmSubmissionStage stage)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	const u32 ticket = MTGS::s_gpu_vu_submission_watchdog_active.load(
		std::memory_order_acquire);
	if (ticket == 0u || ticket == std::numeric_limits<u32>::max())
		return;
	const u64 sequence = MTGS::s_gpu_vu_submission_watchdog_sequence.load(
		std::memory_order_relaxed);
	const u64 scene = MTGS::s_gpu_vu_submission_watchdog_scene.load(
		std::memory_order_relaxed);
	UpdateGpuVuGxmSubmissionWatchdog(ticket, stage, sequence, scene);
#else
	(void)stage;
#endif
}

void VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
	GpuVuGxmSubmissionStage stage,
	const GpuVuGxmConsumerBreadcrumb& breadcrumb)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	const u32 ticket = MTGS::s_gpu_vu_submission_watchdog_active.load(
		std::memory_order_acquire);
	if (ticket == 0u || ticket == std::numeric_limits<u32>::max())
		return;

	MTGS::s_gpu_vu_submission_consumer_total_draws.store(
		breadcrumb.total_draws, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_state_first.store(
		breadcrumb.state_first, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_state_end.store(
		breadcrumb.state_end, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_group_first.store(
		breadcrumb.group_first, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_group_end.store(
		breadcrumb.group_end, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_group_index.store(
		breadcrumb.group_index, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_flags.store(
		breadcrumb.flags, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_consumer_updates.fetch_add(
		1u, std::memory_order_relaxed);

	const u64 sequence = MTGS::s_gpu_vu_submission_watchdog_sequence.load(
		std::memory_order_relaxed);
	const u64 scene = MTGS::s_gpu_vu_submission_watchdog_scene.load(
		std::memory_order_relaxed);
	UpdateGpuVuGxmSubmissionWatchdog(ticket, stage, sequence, scene);
#else
	(void)stage;
	(void)breadcrumb;
#endif
}

void VitaGS::UpdateGpuVuGxmSubmissionWatchdogBreadcrumb(u32 ticket,
	const GpuVuGxmSubmissionBreadcrumb& breadcrumb)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (ticket == 0u ||
		MTGS::s_gpu_vu_submission_watchdog_active.load(
			std::memory_order_acquire) != ticket)
	{
		return;
	}
	MTGS::s_gpu_vu_submission_watchdog_input_owner.store(
		breadcrumb.input_owner, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_output_address.store(
		breadcrumb.output_address, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_input_slot.store(
		breadcrumb.input_slot, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_input_generation.store(
		breadcrumb.input_generation, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_input_payload_count.store(
		breadcrumb.input_payload_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_stream_count.store(
		breadcrumb.stream_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_object_count.store(
		breadcrumb.object_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_private_count.store(
		breadcrumb.private_transaction_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_output_bytes.store(
		breadcrumb.output_bytes, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_watchdog_retirement_slot.store(
		breadcrumb.retirement_slot, std::memory_order_release);
#else
	(void)ticket;
	(void)breadcrumb;
#endif
}

void VitaGS::UpdateGpuVuGxmSubmissionWatchdogOwnership(u32 ticket,
	const GpuVuGxmOwnershipBreadcrumb& breadcrumb)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (ticket == 0u ||
		MTGS::s_gpu_vu_submission_watchdog_active.load(
			std::memory_order_acquire) != ticket)
	{
		return;
	}
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	MTGS::RecordCurrentGpuVuOwnershipActionLatency(now);
	MTGS::s_gpu_vu_submission_ownership_input_owner.store(
		breadcrumb.input_owner, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_slot.store(
		breadcrumb.input_slot, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_generation.store(
		breadcrumb.input_generation, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_offset.store(
		breadcrumb.input_offset, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_size.store(
		breadcrumb.input_size, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_references.store(
		breadcrumb.input_references, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_draw_index.store(
		breadcrumb.draw_index, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_input_index.store(
		breadcrumb.input_index, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_retained_count.store(
		breadcrumb.retained_input_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_descriptor_count.store(
		breadcrumb.descriptor_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_action.store(
		static_cast<u32>(breadcrumb.action), std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_ownership_action_started.store(
		now, std::memory_order_release);
#else
	(void)ticket;
	(void)breadcrumb;
#endif
}

void VitaGS::UpdateGpuVuGxmSubmissionWatchdogRetirementSlot(
	u32 ticket, u32 retirement_slot)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (ticket == 0u ||
		MTGS::s_gpu_vu_submission_watchdog_active.load(
			std::memory_order_acquire) != ticket)
	{
		return;
	}
	MTGS::s_gpu_vu_submission_watchdog_retirement_slot.store(
		retirement_slot, std::memory_order_release);
#else
	(void)ticket;
	(void)retirement_slot;
#endif
}

void VitaGS::CompleteGpuVuGxmSubmissionWatchdog(u32 ticket)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (ticket == 0u)
		return;
	if (MTGS::s_gpu_vu_submission_watchdog_active.load(
			std::memory_order_acquire) != ticket)
	{
		return;
	}
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	MTGS::RecordCurrentGpuVuSubmissionStageLatency(now);
	MTGS::RecordCurrentGpuVuOwnershipActionLatency(now);
	const Common::Timer::Value started =
		MTGS::s_gpu_vu_submission_watchdog_started.load(
			std::memory_order_relaxed);
	const u64 elapsed_us = started != 0u && now >= started ?
		static_cast<u64>(Common::Timer::ConvertValueToSeconds(
			now - started) * 1000000.0) : 0u;
	MTGS::s_gpu_vu_submission_last_completed_ticket.store(
		ticket, std::memory_order_relaxed);
	MTGS::s_gpu_vu_submission_last_completed_us.store(
		elapsed_us, std::memory_order_relaxed);
	MTGS::UpdateGpuVuLatencyMaximum(
		&MTGS::s_gpu_vu_submission_max_completed_us, elapsed_us);
	u32 expected = ticket;
	MTGS::s_gpu_vu_submission_watchdog_active.compare_exchange_strong(
		expected, 0u, std::memory_order_acq_rel,
		std::memory_order_acquire);
#else
	(void)ticket;
#endif
}

u32 VitaGS::BeginGpuVuGxmCall(GpuVuGxmCallKind kind, u64 sequence,
	u64 scene_serial, const GpuVuGxmCallBreadcrumb& breadcrumb)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	constexpr u32 PublishingToken = std::numeric_limits<u32>::max();
	u32 expected = 0u;
	if (!MTGS::s_gpu_vu_gxm_call_active.compare_exchange_strong(
			expected, PublishingToken, std::memory_order_acq_rel,
			std::memory_order_acquire))
	{
		return 0u;
	}
	u32 token = 0u;
	do
	{
		token = MTGS::s_gpu_vu_gxm_call_next.fetch_add(
			1u, std::memory_order_relaxed) + 1u;
	} while (token == 0u || token == PublishingToken);
	MTGS::s_gpu_vu_gxm_call_kind.store(
		static_cast<u32>(kind), std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_sequence.store(sequence, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_scene.store(scene_serial, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_sequence_end.store(
		breadcrumb.sequence_end != 0u ? breadcrumb.sequence_end : sequence,
		std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_program_key_high.store(
		breadcrumb.program_key_high, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_program_key_low.store(
		breadcrumb.program_key_low, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_input_owner.store(
		breadcrumb.input_owner, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_output_address.store(
		breadcrumb.output_address, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_input_slot.store(
		breadcrumb.input_slot, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_input_generation.store(
		breadcrumb.input_generation, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_input_first_qword.store(
		breadcrumb.input_first_qword, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_input_last_qword.store(
		breadcrumb.input_last_qword, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_retirement_slot.store(
		breadcrumb.retirement_slot, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_program_abi.store(
		breadcrumb.program_abi, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_phase_index.store(
		breadcrumb.phase_index, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_module_index.store(
		breadcrumb.module_index, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_object_index.store(
		breadcrumb.object_index, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_group_index.store(
		breadcrumb.group_index, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_index_count.store(
		breadcrumb.index_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_index_minimum.store(
		breadcrumb.index_minimum, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_index_maximum.store(
		breadcrumb.index_maximum, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_object_count.store(
		breadcrumb.object_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_private_count.store(
		breadcrumb.private_transaction_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_output_bytes.store(
		breadcrumb.output_bytes, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_output_maximum_write_word.store(
		breadcrumb.output_maximum_write_word, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_output_payload_capacity_words.store(
		breadcrumb.output_payload_capacity_words, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_output_probe_maximum_write_word.store(
		breadcrumb.output_probe_maximum_write_word, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_output_probe_capacity_words.store(
		breadcrumb.output_probe_capacity_words, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_vertex_program.store(
		breadcrumb.vertex_program, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_fragment_program.store(
		breadcrumb.fragment_program, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_object.store(
		breadcrumb.source_texture_object, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_descriptor.store(
		breadcrumb.source_texture_descriptor, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_data.store(
		breadcrumb.source_texture_data, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_ps_selector_low.store(
		breadcrumb.ps_selector_low, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_ps_selector_high.store(
		breadcrumb.ps_selector_high, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_primitive_type.store(
		breadcrumb.primitive_type, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_topology.store(
		breadcrumb.topology, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_sampler_key.store(
		breadcrumb.sampler_key, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_blend_key.store(
		breadcrumb.blend_key, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_color_mask_key.store(
		breadcrumb.color_mask_key, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_depth_key.store(
		breadcrumb.depth_key, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_type.store(
		breadcrumb.texture_type, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_format.store(
		breadcrumb.texture_format, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_width.store(
		breadcrumb.texture_width, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_height.store(
		breadcrumb.texture_height, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_stride.store(
		breadcrumb.texture_stride, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_mipmap_count.store(
		breadcrumb.texture_mipmap_count, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_texture_sampler_state.store(
		breadcrumb.texture_sampler_state, std::memory_order_relaxed);
	if (kind == GpuVuGxmCallKind::EndScene)
	{
		MTGS::s_gpu_vu_gxm_last_generated_scene.store(
			breadcrumb.last_generated_scene, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_last_generated_sequence.store(
			breadcrumb.last_generated_sequence, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_address.store(
			breadcrumb.fragment_notification_address,
			std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_submitted.store(
			breadcrumb.fragment_notification_value,
			std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_completed.store(
			breadcrumb.fragment_completed_value, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_completed_scene.store(
			breadcrumb.fragment_completed_scene, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_completed_generated_scene.store(
			breadcrumb.fragment_completed_generated_scene,
			std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_completed_generated_sequence.store(
			breadcrumb.fragment_completed_generated_sequence,
			std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_oldest.store(
			breadcrumb.fragment_oldest_value, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_oldest_scene.store(
			breadcrumb.fragment_oldest_scene, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_oldest_generated_scene.store(
			breadcrumb.fragment_oldest_generated_scene,
			std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_fragment_oldest_generated_sequence.store(
			breadcrumb.fragment_oldest_generated_sequence,
			std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_scene_render_target.store(
			breadcrumb.scene_render_target, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_scene_depth_target.store(
			breadcrumb.scene_depth_target, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_scenes_since_generated.store(
			breadcrumb.scenes_since_generated, std::memory_order_relaxed);
		MTGS::s_gpu_vu_gxm_scene_flags.store(
			breadcrumb.scene_flags, std::memory_order_relaxed);
	}
	// Wake the independent observer before starting the measured interval.  The
	// owner publishes only atomics between the timestamp and the libGXM call;
	// synchronous formatting or file I/O would both falsify this duration and
	// delay command submission.
	if (!MTGS::s_gpu_vu_gxm_call_health_enabled.exchange(
			true, std::memory_order_acq_rel))
	{
		MTGS::s_gpu_vu_notification_request_sema.Post();
	}
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	auto& health = MTGS::s_gpu_vu_health_gxm_call;
	health = {};
	health.update_time_us = static_cast<u64>(
		Common::Timer::ConvertValueToSeconds(now) * 1000000.0);
	health.entered_time_us = health.update_time_us;
	health.token = token;
	health.sequence_begin = sequence;
	health.sequence_end = breadcrumb.sequence_end != 0u ?
		breadcrumb.sequence_end : sequence;
	health.scene = scene_serial;
	health.program_key_high = breadcrumb.program_key_high;
	health.program_key_low = breadcrumb.program_key_low;
	health.input_owner = breadcrumb.input_owner;
	health.output_address = breadcrumb.output_address;
	health.notification_address = breadcrumb.notification_address;
	health.buffer0_address = breadcrumb.buffer0_address;
	health.buffer1_address = breadcrumb.buffer1_address;
	health.buffer2_address = breadcrumb.buffer2_address;
	health.buffer4_address = breadcrumb.buffer4_address;
	health.index_address = breadcrumb.index_address;
	health.active = 1u;
	health.kind = static_cast<u32>(kind);
	health.submission_stage =
		MTGS::s_gpu_vu_submission_watchdog_stage.load(
			std::memory_order_relaxed);
	health.program_abi = breadcrumb.program_abi;
	health.retirement_slot = breadcrumb.retirement_slot;
	health.input_slot = breadcrumb.input_slot;
	health.input_generation = breadcrumb.input_generation;
	health.input_first_qword = breadcrumb.input_first_qword;
	health.input_last_qword = breadcrumb.input_last_qword;
	health.object_count = breadcrumb.object_count;
	health.index_count = breadcrumb.index_count;
	health.private_transaction_count =
		breadcrumb.private_transaction_count;
	health.output_bytes = breadcrumb.output_bytes;
	health.output_maximum_write_word =
		breadcrumb.output_maximum_write_word;
	health.notification_required = breadcrumb.notification_value;
	health.notification_observed = breadcrumb.notification_observed;
	health.flags = breadcrumb.draw_flags;
	VitaGpuVu::HealthJournal::PublishGxmCall(health);
	// Start the non-return timer only after the fixed atomic journal has been
	// published. CompleteGpuVuGxmCall() samples time before doing any matching
	// journal work, so the measured interval now brackets libGXM itself.
	const Common::Timer::Value call_started = Common::Timer::GetCurrentValue();
	MTGS::s_gpu_vu_gxm_call_started.store(
		call_started, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_active.store(token, std::memory_order_release);
	return token;
#else
	(void)kind;
	(void)sequence;
	(void)scene_serial;
	(void)breadcrumb;
	return 1u;
#endif
}

void VitaGS::CompleteGpuVuGxmCall(u32 token, int result)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	if (token == 0u || MTGS::s_gpu_vu_gxm_call_active.load(
			std::memory_order_acquire) != token)
	{
		return;
	}
	const Common::Timer::Value started = MTGS::s_gpu_vu_gxm_call_started.load(
		std::memory_order_relaxed);
	const u32 kind = MTGS::s_gpu_vu_gxm_call_kind.load(
		std::memory_order_relaxed);
	const u64 sequence = MTGS::s_gpu_vu_gxm_call_sequence.load(
		std::memory_order_relaxed);
	const u64 scene = MTGS::s_gpu_vu_gxm_call_scene.load(
		std::memory_order_relaxed);
	const u64 elapsed_us = started != 0u && now >= started ?
		static_cast<u64>(Common::Timer::ConvertValueToSeconds(
			now - started) * 1000000.0) : 0u;
	MTGS::RecordGpuVuGxmCallLatency(kind, elapsed_us);
	MTGS::s_gpu_vu_gxm_call_completed_elapsed_us.store(
		elapsed_us, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_completed_kind.store(
		kind, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_completed_sequence.store(
		sequence, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_completed_scene.store(
		scene, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_completed_result.store(
		result, std::memory_order_relaxed);
	MTGS::s_gpu_vu_gxm_call_completed.store(token, std::memory_order_relaxed);
	u32 expected = token;
	MTGS::s_gpu_vu_gxm_call_active.compare_exchange_strong(
		expected, 0u, std::memory_order_acq_rel,
		std::memory_order_acquire);
	auto& health = MTGS::s_gpu_vu_health_gxm_call;
	health.update_time_us = static_cast<u64>(
		Common::Timer::ConvertValueToSeconds(now) * 1000000.0);
	health.returned_time_us = health.update_time_us;
	health.result = result;
	health.active = 0u;
	VitaGpuVu::HealthJournal::PublishGxmCall(health);
#else
	(void)token;
	(void)result;
#endif
}

bool VitaGS::ArmGeneratedGpuVuHandoffWatchdog(
	GeneratedGpuVuHandoffStage stage, u64 sequence)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	constexpr u32 PublishingTicket = std::numeric_limits<u32>::max();
	if (sequence == 0u ||
		!MTGS::s_gpu_vu_notification_available.load(std::memory_order_acquire))
	{
		return false;
	}

	u32 expected = 0u;
	if (!MTGS::s_gpu_vu_handoff_watchdog_active.compare_exchange_strong(
			expected, PublishingTicket, std::memory_order_acq_rel,
			std::memory_order_acquire))
	{
		return false;
	}

	u32 ticket = 0u;
	do
	{
		ticket = MTGS::s_gpu_vu_handoff_watchdog_next.fetch_add(
			1u, std::memory_order_relaxed) + 1u;
	} while (ticket == 0u || ticket == PublishingTicket);
	MTGS::s_gpu_vu_handoff_watchdog_sequence.store(
		sequence, std::memory_order_relaxed);
	MTGS::s_gpu_vu_handoff_watchdog_stage.store(
		static_cast<u32>(stage), std::memory_order_relaxed);
	MTGS::s_gpu_vu_handoff_watchdog_started.store(
		Common::Timer::GetCurrentValue(), std::memory_order_relaxed);
	MTGS::s_gpu_vu_handoff_watchdog_active.store(
		ticket, std::memory_order_release);
	MTGS::s_gpu_vu_notification_request_sema.Post();
	return true;
#else
	(void)stage;
	(void)sequence;
	return true;
#endif
}

void VitaGS::UpdateGeneratedGpuVuHandoffWatchdog(
	GeneratedGpuVuHandoffStage stage, u64 sequence)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (sequence == 0u ||
		MTGS::s_gpu_vu_handoff_watchdog_active.load(
			std::memory_order_acquire) == 0u)
	{
		return;
	}
	const u32 desired = static_cast<u32>(stage);
	u32 observed = MTGS::s_gpu_vu_handoff_watchdog_stage.load(
		std::memory_order_acquire);
	while (observed < desired)
	{
		// Publish the refreshed deadline before the stage. The independent monitor
		// must never observe a newly advanced owner with the preceding stage's
		// already-expired timestamp.
		MTGS::s_gpu_vu_handoff_watchdog_sequence.store(
			sequence, std::memory_order_relaxed);
		MTGS::s_gpu_vu_handoff_watchdog_started.store(
			Common::Timer::GetCurrentValue(), std::memory_order_release);
		if (MTGS::s_gpu_vu_handoff_watchdog_stage.compare_exchange_weak(
				observed, desired, std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
			MTGS::s_gpu_vu_notification_request_sema.Post();
			return;
		}
	}
#else
	(void)stage;
	(void)sequence;
#endif
}

void VitaGS::CompleteGeneratedGpuVuHandoffWatchdog(u64 sequence)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	const u64 observed_sequence =
		MTGS::s_gpu_vu_handoff_watchdog_sequence.load(
			std::memory_order_acquire);
	if (sequence == 0u || sequence < observed_sequence)
		return;
	u32 ticket = MTGS::s_gpu_vu_handoff_watchdog_active.load(
		std::memory_order_acquire);
	while (ticket != 0u && ticket != std::numeric_limits<u32>::max())
	{
		if (MTGS::s_gpu_vu_handoff_watchdog_active.compare_exchange_weak(
				ticket, 0u, std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
			return;
		}
	}
#else
	(void)sequence;
#endif
}

void VitaGS::CompleteMtvuPath1Packet()
{
	if (MTGS::IsOpen())
	{
		MTGS::FlushPendingMtvuDirectRun(true);
		MTGS::PublishMtvuPath1Completion({nullptr, 1});
	}
}

void VitaGS::CompleteMtvuPath1NoOutput()
{
	if (MTGS::IsOpen())
		MTGS::AppendPendingMtvuNoOutput();
}

u32 VitaGS::PublishMtvuPath1Completions()
{
	if (MTGS::IsOpen())
		return MTGS::PublishPendingMtvuDirectPrefix();
	return 0u;
}

void VitaGS::FlushMtvuPath1Completions()
{
	if (MTGS::IsOpen())
		MTGS::FlushPendingMtvuDirectRun(true);
}

void VitaGS::NotifyGpuVuCompilerResult()
{
	MTGS::s_gpu_vu_compiler_result_pending.store(true,
		std::memory_order_release);
	MTGS::s_work_sema.NotifyOfWork();
}

void VitaGS::RequestGpuVuInputRetirement(
	const VitaGpuVu::RawVifPayloadRef& blocked_generation)
{
#if defined(__vita__)
	// A large VIF transfer can fill the immutable ring before returning to its
	// ordinary publish boundary. Make any already-complete capture commands
	// visible, then publish the MTVU-owned descriptor run which retains that
	// generation before asking the GS owner to retire it.
	vu1Thread.PublishPendingVifBatch();
	vu1Thread.RequestGpuVuPath1Flush();
	MTGS::s_gpu_vu_input_retirement_owner.store(
		blocked_generation.owner, std::memory_order_relaxed);
	MTGS::s_gpu_vu_input_retirement_slot.store(
		blocked_generation.slot, std::memory_order_relaxed);
	MTGS::s_gpu_vu_input_retirement_generation.store(
		blocked_generation.generation, std::memory_order_relaxed);
	MTGS::s_gpu_vu_input_retirement_pending.store(
		true, std::memory_order_release);
	MTGS::s_work_sema.NotifyOfWork();
#else
	(void)blocked_generation;
#endif
}

void VitaGS::NotifyPerformanceElfEntry()
{
#if defined(__vita__)
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	VitaGsDrawTraceNotifyElfEntry();
	#endif
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	// PCSX2 owner: MachineCheckpointTrace::NotifyMachineCheckpointElfEntry()
	// snapshots g_FrameCount and measures later captures relative to that frame.
	// Reset only the sampling cadence: lifetime counters remain monotonic.
	MTGS::s_correlated_profile.producer_vsync_origin =
		MTGS::s_correlated_profile.producer_vsyncs;
	MTGS::s_correlated_profile.sampling_boundaries = 0;
	MTGS::s_correlated_profile.boundaries_at_start = 0;
	MTGS::s_correlated_profile.started = false;
	MTGS::s_correlated_profile.origin_kind =
		MTGS::CorrelatedPerformanceProfile::Origin::Elf;
	MTGS::s_correlated_profile.origin =
		MTGS::CaptureCorrelatedPerformanceSnapshot(
			MTGS::s_correlated_profile.producer_vsyncs);
	MTGS::s_live_performance = {};
#endif
}

void VitaGS::NotifyPerformanceWorkloadReplayLoaded()
{
#if defined(__vita__)
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	MTGS::s_correlated_profile.producer_vsync_origin =
		MTGS::s_correlated_profile.producer_vsyncs;
	MTGS::s_correlated_profile.sampling_boundaries = 0;
	MTGS::s_correlated_profile.boundaries_at_start = 0;
	MTGS::s_correlated_profile.started = false;
	MTGS::s_correlated_profile.origin_kind =
		MTGS::CorrelatedPerformanceProfile::Origin::WorkloadReplay;
	MTGS::s_correlated_profile.origin =
		MTGS::CaptureCorrelatedPerformanceSnapshot(
			MTGS::s_correlated_profile.producer_vsyncs);
	MTGS::s_live_performance = {};
#endif
}

bool GSValidatePortableState()
{
	// PCSX2 owner: GS/GS.cpp::GSValidatePortableState(). Query the mailbox's
	// typed live renderer on its worker so GS/GXM ownership never migrates back
	// to the EE thread (QEMU deliberately has no g_gs_renderer).
	if (!MTGS::WaitForOpen())
		return false;
	bool valid = false;
	MTGS::RunOnGSThread([&valid]() {
		valid = MTGS::s_gs && MTGS::s_gs->ValidatePortableState();
	});
	MTGS::WaitGS(false);
	return valid;
}

void GSTraceStateSnapshot(u8 trigger)
{
	if (!MTGS::IsOpen())
		return;
	MTGS::RunOnGSThread([trigger]() {
		if (MTGS::s_gs)
			MTGS::s_gs->TraceGsStateSnapshot(trigger);
	});
	MTGS::WaitGS(false);
}

const u8* GSTraceLocalMemoryData(size_t* size)
{
	if (size)
		*size = 0;
	if (!MTGS::IsOpen())
		return nullptr;

	const u8* data = nullptr;
	size_t data_size = 0;
	MTGS::RunOnGSThread([&data, &data_size]() {
		if (MTGS::s_gs)
			data = MTGS::s_gs->TraceGsLocalMemoryData(&data_size);
	});
	MTGS::WaitGS(false);
	if (size)
		*size = data_size;
	// The pointer remains stable for the GSState lifetime. Its contents are
	// quiescent until this single producer publishes another MTGS command.
	return data;
}

const u8* VitaGS::GetLocalMemoryForTrace(size_t* size)
{
	return GSTraceLocalMemoryData(size);
}

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
bool VitaGS::CopyPrivilegedRegistersForValidation(u8* output, size_t size)
{
	if (!output || size != Ps2MemSize::GSregs || !MTGS::WaitForOpen())
		return false;

	MTGS::RunOnGSThread([output]() {
		std::memcpy(output, MTGS::s_ring.regs, sizeof(MTGS::s_ring.regs));
	});
	MTGS::WaitGS(false);
	return true;
}

u64 VitaGS::GetMtvuPath1CompletionDeferralsForValidation()
{
	return s_mtvu_path1_completion_deferrals_for_validation.load(
		std::memory_order_relaxed);
}

bool VitaGS::IsMtvuPath1BufferWaiterArmedForValidation()
{
	return MTGS::s_mtvu_path1_buffer_progress.IsWaitingForValidation();
}

bool VitaGS::ValidateMtvuPath1NoOutputCompletions()
{
	// Exercise the production publisher, prefix splitter and SPSC queue before
	// starting MTGS. The reference is the actual PCSX2-owned empty/nonempty
	// Gif_Path::FinishGSPacketMTVU queue, not a second completion implementation.
	// These descriptors test host ordering only, not executable GPU programs.
	if (MTGS::s_thread.Joinable() || MTGS::IsOpen())
		return false;
	using namespace MTGS;
	Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
	bool ok = true;
	for (u32 pattern = 0; pattern < 128; ++pattern)
	{
		for (u32 stride = 1; stride <= 8; ++stride)
		{
			path.Reset(false);
			for (u32 index = 0; index < 7; ++index)
			{
				const bool output = (pattern & (1u << index)) != 0;
				path.gsPack.size = output ? 16u : 0u;
				path.curOffset += path.gsPack.size;
				path.FinishGSPacketMTVU();
				if (output)
				{
					auto draw = std::make_unique<VitaGpuVu::GpuVuDraw>();
					draw->ordering_sequence = index + 1u;
					if ((index % 3u) != 0u)
					{
						ok &= draw->ConfigureGeneratedLoopKernelTransaction(1u,
							std::make_shared<VitaGpuVu::GeneratedLoopKernelTransaction>());
					}
					AppendPendingMtvuDirectDraw(draw.release());
				}
				else
					AppendPendingMtvuNoOutput();
				if (index == 2 || index == 5)
					PublishPendingMtvuDirectPrefix();
			}
			const u64 expected_marker = s_pending_mtvu_generated_transaction ?
				s_pending_mtvu_direct_tail->ordering_sequence : 0u;
			FlushPendingMtvuDirectRun(true);
			u64 observed_marker = 0;
			u32 consumed = 0;
			MtvuPath1Completion completion;
			while (s_mtvu_path1_completions.Peek(&completion))
			{
				const u32 prefix = std::min(stride, completion.reservation_count);
				std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws;
				VitaGpuVu::GpuVuDraw* remaining = completion.first_draw ?
					TakeMtvuDirectPrefix(completion.first_draw, prefix, draws) : nullptr;
				ok &= completion.first_draw || completion.no_output_only;
				u32 draw_index = 0;
				for (u32 index = 0; index < prefix; ++index)
				{
					GS_Packet packet;
					if (!path.TryGetGSPacketMTVU(packet))
					{
						ok = false;
						continue;
					}
					if (packet.size != 0)
					{
						ok &= draw_index < draws.size() &&
							draws[draw_index]->ordering_sequence == consumed + index + 1u;
						++draw_index;
					}
					path.readAmount.fetch_sub(packet.size + packet.readAmount,
						std::memory_order_acq_rel);
					path.PopGSPacketMTVU();
				}
				ok &= draw_index == draws.size();
				if (completion.submit_generated_transactions_after_draws &&
					prefix == completion.reservation_count)
				{
					ok &= observed_marker == 0 && !draws.empty();
					observed_marker = draws.empty() ? 0 : draws.back()->ordering_sequence;
				}
				s_mtvu_path1_completions.ConsumePrefix(prefix, remaining);
				consumed += prefix;
			}
			ok &= consumed == 7 && path.GetPendingGSPackets() == 0 &&
				path.readAmount.load(std::memory_order_acquire) == 0 &&
				observed_marker == expected_marker;
		}
	}

	// Bounded no-draw runs must publish without waiting for another draw, and
	// ordinary publication must release a short no-output-only tail as well.
	constexpr u32 EmptyCount = MaximumPendingMtvuDirectRun * 65u + 13u;
	for (u32 index = 0; index < EmptyCount; ++index)
		AppendPendingMtvuNoOutput();
	ok &= s_pending_mtvu_no_output_count == 13u;
	ok &= PublishPendingMtvuDirectPrefix() == 0u;
	u32 empty_consumed = 0;
	MtvuPath1Completion completion;
	while (s_mtvu_path1_completions.Peek(&completion))
	{
		ok &= completion.no_output_only && !completion.first_draw &&
			!completion.submit_generated_transactions_after_draws &&
			completion.reservation_count <= MaximumPendingMtvuDirectRun;
		const u32 prefix = std::min(17u, completion.reservation_count);
		s_mtvu_path1_completions.ConsumePrefix(prefix, nullptr);
		empty_consumed += prefix;
	}
	ok &= empty_consumed == EmptyCount && !s_pending_mtvu_no_output_count;
	s_mtvu_path1_completions.ResetAndDiscard();
	path.Reset(false);
	return ok;
}
#endif

#if defined(VITASX2_PRODUCT_BOOT_VALIDATION) && VITASX2_PRODUCT_BOOT_VALIDATION
bool VitaGS::ValidateCanonicalLocalMemoryRing(
	CanonicalRingValidationResult* result, Error* error)
{
	if (!result)
	{
		Error::SetString(error, "GS canonical-ring validation has no result storage.");
		return false;
	}
	*result = {};

	if (!MTGS::WaitForOpen())
	{
		Error::SetString(error, "GS canonical-ring validation could not open GSState.");
		return false;
	}

	// This is byte-for-byte the boundary packet emitted by
	// tests/ps2/gs_transfer_trace. PCSX2's x86 owner maps one 4 MiB backing
	// store four times, so DBP 0x3fff plus DSAX 64 lands back at canonical
	// byte offset 0x1f00.
	alignas(16) constexpr std::array<u64, 14> PACKET = {
		0x1000000000000004ull, 0x000000000000000eull,
		0x00013fff00010000ull, 0x0000000000000050ull,
		0x0000004000000000ull, 0x0000000000000051ull,
		0x0000000200000002ull, 0x0000000000000052ull,
		0x0000000000000000ull, 0x0000000000000053ull,
		0x0800000000008001ull, 0x0000000000000000ull,
		0xff405060ff102030ull, 0xffa0b0c0ff708090ull,
	};
	constexpr size_t CANONICAL_BYTES = 4 * 1024 * 1024;
	constexpr u64 ZERO_HASH = 0xf8e3e56ce9222325ull;
	constexpr u64 PACKET_HASH = 0x13403ace12c224dcull;
	constexpr u64 LOCAL_HASH = 0xe77d7ce0beced965ull;
	constexpr std::array<u32, 4> PIXELS = {
		0xff102030u, 0xff405060u, 0xff708090u, 0xffa0b0c0u,
	};

	const char* failure = nullptr;
	auto fail = [&failure](const char* reason) {
		if (!failure)
			failure = reason;
		return false;
	};

	bool content_ok = false;
	MTGS::RunOnGSThread([&]() {
		content_ok = [&]() {
		size_t local_bytes = 0;
		u8* const local = const_cast<u8*>(
			MTGS::s_gs->TraceGsLocalMemoryData(&local_bytes));
		result->canonical_bytes = static_cast<u32>(local_bytes);
		result->packet_qwc = static_cast<u32>(PACKET.size() / 2);
		if (!local || local_bytes != CANONICAL_BYTES)
			return fail("GSState did not expose one canonical 4 MiB local-memory window");
		if (Pcsx2Trace::HashGsTraceBytes(local, local_bytes) != ZERO_HASH)
			return fail("new GS local memory was not zero before boundary validation");

		result->packet_hash = Pcsx2Trace::HashGsTraceBytes(
			PACKET.data(), PACKET.size() * sizeof(PACKET[0]));
		if (result->packet_hash != PACKET_HASH)
			return fail("embedded GS boundary packet does not match the x86 oracle fixture");

		MTGS::s_gs->Transfer<3>(reinterpret_cast<const u8*>(PACKET.data()),
			result->packet_qwc);
		result->local_hash = Pcsx2Trace::HashGsTraceBytes(local, local_bytes);
		if (result->local_hash != LOCAL_HASH)
			return fail("boundary GIF upload did not match PCSX2 canonical GS memory");

		for (u32 i = 0; i < PIXELS.size(); i++)
		{
			const int x = 64 + static_cast<int>(i & 1u);
			const int y = static_cast<int>(i >> 1);
			const u32 address = GSLocalMemory::PixelAddress32(x, y, 0x3fff, 1);
			if (address >= (CANONICAL_BYTES / sizeof(u32)) ||
				MTGS::s_gs->m_mem.ReadPixel32(address) != PIXELS[i])
			{
				return fail("boundary GIF pixel address or value was not canonical");
			}
			result->pixel_checks++;
		}

		// Cover every distinct scalar swizzle representation which consumes the
		// final PAHelper address. One page step from the last GS block must fold
		// into the canonical window in the PSM's own address units.
		const u32 address32 = GSLocalMemory::PixelAddress32(64, 0, 0x3fff, 1);
		const u32 address16 = GSLocalMemory::PixelAddress16(64, 0, 0x3fff, 1);
		const u32 address16s = GSLocalMemory::PixelAddress16S(64, 0, 0x3fff, 1);
		const u32 address8 = GSLocalMemory::PixelAddress8(128, 0, 0x3fff, 1);
		const u32 address4 = GSLocalMemory::PixelAddress4(128, 0, 0x3fff, 1);
		const u32 address32z = GSLocalMemory::PixelAddress32Z(64, 0, 0x3fff, 1);
		const u32 address16z = GSLocalMemory::PixelAddress16Z(64, 0, 0x3fff, 1);
		const u32 address16sz = GSLocalMemory::PixelAddress16SZ(64, 0, 0x3fff, 1);
		if (address32 != 0x07c0u || address16 != 0x0f80u ||
			address16s != 0x0f80u || address8 != 0x1f00u ||
			address4 != 0x3e00u || address32z != 0x01c0u ||
			address16z != 0x0380u || address16sz != 0x0380u)
		{
			return fail("a scalar GS swizzle did not match PCSX2's modulo address");
		}

		auto check32 = [&](u32 address, u32 value) {
			if (address >= CANONICAL_BYTES / sizeof(u32))
				return false;
			MTGS::s_gs->m_mem.WritePixel32(address, value);
			return MTGS::s_gs->m_mem.ReadPixel32(address) == value;
		};
		auto check16 = [&](u32 address, u16 value) {
			if (address >= CANONICAL_BYTES / sizeof(u16))
				return false;
			MTGS::s_gs->m_mem.WritePixel16(address, value);
			return MTGS::s_gs->m_mem.ReadPixel16(address) == value;
		};
		if (!check32(address32, 0x12345678u) ||
			!check16(address16, 0x1357u) ||
			!check16(address16s, 0x2468u) ||
			address8 >= CANONICAL_BYTES ||
			address4 >= CANONICAL_BYTES * 2u ||
			!check32(address32z, 0x89abcdefu) ||
			!check16(address16z, 0x55aau) ||
			!check16(address16sz, 0xaa55u))
		{
			return fail("a scalar GS swizzle escaped its canonical address units");
		}
		MTGS::s_gs->m_mem.WritePixel8(address8, 0x5au);
		if (MTGS::s_gs->m_mem.ReadPixel8(address8) != 0x5au)
			return fail("8-bit GS scalar wrapping changed the stored value");
		// address4 is a nibble address for the same physical byte as address8.
		// Validate it only after the complete-byte round trip above.
		MTGS::s_gs->m_mem.WritePixel4(address4, 0x0du);
		if (MTGS::s_gs->m_mem.ReadPixel4(address4) != 0x0du)
		{
			return fail("4-bit GS scalar wrapping changed the stored value");
		}
		result->address_checks = 8;

		// PCSX2's CSM1 loaders consume two or four contiguous blocks. Compare a
		// normal source against the same bytes split across the canonical seam;
		// this enters all three cold staging call sites without duplicating the
		// owner's CLUT swizzle algorithm in the test.
		std::array<u8, 1024> source_bytes = {};
		std::array<u32, 256> reference_palette = {};
		std::array<u32, 256> wrapped_palette = {};
		const GIFRegTEXCLUT texclut = {};
		GIFRegTEXA texa = {};
		texa.TA0 = 0x80;
		texa.TA1 = 0xff;

		auto block_pointer = [&](u32 cpsm, u32 bp) -> u8* {
			switch (cpsm)
			{
				case PSMCT32: return MTGS::s_gs->m_mem.BlockPtr32(0, 0, bp, 1);
				case PSMCT16: return MTGS::s_gs->m_mem.BlockPtr16(0, 0, bp, 1);
				case PSMCT16S: return MTGS::s_gs->m_mem.BlockPtr16S(0, 0, bp, 1);
				default: return nullptr;
			}
		};
		auto load_palette = [&](u32 cpsm, u32 cbp,
			std::array<u32, 256>* output) {
			GIFRegTEX0 tex0 = {};
			tex0.PSM = PSMT8;
			tex0.CBP = cbp;
			tex0.CPSM = cpsm;
			tex0.CSM = 0;
			tex0.CSA = 0;
			MTGS::s_gs->m_mem.m_clut.Reset();
			MTGS::s_gs->m_mem.m_clut.Write(tex0, texclut);
			MTGS::s_gs->m_mem.m_clut.Read32(tex0, texa);
			for (u32 i = 0; i < output->size(); i++)
				(*output)[i] = MTGS::s_gs->m_mem.m_clut[i];
		};

		constexpr std::array<u32, 3> CLUT_FORMATS = {
			PSMCT32, PSMCT16, PSMCT16S,
		};
		for (u32 cpsm : CLUT_FORMATS)
		{
			const size_t span = (cpsm == PSMCT32) ? 1024 : 512;
			u32 pattern = 0x6d2b79f5u ^ (cpsm * 0x9e3779b9u);
			for (size_t i = 0; i < span; i++)
			{
				pattern = pattern * 1664525u + 1013904223u;
				source_bytes[i] = static_cast<u8>(pattern >> 24);
			}

			std::memset(local, 0, local_bytes);
			u8* const normal_source = block_pointer(cpsm, 0x0100);
			if (!normal_source)
				return fail("CLUT validation selected an unsupported source format");
			std::memcpy(normal_source, source_bytes.data(), span);
			load_palette(cpsm, 0x0100, &reference_palette);

			std::memset(local, 0, local_bytes);
			u8* const wrapped_source = block_pointer(cpsm, 0x3fff);
			if (!wrapped_source)
				return fail("CLUT boundary source pointer was unavailable");
			const size_t tail = static_cast<size_t>((local + local_bytes) - wrapped_source);
			if (tail >= span)
				return fail("CLUT boundary source did not cross the canonical seam");
			std::memcpy(wrapped_source, source_bytes.data(), tail);
			std::memcpy(local, source_bytes.data() + tail, span - tail);
			load_palette(cpsm, 0x3fff, &wrapped_palette);
			if (wrapped_palette != reference_palette)
				return fail("seam-staged CSM1 palette differs from PCSX2's contiguous loader");
			result->clut_cases++;
		}

		// The optimized PSMCT32 download reads two aligned eight-pixel groups.
		// Starting at the final block makes the second group advance from block
		// 0x3fff to block 0, while each vector load remains within one block.
		std::array<u32, 16> expected_readback = {};
		alignas(16) std::array<u32, 16> actual_readback = {};
		for (u32 i = 0; i < expected_readback.size(); i++)
		{
			expected_readback[i] = 0xa5000000u | (i * 0x010101u);
			MTGS::s_gs->m_mem.WritePixel32(static_cast<int>(i), 0,
				expected_readback[i], 0x3fff, 1);
		}
		GIFRegBITBLTBUF blit = {};
		blit.SBP = 0x3fff;
		blit.SBW = 1;
		blit.SPSM = PSMCT32;
		GIFRegTRXPOS position = {};
		position.SSAX = 0;
		GIFRegTRXREG extent = {};
		extent.RRW = static_cast<u32>(expected_readback.size());
		extent.RRH = 1;
		int tx = position.SSAX;
		int ty = position.SSAY;
		MTGS::s_gs->m_mem.ReadImageX(tx, ty,
			reinterpret_cast<u8*>(actual_readback.data()),
			static_cast<int>(actual_readback.size() * sizeof(actual_readback[0])),
			blit, position, extent);
		if (actual_readback != expected_readback || tx != position.SSAX || ty != 1)
			return fail("optimized GS readback differed at the canonical seam");
		result->readback_checks = static_cast<u32>(actual_readback.size());
			return true;
		}();
	});
	MTGS::WaitGS(false);

	// Transfer() retains upload bookkeeping which Reset() intentionally does
	// not discard. Destroy and recreate the mailbox-owned GSState so the guest
	// begins from the same clean state it had before this validation.
	MTGS::WaitForClose();
	const bool reopened = MTGS::WaitForOpen();
	if (reopened)
	{
		MTGS::ResetGS(true);
		MTGS::WaitGS(false);
	}

	size_t reopened_bytes = 0;
	const u8* const reopened_local = reopened ?
		GSTraceLocalMemoryData(&reopened_bytes) : nullptr;
	result->reopened_hash = reopened_local ?
		Pcsx2Trace::HashGsTraceBytes(reopened_local, reopened_bytes) : 0;
	result->reopened_clean = reopened && MTGS::IsOpen() &&
		reopened_bytes == CANONICAL_BYTES && result->reopened_hash == ZERO_HASH;
	if (!result->reopened_clean && !failure)
		failure = "GSState did not reopen with a clean canonical local-memory window";

	if (!content_ok || !result->reopened_clean)
	{
		Error::SetString(error, failure ? failure : "GS canonical-ring validation failed.");
		return false;
	}
	return true;
}
#endif

void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path)
{
	(void)gsPack;
	MTGS::SendSimpleGSPacket(MTGS::Command::MTVUGSPacket, 0, 0, path);
}

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	pxAssertMsg(!gsPack.readAmount,
		"Gif Unit - gsPack.readAmount is only valid for MTVU path 1");
	gifUnit.gifPath[path].readAmount.fetch_add(gsPack.size,
		std::memory_order_acq_rel);
	MTGS::SendSimpleGSPacket(MTGS::Command::GSPacket, gsPack.offset,
		gsPack.size, path);
}

void Gif_AddBlankGSPacket(u32 size, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(size, std::memory_order_acq_rel);
	MTGS::SendSimpleGSPacket(MTGS::Command::GSPacket, ~0u, size, path);
}

void Gif_MTGS_Wait(bool isMTVU)
{
	MTGS::WaitGS(false, true, isMTVU);
}
