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
#include "vita/VitaGxmGsState.h"
#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
#include "vita/VitaGsDrawTrace.h"
#endif
#include "vita/VitaGsMailbox.h"
#include "vita/VitaCore.h"
#include "vita/VitaGpuVuDirectProgram.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuVifInput.h"
#include "vita/VitaPerformanceTelemetry.h"
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
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
	};

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

			completion.first_draw = remaining_first_draw;
			completion.reservation_count -= count;
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
	};

	struct GsWorkerPerformanceTotals
	{
		u64 commands = 0;
		u64 gs_packets = 0;
		u64 gs_packet_bytes = 0;
		u64 mtvu_packets = 0;
		u64 mtvu_packet_bytes = 0;
		u64 mtvu_path1_completion_deferrals = 0;
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
	static std::atomic<u64> s_gs_published_completed_vsyncs{0};

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
		CorrelatedPerformanceBatch()
		{
			m_lines.reserve(8192);
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

			if (!m_lines.empty())
				m_lines.push_back('\n');
			if (static_cast<size_t>(length) < sizeof(local))
			{
				m_lines.append(local, static_cast<size_t>(length));
			}
			else
			{
				const size_t offset = m_lines.size();
				m_lines.resize(offset + static_cast<size_t>(length) + 1);
				std::vsnprintf(m_lines.data() + offset,
					static_cast<size_t>(length) + 1, format, retry_arguments);
				m_lines.resize(offset + static_cast<size_t>(length));
			}
			va_end(retry_arguments);
		}

		void Flush() const
		{
			if (!m_lines.empty())
			{
				Log::WriteMultilineBatch(
					LOGLEVEL_INFO, Color_Default, m_lines);
			}
		}

	private:
		std::string m_lines;
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
		VitaA32EeProviderStats ee;
		VitaVU::Vu0TelemetryStats vu0;
		VitaVU::Vu1TelemetryStats vu1;
		VU_Thread::ProducerProfileStats mtvu{};
		GsProducerPerformanceTotals gs_producer;
		GsWorkerPerformanceTotals gs_worker;
		VitaGxmPerformanceCounters gxm;
		VitaGpuVu::DirectProgramStatistics gpu_vu_direct;
		VitaGpuVu::ProgramRegistryStatistics gpu_vu_programs;
		VitaGpuVu::InputRingStatistics gpu_vu_input;
		VitaGpuVu::DrawStatistics gpu_vu_draw;
	};

	struct CorrelatedPerformanceProfile
	{
		u64 producer_vsyncs = 0;
		u64 producer_vsync_origin = 0;
		u32 sampling_boundaries = 0;
		u32 boundaries_at_start = 0;
		u64 window = 0;
		bool started = false;
		bool elf_origin = false;
		CorrelatedPerformanceSnapshot origin;
		CorrelatedPerformanceSnapshot start;
	};
	static CorrelatedPerformanceProfile s_correlated_profile;

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
		snapshot.ee = VitaGetA32EeProviderStats();
		snapshot.vu0 = VitaVU::GetVu0TelemetryStats();
		snapshot.vu1 = VitaVU::GetVu1TelemetryStats();
		snapshot.mtvu = vu1Thread.GetProducerProfileStats();
		snapshot.gs_producer = s_gs_producer_performance;
		snapshot.gs_worker = GetPublishedGsWorkerPerformance();
		snapshot.completed_vsyncs = snapshot.gs_worker.completed_vsyncs;
		snapshot.gxm = VitaGxmGetPublishedPerformanceCounters();
		snapshot.gpu_vu_direct = VitaGpuVu::GetDirectProgramStatistics();
		snapshot.gpu_vu_programs =
			VitaGpuVu::GetGeneratedProgramRegistryStatistics();
		snapshot.gpu_vu_input = VitaGpuVu::GetInputRingStatistics();
		snapshot.gpu_vu_draw = VitaGpuVu::GetGpuVuDrawStatistics();
		return snapshot;
	}

	static void RecordCorrelatedPerformanceProfile()
	{
		constexpr u32 WARMUP_VSYNCS = 60;
		constexpr u32 WINDOW_VSYNCS = 120;
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
			s_correlated_profile.boundaries_at_start = boundary;
			s_correlated_profile.started = true;
			return;
		}
		if (boundary - s_correlated_profile.boundaries_at_start < WINDOW_VSYNCS)
			return;

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
		CorrelatedPerformanceBatch output;

		output.WriteLn(
			"Vita perf v=1 window=%llu kind=anchor origin=%s origin_vsync=%llu "
			"producer_vsync_start=%llu "
			"producer_vsync_end=%llu completed_vsync_start=%llu completed_vsync_end=%llu "
			"ee_cycle_start=%llu ee_cycle_end=%llu ee_pc_start=%08x ee_pc_end=%08x "
			"iop_cycle_start=%llu iop_cycle_end=%llu iop_pc_start=%08x iop_pc_end=%08x",
			static_cast<unsigned long long>(window),
			s_correlated_profile.elf_origin ? "elf" : "boot",
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
			const u64 stage_samples = CounterDelta(
				end.cpu_stage_profiler.stage_samples,
				start.cpu_stage_profiler.stage_samples);
			const u64 unbalanced_samples = CounterDelta(
				end.cpu_stage_profiler.unbalanced_samples,
				start.cpu_stage_profiler.unbalanced_samples);
			output.WriteLn(
				"Vita perf v=1 window=%llu kind=cpu_stage_summary "
				"sample_period=%u scheduler_entries=%llu samples=%llu "
				"unbalanced=%llu",
				static_cast<unsigned long long>(window),
				VitaPerformanceTelemetry::CPU_STAGE_SAMPLE_PERIOD,
				static_cast<unsigned long long>(scheduler_entries),
				static_cast<unsigned long long>(stage_samples),
				static_cast<unsigned long long>(unbalanced_samples));
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
				end.ee.two_predicate_wait_fast_forwards,
				start.ee.two_predicate_wait_fast_forwards)),
			static_cast<unsigned long long>(CounterDelta(end.ee.invalidated_blocks,
				start.ee.invalidated_blocks)),
			static_cast<unsigned long long>(CounterDelta(end.ee.failed_blocks,
				start.ee.failed_blocks)));
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
			"Vita perf v=1 window=%llu kind=gpu_vu_input captures=%llu bytes=%llu "
			"publish_batches=%llu published_bytes=%llu "
			"capture_bypasses=%llu capture_bypass_bytes=%llu "
			"fallbacks=%llu slot_reuses=%llu ring_waits=%llu ring_spins=%llu "
			"deferred=%llu affine_merges=%llu replayed=%llu "
			"live_start=%llu live_end=%llu peak_live=%llu",
			static_cast<unsigned long long>(window),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.captures, start.gpu_vu_input.captures)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.captured_bytes,
				start.gpu_vu_input.captured_bytes)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.publication_batches,
				start.gpu_vu_input.publication_batches)),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_input.published_bytes,
				start.gpu_vu_input.published_bytes)),
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
			static_cast<unsigned long long>(
				start.gpu_vu_input.live_references),
			static_cast<unsigned long long>(
				end.gpu_vu_input.live_references),
			static_cast<unsigned long long>(
				end.gpu_vu_input.peak_live_references));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gpu_vu_program prepared=%llu "
			"prepare_hits=%llu evictions=%llu analysis_failures=%llu "
			"without_parallel=%llu candidates=%llu prime_attempts=%llu "
			"prime_hits=%llu stale_tokens=%llu gif_address_failures=%llu "
			"gif_contract_rejections=%llu generated_roots=%llu "
			"compiler_requests=%llu compiler_request_retries=%llu "
			"continuation_shared=%llu continuation_general=%llu "
			"registry_requests=%llu unavailable=%llu registry_hits=%llu "
			"registry_misses=%llu queue_retries=%llu compile_successes=%llu "
			"compile_failures=%llu ready=%llu failed=%llu "
			"compiler_submit_attempts=%llu compiler_submitted=%llu "
			"compiler_reject_state=%llu compiler_reject_source=%llu "
			"compiler_reject_capacity=%llu compiler_reject_duplicate=%llu "
			"compiler_dequeued=%llu compiler_starts=%llu "
			"compiler_completions=%llu compiler_successes=%llu "
			"compiler_failures=%llu compiler_polled=%llu "
			"compiler_dropped=%llu compiler_time_us=%llu "
			"compiler_longest_us=%llu compiler_pending_end=%llu "
			"compiler_results_end=%llu compiler_in_flight_end=%llu "
			"compiler_active_end=%llu compiler_invalid_output=%llu "
			"compiler_diag_truncated=%llu compiler_arena_capacity=%llu "
			"compiler_arena_peak=%llu compiler_arena_current=%llu "
			"compiler_arena_guard_failures=%llu",
			static_cast<unsigned long long>(window),
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
				end.gpu_vu_programs.compiler.truncated_diagnostics,
				start.gpu_vu_programs.compiler.truncated_diagnostics)),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.private_arena_capacity),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.private_arena_peak),
			static_cast<unsigned long long>(
				end.gpu_vu_programs.compiler.private_arena_current),
			static_cast<unsigned long long>(CounterDelta(
				end.gpu_vu_programs.compiler.private_arena_guard_failures,
				start.gpu_vu_programs.compiler.private_arena_guard_failures)));
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
			"path1_completion_ring_waits=%llu path1_completion_deferrals=%llu",
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
				start.gs_worker.mtvu_path1_completion_deferrals)));
		output.WriteLn(
			"Vita perf v=1 window=%llu kind=gxm draws=%llu indices=%llu "
			"vertex_bytes=%llu index_bytes=%llu texture_uploads=%llu "
			"texture_upload_bytes=%llu readbacks=%llu readback_bytes=%llu "
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

		output.Flush();
		s_correlated_profile.start = end;
		s_correlated_profile.boundaries_at_start = boundary;
	}
