// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Gif_Unit.h"
#include "Config.h"
#include "DebugTools/GsTrace.h"
#include "GS.h"
#include "GS/GSPerfMon.h"
#include "GS/GSState.h"
#include "MTGS.h"
#include "MTVU.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FPControl.h"
#include "common/Timer.h"
#include "common/WrappedMemCopy.h"
#include "vita/VitaGxmGsState.h"
#include "vita/VitaGsMailbox.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

Pcsx2Config::GSOptions GSConfig;

GSRendererType GSGetCurrentRenderer()
{
	return GSRendererType::SW;
}

bool GSIsHardwareRenderer()
{
	// This predicate selects PCSX2 hardware texture-cache, transfer, reset and
	// PCRTC semantics; it is not an accelerator-presence bit. VitaGxmGsState
	// still owns canonical GSLocalMemory like the software path, so reporting
	// hardware here would enter unported GSRendererHW mechanisms.
	return false;
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
	static std::mutex s_mtvu_wait_mutex;
	static Threading::WorkSema s_work_sema;
	static Threading::UserspaceSemaphore s_ring_reset_sema;
	static Threading::UserspaceSemaphore s_vsync_sema;
	static Threading::UserspaceSemaphore s_open_or_close_done;
	static int s_copy_data_tally = 0;
	static Threading::Thread s_thread;
	static std::atomic_bool s_open_flag{false};
	static std::atomic_bool s_shutdown_flag{false};
	static bool s_native_presenter_enabled = false;
	static std::unique_ptr<VitaGxmGsState> s_gs;

#if defined(__vita__)
	struct HardwareVsyncProfile
	{
		u32 boundaries = 0;
		Common::Timer::Value wall_start = 0;
		u64 cpu_start = 0;
	};
	static HardwareVsyncProfile s_producer_profile;
	static HardwareVsyncProfile s_worker_profile;
	static std::atomic<u32> s_profile_ring_stalls{0};
	static std::atomic<u32> s_profile_vsync_waits{0};
	static std::atomic<int> s_profile_max_queued_frames{0};

	static void RecordHardwareVsyncProfile(HardwareVsyncProfile& profile,
		const char* owner)
	{
		constexpr u32 WARMUP_BOUNDARIES = 60;
		constexpr u32 PROFILE_INTERVALS = 120;
		if (!s_native_presenter_enabled ||
			profile.boundaries > WARMUP_BOUNDARIES + PROFILE_INTERVALS)
			return;

		const u32 boundary = profile.boundaries++;
		if (boundary == WARMUP_BOUNDARIES)
		{
			profile.wall_start = Common::Timer::GetCurrentValue();
			profile.cpu_start = Threading::GetThreadCpuTime();
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
		Console.WriteLn("Vita MTGS %s profile: warmup_vsyncs=60 interval_vsyncs=120 wall_us=%llu cpu_us=%llu cpu_util=%.1f%% ring_stalls=%u vsync_waits=%u max_queued=%d",
			owner,
			static_cast<unsigned long long>(wall_us),
			static_cast<unsigned long long>(cpu_us),
			utilization,
			s_profile_ring_stalls.load(std::memory_order_relaxed),
			s_profile_vsync_waits.load(std::memory_order_relaxed),
			s_profile_max_queued_frames.load(std::memory_order_relaxed));
	}
#endif

	static void SetEvent();
	static void MainLoop();

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

	static bool OpenGsOnWorker()
	{
		GSConfig = EmuConfig.GS;
		GSConfig.Renderer = GSRendererType::SW;
		GSConfig.UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;

		s_gs = std::make_unique<VitaGxmGsState>(s_native_presenter_enabled);
		if (s_native_presenter_enabled && !s_gs->IsNativePresenterReady())
		{
			s_gs.reset();
			return false;
		}
		s_gs->SetRegsMem(s_ring.regs);
		// PCSX2 owner: GS/GS.cpp::OpenGSRenderer(). Construction establishes
		// GSState; the MTGS::ResetGS() caller applies the requested hardware or
		// soft reset. Repeating a hardware reset here was both redundant and
		// observably different from the renderer lifecycle owner.
		g_perfmon.Reset();
		return true;
	}

	static void ProcessVSync(bool registers_written)
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
		s_gs->VSync();
#if defined(__vita__)
		RecordHardwareVsyncProfile(s_worker_profile, "worker");
#endif
		g_perfmon.EndFrame(false);
		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();
		// PCSX2 owner: GS.cpp::GSvsync() snapshots after Flush() and VSync().
		s_gs->TraceGsStateSnapshot(Pcsx2Trace::GsTraceStateTriggerVSyncStart);
		(void)registers_written;
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
			s_gs.reset();
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
		s_work_sema.Reset();
		s_shutdown_flag.store(false, std::memory_order_release);
		s_thread.SetStackSize(256 * 1024);
		if (!s_thread.Start(&ThreadEntryPoint))
		{
			Console.Error("Failed to start the Vita GS worker.");
			return;
		}
#if defined(__vita__)
		// PCSX2's three-user-core layout starts with EE on core 0 and GS on
		// core 1 while MTVU is absent. A rejected affinity is non-fatal.
		if (!s_thread.SetAffinity(1u << 1))
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

	static void MainLoop()
	{
		std::unique_lock mtvu_lock(s_mtvu_wait_mutex);
		while (true)
		{
			mtvu_lock.unlock();
			s_work_sema.WaitForWork();
			mtvu_lock.lock();

			if (!s_open_flag.load(std::memory_order_acquire))
				break;

			while (s_read_pos.load(std::memory_order_relaxed) !=
				s_write_pos.load(std::memory_order_acquire))
			{
				const u32 read_pos = s_read_pos.load(std::memory_order_relaxed);
				const PacketTag& tag = reinterpret_cast<const PacketTag&>(s_ring[read_pos]);
				u32 ring_advance = 1;

				switch (static_cast<Command>(tag.command))
				{
					case Command::GSPacket:
					{
						const GIF_PATH path_index = static_cast<GIF_PATH>(tag.data[2]);
						Gif_Path& path = gifUnit.gifPath[path_index];
						const u32 offset = tag.data[0];
						const u32 size = tag.data[1];
						if (s_gs && offset != ~0u)
						{
							const Pcsx2Trace::ScopedGsTraceSourceOverride trace_source(
								GsTraceSourceForGifPath(path_index));
							s_gs->Transfer<3>(&path.buffer[offset], size / 16);
						}
						path.readAmount.fetch_sub(size, std::memory_order_acq_rel);
						break;
					}

					case Command::MTVUGSPacket:
					{
						if (!vu1Thread.semaXGkick.TryWait())
						{
							mtvu_lock.unlock();
							vu1Thread.semaXGkick.Wait();
							mtvu_lock.lock();
						}
						Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
						const GS_Packet packet = path.GetGSPacketMTVU();
						if (s_gs && packet.size)
						{
							const Pcsx2Trace::ScopedGsTraceSourceOverride trace_source(
								Pcsx2Trace::GsTraceSourcePath1);
							s_gs->Transfer<3>(&path.buffer[packet.offset], packet.size / 16);
						}
						path.readAmount.fetch_sub(packet.size + packet.readAmount,
							std::memory_order_acq_rel);
						path.PopGSPacketMTVU();
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
						ProcessVSync(snapshot.registers_written != 0);
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
							data->retval = s_gs->Defrost(data->fdata);
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

				const u32 new_read_pos = (read_pos + ring_advance) & RingBufferMask;
				s_read_pos.store(new_read_pos, std::memory_order_release);
				if (s_signal_ring_enabled.load(std::memory_order_acquire) &&
					s_signal_ring_position.fetch_sub(static_cast<int>(ring_advance),
						std::memory_order_acq_rel) <= 0)
				{
					s_signal_ring_enabled.store(false, std::memory_order_release);
					s_ring_reset_sema.Post();
				}
			}

			if (s_signal_ring_enabled.exchange(false, std::memory_order_acq_rel))
			{
				s_signal_ring_position.store(0, std::memory_order_release);
				s_ring_reset_sema.Post();
			}
			if (s_vsync_signal_listener.exchange(false, std::memory_order_acq_rel))
				s_vsync_sema.Post();
		}

		s_read_pos.store(s_write_pos.load(std::memory_order_acquire),
			std::memory_order_release);
		s_work_sema.Kill();
	}

	static void GenericStall(u32 size)
	{
		const u32 write_pos = s_write_pos.load(std::memory_order_relaxed);
		pxAssert(size < RingBufferSize);
		u32 read_pos = s_read_pos.load(std::memory_order_acquire);
		u32 free_room = write_pos < read_pos ? read_pos - write_pos :
			RingBufferSize - (write_pos - read_pos);
		if (free_room > size)
			return;

#if defined(__vita__)
		if (s_native_presenter_enabled)
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
				read_pos = s_read_pos.load(std::memory_order_acquire);
				free_room = write_pos < read_pos ? read_pos - write_pos :
					RingBufferSize - (write_pos - read_pos);
				if (free_room > size)
					break;
			}
		}
	}

	static void PrepareDataPacket(Command command, u32 size)
	{
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

	static void SendSimplePacket(Command command, u32 data0, u32 data1, u32 data2)
	{
		GenericStall(1);
		PacketTag& tag = reinterpret_cast<PacketTag&>(
			s_ring[s_write_pos.load(std::memory_order_relaxed)]);
		tag.command = static_cast<u32>(command);
		tag.data[0] = data0;
		tag.data[1] = data1;
		tag.data[2] = data2;
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
		GenericStall(1);
		PacketTag& tag = reinterpret_cast<PacketTag&>(
			s_ring[s_write_pos.load(std::memory_order_relaxed)]);
		tag.command = static_cast<u32>(command);
		tag.data[0] = data0;
		tag.pointer = reinterpret_cast<uptr>(pointer);
		FinishSimplePacket();
	}

	void PresentCurrentFrame()
	{
		if (!IsOpen())
			return;
		RunOnGSThread([]() {
			if (s_gs)
				s_gs->Present();
		});
	}

	void WaitGS(bool sync_regs, bool weak_wait, bool is_mtvu)
	{
		if (!IsOpen())
			return;

		SetEvent();
		if (weak_wait && is_mtvu)
		{
			Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
			const u32 pending_packets = path.GetPendingGSPackets();
			if (pending_packets)
			{
				while (true)
				{
					std::lock_guard lock(s_mtvu_wait_mutex);
					if (path.GetPendingGSPackets() != pending_packets)
						break;
				}
			}
		}
		else if (!s_work_sema.WaitForEmpty())
		{
			pxFailRel("Vita GS worker died while waiting for an empty queue");
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

#if defined(__vita__)
		RecordHardwareVsyncProfile(s_producer_profile, "producer");
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
		if (s_native_presenter_enabled)
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
			if (s_native_presenter_enabled)
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
		if (s_native_presenter_enabled)
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
		options.Renderer = GSRendererType::SW;
		options.UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;
		RunOnGSThread([options = std::move(options)]() {
			Pcsx2Config::GSOptions old_options = std::move(GSConfig);
			GSConfig = options;
			if (s_gs)
				s_gs->UpdateSettings(old_options);
		});
	}

	void ResizeDisplayWindow(u32 width, u32 height, float scale)
	{
		(void)width;
		(void)height;
		(void)scale;
	}

	void UpdateDisplayWindow()
	{
	}

	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
	{
		(void)mode;
		(void)allow_present_throttle;
	}

	void UpdateVSyncMode()
	{
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
	// The GS owner must be selected before VMManager opens it. Changing GXM
	// process ownership while a GSState is live would violate libgxm teardown.
	pxAssertRel(!MTGS::IsOpen(),
		"native GS presenter selection changed while open");
	MTGS::s_native_presenter_enabled = enabled;
}

bool VitaGS::IsNativePresenterEnabled()
{
	return MTGS::s_native_presenter_enabled;
}

bool GSValidatePortableState()
{
	// PCSX2 owner: GS/GS.cpp::GSValidatePortableState(). The Vita mailbox owns
	// the live canonical GSState instance instead of g_gs_renderer. Query it on
	// the worker so its GS/GXM ownership never migrates back to the EE thread.
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
