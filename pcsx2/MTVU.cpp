// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "VMManager.h"
#include "Vif_Dynarec.h"
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#include "vita/VitaGpuVuDirectProgram.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGsMailbox.h"
#include "vita/VitaPerformanceTelemetry.h"
#include "vita/VitaVuBlockCompiler.h"

#include <array>
#include <cstring>
#include <thread>
#include <utility>

VU_Thread vu1Thread;

#define MTVU_ALWAYS_KICK 0
#define MTVU_SYNC_MODE 0

// A direct affine journal normally collapses to the latest few VU-memory
// ranges. Keep its storage fixed after construction and materialize only when
// an adversarial partial-overlap stream exceeds this bounded first lowering.
static constexpr size_t MaximumDeferredAffineSpans = 64;

// Rounds up a size in bytes for size in u32's
static __fi u32 size_u32(u32 x) { return (x + 3) >> 2; }

enum MTVU_EVENT
{
	MTVU_VU_EXECUTE,     // Execute VU program
	MTVU_VU_EXECUTE_DIRECT, // Execute with an admitted direct-program token
	// Cold-compiles an explicit-entry root on the worker, but deliberately
	// retains CpuVU1 execution until its MSCNT continuation state is owned.
	MTVU_VU_EXECUTE_DIRECT_PRIME,
	MTVU_VU_WRITE_MICRO, // Write to VU micro-mem
	MTVU_VU_WRITE_DATA,  // Write to VU data-mem
	MTVU_VU_WRITE_VIREGS,// Write to VU registers
	MTVU_VU_WRITE_VFREGS,// Write to VU registers
	MTVU_VIF_WRITE_COL,  // Write to Vif col reg
	MTVU_VIF_WRITE_ROW,  // Write to Vif row reg
	MTVU_VIF_UNPACK,     // Execute Vif Unpack
	MTVU_VIF_UNPACK_CAPTURED, // Immutable raw VIF payload, deferred to Execute
	MTVU_FLUSH_VIF_UNPACKS,   // Materialize before an external observer
	MTVU_NULL_PACKET,    // Go back to beginning of buffer
	MTVU_RESET
};

// Calls the vif unpack functions from the MTVU thread
static void MTVU_Unpack(void* data, VIFregisters& vifRegs)
{
	u16 wl = vifRegs.cycle.wl > 0 ? vifRegs.cycle.wl : 256;
	bool isFill = vifRegs.cycle.cl < wl;
	if (newVifDynaRec)
		dVifUnpack<1>((u8*)data, isFill);
	else
		_nVifUnpack(1, (u8*)data, vifRegs.mode, isFill);
}

struct GpuVuMemoryView
{
	const u8* canonical = nullptr;
	const std::vector<VitaGpuVu::VifUnpackSpan>* spans = nullptr;
};

static bool ReadGpuVuMemoryU32(void* user, u16 qword_address, u8 lane,
	u32* value)
{
	if (!user || !value || lane >= 4)
		return false;
	const GpuVuMemoryView& view =
		*static_cast<const GpuVuMemoryView*>(user);
	const u32 address = static_cast<u32>(qword_address) & 0x3ffu;
	if (view.spans)
	{
		for (auto it = view.spans->rbegin(); it != view.spans->rend(); ++it)
		{
			if (!VitaGpuVu::IsDirectAffineV4_32Span(*it))
				continue;
			const u32 vector =
				(address - static_cast<u32>(it->destination_qword)) & 0x3ffu;
			if (vector >= it->vector_count)
				continue;
			const u8* const payload =
				VitaGpuVu::ResolveRawVifPayload(it->payload);
			const u32 byte_offset = vector * 16u + lane * sizeof(u32);
			if (!payload || byte_offset > it->payload.size ||
				sizeof(u32) > it->payload.size - byte_offset)
			{
				return false;
			}
			std::memcpy(value, payload + byte_offset, sizeof(*value));
			return true;
		}
	}
	if (!view.canonical)
		return false;
	const u32 byte_address = address * 16u + lane * sizeof(u32);
	std::memcpy(value, view.canonical + byte_address, sizeof(*value));
	return true;
}

static bool ReadGpuVuMemoryU16(void* user, u16 qword_address, u8 lane,
	u16* value)
{
	if (!value)
		return false;
	u32 word = 0;
	if (!ReadGpuVuMemoryU32(user, qword_address, lane, &word))
		return false;
	*value = static_cast<u16>(word);
	return true;
}

// Called on Saving/Loading states...
bool SaveStateBase::mtvuFreeze()
{
	if (!FreezeTag("MTVU"))
		return false;

	if (IsPortableReplay())
	{
		// Portable replay explicitly requires MTVU to be disabled and both VUs
		// idle. In that configuration the worker ring is host-private cache state:
		// encode one canonical empty state and reset it directly on load instead of
		// enqueueing architecture-owned pointers into an inactive worker.
		u32 vu_cycles[4] = {};
		u32 interrupts = 0;
		u64 signal = 0;
		u64 label = 0;
		u32 cycle_index = 0;
		Freeze(vu_cycles);
		Freeze(interrupts);
		Freeze(signal);
		Freeze(label);
		Freeze(cycle_index);
		if (IsLoading())
		{
			for (const u32 cycles : vu_cycles)
			{
				if (cycles != 0)
					m_error = true;
			}
			if (interrupts != 0 || signal != 0 || label != 0 || cycle_index != 0)
				m_error = true;
			if (m_error)
				return false;
			vu1Thread.Reset();
		}
		return IsOkay();
	}

	pxAssert(vu1Thread.IsDone());
	if (!IsSaving())
	{
		vu1Thread.Reset();
		vu1Thread.WriteCol(vif1);
		vu1Thread.WriteRow(vif1);
		vu1Thread.WriteMicroMem(0, VU1.Micro, 0x4000);
		vu1Thread.WriteDataMem(0, VU1.Mem, 0x4000);
		vu1Thread.WriteVIRegs(&VU1.VI[0]);
		vu1Thread.WriteVFRegs(&VU1.VF[0]);
	}
	for (size_t i = 0; i < 4; ++i)
	{
		unsigned int v = vu1Thread.vuCycles[i].load();
		Freeze(v);
	}

	u32 gsInterrupts = vu1Thread.mtvuInterrupts.load();
	Freeze(gsInterrupts);
	vu1Thread.mtvuInterrupts.store(gsInterrupts);
	u64 gsSignal = vu1Thread.gsSignal.load();
	Freeze(gsSignal);
	vu1Thread.gsSignal.store(gsSignal);
	u64 gsLabel = vu1Thread.gsLabel.load();
	Freeze(gsLabel);
	vu1Thread.gsLabel.store(gsLabel);

	Freeze(vu1Thread.vuCycleIdx);
	return IsOkay();
}

