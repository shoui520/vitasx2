// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MTVU.h"
#include "R3000A.h"
#include "R5900.h"
#include "SaveState.h"
#include "VUmicro.h"
#include "vita/VitaCore.h"
#include "vtlb.h"

#include "common/Assertions.h"
#include "common/Console.h"

static void recReserve()
{
}

static void recShutdown()
{
}

static void recReset()
{
	intCpu.Reset();
}

static void recStep()
{
	intCpu.Step();
}

static void recExecute()
{
	intCpu.Execute();
}

static void recExitExecution()
{
	intCpu.ExitExecution();
}

static void recCancelInstruction()
{
	intCpu.CancelInstruction();
}

static void recClear(u32 addr, u32 size)
{
}

R5900cpu recCpu = {
	recReserve,
	recShutdown,
	recReset,
	recStep,
	recExecute,
	recExitExecution,
	recCancelInstruction,
	recClear,
};

static void psxRecReserve()
{
}

static void psxRecReset()
{
	psxInt.Reset();
}

static s32 psxRecExecuteBlock(s32 eeCycles)
{
	return psxInt.ExecuteBlock(eeCycles);
}

static void psxRecClear(u32 addr, u32 size)
{
}

static void psxRecShutdown()
{
}

R3000Acpu psxRec = {
	psxRecReserve,
	psxRecReset,
	psxRecExecuteBlock,
	psxRecClear,
	psxRecShutdown,
};

recMicroVU0 CpuMicroVU0;
recMicroVU1 CpuMicroVU1;

recMicroVU0::recMicroVU0()
{
	m_Idx = 0;
	IsInterpreter = false;
}

void recMicroVU0::Reserve()
{
}

void recMicroVU0::Shutdown()
{
}

void recMicroVU0::Reset()
{
	CpuIntVU0.Reset();
}

void recMicroVU0::Step()
{
	CpuIntVU0.Step();
}

void recMicroVU0::SetStartPC(u32 startPC)
{
	CpuIntVU0.SetStartPC(startPC);
}

void recMicroVU0::Execute(u32 cycles)
{
	CpuIntVU0.Execute(cycles);
}

void recMicroVU0::Clear(u32 addr, u32 size)
{
}

recMicroVU1::recMicroVU1()
{
	m_Idx = 1;
	IsInterpreter = false;
}

void recMicroVU1::Reserve()
{
}

void recMicroVU1::Shutdown()
{
}

void recMicroVU1::Reset()
{
	CpuIntVU1.Reset();
}

void recMicroVU1::Step()
{
	CpuIntVU1.Step();
}

void recMicroVU1::SetStartPC(u32 startPC)
{
	CpuIntVU1.SetStartPC(startPC);
}

void recMicroVU1::Execute(u32 cycles)
{
	CpuIntVU1.Execute(cycles);
}

void recMicroVU1::Clear(u32 addr, u32 size)
{
}

void recMicroVU1::ResumeXGkick()
{
	CpuIntVU1.ResumeXGkick();
}

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits,
	bool is_signed, bool is_load, bool is_fpr)
{
	pxFailRel("Vita ARM32 fastmem backpatching is disabled.");
}

bool SaveStateBase::vuJITFreeze()
{
	if (IsSaving())
		vu1Thread.WaitVU();

	Console.Warning("recompiler state is unavailable in the Vita ARM32 interpreter build.");

	std::array<u8, 96> empty_data{};
	Freeze(empty_data);
	Freeze(empty_data);
	return true;
}

void VitaSelectInterpreterCpuProviders()
{
	Cpu = &intCpu;
	psxCpu = &psxInt;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
}