#endif

	static void SetEvent();
	static void MainLoop();

	static void PublishMtvuPath1Completion(
		const MtvuPath1Completion& completion)
	{
		pxAssertRel(completion.reservation_count != 0,
			"empty MTVU PATH1 completion run");
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

	static void FlushPendingMtvuDirectRun()
	{
		if (!s_pending_mtvu_direct_head)
			return;
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
		pxAssertRel(VitaGpuVu::PublishPendingRawVifPayloads(
				s_pending_mtvu_direct_head, s_pending_mtvu_direct_count),
			"failed to publish immutable GPU-VU inputs");
		PublishMtvuPath1Completion({
			s_pending_mtvu_direct_head,
			s_pending_mtvu_direct_count,
		});
		s_pending_mtvu_direct_head = nullptr;
		s_pending_mtvu_direct_tail = nullptr;
		s_pending_mtvu_direct_count = 0;
	}

	static void AppendPendingMtvuDirectDraw(VitaGpuVu::GpuVuDraw* draw)
	{
		pxAssertRel(draw && !draw->path1_next,
			"invalid direct draw appended to MTVU PATH1 run");
		if (!draw)
			return;
		if (s_pending_mtvu_direct_tail)
			s_pending_mtvu_direct_tail->path1_next = draw;
		else
			s_pending_mtvu_direct_head = draw;
		s_pending_mtvu_direct_tail = draw;
		s_pending_mtvu_direct_count++;
		if (s_pending_mtvu_direct_count >= MaximumPendingMtvuDirectRun)
			FlushPendingMtvuDirectRun();
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
		// PCSX2 owner: GS.cpp::OpenGSDevice(). GSDeviceGXM owns the process's
		// only immediate GXM context and must precede GSRendererHW/GSTextureCache.
		g_gs_device = std::make_unique<GSDeviceGXM>();
		if (!g_gs_device->Create(VMManager::GetEffectiveVSyncMode(),
			VMManager::ShouldAllowPresentThrottle()))
		{
			g_gs_device->Destroy();
			g_gs_device.reset();
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

		pxAssertRel(!IsOpen(), "GS worker should be closed when starting");
		s_read_pos.store(0, std::memory_order_relaxed);
		s_write_pos.store(0, std::memory_order_relaxed);
		for (std::atomic<u32>& run : s_mtvu_reservation_runs)
			run.store(0, std::memory_order_relaxed);
		s_open_mtvu_reservation_run = NoOpenMtvuReservationRun;
		s_mtvu_reservations_remaining = 0;
		s_mtvu_path1_completions.ResetAndDiscard();
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
		if (!s_thread.Joinable())
			return;

		s_shutdown_flag.store(true, std::memory_order_release);
		if (IsOpen())
			WaitForClose();
		s_work_sema.NotifyOfWork();
		s_thread.Join();
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

	static void MainLoop()
	{
#if defined(__vita__)
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();
#endif
		while (true)
		{
			s_work_sema.WaitForWork();

			if (!s_open_flag.load(std::memory_order_acquire))
				break;

#if defined(__vita__)
			// Completed ShaccCg output is registered only here, on the thread
			// which owns the immediate context and shader patcher. Compilation
			// itself never blocks this worker.
			PollGpuVuProgramsOnOwner(true);
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
							s_gs->Transfer<3>(&path.buffer[offset], size / 16);
						}
						path.readAmount.fetch_sub(size, std::memory_order_acq_rel);
						break;
					}

					case Command::MTVUGSPacket:
					{
						MtvuPath1Completion completion;
						if (!TryTakeMtvuPath1Completion(&completion))
						{
							// Retain the reservation at the head of the MTGS
							// ring. The ordinary worker semaphore sleeps until
							// the single MTVU producer publishes its immutable
							// completion; no per-dispatch semaphore resource is
							// consumed.
							ring_advance = 0;
							break;
						}
						pxAssertRel(completion.reservation_count != 0,
							"MTVU PATH1 completion has no EE reservation");
						if (completion.reservation_count == 0)
							break;

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
						const u32 reservation_prefix = std::min(
							completion.reservation_count,
							s_mtvu_reservations_remaining);
						pxAssertRel(reservation_prefix != 0,
							"MTVU PATH1 command has no logical reservation");
						if (reservation_prefix == 0)
							break;
						s_mtvu_reservations_remaining -= reservation_prefix;
						ring_advance =
							s_mtvu_reservations_remaining == 0 ? 1 : 0;
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
							std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>>
								draws;
							draws.reserve(reservation_prefix);
							VitaGpuVu::GpuVuDraw* direct_draw =
								completion.first_draw;
							for (u32 index = 0;
								index < reservation_prefix; index++)
							{
								if (!direct_draw)
									break;
								VitaGpuVu::GpuVuDraw* const next =
									direct_draw->path1_next;
								direct_draw->path1_next = nullptr;
								draws.emplace_back(direct_draw);
								VitaGpuVu::RecordGpuVuDrawConsumed();
								direct_draw = next;
							}
							pxAssertRel(
								draws.size() ==
										reservation_prefix,
								"MTVU PATH1 direct prefix is shorter than "
								"its logical reservations");
							s_mtvu_path1_completions.ConsumePrefix(
								reservation_prefix, direct_draw);
							if (s_gs)
							{
								s_gs->ConsumeGpuVuDraws(std::move(draws));
							}
							else
							{
								for (const auto& draw : draws)
									VitaGpuVu::RecordGpuVuDrawRejected();
							}
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
							s_gs->Transfer<3>(&path.buffer[packet.offset], packet.size / 16);
						}
						path.readAmount.fetch_sub(packet.size + packet.readAmount,
							std::memory_order_acq_rel);
						path.PopGSPacketMTVU();
						s_mtvu_path1_buffer_progress.NotifyOfProgress();
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
		}

		s_read_pos.store(s_write_pos.load(std::memory_order_acquire),
			std::memory_order_release);
		s_mtvu_path1_buffer_progress.PublishQuiescence();
		s_work_sema.Kill();
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

		// A VSync can make the EE wait for the GS worker. Publish any partial
		// direct PATH1 run first so a frame with fewer than the size threshold
		// cannot leave GS asleep at an earlier MTVUGSPacket reservation.
		vu1Thread.RequestGpuVuPath1Flush();

#if defined(__vita__)
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();
		if (performance_telemetry_enabled)
		{
			RecordHardwareVsyncProfile(s_producer_profile, "producer");
			RecordCorrelatedPerformanceProfile();
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
		s_vsync_sema.Wait();
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

void VitaGS::CompleteMtvuPath1Packet()
{
	if (MTGS::IsOpen())
	{
		MTGS::FlushPendingMtvuDirectRun();
		MTGS::PublishMtvuPath1Completion({nullptr, 1});
	}
}

void VitaGS::FlushMtvuPath1Completions()
{
	if (MTGS::IsOpen())
		MTGS::FlushPendingMtvuDirectRun();
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
	MTGS::s_correlated_profile.elf_origin = true;
	MTGS::s_correlated_profile.origin =
		MTGS::CaptureCorrelatedPerformanceSnapshot(
			MTGS::s_correlated_profile.producer_vsyncs);
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