VU_Thread::VU_Thread()
{
	m_deferred_vif_unpacks.reserve(MaximumDeferredAffineSpans);
	Reset();
}

VU_Thread::~VU_Thread()
{
	Close();
}

void VU_Thread::Open()
{
	if (IsOpen())
		return;

	Reset();
	semaEvent.Reset();
	m_shutdown_flag.store(false, std::memory_order_release);
	m_thread.SetStackSize(VMManager::EMU_THREAD_STACK_SIZE);
	m_thread.Start([this]() { ExecuteRingBuffer(); });
#if defined(__vita__)
	// PCSX2 owner: VMManager::SetEmuThreadAffinities(). Sony's documented
	// application topology is EE=USER_0, VU=USER_1, GS=USER_2. CPU3 remains
	// reserved for the shell, plugins, and system services.
	if (!m_thread.SetAffinity(1u << 1))
		Console.Warning("Vita VU worker affinity was rejected; using the scheduler default.");
#endif
}

void VU_Thread::Close()
{
	if (!IsOpen())
		return;

	WaitVU();
	m_shutdown_flag.store(true, std::memory_order_release);
	semaEvent.NotifyOfWork();
	m_thread.Join();
	ReleaseDeferredVifUnpacks();
}

void VU_Thread::Reset()
{
	ReleaseDeferredVifUnpacks();
	m_program_active = false;
	m_dt_program_end = false;
	m_pending_program_interrupts = 0;
	m_profile_execute_enqueues = 0;
	m_profile_wait_calls = 0;
	m_profile_ring_waits = 0;
	m_profile_ring_wait_spins = 0;
	m_profile_compile_barriers = 0;
	m_profile_queue_submissions = 0;
	m_profile_queue_words = 0;
	m_micro_write_pending = false;
	m_micro_invalidate_start = 0;
	m_micro_invalidate_end = 0;
	m_gpu_vu_direct_program_token = 0;
	m_gpu_vu_direct_resume_token = 0;
	m_gpu_vu_direct_program_start_pc = 0;
	m_gpu_vu_direct_program_prepared = false;
	m_gpu_vu_path1_flush_requested.store(false, std::memory_order_relaxed);
	m_vif_span_sequence = 0;
	m_pending_vif_batch = false;
	m_pending_captured_vif_span_pos = -1;
	vuCycleIdx = 0;
	m_ato_write_pos = 0;
	m_write_pos = 0;
	m_ato_read_pos = 0;
	m_read_pos = 0;
	std::memset(&vif, 0, sizeof(vif));
	std::memset(&vifRegs, 0, sizeof(vifRegs));
	for (size_t i = 0; i < 4; ++i)
		vu1Thread.vuCycles[i] = 0;
	vu1Thread.mtvuInterrupts = 0;
}

void VU_Thread::RebuildFromCanonicalStateAfterPortableLoad()
{
	pxAssert(IsOpen());
	pxAssert(IsDone());
	Reset();
	WriteCol(vif1);
	WriteRow(vif1);
	WriteMicroMem(0, VU1.Micro, VU1_PROGSIZE);
	WriteDataMem(0, VU1.Mem, VU1_MEMSIZE);
	WriteVIRegs(&VU1.VI[0]);
	WriteVFRegs(&VU1.VF[0]);
}

void VU_Thread::BeginProgram()
{
	m_program_active = true;
	m_dt_program_end = false;
	m_pending_program_interrupts = 0;
}

void VU_Thread::MarkDtProgramEnd(u32 interrupt_flag)
{
	m_dt_program_end = true;
	m_pending_program_interrupts |= interrupt_flag;
}

void VU_Thread::EndProgram(u32 interrupt_flag)
{
	// PCSX2 owner: x86/microVU_Branch.inl publishes E and T through
	// mVUEBit()/mVUTBit(). Its MTVU path uses the VUTBit handoff for either
	// enabled D or T because both stop VU1 and raise the EE-side VU1 event.
	// A D/T exit takes precedence over a static E bit on the same pair.
	if (m_dt_program_end)
		interrupt_flag &= ~InterruptFlagVUEBit;
	interrupt_flag |= m_pending_program_interrupts;
	m_pending_program_interrupts = 0;
	m_dt_program_end = false;
	m_program_active = false;
	if (interrupt_flag != 0)
		mtvuInterrupts.fetch_or(interrupt_flag, std::memory_order_release);
}

