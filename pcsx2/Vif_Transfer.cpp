// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "DebugTools/VifTrace.h"
#include "MTVU.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"

#if defined(VITASX2_CPU_PROFILER)
#include "vita/VitaPerformanceTelemetry.h"
#endif
#if defined(VITASX2_VIF_EPOCH_CENSUS)
#include "vita/VitaVifEpochCensus.h"
#endif

#if defined(VITASX2_VIF_EPOCH_CENSUS)
namespace
{
	VitaVifEpochCensus::StatisticalStage VifEpochCommandStage(
		VitaVifEpochCensus::CommandClass command_class)
	{
		using CommandClass = VitaVifEpochCensus::CommandClass;
		using StatisticalStage = VitaVifEpochCensus::StatisticalStage;
		switch (command_class)
		{
			case CommandClass::State:
				return StatisticalStage::State;
			case CommandClass::StatePayload:
				return StatisticalStage::StatePayload;
			case CommandClass::Unpack:
				return StatisticalStage::Unpack;
			case CommandClass::Mpg:
				return StatisticalStage::Mpg;
			case CommandClass::Direct:
				return StatisticalStage::Direct;
			case CommandClass::Synchronize:
				return StatisticalStage::Synchronize;
			case CommandClass::Error:
			default:
				return StatisticalStage::Error;
		}
	}
} // namespace
#endif

//------------------------------------------------------------------
// VifCode Transfer Interpreter (Vif0/Vif1)
//------------------------------------------------------------------

// Interprets packet
_vifT void vifTransferLoop(u32* &data) {
	vifStruct& vifX = GetVifX;

	u32& pSize = vifX.vifpacketsize;

	int ret = 0;

	vifXRegs.stat.VPS |= VPS_TRANSFERRING;
	vifXRegs.stat.ER1  = false;
	//VIF_LOG("Starting VIF%d loop, pSize = %x, stalled = %x", idx, pSize, vifX.vifstalled.enabled );
	while (pSize > 0 && !vifX.vifstalled.enabled) {

		if(!vifX.cmd) { // Get new VifCode

			if(!vifXRegs.err.MII)
			{
				if(vifX.irq && !CHECK_VIF1STALLHACK)
					break;

				vifX.irq      |= data[0] >> 31;
			}

		#if defined(VITASX2_VIF_EPOCH_CENSUS)
			{
				const VitaVifEpochCensus::ScopedStatisticalStage parse_stage(
					VitaVifEpochCensus::StatisticalStage::Parse);
		#endif
			vifXRegs.code = data[0];
			vifX.cmd	  = data[0] >> 24;

			const DMACh& vifChannel = idx ? vif1ch : vif0ch;
			const u32 trace_cycle = static_cast<u32>(vifXRegs.cycle.cl) |
				(static_cast<u32>(vifXRegs.cycle.wl) << 8);
			Pcsx2Trace::RecordVifCommand(static_cast<u8>(idx), vifXRegs.code,
				vifXRegs.stat._u32, trace_cycle,
				vifXRegs.mode, vifXRegs.num, vifXRegs.mask, vifX.tag.addr,
				vifX.tag.size, vifX.vifpacketsize, vifChannel.madr, vifChannel.qwc);

			VIF_LOG("New VifCMD %x tagsize %x irq %d", vifX.cmd, vifX.tag.size, vifX.irq);
			if (IsDevBuild && TraceLogging.EE.VIFcode.IsActive()) {
				// Pass 2 means "log it"
				vifCmdHandler[idx][vifX.cmd & 0x7f](2, data);
			}
		#if defined(VITASX2_VIF_EPOCH_CENSUS)
			}
			{
				const VitaVifEpochCensus::ScopedStatisticalStage accounting_stage(
					VitaVifEpochCensus::StatisticalStage::Accounting);
				VitaVifEpochCensus::BeginCommand(idx,
					VitaVifEpochCensus::ClassifyCommand(idx, vifX.cmd));
			}
		#endif
		}

	#if defined(VITASX2_VIF_EPOCH_CENSUS)
		const VitaVifEpochCensus::CommandClass command_class =
			VitaVifEpochCensus::ClassifyCommand(idx, vifX.cmd);
		{
			const VitaVifEpochCensus::ScopedStatisticalStage command_stage(
				VifEpochCommandStage(command_class));
			ret = vifCmdHandler[idx][vifX.cmd & 0x7f](vifX.pass, data);
		}
		{
			const VitaVifEpochCensus::ScopedStatisticalStage accounting_stage(
				VitaVifEpochCensus::StatisticalStage::Accounting);
			VitaVifEpochCensus::RecordCommandStep(idx, command_class,
				ret > 0 ? static_cast<u32>(ret) : 0u, vifX.cmd == 0);
		}
	#else
		ret = vifCmdHandler[idx][vifX.cmd & 0x7f](vifX.pass, data);
	#endif
	#if defined(VITASX2_VIF_EPOCH_CENSUS)
		{
			const VitaVifEpochCensus::ScopedStatisticalStage bookkeeping_stage(
				VitaVifEpochCensus::StatisticalStage::CommandBookkeeping);
	#endif
		data   += ret;
		pSize  -= ret;
	#if defined(VITASX2_VIF_EPOCH_CENSUS)
		}
	#endif
		if (vifX.vifstalled.enabled)
		{
		#if defined(VITASX2_VIF_EPOCH_CENSUS)
			const VitaVifEpochCensus::ScopedStatisticalStage stall_stage(
				VitaVifEpochCensus::StatisticalStage::Stall);
		#endif
			int current_STR = idx ? vif1ch.chcr.STR : vif0ch.chcr.STR;
			if (!current_STR)
				DevCon.Warning("Warning! VIF%d stalled during FIFO transfer!", idx);
		}
	}
}

