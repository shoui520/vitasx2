// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/MachineCheckpointTrace.h"

#include "Common.h"
#include "Counters.h"
#include "DebugTools/CoreEventTrace.h"
#include "DebugTools/GsTrace.h"
#include "DebugTools/SifTrace.h"
#include "DebugTools/VifTrace.h"
#include "Dmac.h"
#include "GS/GS.h"
#include "IopCounters.h"
#include "IopMem.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SPU2/defs.h"
#include "Sif.h"
#include "VUmicro.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace Pcsx2Trace
{
	namespace
	{
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'C', 'K', 'P'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_PROJECTION_VERSION = 4;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u32 TRACE_FLAG_GATED_ON_SIF_RECORDS = 1u << 1;
		static constexpr u32 TRACE_FLAG_GATED_ON_VIF_RECORDS = 1u << 2;
		static constexpr u32 CHECKPOINT_TRIGGER_VU1_COMPLETED_EVENT_TEST = 1;
		static constexpr u64 FNV1A64_OFFSET = 14695981039346656037ull;
		static constexpr u64 FNV1A64_PRIME = 1099511628211ull;

#pragma pack(push, 1)
		struct MachineCheckpointTraceFileHeader
		{
			char magic[8];
			u32 version;
			u32 header_size;
			u32 record_size;
			u32 flags;
			u64 max_records;
			u64 records_written;
			u32 entry_pc;
			u32 projection_version;
		};

		struct MachineCheckpointVuHashes
		{
			u64 vf;
			u64 vi;
			u64 acc_and_scalars;
			u64 published_flags;
			u64 fmac_pipeline;
			u64 fdiv_efu_pipeline;
			u64 ialu_pipeline;
			u64 controller;
			u64 xgkick;
		};

		// All multibyte fields are fixed-width little-endian values. The hashes
		// are built from explicit guest-visible fields, never C++ struct padding
		// or host pointers. Component hashes keep the record small while making a
		// first divergence immediately attributable to one architectural domain.
		struct MachineCheckpointTraceRecord
		{
			u64 index;
			u64 vu1_completion_ordinal;
			u32 vu1_completions_at_event_test;
			u32 trigger;
			u32 status;
			u32 memory_available_mask;
			u64 configured_after_sif_records;
			u64 configured_after_vif_records;
			u64 sif_records;
			u64 vif_records;
			u64 core_event_records;
			u64 gs_records;

			u64 ee_cycle;
			u64 ee_next_event_cycle;
			u64 ee_last_event_cycle;
			u32 ee_pc;
			u32 ee_opcode;
			u32 ee_branch;
			u32 ee_is_delay_slot;
			u32 ee_interrupt;
			s32 ee_iop_cycle_balance;
			s32 ee_counter_delta;
			u32 reserved0;
			u64 ee_counter_start;
			u64 ee_gpr_hash;
			u64 ee_cp0_tlb_perf_hash;
			u64 ee_fpu_hash;
			u64 ee_control_hash;
			u64 ee_event_schedule_hash;

			u64 iop_cycle;
			u64 iop_next_event_cycle;
			u32 iop_pc;
			u32 iop_opcode;
			u32 iop_is_delay_slot;
			u32 iop_interrupt;
			s32 iop_counter_delta;
			u32 reserved1;
			u64 iop_counter_start;
			u64 iop_gpr_hash;
			u64 iop_cp0_hash;
			u64 iop_cp2_hash;
			u64 iop_control_hash;
			u64 iop_event_schedule_hash;

			u64 vu_cycle[2];
			s64 vu_next_block_cycles[2];
			u32 vu_tpc[2];
			u32 vu_busy_bits;
			MachineCheckpointVuHashes vu[2];

			u64 memory_hash[MachineCheckpointMemoryRegionCount];
			u64 sif_state_hash[2];
			u64 vif_state_hash[2];
			u64 dmac_state_hash;
		};
#pragma pack(pop)

		static_assert(sizeof(MachineCheckpointTraceFileHeader) == 48,
			"Machine checkpoint trace header size must stay fixed.");
		static_assert(sizeof(MachineCheckpointTraceRecord) == 588,
			"Machine checkpoint trace record size must stay fixed.");

		class StableHash final
		{
		public:
			void AddBytes(const void* data, size_t size)
			{
				const u8* bytes = static_cast<const u8*>(data);
				for (size_t i = 0; i < size; i++)
				{
					m_value ^= bytes[i];
					m_value *= FNV1A64_PRIME;
				}
			}

			void AddU8(u8 value) { AddBytes(&value, sizeof(value)); }
			void AddU16(u16 value)
			{
				AddU8(static_cast<u8>(value));
				AddU8(static_cast<u8>(value >> 8));
			}
			void AddU32(u32 value)
			{
				AddU16(static_cast<u16>(value));
				AddU16(static_cast<u16>(value >> 16));
			}
			void AddU64(u64 value)
			{
				AddU32(static_cast<u32>(value));
				AddU32(static_cast<u32>(value >> 32));
			}
			u64 Value() const { return m_value; }

		private:
			u64 m_value = FNV1A64_OFFSET;
		};

		FILE* s_trace_file = nullptr;
		MachineCheckpointTraceConfig s_config;
		u64 s_eligible_completions_seen = 0;
		u64 s_records_written = 0;
		u64 s_last_completion_ordinal = 0;
		u32 s_pending_completion_count = 0;
		bool s_started = false;
		bool s_hit_limit = false;
		bool s_vu1_program_armed = false;
		bool s_pending_checkpoint = false;
		u32 s_entry_pc = 0;
		std::string s_error;

		void SetError(std::string error)
		{
			if (s_error.empty())
				s_error = std::move(error);
		}

		MachineCheckpointTraceFileHeader MakeHeader()
		{
			MachineCheckpointTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(header);
			header.record_size = sizeof(MachineCheckpointTraceRecord);
			header.flags =
				(s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0) |
				(s_config.after_sif_records != 0 ? TRACE_FLAG_GATED_ON_SIF_RECORDS : 0) |
				(s_config.after_vif_records != 0 ? TRACE_FLAG_GATED_ON_VIF_RECORDS : 0);
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			header.projection_version = TRACE_PROJECTION_VERSION;
			return header;
		}

		bool WriteHeader()
		{
			const MachineCheckpointTraceFileHeader header = MakeHeader();
			return std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1;
		}

		u64 HashEeGprs()
		{
			StableHash hash;
			for (u32 reg = 0; reg < 32; reg++)
				for (u32 word = 0; word < 4; word++)
					hash.AddU32(cpuRegs.GPR.r[reg].UL[word]);
			for (u32 word = 0; word < 4; word++)
			{
				hash.AddU32(cpuRegs.HI.UL[word]);
				hash.AddU32(cpuRegs.LO.UL[word]);
			}
			return hash.Value();
		}

		u64 HashEeCp0TlbPerf()
		{
			StableHash hash;
			for (u32 reg = 0; reg < 32; reg++)
				hash.AddU32(cpuRegs.CP0.r[reg]);
			// SaveStateBase::FreezeInternals() owns the complete architectural
			// TLB array separately from the CP0 staging registers. TLBWI/TLBWR can
			// therefore change future translation without leaving the written entry
			// represented by cpuRegs.CP0.
			for (u32 entry = 0; entry < 48; entry++)
			{
				hash.AddU32(tlb[entry].PageMask.UL);
				hash.AddU32(tlb[entry].EntryHi.UL);
				hash.AddU32(tlb[entry].EntryLo0.UL);
				hash.AddU32(tlb[entry].EntryLo1.UL);
			}
			for (u32 reg = 0; reg < 4; reg++)
				hash.AddU32(cpuRegs.PERF.r[reg]);
			return hash.Value();
		}

		u64 HashEeFpu()
		{
			StableHash hash;
			for (u32 reg = 0; reg < 32; reg++)
				hash.AddU32(fpuRegs.fpr[reg].UL);
			// FCR0 and FCR31 are the only control registers exposed by
			// FPU.cpp::CFC1()/CTC1() and x86/iFPU.cpp::recCFC1()/recCTC1().
			// The other fprc[] words are storage residue, not EE state.
			hash.AddU32(fpuRegs.fprc[0]);
			hash.AddU32(fpuRegs.fprc[31]);
			hash.AddU32(fpuRegs.ACC.UL);
			// PCSX2's non-IEEE MADD/MSUB path consumes this internal flag, so
			// it is continuation-semantic even though the EE cannot read it.
			hash.AddU32(fpuRegs.ACCflag);
			return hash.Value();
		}

		u64 HashEeControl()
		{
			StableHash hash;
			hash.AddU32(cpuRegs.sa);
			hash.AddU32(static_cast<u32>(cpuRegs.branch));
			hash.AddU32(cpuRegs.IsDelaySlot);
			hash.AddU32(cpuRegs.pcWriteback);
			hash.AddU32(static_cast<u32>(cpuRegs.opmode));
			hash.AddU32(cpuRegs.dmastall);
			return hash.Value();
		}

		u64 HashEeEventSchedule()
		{
			StableHash hash;
			hash.AddU32(cpuRegs.tempcycles);
			hash.AddU64(cpuRegs.lastCOP0Cycle);
			hash.AddU64(cpuRegs.lastPERFCycle[0]);
			hash.AddU64(cpuRegs.lastPERFCycle[1]);
			for (u32 event = 0; event < 32; event++)
			{
				hash.AddU32(cpuRegs.eCycle[event]);
				hash.AddU64(cpuRegs.sCycle[event]);
			}
			// Counters.cpp::SaveStateBase::rcntFreeze() is the continuation owner.
			// Serialize named fields so host padding never enters the projection.
			for (const Counter& counter : counters)
			{
				hash.AddU32(counter.count);
				hash.AddU32(counter.modeval);
				hash.AddU32(counter.target);
				hash.AddU32(counter.hold);
				hash.AddU32(counter.rate);
				hash.AddU32(counter.interrupt);
				hash.AddU64(counter.startCycle);
			}
			const auto add_sync_counter = [&hash](const SyncCounter& counter) {
				hash.AddU32(counter.Mode);
				hash.AddU64(counter.startCycle);
				hash.AddU32(static_cast<u32>(counter.deltaCycles));
			};
			add_sync_counter(hsyncCounter);
			add_sync_counter(vsyncCounter);
			return hash.Value();
		}

		u64 HashIopGprs()
		{
			StableHash hash;
			for (u32 reg = 0; reg < 34; reg++)
				hash.AddU32(psxRegs.GPR.r[reg]);
			return hash.Value();
		}

		u64 HashIopCp0()
		{
			StableHash hash;
			for (u32 reg = 0; reg < 32; reg++)
				hash.AddU32(psxRegs.CP0.r[reg]);
			return hash.Value();
		}

		u64 HashIopCp2()
		{
			StableHash hash;
			for (u32 reg = 0; reg < 32; reg++)
			{
				hash.AddU32(psxRegs.CP2D.r[reg]);
				hash.AddU32(psxRegs.CP2C.r[reg]);
			}
			return hash.Value();
		}

		u64 HashIopControl()
		{
			StableHash hash;
			hash.AddU32(psxRegs.pcWriteback);
			return hash.Value();
		}

		u64 HashIopEventSchedule()
		{
			StableHash hash;
			hash.AddU32(static_cast<u32>(psxRegs.iopBreak));
			hash.AddU32(static_cast<u32>(psxRegs.iopCycleEE));
			hash.AddU32(psxRegs.iopCycleEECarry);
			for (u32 event = 0; event < 32; event++)
			{
				hash.AddU64(psxRegs.sCycle[event]);
				hash.AddU32(static_cast<u32>(psxRegs.eCycle[event]));
			}
			// IopCounters.cpp::SaveStateBase::psxRcntFreeze() owns these fields.
			// Hash booleans explicitly instead of copying ABI-dependent padding.
			for (const psxCounter& counter : psxCounters)
			{
				hash.AddU64(counter.count);
				hash.AddU64(counter.target);
				hash.AddU32(counter.rate);
				hash.AddU32(counter.interrupt);
				hash.AddU64(counter.startCycle);
				hash.AddU32(static_cast<u32>(counter.deltaCycles));
				hash.AddU32(counter.mode.modeval);
				hash.AddU8(counter.currentIrqMode.repeatInterrupt ? 1 : 0);
				hash.AddU8(counter.currentIrqMode.toggleInterrupt ? 1 : 0);
			}
			hash.AddU8(hBlanking ? 1 : 0);
			hash.AddU8(vBlanking ? 1 : 0);
			return hash.Value();
		}

		u64 RemainingVuPipeCycles(u64 vu_cycle, u64 start_cycle, u32 latency)
		{
			const u64 completion_cycle = start_cycle + latency;
			return completion_cycle > vu_cycle ? completion_cycle - vu_cycle : 0;
		}

		MachineCheckpointVuHashes HashVuState(const VURegs& vu)
		{
			MachineCheckpointVuHashes out = {};
			StableHash vf;
			for (u32 reg = 0; reg < 32; reg++)
				for (u32 lane = 0; lane < 4; lane++)
					vf.AddU32(vu.VF[reg].UL[lane]);
			out.vf = vf.Value();

			// VI0..VI15 are 16-bit architectural integer registers. Do not
			// compare the host padding or unused upper half of REG_VI.
			StableHash vi;
			for (u32 reg = 0; reg < 16; reg++)
				vi.AddU16(vu.VI[reg].US[0]);
			out.vi = vi.Value();

			StableHash scalars;
			for (u32 lane = 0; lane < 4; lane++)
				scalars.AddU32(vu.ACC.UL[lane]);
			// Published I/R/Q/P live in VI[]. VURegs::q/p are interpreter
			// pending-result scratch and microVU uses pending_q/p instead.
			scalars.AddU32(vu.VI[REG_I].UL);
			scalars.AddU32(vu.VI[REG_R].UL);
			scalars.AddU32(vu.VI[REG_Q].UL);
			scalars.AddU32(vu.VI[REG_P].UL);
			out.acc_and_scalars = scalars.Value();

			StableHash flags;
			// Checkpoints require both VPU busy bits clear. Natural E-bit completion
			// flushes microVU's four delayed flag lanes to these published values;
			// the next VU0 start also rebuilds them after any COP2 macro changes.
			// Provider-private micro_* arrays and interpreter working flags are thus
			// reconstructible at this deliberately idle boundary.
			flags.AddU32(vu.VI[REG_STATUS_FLAG].UL & 0x00000fffu);
			flags.AddU32(vu.VI[REG_MAC_FLAG].UL & 0x0000ffffu);
			flags.AddU32(vu.VI[REG_CLIP_FLAG].UL & 0x00ffffffu);
			out.published_flags = flags.Value();

			// Hash active pipeline entries in logical retirement order. Empty
			// rings canonicalize to their zero occupancy: _vuFlushAll() empties
			// them but deliberately leaves stale slot contents and ring positions.
			StableHash fmac;
			fmac.AddU32(vu.fmaccount);
			for (u32 offset = 0; offset < vu.fmaccount && offset < 4; offset++)
			{
				const fmacPipe& pipe = vu.fmac[(vu.fmacreadpos + offset) & 3];
				fmac.AddU32(pipe.regupper);
				fmac.AddU32(pipe.reglower);
				fmac.AddU32(static_cast<u32>(pipe.flagreg));
				fmac.AddU32(pipe.regupper == 0 ? 0 : pipe.xyzwupper);
				fmac.AddU32(pipe.reglower == 0 ? 0 : pipe.xyzwlower);
				fmac.AddU64(RemainingVuPipeCycles(vu.cycle, pipe.sCycle, pipe.Cycle));
				fmac.AddU32(pipe.macflag);
				fmac.AddU32(pipe.statusflag);
				fmac.AddU32(pipe.clipflag);
			}
			out.fmac_pipeline = fmac.Value();

			StableHash fdiv_efu;
			fdiv_efu.AddU32(static_cast<u32>(vu.fdiv.enable));
			if (vu.fdiv.enable)
			{
				fdiv_efu.AddU32(vu.fdiv.reg.UL);
				fdiv_efu.AddU64(RemainingVuPipeCycles(vu.cycle, vu.fdiv.sCycle, vu.fdiv.Cycle));
				fdiv_efu.AddU32(vu.fdiv.statusflag);
			}
			fdiv_efu.AddU32(static_cast<u32>(vu.efu.enable));
			if (vu.efu.enable)
			{
				fdiv_efu.AddU32(vu.efu.reg.UL);
				fdiv_efu.AddU64(RemainingVuPipeCycles(vu.cycle, vu.efu.sCycle, vu.efu.Cycle));
			}
			out.fdiv_efu_pipeline = fdiv_efu.Value();

			StableHash ialu;
			ialu.AddU32(vu.ialucount);
			for (u32 offset = 0; offset < vu.ialucount && offset < 4; offset++)
			{
				const ialuPipe& pipe = vu.ialu[(vu.ialureadpos + offset) & 3];
				ialu.AddU32(static_cast<u32>(pipe.reg));
				ialu.AddU64(RemainingVuPipeCycles(vu.cycle, pipe.sCycle, pipe.Cycle));
			}
			out.ialu_pipeline = ialu.Value();

			// TPC is published provider state. The remaining controller registers
			// live in VU0's VI array and control both VUs.
			StableHash controller;
			controller.AddU32(vu.VI[REG_TPC].UL);
			if (vu.IsVU0())
			{
				controller.AddU32(vu.VI[REG_CMSAR0].UL);
				controller.AddU32(vu.VI[REG_CMSAR1].UL);
				controller.AddU32(vu.VI[REG_FBRST].UL);
				controller.AddU32(vu.VI[REG_VPU_STAT].UL);
			}
			out.controller = controller.Value();

			StableHash xgkick;
			xgkick.AddU32(vu.xgkickenable);
			if (vu.xgkickenable)
			{
				xgkick.AddU32(vu.xgkickaddr);
				xgkick.AddU32(vu.xgkickdiff);
				xgkick.AddU32(vu.xgkicksizeremaining);
				xgkick.AddU64(vu.cycle - vu.xgkicklastcycle);
				xgkick.AddU32(vu.xgkickcyclecount);
				xgkick.AddU32(vu.xgkickendpacket);
			}
			out.xgkick = xgkick.Value();
			return out;
		}

		u64 HashMemory(const void* data, size_t size)
		{
			StableHash hash;
			if (data && size != 0)
				hash.AddBytes(data, size);
			return hash.Value();
		}

		u64 HashSifState(const _sif& sif)
		{
			StableHash hash;
			// Sif.h::sifFifo owns a circular queue. Only the size words beginning
			// at readPos can be observed by a future read; consumed data[] slots and
			// junk[] are storage residue. writeJunk() rebuilds junk[] before use.
			if (sif.iop.writeJunk == 0 && sif.fifo.size >= 0 && sif.fifo.size <= FIFO_SIF_W)
			{
				for (s32 word = 0; word < sif.fifo.size; word++)
					hash.AddU32(sif.fifo.data[(sif.fifo.readPos + word) & (FIFO_SIF_W - 1)]);
			}
			else
			{
				// A pending SIF0 tail fill can source the preceding complete QW from
				// data[] even after it left the active queue. Preserve the full ring
				// for that uncommon continuation state.
				hash.AddBytes(sif.fifo.data, sizeof(sif.fifo.data));
			}
			hash.AddU32(static_cast<u32>(sif.fifo.readPos));
			hash.AddU32(static_cast<u32>(sif.fifo.writePos));
			hash.AddU32(static_cast<u32>(sif.fifo.size));
			hash.AddU8(sif.ee.end ? 1 : 0);
			hash.AddU8(sif.ee.busy ? 1 : 0);
			hash.AddU32(static_cast<u32>(sif.ee.cycles));
			hash.AddU8(sif.iop.end ? 1 : 0);
			hash.AddU8(sif.iop.busy ? 1 : 0);
			hash.AddU32(static_cast<u32>(sif.iop.cycles));
			hash.AddU32(static_cast<u32>(sif.iop.writeJunk));
			hash.AddU32(static_cast<u32>(sif.iop.counter));
			hash.AddU32(static_cast<u32>(sif.iop.data.data));
			hash.AddU32(static_cast<u32>(sif.iop.data.words));
			hash.AddU32(sif.iop.data.tag_lo._u32);
			hash.AddU32(sif.iop.data.tag_hi._u32);
			return hash.Value();
		}

		u64 HashVifState(const vifStruct& vif, u32 unit)
		{
			StableHash hash;
			const u32 command = static_cast<u32>(vif.cmd) & 0x7fu;
			const bool active_unpack = command >= 0x60u && command <= 0x7fu && vif.pass != 0;
			// Vif.cpp::vif0Freeze()/vif1Freeze() saves the in-flight DMA cycle
			// accumulator alongside vifStruct and the partial-transfer buffer.
			hash.AddU32(unit == 0 ? g_vif0Cycles : g_vif1Cycles);
			hash.AddBytes(&vif.MaskRow, sizeof(vif.MaskRow));
			hash.AddBytes(&vif.MaskCol, sizeof(vif.MaskCol));
			hash.AddU32(vif.tag.addr);
			hash.AddU32(vif.tag.size);
			hash.AddU32(vif.tag.cmd);
			hash.AddU16(vif.tag.wl);
			hash.AddU16(vif.tag.cl);
			hash.AddU32(static_cast<u32>(vif.cmd));
			hash.AddU32(static_cast<u32>(vif.pass));
			// vifUnpackSetup() establishes UNPACK cmd/pass and resets cl together;
			// full completion clears cmd/pass/NUM synchronously before an event-test
			// checkpoint. The next setup resets a completed unpack's cl residue.
			hash.AddU32(active_unpack ? static_cast<u32>(vif.cl) : 0u);
			hash.AddU8(vif.usn);
			hash.AddU8(vif.start_aligned);
			hash.AddU32(static_cast<u32>(vif.irq));
			hash.AddU8(vif.done ? 1 : 0);
			hash.AddU8(vif.vifstalled.enabled ? 1 : 0);
			hash.AddU32(vif.vifstalled.value);
			hash.AddU8(vif.stallontag ? 1 : 0);
			hash.AddU8(vif.waitforvu ? 1 : 0);
			hash.AddU32(static_cast<u32>(vif.unpackcalls));
			hash.AddU64(vif.BITBLTBUF._u64);
			hash.AddU64(vif.TRXPOS._u64);
			hash.AddU64(vif.TRXREG._u64);
			hash.AddU32(vif.GSLastDownloadSize);
			hash.AddU8(vif.irqoffset.enabled ? 1 : 0);
			hash.AddU32(vif.irqoffset.value);
			hash.AddU32(vif.vifpacketsize);
			hash.AddU8(vif.inprogress);
			hash.AddU8(vif.dmamode);
			hash.AddU8(vif.queued_program ? 1 : 0);
			hash.AddU32(vif.queued_pc);
			hash.AddU8(vif.queued_gif_wait ? 1 : 0);
			// Vif.cpp::vif0Freeze()/vif1Freeze() owns the partial-transfer bytes.
			// They are required to make an active checkpoint continuation-complete.
			hash.AddU32(nVif[unit].bSize);
			if (nVif[unit].bSize <= sizeof(nVif[unit].buffer))
				hash.AddBytes(nVif[unit].buffer, nVif[unit].bSize);
			else
				hash.AddBytes(nVif[unit].buffer, sizeof(nVif[unit].buffer));
			return hash.Value();
		}

		void HashDmaChannel(StableHash& hash, const DMACh& channel)
		{
			hash.AddU32(channel.chcr._u32);
			hash.AddU32(channel.madr);
			hash.AddU32(channel.qwc);
			hash.AddU32(channel.tadr);
			hash.AddU32(channel.asr0);
			hash.AddU32(channel.asr1);
			hash.AddU32(channel.sadr);
		}

		u64 HashDmacState()
		{
			StableHash hash;
			hash.AddU32(dmacRegs.ctrl._u32);
			hash.AddU32(dmacRegs.stat._u32);
			hash.AddU32(dmacRegs.pcr._u32);
			hash.AddU32(dmacRegs.sqwc._u32);
			hash.AddU32(dmacRegs.rbsr._u32);
			hash.AddU32(dmacRegs.rbor._u32);
			hash.AddU32(dmacRegs.stadr._u32);
			HashDmaChannel(hash, vif0ch);
			HashDmaChannel(hash, vif1ch);
			HashDmaChannel(hash, gifch);
			HashDmaChannel(hash, ipu0ch);
			HashDmaChannel(hash, ipu1ch);
			HashDmaChannel(hash, sif0ch);
			HashDmaChannel(hash, sif1ch);
			HashDmaChannel(hash, spr0ch);
			HashDmaChannel(hash, spr1ch);
			return hash.Value();
		}

		void CaptureMemoryHashes(MachineCheckpointTraceRecord& record)
		{
			auto capture = [&](MachineCheckpointMemoryRegion region, const void* data, size_t size) {
				if (data)
					record.memory_available_mask |= 1u << static_cast<u32>(region);
				record.memory_hash[region] = HashMemory(data, size);
			};

			capture(MachineCheckpointMemoryEeRam,
				eeMem ? eeMem->Main : nullptr, Ps2MemSize::MainRam);
			capture(MachineCheckpointMemoryIopRam,
				iopMem ? iopMem->Main : nullptr, Ps2MemSize::IopRam);
			capture(MachineCheckpointMemoryEeScratchpad,
				eeMem ? eeMem->Scratch : nullptr, Ps2MemSize::Scratch);
			capture(MachineCheckpointMemoryVu0Micro, VU0.Micro, VU0_PROGSIZE);
			capture(MachineCheckpointMemoryVu0Data, VU0.Mem, VU0_MEMSIZE);
			capture(MachineCheckpointMemoryVu1Micro, VU1.Micro, VU1_PROGSIZE);
			capture(MachineCheckpointMemoryVu1Data, VU1.Mem, VU1_MEMSIZE);
			capture(MachineCheckpointMemoryEeHardware, eeHw, Ps2MemSize::Hardware);
			capture(MachineCheckpointMemoryIopHardware, iopHw, Ps2MemSize::IopHardware);
			capture(MachineCheckpointMemorySpu2Ram, _spu2mem, sizeof(_spu2mem));
			size_t gs_local_size = 0;
			const u8* gs_local = GSTraceLocalMemoryData(&gs_local_size);
			capture(MachineCheckpointMemoryGsLocal, gs_local, gs_local_size);
		}

		std::string DiagnosticPath(u64 index, const char* suffix)
		{
			char filename[96];
			std::snprintf(filename, sizeof(filename), "checkpoint-%06llu.%s",
				static_cast<unsigned long long>(index), suffix);
			return Path::Combine(s_config.diagnostic_dump_directory, filename);
		}

		bool WriteRawDiagnosticFile(const std::string& path, const void* data, size_t size)
		{
			FILE* file = FileSystem::OpenCFile(path.c_str(), "wb");
			if (!file)
			{
				SetError("Failed to open a machine-checkpoint diagnostic file.");
				return false;
			}
			const bool wrote = std::fwrite(data, size, 1, file) == 1;
			const bool closed = std::fclose(file) == 0;
			const bool success = wrote && closed;
			if (!success)
				SetError("Failed to write a machine-checkpoint diagnostic file.");
			return success;
		}

		bool WriteProjectionDetailDiagnostic(const std::string& path, const MachineCheckpointTraceRecord& record)
		{
			FILE* file = FileSystem::OpenCFile(path.c_str(), "wb");
			if (!file)
			{
				SetError("Failed to open the machine-checkpoint projection-detail file.");
				return false;
			}

			std::fprintf(file, "format=PCSX2-machine-checkpoint-projection-detail-v1\n");
			std::fprintf(file, "checkpoint.index=%llu\n",
				static_cast<unsigned long long>(record.index));
			std::fprintf(file, "ee.pc=%08x\n", record.ee_pc);
			std::fprintf(file, "ee.cycle=%016llx\n",
				static_cast<unsigned long long>(record.ee_cycle));
			std::fprintf(file, "iop.pc=%08x\n", record.iop_pc);
			std::fprintf(file, "iop.cycle=%016llx\n",
				static_cast<unsigned long long>(record.iop_cycle));

			for (u32 reg = 0; reg < 32; reg++)
				std::fprintf(file, "ee.cp0.%02u=%08x\n", reg, cpuRegs.CP0.r[reg]);
			for (u32 entry = 0; entry < 48; entry++)
			{
				std::fprintf(file, "ee.tlb.%02u.page_mask=%08x\n", entry, tlb[entry].PageMask.UL);
				std::fprintf(file, "ee.tlb.%02u.entry_hi=%08x\n", entry, tlb[entry].EntryHi.UL);
				std::fprintf(file, "ee.tlb.%02u.entry_lo0=%08x\n", entry, tlb[entry].EntryLo0.UL);
				std::fprintf(file, "ee.tlb.%02u.entry_lo1=%08x\n", entry, tlb[entry].EntryLo1.UL);
			}
			for (u32 reg = 0; reg < 4; reg++)
				std::fprintf(file, "ee.perf.%u=%08x\n", reg, cpuRegs.PERF.r[reg]);

			for (u32 reg = 0; reg < 32; reg++)
				std::fprintf(file, "ee.fpu.fpr.%02u=%08x\n", reg, fpuRegs.fpr[reg].UL);
			std::fprintf(file, "ee.fpu.fcr0=%08x\n", fpuRegs.fprc[0]);
			std::fprintf(file, "ee.fpu.fcr31=%08x\n", fpuRegs.fprc[31]);
			std::fprintf(file, "ee.fpu.acc=%08x\n", fpuRegs.ACC.UL);
			std::fprintf(file, "ee.fpu.accflag=%08x\n", fpuRegs.ACCflag);

			std::fprintf(file, "vu0.status=%08x\n", VU0.VI[REG_STATUS_FLAG].UL & 0x00000fffu);
			std::fprintf(file, "vu0.mac=%08x\n", VU0.VI[REG_MAC_FLAG].UL & 0x0000ffffu);
			std::fprintf(file, "vu0.clip=%08x\n", VU0.VI[REG_CLIP_FLAG].UL & 0x00ffffffu);

			for (u32 word = 0; word < FIFO_SIF_W; word++)
				std::fprintf(file, "sif0.fifo.data.%03u=%08x\n", word, sif0.fifo.data[word]);
			for (u32 word = 0; word < 4; word++)
				std::fprintf(file, "sif0.fifo.junk.%u=%08x\n", word, sif0.fifo.junk[word]);
			std::fprintf(file, "sif0.fifo.read_pos=%08x\n", static_cast<u32>(sif0.fifo.readPos));
			std::fprintf(file, "sif0.fifo.write_pos=%08x\n", static_cast<u32>(sif0.fifo.writePos));
			std::fprintf(file, "sif0.fifo.size=%08x\n", static_cast<u32>(sif0.fifo.size));
			std::fprintf(file, "sif0.ee.end=%u\n", sif0.ee.end ? 1u : 0u);
			std::fprintf(file, "sif0.ee.busy=%u\n", sif0.ee.busy ? 1u : 0u);
			std::fprintf(file, "sif0.ee.cycles=%08x\n", static_cast<u32>(sif0.ee.cycles));
			std::fprintf(file, "sif0.iop.end=%u\n", sif0.iop.end ? 1u : 0u);
			std::fprintf(file, "sif0.iop.busy=%u\n", sif0.iop.busy ? 1u : 0u);
			std::fprintf(file, "sif0.iop.cycles=%08x\n", static_cast<u32>(sif0.iop.cycles));
			std::fprintf(file, "sif0.iop.write_junk=%08x\n", static_cast<u32>(sif0.iop.writeJunk));
			std::fprintf(file, "sif0.iop.counter=%08x\n", static_cast<u32>(sif0.iop.counter));
			std::fprintf(file, "sif0.iop.data.data=%08x\n", static_cast<u32>(sif0.iop.data.data));
			std::fprintf(file, "sif0.iop.data.words=%08x\n", static_cast<u32>(sif0.iop.data.words));
			std::fprintf(file, "sif0.iop.data.tag_lo=%08x\n", sif0.iop.data.tag_lo._u32);
			std::fprintf(file, "sif0.iop.data.tag_hi=%08x\n", sif0.iop.data.tag_hi._u32);

			for (u32 word = 0; word < 4; word++)
			{
				std::fprintf(file, "vif1.mask_row.%u=%08x\n", word, vif1.MaskRow._u32[word]);
				std::fprintf(file, "vif1.mask_col.%u=%08x\n", word, vif1.MaskCol._u32[word]);
			}
			std::fprintf(file, "vif1.tag.addr=%08x\n", vif1.tag.addr);
			std::fprintf(file, "vif1.tag.size=%08x\n", vif1.tag.size);
			std::fprintf(file, "vif1.tag.cmd=%08x\n", vif1.tag.cmd);
			std::fprintf(file, "vif1.tag.wl=%04x\n", vif1.tag.wl);
			std::fprintf(file, "vif1.tag.cl=%04x\n", vif1.tag.cl);
			std::fprintf(file, "vif1.cmd=%08x\n", static_cast<u32>(vif1.cmd));
			std::fprintf(file, "vif1.pass=%08x\n", static_cast<u32>(vif1.pass));
			std::fprintf(file, "vif1.cl=%08x\n", static_cast<u32>(vif1.cl));
			std::fprintf(file, "vif1.regs.num=%08x\n", vif1Regs.num);
			std::fprintf(file, "vif1.dma_cycles=%08x\n", g_vif1Cycles);
			std::fprintf(file, "vif1.partial_buffer_size=%08x\n", nVif[1].bSize);
			std::fprintf(file, "vif1.partial_buffer_hash=%016llx\n",
				static_cast<unsigned long long>(HashMemory(nVif[1].buffer,
					std::min<size_t>(nVif[1].bSize, sizeof(nVif[1].buffer)))));
			std::fprintf(file, "vif1.usn=%02x\n", vif1.usn);
			std::fprintf(file, "vif1.start_aligned=%02x\n", vif1.start_aligned);
			std::fprintf(file, "vif1.irq=%08x\n", static_cast<u32>(vif1.irq));
			std::fprintf(file, "vif1.done=%u\n", vif1.done ? 1u : 0u);
			std::fprintf(file, "vif1.stalled.enabled=%u\n", vif1.vifstalled.enabled ? 1u : 0u);
			std::fprintf(file, "vif1.stalled.value=%08x\n", vif1.vifstalled.value);
			std::fprintf(file, "vif1.stall_on_tag=%u\n", vif1.stallontag ? 1u : 0u);
			std::fprintf(file, "vif1.wait_for_vu=%u\n", vif1.waitforvu ? 1u : 0u);
			std::fprintf(file, "vif1.unpack_calls=%08x\n", static_cast<u32>(vif1.unpackcalls));
			std::fprintf(file, "vif1.bitbltbuf=%016llx\n",
				static_cast<unsigned long long>(vif1.BITBLTBUF._u64));
			std::fprintf(file, "vif1.trxpos=%016llx\n",
				static_cast<unsigned long long>(vif1.TRXPOS._u64));
			std::fprintf(file, "vif1.trxreg=%016llx\n",
				static_cast<unsigned long long>(vif1.TRXREG._u64));
			std::fprintf(file, "vif1.gs_last_download_size=%08x\n", vif1.GSLastDownloadSize);
			std::fprintf(file, "vif1.irq_offset.enabled=%u\n", vif1.irqoffset.enabled ? 1u : 0u);
			std::fprintf(file, "vif1.irq_offset.value=%08x\n", vif1.irqoffset.value);
			std::fprintf(file, "vif1.packet_size=%08x\n", vif1.vifpacketsize);
			std::fprintf(file, "vif1.in_progress=%02x\n", vif1.inprogress);
			std::fprintf(file, "vif1.dma_mode=%02x\n", vif1.dmamode);
			std::fprintf(file, "vif1.queued_program=%u\n", vif1.queued_program ? 1u : 0u);
			std::fprintf(file, "vif1.queued_pc=%08x\n", vif1.queued_pc);
			std::fprintf(file, "vif1.queued_gif_wait=%u\n", vif1.queued_gif_wait ? 1u : 0u);

			const bool wrote = std::ferror(file) == 0;
			const bool closed = std::fclose(file) == 0;
			const bool success = wrote && closed;
			if (!success)
				SetError("Failed to write the machine-checkpoint projection-detail file.");
			return success;
		}

		bool WriteDiagnosticSnapshot(const MachineCheckpointTraceRecord& record)
		{
			if (s_config.diagnostic_dump_directory.empty())
				return true;
			if (!eeMem || !iopMem)
			{
				SetError("Machine-checkpoint diagnostic RAM is unavailable.");
				return false;
			}
			return WriteRawDiagnosticFile(DiagnosticPath(record.index, "ee-ram.bin"),
					eeMem->Main, Ps2MemSize::MainRam) &&
				WriteRawDiagnosticFile(DiagnosticPath(record.index, "iop-ram.bin"),
					iopMem->Main, Ps2MemSize::IopRam) &&
				WriteProjectionDetailDiagnostic(
					DiagnosticPath(record.index, "projection.txt"), record);
		}

		void CaptureRecord(MachineCheckpointTraceRecord& record)
		{
			record = {};
			record.index = s_records_written;
			record.vu1_completion_ordinal = s_last_completion_ordinal;
			record.vu1_completions_at_event_test = s_pending_completion_count;
			record.trigger = CHECKPOINT_TRIGGER_VU1_COMPLETED_EVENT_TEST;
			record.configured_after_sif_records = s_config.after_sif_records;
			record.configured_after_vif_records = s_config.after_vif_records;
			record.sif_records = GetSifTraceRecordsWritten();
			record.vif_records = GetVifTraceRecordsWritten();
			record.core_event_records = GetCoreEventTraceRecordsWritten();
			record.gs_records = GetGsTraceRecordsWritten();

			record.ee_cycle = cpuRegs.cycle;
			record.ee_next_event_cycle = cpuRegs.nextEventCycle;
			record.ee_last_event_cycle = cpuRegs.lastEventCycle;
			record.ee_pc = cpuRegs.pc;
			// cpuRegs.code/psxRegs.code are provider-private decode scratch and
			// need not name the instruction at a shared event seam. Project the
			// guest word at each architectural PC instead.
			record.ee_opcode = memRead32(cpuRegs.pc);
			record.ee_branch = static_cast<u32>(cpuRegs.branch);
			record.ee_is_delay_slot = cpuRegs.IsDelaySlot;
			record.ee_interrupt = cpuRegs.interrupt;
			record.ee_iop_cycle_balance = EEsCycle;
			record.ee_counter_delta = nextDeltaCounter;
			record.ee_counter_start = nextStartCounter;
			record.ee_gpr_hash = HashEeGprs();
			record.ee_cp0_tlb_perf_hash = HashEeCp0TlbPerf();
			record.ee_fpu_hash = HashEeFpu();
			record.ee_control_hash = HashEeControl();
			record.ee_event_schedule_hash = HashEeEventSchedule();

			record.iop_cycle = psxRegs.cycle;
			record.iop_next_event_cycle = psxRegs.iopNextEventCycle;
			record.iop_pc = psxRegs.pc;
			record.iop_opcode = iopMemRead32(psxRegs.pc);
			record.iop_is_delay_slot = iopIsDelaySlot ? 1u : 0u;
			record.iop_interrupt = psxRegs.interrupt;
			record.iop_counter_delta = psxNextDeltaCounter;
			record.iop_counter_start = psxNextStartCounter;
			record.iop_gpr_hash = HashIopGprs();
			record.iop_cp0_hash = HashIopCp0();
			record.iop_cp2_hash = HashIopCp2();
			record.iop_control_hash = HashIopControl();
			record.iop_event_schedule_hash = HashIopEventSchedule();

			for (u32 unit = 0; unit < 2; unit++)
			{
				const VURegs& vu = unit == 0 ? VU0 : VU1;
				record.vu_cycle[unit] = vu.cycle;
				record.vu_next_block_cycles[unit] = vu.nextBlockCycles;
				record.vu_tpc[unit] = vu.VI[REG_TPC].UL;
				record.vu[unit] = HashVuState(vu);
			}
			record.vu_busy_bits = VU0.VI[REG_VPU_STAT].UL & 0x101u;
			CaptureMemoryHashes(record);
			record.sif_state_hash[0] = HashSifState(sif0);
			record.sif_state_hash[1] = HashSifState(sif1);
			record.vif_state_hash[0] = HashVifState(vif0, 0);
			record.vif_state_hash[1] = HashVifState(vif1, 1);
			record.dmac_state_hash = HashDmacState();
		}
	} // namespace

	bool StartMachineCheckpointTrace(const MachineCheckpointTraceConfig& config, Error* error)
	{
		StopMachineCheckpointTrace();
		if (config.output_path.empty())
		{
			Error::SetStringView(error, "Machine checkpoint trace output path is empty.");
			return false;
		}
		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() &&
			!FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
		{
			return false;
		}
		if (!config.diagnostic_dump_directory.empty())
		{
			if (config.max_records == 0)
			{
				Error::SetStringView(error,
					"Machine checkpoint diagnostic dumps require a bounded max_records value.");
				return false;
			}
			if (!FileSystem::EnsureDirectoryExists(
					config.diagnostic_dump_directory.c_str(), true, error))
			{
				return false;
			}
		}
		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open machine checkpoint trace output '{}'.",
				config.output_path);
			return false;
		}

		s_config = config;
		s_eligible_completions_seen = 0;
		s_records_written = 0;
		s_last_completion_ordinal = 0;
		s_pending_completion_count = 0;
		s_started = !s_config.wait_for_elf_entry;
		s_hit_limit = false;
		s_vu1_program_armed = false;
		s_pending_checkpoint = false;
		s_entry_pc = s_started ? 0xbfc00000 : 0;
		s_error.clear();
		if (!WriteHeader())
		{
			Error::SetStringFmt(error, "Failed to write machine checkpoint trace header to '{}'.",
				config.output_path);
			StopMachineCheckpointTrace();
			return false;
		}
		return true;
	}

	void StopMachineCheckpointTrace()
	{
		if (!s_trace_file)
			return;
		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();
		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
		s_vu1_program_armed = false;
		s_pending_checkpoint = false;
	}

	bool IsMachineCheckpointTraceEnabled()
	{
		return s_trace_file && s_started && !s_hit_limit;
	}

	void NotifyMachineCheckpointElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;
		s_entry_pc = pc;
		s_started = true;
	}

	void NotifyMachineCheckpointVu1ProgramStarted()
	{
		if (IsMachineCheckpointTraceEnabled())
			s_vu1_program_armed = true;
	}

	void NotifyMachineCheckpointVu1ExecutionCompleted()
	{
		if (!IsMachineCheckpointTraceEnabled() || !s_vu1_program_armed ||
			(VU0.VI[REG_VPU_STAT].UL & 0x100) != 0)
		{
			return;
		}
		s_vu1_program_armed = false;
		if (GetSifTraceRecordsWritten() < s_config.after_sif_records ||
			GetVifTraceRecordsWritten() < s_config.after_vif_records)
		{
			return;
		}
		const u64 completion_ordinal = s_eligible_completions_seen++;
		if (completion_ordinal < s_config.skip_records)
			return;
		if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
		{
			s_hit_limit = true;
			return;
		}
		s_pending_checkpoint = true;
		s_last_completion_ordinal = completion_ordinal;
		s_pending_completion_count++;
	}

	bool RecordPendingMachineCheckpointAtEventTest()
	{
		if (!IsMachineCheckpointTraceEnabled() || !s_pending_checkpoint)
			return s_hit_limit;
		// A completed VU1 program can overlap an active VU0 program or be followed
		// by another MSCAL in the same EE event test. Preserve the pending marker
		// and emit only at the first shared event seam where both VUs are idle.
		// Provider-private branch/backup state is then non-continuation residue.
		if ((VU0.VI[REG_VPU_STAT].UL & 0x101) != 0)
			return false;
		GSTraceStateSnapshot(GsTraceStateTriggerManual);
		MachineCheckpointTraceRecord record = {};
		CaptureRecord(record);
		if (!WriteDiagnosticSnapshot(record))
		{
			s_hit_limit = true;
			return true;
		}
		if (std::fwrite(&record, sizeof(record), 1, s_trace_file) != 1)
		{
			SetError("Failed to write machine checkpoint trace record.");
			s_hit_limit = true;
			return true;
		}
		s_records_written++;
		s_pending_checkpoint = false;
		s_pending_completion_count = 0;
		if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
			s_hit_limit = true;
		return s_hit_limit;
	}

	u64 GetMachineCheckpointTraceRecordsWritten() { return s_records_written; }
	bool DidMachineCheckpointTraceHitLimit() { return s_hit_limit; }
	const std::string& GetMachineCheckpointTraceError() { return s_error; }
} // namespace Pcsx2Trace
