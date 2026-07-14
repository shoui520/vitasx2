// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/DEV9.h"
#include "DebugTools/MachineCheckpointTrace.h"
#include "FW.h"

#include <array>
#include <cstring>

struct NetPacket;

namespace
{
	std::array<s8, 0x10000> s_fwregs = {};
}

s8* fwregs = s_fwregs.data();

void rx_process(NetPacket* pk) {}
bool rx_fifo_can_rx() { return false; }

s32 DEV9init() { return 0; }
void DEV9close() {}
s32 DEV9open() { return 0; }
void DEV9shutdown() {}
void _DEV9irq(int cause, int cycles)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9IrqScheduled);
#endif
}
int DEV9irqHandler(void)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9IrqDelivered);
#endif
	return 0;
}
void DEV9async(u32 cycles) {}
void DEV9runFIFO() {}
void DEV9writeDMA8Mem(u32* pMem, int size)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Dma);
#endif
}
void DEV9readDMA8Mem(u32* pMem, int size)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Dma);
#endif
	if (pMem && size > 0)
		std::memset(pMem, 0, static_cast<size_t>(size));
}
u8 DEV9read8(u32 addr)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Read);
#endif
	return 0xff;
}
u16 DEV9read16(u32 addr)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Read);
#endif
	return 0xffff;
}
u32 DEV9read32(u32 addr)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Read);
#endif
	return 0xffffffffu;
}
void DEV9write8(u32 addr, u8 value)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Write);
#endif
}
void DEV9write16(u32 addr, u16 value)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Write);
#endif
}
void DEV9write32(u32 addr, u32 value)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::Dev9Write);
#endif
}
void DEV9CheckChanges(const Pcsx2Config& old_config) {}

// Retained for the legacy plugin-facing declaration in USB/USB.h. The PCSX2
// OHCI owner accesses iopMem directly and has no implementation of this hook.
void USBsetRAM(void* mem) {}

s32 FWopen() { return 0; }
void FWclose() {}
void PHYWrite() {}
void PHYRead() {}
u32 FWread32(u32 addr)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::FireWireRead);
#endif
	return fwRu32(addr);
}
void FWwrite32(u32 addr, u32 value)
{
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	Pcsx2Trace::NotifyPortableReplayExternalDeviceAccess(
		Pcsx2Trace::PortableReplayExternalDeviceAccess::FireWireWrite);
#endif
	fwRu32(addr) = value;
}