void VU_Thread::ExecuteRingBuffer()
{
	Threading::SetNameOfCurrentThread("MTVU");
	u32 primed_direct_entry_token = 0;
	u32 primed_direct_resume_token = 0;
	VitaGpuVu::DirectContinuationSeed active_direct_continuation;
	VitaGpuVu::DirectContinuationSeed pending_direct_continuation;
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	bool reported_first_direct_job = false;
	bool reported_first_queued_direct_draw = false;
#endif

	for (;;)
	{
		semaEvent.WaitForWork();
		if (m_shutdown_flag.load(std::memory_order_acquire))
			break;

		while (m_ato_read_pos.load(std::memory_order_relaxed) != GetWritePos())
		{
			u32 tag = Read();
			switch (tag)
			{
				case MTVU_VU_EXECUTE:
				case MTVU_VU_EXECUTE_DIRECT:
				case MTVU_VU_EXECUTE_DIRECT_PRIME:
				{
					const bool has_direct_program =
						tag == MTVU_VU_EXECUTE_DIRECT ||
						tag == MTVU_VU_EXECUTE_DIRECT_PRIME;
					const bool allow_direct_draw =
						tag == MTVU_VU_EXECUTE_DIRECT;
					VU1.cycle = 0;
					s32 addr = Read();
					vifRegs.top = Read();
					vifRegs.itop = Read();
					const u32 execution_vif_top = vifRegs.top;
					const u32 execution_vif_itop = vifRegs.itop;
					vuFBRST = Read();
					const VitaGpuVu::DirectProgramToken direct_program{
						has_direct_program ? Read() : 0};
					const VitaGpuVu::DirectProgramToken continuation_program{
						has_direct_program ? Read() : 0};
					if (addr != -1)
						active_direct_continuation = {};
					if (addr != -1)
						VU1.VI[REG_TPC].UL = addr & 0x7FF;
					CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
					// Under MTVU this worker, not vu1ExecMicro() on CPU0, owns the
					// architectural start/completion pair. Keep both notifications on
					// this side of the queue so multiple pending MSCAL/MSCNT jobs cannot
					// collapse into one armed boolean.
					Pcsx2Trace::NotifyMachineCheckpointVu1ProgramStarted();
#endif
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
					// Bounded arrival evidence for the first direct job only.
					if (has_direct_program && !reported_first_direct_job)
					{
						reported_first_direct_job = true;
						Console.WriteLn(
							"GPU-VU: first direct VU1 job token %08x, TPC "
							"%04x, TOP %04x, ITOP %04x, %u deferred spans, "
							"connected %u.",
							direct_program.value, VU1.VI[REG_TPC].UL << 3,
							vifRegs.top, vifRegs.itop,
							static_cast<u32>(m_deferred_vif_unpacks.size()),
							VitaGpuVu::IsDirectDrawAdmissionConnected() ? 1u :
																		  0u);
					}
#endif
					bool queued_direct_draw = false;
					if (!allow_direct_draw)
					{
						// Prime-only jobs are an intentional CPU execution, not
						// a rejected admission attempt.
					}
					else if (!VitaGpuVu::IsDirectDrawAdmissionConnected())
					{
						VitaGpuVu::RecordDirectAdmissionFailure(
							VitaGpuVu::AdmissionFailure::Disconnected);
					}
					else if (!direct_program.IsValid())
					{
						VitaGpuVu::RecordDirectAdmissionFailure(
							VitaGpuVu::AdmissionFailure::NoProgramToken);
					}
					if (VitaGpuVu::IsDirectDrawAdmissionConnected() &&
						direct_program.IsValid())
					{
						std::array<u16, 16> initial_vi{};
						for (u32 reg = 0; reg < initial_vi.size(); reg++)
							initial_vi[reg] = VU1.VI[reg].US[0];

						GpuVuMemoryView memory_view{
							VU1.Mem, &m_deferred_vif_unpacks};
						VitaGpuVu::InvocationEvaluationContext context;
						context.vif_top = static_cast<u16>(vifRegs.top);
						context.vif_itop = static_cast<u16>(vifRegs.itop);
						context.initial_vi = initial_vi.data();
						context.initial_vf_words = &VU1.VF[0].UL[0];
						context.initial_acc_words = &VU1.ACC.UL[0];
						context.initial_q = VU1.VI[REG_Q].UL;
						context.initial_p = VU1.VI[REG_P].UL;
						context.initial_i = VU1.VI[REG_I].UL;
						context.memory_user = &memory_view;
						context.read_memory_u16 = ReadGpuVuMemoryU16;
						context.read_memory_u32 = ReadGpuVuMemoryU32;
						u32& primed_direct_program_token =
							addr == -1 ? primed_direct_resume_token :
								primed_direct_entry_token;
						if (direct_program.value !=
								primed_direct_program_token &&
							VitaGpuVu::PrimeDirectProgram(
								direct_program, context))
						{
							primed_direct_program_token =
								direct_program.value;
						}
						bool begins_continuation = false;
						std::unique_ptr<VitaGpuVu::GpuVuDraw> draw;
						if (allow_direct_draw && addr != -1 &&
							continuation_program.IsValid() &&
							VitaGpuVu::IsDirectContinuationPair(
								direct_program, continuation_program))
						{
							pending_direct_continuation = {};
							draw = VitaGpuVu::BuildDirectGpuVuDraw(
								direct_program, context,
								m_deferred_vif_unpacks);
							begins_continuation = draw &&
								VitaGpuVu::CaptureDirectContinuationSeed(
									direct_program, continuation_program,
									context, *draw,
									&pending_direct_continuation);
							if (!begins_continuation)
								draw.reset();
						}
						else if (allow_direct_draw && addr == -1 &&
							active_direct_continuation.IsValid() &&
							active_direct_continuation.entry_program ==
								continuation_program &&
							active_direct_continuation.resume_program ==
								direct_program)
						{
							draw =
								VitaGpuVu::BuildDirectGpuVuContinuationDraw(
									active_direct_continuation, context,
									m_deferred_vif_unpacks);
						}
						else if (allow_direct_draw)
						{
							draw = VitaGpuVu::BuildDirectGpuVuDraw(
								direct_program, context,
								m_deferred_vif_unpacks);
						}
						const u32 direct_vertices =
							draw ? draw->vertex_count : 0;
						const u32 direct_primitives =
							draw ? draw->primitive_count : 0;
						const u32 direct_streams =
							draw ? static_cast<u32>(draw->streams.size()) : 0;
						const std::array<u16, 16> direct_final_vi =
							draw ? draw->final_vi_values :
								   std::array<u16, 16>{};
						const u32 direct_final_vi_mask =
							draw ? draw->final_vi_write_mask : 0;
						const bool built = static_cast<bool>(draw);
						if (allow_direct_draw && !built)
						{
							VitaGpuVu::RecordDirectAdmissionFailure(
								VitaGpuVu::AdmissionFailure::BuildFailed);
						}
						if (built &&
							VitaGS::QueueGpuVuDraw(std::move(draw)))
						{
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
							if (!reported_first_queued_direct_draw)
							{
								reported_first_queued_direct_draw = true;
								Console.WriteLn(
									"GPU-VU: first direct draw queued from MTVU "
									"(%u vertices, %u primitives, %u raw streams); "
									"CpuVU1->Execute bypassed.",
									direct_vertices, direct_primitives,
									direct_streams);
							}
#endif
							VitaGpuVu::DirectProgramInfo info;
							if (VitaGpuVu::GetDirectProgramInfo(
									direct_program, &info) &&
								info.resume_pc_count == 1)
							{
								VU1.VI[REG_TPC].UL =
									info.unique_resume_pc >> 3;
							}
							// Keep VIF writes in the immutable overlay. The draw
							// retained each payload it references, and a later CPU
							// fallback or architectural observer materializes the
							// remaining journal before reading VU memory.
							for (u32 reg = 1;
								 reg < direct_final_vi.size(); reg++)
							{
								if ((direct_final_vi_mask &
										(1u << reg)) != 0)
								{
									VU1.VI[reg].US[0] =
										direct_final_vi[reg];
								}
							}
							BeginProgram();
							EndProgram(InterruptFlagVUEBit);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION) || \
	defined(VITASX2_WORKLOAD_REPLAY_CHECKPOINT)
							Pcsx2Trace::NotifyMachineCheckpointVu1ExecutionCompleted();
#endif
							vuCycles[vuCycleIdx].store(
								4, std::memory_order_release);
							vuCycleIdx = (vuCycleIdx + 1) & 3;
							if (begins_continuation)
								active_direct_continuation =
									std::move(
										pending_direct_continuation);
							queued_direct_draw = true;
						}
						else if (built)
						{
							VitaGpuVu::RecordDirectAdmissionFailure(
								VitaGpuVu::AdmissionFailure::QueueRejected);
						}
					}
					if (queued_direct_draw)
						break;

					ReplayDeferredVifUnpacks();
					// PCSX2 Vif_Codes.cpp::vuExecMicro() publishes TOP/ITOP
					// for this exact MSCAL/MSCNT before it advances the VIF1
					// double buffer. Replaying an earlier UNPACK reconstructs
					// that command's VIF registers as an implementation detail;
					// it must not replace the execution snapshot observed by
					// XTOP/XITOP in the VU program.
					vifRegs.top = execution_vif_top;
					vifRegs.itop = execution_vif_itop;
					BeginProgram();
					CpuVU1->Execute(vu1RunCycles);
					// gsPack accumulates this dispatch's XGKICK bytes and is reset
					// by FinishGSPacketMTVU, so sample it first.
					VitaGpuVu::RecordCpuVu1Execution(
						gifUnit.gifPath[GIF_PATH_1].gsPack.size);
					gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU();
					VitaGS::CompleteMtvuPath1Packet();
					vuCycles[vuCycleIdx].store(VU1.cycle, std::memory_order_release);
					vuCycleIdx = (vuCycleIdx + 1) & 3;
					break;
				}
				case MTVU_VU_WRITE_MICRO:
				{
					u32 vu_micro_addr = Read();
					u32 size = Read();
						Read(&VU1.Micro[vu_micro_addr], size);
						primed_direct_entry_token = 0;
						primed_direct_resume_token = 0;
						active_direct_continuation = {};
						break;
					}
					case MTVU_VU_WRITE_DATA:
					{
						active_direct_continuation = {};
						ReplayDeferredVifUnpacks();
					u32 vu_data_addr = Read();
					u32 size = Read();
					Read(&VU1.Mem[vu_data_addr], size);
					break;
					}
					case MTVU_VU_WRITE_VIREGS:
						active_direct_continuation = {};
						Read(&VU1.VI, size_u32(32));
						break;
					case MTVU_VU_WRITE_VFREGS:
						active_direct_continuation = {};
						Read(&VU1.VF, size_u32(4*32));
					break;
				case MTVU_VIF_WRITE_COL:
					Read(&vif.MaskCol, sizeof(vif.MaskCol));
					break;
				case MTVU_VIF_WRITE_ROW:
					Read(&vif.MaskRow, sizeof(vif.MaskRow));
					break;
					case MTVU_VIF_UNPACK:
					{
						active_direct_continuation = {};
						ReplayDeferredVifUnpacks();
					u32 vif_copy_size = static_cast<u32>((uptr)&vif.StructEnd - (uptr)&vif.tag);
					Read(&vif.tag, vif_copy_size);
					ReadRegs(&vifRegs);
					u32 size = Read();
					MTVU_Unpack(&buffer[m_read_pos], vifRegs);
					m_read_pos += size_u32(size);
					break;
				}
				case MTVU_VIF_UNPACK_CAPTURED:
				{
					VitaGpuVu::VifUnpackSpan span;
					Read(&span, sizeof(span));
					if (!VitaGpuVu::ResolveRawVifPayload(span.payload))
					{
						Console.Error(
							"GPU-VU: immutable VIF payload became invalid before MTVU consumed it.");
						VitaGpuVu::ReleaseRawVifPayload(&span.payload);
						break;
					}
					AppendDeferredVifUnpack(std::move(span));
					break;
				}
					case MTVU_FLUSH_VIF_UNPACKS:
						active_direct_continuation = {};
						ReplayDeferredVifUnpacks();
					VitaGS::FlushMtvuPath1Completions();
					break;
				case MTVU_NULL_PACKET:
					m_read_pos = 0;
					break;
					jNO_DEFAULT;
			}

			CommitReadPos();
			if (m_gpu_vu_path1_flush_requested.exchange(
					false, std::memory_order_acquire))
			{
				VitaGS::FlushMtvuPath1Completions();
			}
		}
		// Pair with a producer which arms after the final CommitReadPos().
		// The unconditional release RMW makes that post-drain recheck observe
		// all preceding read-position publications without adding an RMW to
		// every ring record.
		m_ring_space_progress.PublishQuiescence();

		// A fast continuation builder can briefly catch the EE producer between
		// VIF transfers thousands of times per frame. Queue-empty is therefore
		// not an epoch boundary: publishing here fragmented cacheable-to-GXM
		// copies and made CPU0 contend on every short producer burst. Explicit
		// VSync, input-pressure and observation requests close partial runs.
		if (m_gpu_vu_path1_flush_requested.exchange(
				false, std::memory_order_acquire))
		{
			VitaGS::FlushMtvuPath1Completions();
		}
	}

	VitaGS::FlushMtvuPath1Completions();
	m_ring_space_progress.PublishQuiescence();
	semaEvent.Kill();
}