_vifT static __fi bool vifTransfer(u32 *data, int size, bool TTE) {
#if defined(VITASX2_CPU_PROFILER)
	const VitaPerformanceTelemetry::ScopedCpuStatisticalStage profile_stage(
		VitaPerformanceTelemetry::CpuStage::VifTransfer);
#endif
	vifStruct& vifX = GetVifX;

#if defined(VITASX2_VIF_EPOCH_CENSUS)
	VitaVifEpochCensus::BeginTransfer(idx,
		size > 0 ? static_cast<u32>(size) : 0u, TTE,
		static_cast<u32>(vifX.cmd), static_cast<u32>(vifX.pass));
#endif

	// irqoffset necessary to add up the right qws, or else will spin (spiderman)
	int transferred = vifX.irqoffset.enabled ? vifX.irqoffset.value : 0;

	vifX.vifpacketsize = size;
	vifTransferLoop<idx>(data);

#if defined(VITASX2_VIF_EPOCH_CENSUS)
	const u32 remaining_words = vifX.vifpacketsize;
	const bool made_progress =
		size > 0 && remaining_words < static_cast<u32>(size);
	{
		const VitaVifEpochCensus::ScopedStatisticalStage bookkeeping_stage(
			VitaVifEpochCensus::StatisticalStage::DmaBookkeeping);
#endif
	transferred += size - vifX.vifpacketsize;

	//Make this a minimum of 1 cycle so if it's the end of the packet it doesnt just fall through.
	//Metal Saga can do this, just to be safe :)
	if (!idx) g_vif0Cycles += std::max(1, (int)((transferred * BIAS) >> 2));
	else	  g_vif1Cycles += std::max(1, (int)((transferred * BIAS) >> 2));

	vifX.irqoffset.value = transferred % 4; // cannot lose the offset

	if (vifX.irq && vifX.cmd == 0) {
		VIF_LOG("Vif%d IRQ Triggering", idx);
		//Always needs to be set to return to the correct offset if there is data left.
		vifX.vifstalled.enabled = VifStallEnable(vifXch);
		vifX.vifstalled.value = VIF_IRQ_STALL;
	}

	if (!TTE) // *WARNING* - Tags CAN have interrupts! so lets just ignore the dma modifying stuffs (GT4)
	{
		transferred  = transferred >> 2;
		transferred = std::min((int)vifXch.qwc, transferred);
		vifXch.madr +=(transferred << 4);
		vifXch.qwc  -= transferred;

		hwDmacSrcTadrInc(vifXch);

		vifX.irqoffset.enabled = false;

		if(!vifXch.qwc)
			vifX.inprogress &= ~0x1;
		else if (vifX.irqoffset.value != 0)
			vifX.irqoffset.enabled = true;
	}
	else
	{
		if(vifX.irqoffset.value != 0){
			vifX.irqoffset.enabled = true;
		}else
				vifX.irqoffset.enabled = false;
	}
#if defined(VITASX2_VIF_EPOCH_CENSUS)
	}
#endif

	vifExecQueue(idx);

#if defined(VITASX2_VIF_EPOCH_CENSUS)
	{
		const VitaVifEpochCensus::ScopedStatisticalStage accounting_stage(
			VitaVifEpochCensus::StatisticalStage::Accounting);
		VitaVifEpochCensus::EndTransfer(idx, remaining_words,
			vifX.vifstalled.enabled, vifX.vifstalled.value,
			vifX.cmd != 0, vifX.irq != 0, made_progress);
	}
#endif

	return !vifX.vifstalled.enabled;
}

// When TTE is set to 1, MADR and QWC are not updated as part of the transfer.
bool VIF0transfer(u32 *data, int size, bool TTE) {
	return vifTransfer<0>(data, size, TTE);
}
bool VIF1transfer(u32 *data, int size, bool TTE) {
	const bool result = vifTransfer<1>(data, size, TTE);
	if (THREAD_VU1)
		vu1Thread.PublishPendingVifBatch();
	return result;
}