// Should only be called by ReserveSpace()
__ri void VU_Thread::WaitOnSize(s32 size)
{
	const bool performance_telemetry_enabled =
		VitaPerformanceTelemetry::IsEnabled();
	bool counted_wait = false;
	for (;;)
	{
		s32 readPos = GetReadPos();
		if (readPos <= m_write_pos)
			break; // MTVU is reading in back of write_pos
		// FIXME greg: there is a bug somewhere in the queue pointer
		// management. It creates a deadlock/corruption in SotC intro (before
		// the first menu). I added a 4KB safety net which seem to avoid to
		// trigger the bug.
		// Note: a wait lock instead of a yield also helps to avoid the bug.
		if (readPos > m_write_pos + size + _4kb)
			break; // Enough free front space
		if (m_pending_vif_batch)
		{
			// A transfer larger than the producer ring must expose its earlier
			// complete UNPACK commands before waiting for the worker to make
			// room. This is a pressure escape, not the ordinary hot path.
			PublishPendingVifBatch();
			continue;
		}
		{          // Let MTVU run to free up buffer space
			if (performance_telemetry_enabled && !counted_wait)
			{
				m_profile_ring_waits++;
				counted_wait = true;
			}
			KickStart();
			m_ring_space_progress.WaitForChange(readPos, [this]() {
				return GetReadPos();
			});
		}
	}
}

// Makes sure theres enough room in the ring buffer
// to write a continuous 'size * sizeof(u32)' bytes
void VU_Thread::ReserveSpace(s32 size)
{
	pxAssert(m_write_pos < buffer_size);
	pxAssert(size < buffer_size);
	pxAssert(size > 0);

	if (m_write_pos + size > (buffer_size - 1))
	{
		WaitOnSize(1); // Size of MTVU_NULL_PACKET
		Write(MTVU_NULL_PACKET);
		// Reset local write pointer/position
		m_write_pos = 0;
		CommitWritePos();
	}

	WaitOnSize(size);
}

// Use this when reading read_pos from ee thread
__fi s32 VU_Thread::GetReadPos()
{
	return m_ato_read_pos.load(std::memory_order_acquire);
}

// Use this when reading write_pos from vu thread
__fi s32 VU_Thread::GetWritePos()
{
	return m_ato_write_pos.load(std::memory_order_acquire);
}

// Gets the effective write pointer after
__fi u32* VU_Thread::GetWritePtr()
{
	pxAssert(m_write_pos < buffer_size);
	return &buffer[m_write_pos];
}

__fi void VU_Thread::CommitWritePos()
{
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		const s32 previous_write_pos =
			m_ato_write_pos.load(std::memory_order_relaxed);
		const u32 queued_words = static_cast<u32>(
			m_write_pos - previous_write_pos) & (buffer_size - 1);
		m_profile_queue_submissions++;
		m_profile_queue_words += queued_words;
	}
	m_ato_write_pos.store(m_write_pos, std::memory_order_release);
	m_pending_vif_batch = false;
	m_pending_captured_vif_span_pos = -1;

	if (MTVU_ALWAYS_KICK)
		KickStart();
	if (MTVU_SYNC_MODE)
		WaitVU();
}

__fi void VU_Thread::CommitReadPos()
{
	m_ato_read_pos.store(m_read_pos, std::memory_order_release);
	m_ring_space_progress.NotifyOfProgress();
}

__fi u32 VU_Thread::Read()
{
	u32 ret = buffer[m_read_pos];
	m_read_pos++;
	return ret;
}

__fi void VU_Thread::Read(void* dest, u32 size)
{
	memcpy(dest, &buffer[m_read_pos], size);
	m_read_pos += size_u32(size);
}

__fi void VU_Thread::ReadRegs(VIFregisters* dest)
{
	VIFregistersMTVU* src = (VIFregistersMTVU*)&buffer[m_read_pos];
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->itop = src->itop;
	dest->top = src->top;
	m_read_pos += size_u32(sizeof(VIFregistersMTVU));
}

__fi void VU_Thread::Write(u32 val)
{
	GetWritePtr()[0] = val;
	m_write_pos += 1;
}

__fi void VU_Thread::Write(const void* src, u32 size)
{
	memcpy(GetWritePtr(), src, size);
	m_write_pos += size_u32(size);
}

__fi void VU_Thread::WriteRegs(VIFregisters* src)
{
	VIFregistersMTVU* dest = (VIFregistersMTVU*)GetWritePtr();
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->top = src->top;
	dest->itop = src->itop;
	m_write_pos += size_u32(sizeof(VIFregistersMTVU));
}

// Returns Average number of vu Cycles from last 4 runs
// Used for vu cycle stealing hack
u32 VU_Thread::Get_vuCycles()
{
	return (vuCycles[0].load(std::memory_order_acquire) +
			vuCycles[1].load(std::memory_order_acquire) +
			vuCycles[2].load(std::memory_order_acquire) +
			vuCycles[3].load(std::memory_order_acquire)) >>
		   2;
}

void VU_Thread::Get_MTVUChanges()
{
	// Note: Atomic communication is with Gif_Unit.cpp Gif_HandlerAD_MTVU
	// The Vita worker publishes completed VU register/memory state before its
	// E/D/T flag. Cortex-A9 needs an acquire here; x86's ordering made the
	// upstream relaxed load sufficient, but it does not establish that contract
	// on a three-core ARM execution route.
	u32 interrupts = mtvuInterrupts.load(std::memory_order_acquire);
	if (!interrupts)
		return;

	if (interrupts & InterruptFlagSignal)
	{
		std::atomic_thread_fence(std::memory_order_acquire);
		const u64 signal = gsSignal.load(std::memory_order_relaxed);
		// If load of signal was moved after clearing the flag, the other thread could write a new value before we load without noticing the double signal
		// Prevent that with release semantics
		mtvuInterrupts.fetch_and(~InterruptFlagSignal, std::memory_order_release);
		GUNIT_WARN("SIGNAL firing");
		const u32 signalMsk = (u32)(signal >> 32);
		const u32 signalData = (u32)signal;
		if (CSRreg.SIGNAL)
		{
			GUNIT_WARN("Queue SIGNAL");
			gifUnit.gsSIGNAL.queued = true;
			//DevCon.Warning("Firing pending signal");
			gifUnit.gsSIGNAL.data[0] = signalData;
			gifUnit.gsSIGNAL.data[1] = signalMsk;
		}
		else
		{
			CSRreg.SIGNAL = true;
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~signalMsk) | (signalData & signalMsk);

			if (!GSIMR.SIGMSK)
				gsIrq();
		}
	}
	if (interrupts & InterruptFlagFinish)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagFinish, std::memory_order_relaxed);
		GUNIT_WARN("Finish firing");
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;

		if (!gifUnit.checkPaths(false, true, true, true))
			Gif_FinishIRQ();
	}
	if (interrupts & InterruptFlagLabel)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagLabel, std::memory_order_acquire);
		// If other thread updates gsLabel for a second interrupt, that's okay.  Worst case we think there's a label interrupt but gsLabel is 0
		// We do not want the exchange of gsLabel to move ahead of clearing the flag, or the other thread could add more work before we clear the flag, resulting in an update with the flag unset
		// acquire semantics should supply that guarantee
		const u64 label = gsLabel.exchange(0, std::memory_order_relaxed);
		GUNIT_WARN("LABEL firing");
		const u32 labelMsk = (u32)(label >> 32);
		const u32 labelData = (u32)label;
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~labelMsk) | (labelData & labelMsk);
	}
	if (interrupts & InterruptFlagVUEBit)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagVUEBit, std::memory_order_relaxed);

		if(INSTANT_VU1)
			VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
		//DevCon.Warning("E-Bit registered %x", VU0.VI[REG_VPU_STAT].UL);
	}
	if (interrupts & InterruptFlagVUTBit)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagVUTBit, std::memory_order_relaxed);
		VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
		VU0.VI[REG_VPU_STAT].UL |= 0x0400;
		//DevCon.Warning("T-Bit registered %x", VU0.VI[REG_VPU_STAT].UL);
		hwIntcIrq(7);
	}
}

void VU_Thread::KickStart()
{
	semaEvent.NotifyOfWork();
}

bool VU_Thread::IsDone()
{
	return GetReadPos() == GetWritePos() &&
		!m_pending_vif_batch &&
		m_deferred_vif_unpack_count.load(std::memory_order_acquire) == 0;
}

void VU_Thread::PublishPendingVifBatch()
{
	if (!m_pending_vif_batch)
		return;
	CommitWritePos();
	KickStart();
}

void VU_Thread::RequestGpuVuPath1Flush()
{
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	m_gpu_vu_path1_flush_requested.store(true, std::memory_order_release);
	KickStart();
#endif
}

void VU_Thread::WaitForQueue()
{
	PublishPendingVifBatch();
	KickStart();
	semaEvent.WaitForEmpty();
}

void VU_Thread::WaitVU()
{
	MTVU_LOG("MTVU - WaitVU!");
	if (VitaPerformanceTelemetry::IsEnabled())
		m_profile_wait_calls++;
	if (!IsOpen())
		return;

	// Public waits are architectural observation boundaries. Queue one ordered
	// materialization command even when the worker has not yet consumed the
	// preceding capture event; checking only the current pending count would
	// race that handoff.
	ReserveSpace(1);
	Write(MTVU_FLUSH_VIF_UNPACKS);
	CommitWritePos();
	WaitForQueue();
}

void VU_Thread::AppendDeferredVifUnpack(
	VitaGpuVu::VifUnpackSpan span)
{
	// PCSX2 owner: Vif_Unpack.cpp::_nVifUnpackLoop(). Mode-zero V4-32 with
	// CL==WL has no side effect beyond one sequential, wrapping VU-memory
	// write per source vector. A later span which covers that complete modular
	// range therefore makes the older payload unreachable.
	for (auto it = m_deferred_vif_unpacks.begin();
		 it != m_deferred_vif_unpacks.end();)
	{
		if (VitaGpuVu::DirectAffineSpanFullyOverwrites(span, *it))
		{
			VitaGpuVu::ReleaseRawVifPayload(&it->payload);
			it = m_deferred_vif_unpacks.erase(it);
		}
		else
		{
			++it;
		}
	}

	// Partial overlaps require segment splitting to compact further. Preserve
	// exact behavior by materializing the bounded journal instead of allowing
	// unbounded storage or search work on the 496 MHz VU worker.
	if (m_deferred_vif_unpacks.size() >= MaximumDeferredAffineSpans)
		ReplayDeferredVifUnpacks();

	m_deferred_vif_unpacks.push_back(std::move(span));
	m_deferred_vif_unpack_count.store(
		static_cast<u32>(m_deferred_vif_unpacks.size()),
		std::memory_order_release);
	VitaGpuVu::RecordDeferredVifUnpack();
}

void VU_Thread::ReplayDeferredVifUnpacks()
{
	for (VitaGpuVu::VifUnpackSpan& span : m_deferred_vif_unpacks)
	{
		const u8* const source =
			VitaGpuVu::ResolveRawVifPayload(span.payload);
		if (!source)
		{
			Console.Error(
				"GPU-VU: deferred VIF payload was unavailable at its observation boundary.");
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
			continue;
		}

		if (VitaGpuVu::MaterializeDirectAffineV4_32Span(
				span, source, VU1.Mem, VU1_MEMSIZE))
		{
			VitaGpuVu::RecordReplayedVifUnpack();
			VitaGpuVu::ReleaseRawVifPayload(&span.payload);
			continue;
		}

		// Captured spans are admitted only by IsDirectAffineV4_32Span(), so this
		// is a defensive semantic fallback for corrupted or future descriptor
		// forms rather than the ordinary replay path.
		vif.tag.addr = static_cast<u32>(span.destination_qword) * 16u;
		vif.tag.size = span.tag_size_words;
		vif.tag.cmd = span.command;
		vif.cmd = span.command;
		vif.pass = 1;
		vif.cl = 0;
		vif.usn = span.unsigned_data;
		vif.start_aligned = span.start_alignment;
		vifRegs.cycle.cl = span.cycle_cl;
		vifRegs.cycle.wl = span.cycle_wl;
		vifRegs.mode = span.mode;
		vifRegs.num = span.vector_count;
		vifRegs.mask = span.mask;
		vifRegs.top = span.vif_top;
		vifRegs.itop = span.vif_itop;
		MTVU_Unpack(const_cast<u8*>(source), vifRegs);
		VitaGpuVu::RecordReplayedVifUnpack();
		VitaGpuVu::ReleaseRawVifPayload(&span.payload);
	}
	m_deferred_vif_unpacks.clear();
	m_deferred_vif_unpack_count.store(0, std::memory_order_release);
}

void VU_Thread::ReleaseDeferredVifUnpacks()
{
	for (VitaGpuVu::VifUnpackSpan& span : m_deferred_vif_unpacks)
		VitaGpuVu::ReleaseRawVifPayload(&span.payload);
	m_deferred_vif_unpacks.clear();
	m_deferred_vif_unpack_count.store(0, std::memory_order_release);
}

void VU_Thread::ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst)
{
	MTVU_LOG("MTVU - ExecuteVU!");
	PrepareVuCodeForExecute(static_cast<s32>(vu_addr));
	Get_MTVUChanges(); // Clear any pending interrupts
	bool direct_program_job = false;
	bool prime_program_job = false;
	VitaGpuVu::DirectProgramToken execution_direct_program{};
	VitaGpuVu::DirectProgramToken continuation_direct_program{};
	if (VitaGpuVu::IsDirectDrawAdmissionConnected() &&
		vu_addr == static_cast<u32>(-1) &&
		m_gpu_vu_direct_resume_token != 0)
	{
		const VitaGpuVu::DirectInputState input_state =
			VitaGpuVu::GetDirectProgramInputState(
				{m_gpu_vu_direct_resume_token});
		if (input_state == VitaGpuVu::DirectInputState::Pending ||
			input_state == VitaGpuVu::DirectInputState::Ready)
		{
			direct_program_job = true;
			execution_direct_program = {m_gpu_vu_direct_resume_token};
			continuation_direct_program = {m_gpu_vu_direct_program_token};
		}
		else
		{
			m_gpu_vu_direct_resume_token = 0;
		}
	}
	else if (VitaGpuVu::IsDirectDrawAdmissionConnected() &&
		vu_addr != static_cast<u32>(-1) &&
		m_gpu_vu_direct_program_token != 0)
	{
		const VitaGpuVu::DirectInputState input_state =
			VitaGpuVu::GetDirectProgramInputState(
				{m_gpu_vu_direct_program_token});
			if (input_state == VitaGpuVu::DirectInputState::Pending)
			{
				// The explicit root contains the skipped MSCAL prologue and is the
				// eventual GPU-owned continuation seed. Compile it now, but keep the
				// CPU execution authoritative until its live VF/VI state can be
				// carried across following MSCNT commands without a stale snapshot.
				prime_program_job = true;
				execution_direct_program = {m_gpu_vu_direct_program_token};
			}
			else if (input_state == VitaGpuVu::DirectInputState::Ready &&
				m_gpu_vu_direct_resume_token != 0 &&
				VitaGpuVu::GetDirectProgramInputState(
					{m_gpu_vu_direct_resume_token}) ==
						VitaGpuVu::DirectInputState::Ready)
			{
				// Both exact-image entries are registered. The worker performs
				// the semantic same-loop continuation proof before it may replace
				// this explicit MSCAL with the generated entry root.
				direct_program_job = true;
				execution_direct_program = {m_gpu_vu_direct_program_token};
				continuation_direct_program = {
					m_gpu_vu_direct_resume_token};
			}
		}
	const bool tagged_program_job =
		direct_program_job || prime_program_job;
	ReserveSpace(tagged_program_job ? 7 : 5);
	Write(direct_program_job ? MTVU_VU_EXECUTE_DIRECT :
		(prime_program_job ? MTVU_VU_EXECUTE_DIRECT_PRIME :
			MTVU_VU_EXECUTE));
	Write(vu_addr);
	Write(vif_top);
	Write(vif_itop);
	Write(fbrst);
	// MSCNT resumes from the MTVU-owned TPC. The token is prepared from the
	// exact post-E PC, never guessed from CPU0's deliberately stale TPC. An
	// explicit MSCAL carries both exact entry and continuation tokens so the
	// worker can prove and seed a GPU-owned chain before bypassing ARM work.
	if (tagged_program_job)
	{
		Write(execution_direct_program.value);
		Write(continuation_direct_program.value);
	}
	CommitWritePos();
	if (VitaPerformanceTelemetry::IsEnabled())
		m_profile_execute_enqueues++;
	gifUnit.TransferGSPacketData(GIF_TRANS_MTVU, NULL, 0);
	KickStart();
	u32 cycles = std::max(Get_vuCycles(), 4u);
	u32 skip_cycles = std::min(cycles, 3000u);
	cpuRegs.cycle += skip_cycles * EmuConfig.Speedhacks.EECycleSkip;
	VU0.cycle += skip_cycles * EmuConfig.Speedhacks.EECycleSkip;
	Get_MTVUChanges();

	if (!INSTANT_VU1)
	{
		VU0.VI[REG_VPU_STAT].UL |= 0x100;
		CPU_INT(VU_MTVU_BUSY, cycles);
	}
}

void VU_Thread::VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size)
{
	MTVU_LOG("MTVU - VifUnpack!");
#if defined(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY) || \
	defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	bool retain_immutable_input = false;
#if defined(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY)
	retain_immutable_input = true;
#elif defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	if (m_gpu_vu_direct_resume_token != 0)
	{
		const VitaGpuVu::DirectInputState input_state =
			VitaGpuVu::GetDirectProgramInputState(
				{m_gpu_vu_direct_resume_token});
		if (input_state == VitaGpuVu::DirectInputState::Ready)
		{
			retain_immutable_input = true;
		}
		else if (input_state == VitaGpuVu::DirectInputState::Unavailable)
		{
			VitaGpuVu::RecordCaptureBypass(size);
			m_gpu_vu_direct_resume_token = 0;
		}
	}
#endif
	if (retain_immutable_input)
	{
		VitaGpuVu::VifUnpackSpan span;
		span.sequence = ++m_vif_span_sequence;
		span.source_size = size;
		span.tag_size_words = _vif.tag.size;
		span.mask = _vifRegs.mask;
		span.destination_qword = static_cast<u16>(_vif.tag.addr >> 4);
		span.vector_count = static_cast<u16>(_vifRegs.num);
		span.vif_top = static_cast<u16>(_vifRegs.top);
		span.vif_itop = static_cast<u16>(_vifRegs.itop);
		span.command = static_cast<u8>(_vif.tag.cmd);
		span.cycle_cl = _vifRegs.cycle.cl;
		span.cycle_wl = _vifRegs.cycle.wl;
		span.mode = static_cast<u8>(_vifRegs.mode);
		span.unsigned_data = _vif.usn;
		span.start_alignment = _vif.start_aligned;
		u32 affine_payload_size = 0;
		if (VitaGpuVu::GetDirectAffineV4_32PayloadSize(
				span, &affine_payload_size))
		{
			// nVifUnpack() passes MTVU (size + 4) bytes for the general unpack
			// implementation. Once the direct V4-32, mode-zero, CL==WL
			// contract is proven, _nVifUnpackLoop<1>() consumes exactly one
			// 16-byte source vector per NUM. The trailing safety word is not
			// PS2-visible input; excluding it avoids a 16-byte ring-alignment
			// gap between consecutive affine commands. Every unproven or
			// failed-capture path below retains the original size and owner.
			span.source_size = affine_payload_size;
			if (VitaGpuVu::CaptureRawVifPayload(
				data, affine_payload_size,
				m_pending_vif_batch ?
					VitaGpuVu::RawVifCaptureMode::ContinueEpoch :
					VitaGpuVu::RawVifCaptureMode::BeginVuCommandEpoch,
				&span.payload))
			{
				// The record is still EE-private until CommitWritePos().
				// Adjacent source and destination ranges therefore have the
				// exact effect of one larger affine journal span. The generated
				// shader continues to derive each attribute offset from it.
				if (m_pending_vif_batch &&
					m_pending_captured_vif_span_pos >= 0)
				{
					VitaGpuVu::VifUnpackSpan previous;
					std::memcpy(&previous,
						&buffer[m_pending_captured_vif_span_pos],
						sizeof(previous));
					if (VitaGpuVu::MergeAdjacentDirectAffineV4_32Spans(
							&previous, &span))
					{
						std::memcpy(
							&buffer[m_pending_captured_vif_span_pos],
							&previous, sizeof(previous));
						return;
					}
				}

				ReserveSpace(1 + size_u32(sizeof(span)));
				Write(MTVU_VIF_UNPACK_CAPTURED);
				m_pending_captured_vif_span_pos = m_write_pos;
				Write(&span, sizeof(span));
				// The common VIF packet immediately follows its affine UNPACKs
				// with MSCNT. ExecuteVU() publishes the whole group with one
				// release store and one worker wake. VIF1transfer() publishes at
				// return when no execute follows, preserving ordinary MTVU
				// visibility and every observation boundary.
				m_pending_vif_batch = true;
				return;
			}
		}
	}
#endif

	u32 vif_copy_size = (u32)((uptr)&_vif.StructEnd - (uptr)&_vif.tag);
	ReserveSpace(1 + size_u32(vif_copy_size) + size_u32(sizeof(VIFregistersMTVU)) + 1 + size_u32(size));
	Write(MTVU_VIF_UNPACK);
	Write(&_vif.tag, vif_copy_size);
	WriteRegs(&_vifRegs);
	Write(size);
	Write(data, size);
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteMicroMem(u32 vu_micro_addr, const void* data, u32 size)
{
	MTVU_LOG("MTVU - WriteMicroMem!");
	if (size != 0)
	{
		m_gpu_vu_direct_program_prepared = false;
		m_gpu_vu_direct_program_token = 0;
		m_gpu_vu_direct_resume_token = 0;
		const u32 end = std::min<u32>(vu_micro_addr + size, VU1_PROGSIZE);
		if (!m_micro_write_pending)
		{
			m_micro_invalidate_start = vu_micro_addr;
			m_micro_invalidate_end = end;
			m_micro_write_pending = true;
		}
		else
		{
			m_micro_invalidate_start = std::min(m_micro_invalidate_start, vu_micro_addr);
			m_micro_invalidate_end = std::max(m_micro_invalidate_end, end);
		}
	}
	ReserveSpace(3 + size_u32(size));
	Write(MTVU_VU_WRITE_MICRO);
	Write(vu_micro_addr);
	Write(size);
	Write(data, size);
	CommitWritePos();
	KickStart();
}

void VU_Thread::PrepareVuCodeForExecute(s32 vu_addr)
{
	const bool native_preparation_required =
		m_micro_write_pending || VitaVU::Vu1ProgramNeedsPreparation(vu_addr);
	const bool direct_admission_connected =
		VitaGpuVu::IsDirectDrawAdmissionConnected();
	const bool direct_start_known =
		direct_admission_connected && vu_addr != -1;
	const u32 direct_start_pc = direct_start_known ?
		((static_cast<u32>(vu_addr) & 0x7ffu) << 3) : 0;
	if (!native_preparation_required &&
		(!direct_start_known ||
			(m_gpu_vu_direct_program_prepared &&
				m_gpu_vu_direct_program_start_pc == direct_start_pc)))
		return;

	// Sony PSP2 VM-domain write mode applies to the process, not one core. The
	// PCSX2 x86 MTVU worker may compile at first use, but doing so on Vita races
	// CPU0's executable EE cache. Drain pending micro writes, invalidate and
	// compile on the EE-side C++ seam, then publish only executable code to CPU1.
	WaitForQueue();
	if (VitaPerformanceTelemetry::IsEnabled())
		m_profile_compile_barriers++;
	if (m_micro_write_pending)
	{
		CpuVU1->Clear(m_micro_invalidate_start,
			m_micro_invalidate_end - m_micro_invalidate_start);
		m_micro_write_pending = false;
		m_micro_invalidate_start = 0;
		m_micro_invalidate_end = 0;
	}
	if (native_preparation_required)
		VitaVU::PrepareVu1Program(vu_addr);

	if (!direct_start_known)
	{
		// MSCNT has no explicit VIF address, but the exact post-E token was
		// proven from the preceding explicit program and remains valid across
		// preparation of the native fallback's live resume map. Micro writes,
		// Reset(), and a later explicit MSCAL own invalidation/replacement.
		if (!direct_admission_connected)
		{
			m_gpu_vu_direct_program_token = 0;
			m_gpu_vu_direct_resume_token = 0;
			m_gpu_vu_direct_program_prepared = false;
		}
		return;
	}

	const VitaGpuVu::DirectProgramToken prepared =
		VitaGpuVu::PrepareDirectProgram(VU1.Micro, VU1_PROGSIZE,
			direct_start_pc);
	VitaGpuVu::DirectProgramInfo info;
	m_gpu_vu_direct_program_token =
		prepared.IsValid() &&
			VitaGpuVu::GetDirectProgramInfo(prepared, &info) &&
			info.parallel_candidates != 0 ?
		prepared.value : 0;
	m_gpu_vu_direct_resume_token = 0;
	VitaGpuVu::DirectProgramInfo resume_info;
	VitaGpuVu::DirectProgramToken resume{0};
	if (prepared.IsValid() && info.resume_pc_count == 1)
	{
		resume = VitaGpuVu::PrepareDirectProgram(VU1.Micro, VU1_PROGSIZE,
			info.unique_resume_pc);
		if (resume.IsValid() &&
			VitaGpuVu::GetDirectProgramInfo(resume, &resume_info) &&
			resume_info.parallel_candidates != 0 &&
			resume_info.resume_pc_count == 1 &&
			resume_info.unique_resume_pc == info.unique_resume_pc)
		{
			m_gpu_vu_direct_resume_token = resume.value;
		}
	}
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	// Bounded publication evidence: preparation only reaches here on a real
	// derivation, and the report is further limited to observed changes.
	if (m_gpu_vu_direct_program_token != m_gpu_vu_direct_reported_program_token ||
		m_gpu_vu_direct_resume_token != m_gpu_vu_direct_reported_resume_token)
	{
		m_gpu_vu_direct_reported_program_token = m_gpu_vu_direct_program_token;
		m_gpu_vu_direct_reported_resume_token = m_gpu_vu_direct_resume_token;
		Console.WriteLn(
			"GPU-VU: prepare entry %04x token %08x (%u candidates, %u resume "
			"pcs, resume pc %04x) -> resume token %08x (raw %08x, %u "
			"candidates, %u resume pcs, resume pc %04x).",
			direct_start_pc, m_gpu_vu_direct_program_token,
			info.parallel_candidates, info.resume_pc_count,
			info.unique_resume_pc, m_gpu_vu_direct_resume_token, resume.value,
			resume_info.parallel_candidates, resume_info.resume_pc_count,
			resume_info.unique_resume_pc);
	}
#endif
	m_gpu_vu_direct_program_start_pc = direct_start_pc;
	m_gpu_vu_direct_program_prepared = true;
}

void VU_Thread::WriteDataMem(u32 vu_data_addr, const void* data, u32 size)
{
	MTVU_LOG("MTVU - WriteDataMem!");
	ReserveSpace(3 + size_u32(size));
	Write(MTVU_VU_WRITE_DATA);
	Write(vu_data_addr);
	Write(size);
	Write(data, size);
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteVIRegs(REG_VI* viRegs)
{
	MTVU_LOG("MTVU - WriteRegs!");
	ReserveSpace(1 + size_u32(32));
	Write(MTVU_VU_WRITE_VIREGS);
	Write(viRegs, size_u32(32));
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteVFRegs(VECTOR* vfRegs)
{
	MTVU_LOG("MTVU - WriteRegs!");
	ReserveSpace(1 + size_u32(32*4));
	Write(MTVU_VU_WRITE_VFREGS);
	Write(vfRegs, size_u32(32*4));
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteCol(vifStruct& _vif)
{
	MTVU_LOG("MTVU - WriteCol!");
	ReserveSpace(1 + size_u32(sizeof(_vif.MaskCol)));
	Write(MTVU_VIF_WRITE_COL);
	Write(&_vif.MaskCol, sizeof(_vif.MaskCol));
	CommitWritePos();
	KickStart();
}

void VU_Thread::WriteRow(vifStruct& _vif)
{
	MTVU_LOG("MTVU - WriteRow!");
	ReserveSpace(1 + size_u32(sizeof(_vif.MaskRow)));
	Write(MTVU_VIF_WRITE_ROW);
	Write(&_vif.MaskRow, sizeof(_vif.MaskRow));
	CommitWritePos();
	KickStart();
}
