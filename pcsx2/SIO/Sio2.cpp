// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Host.h"
#include "IopDma.h"
#include "Recording/InputRecording.h"
#include "SIO/Memcard/MemoryCardProtocol.h"
#include "SIO/Multitap/MultitapProtocol.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Sio.h"
#include "SIO/Sio2.h"
#include "SIO/SioTypes.h"
#include "StateWrapper.h"

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <cstring>

#define SIO2LOG_ENABLE 0
#define Sio2Log if (SIO2LOG_ENABLE) DevCon

Sio2ByteFifo g_Sio2FifoIn;
Sio2ByteFifo g_Sio2FifoOut;

Sio2 g_Sio2;

namespace
{
	static constexpr u32 PORTABLE_SIO2_INPUT_FIFO_LIMIT = 0xffffu * 4u;
	// DMA11's BCR can append more than one padded response block, so one block
	// is not a complete output bound. A retail portable replay cannot consume
	// more than its 2 MiB IOP RAM without wrapping; reject a larger retained FIFO.
	static constexpr u32 PORTABLE_SIO2_OUTPUT_FIFO_LIMIT = Ps2MemSize::IopRam;
}

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuSio2FifoBulkReadBytes = 0;
u32 g_qemuSio2FifoBulkWriteBytes = 0;
u32 g_qemuSio2FifoNeonQwords = 0;
u32 g_qemuSio2FifoNeon64ByteGroups = 0;
u32 g_qemuSio2FifoNeon256ByteGroups = 0;
u32 g_qemuSio2FifoCompactBytes = 0;
u32 g_qemuSio2FifoExactSpanCopies = 0;
u32 g_qemuSio2FifoExact16ByteCopies = 0;
u32 g_qemuSio2FifoExact320ByteCopies = 0;
u32 g_qemuSio2FifoExact640ByteCopies = 0;
u32 g_qemuSio2FifoBulkFillBytes = 0;
u32 g_qemuSio2FifoNeonFillQwords = 0;
u32 g_qemuSio2FifoNeonFill64ByteGroups = 0;
u32 g_qemuSio2FifoNeonFill256ByteGroups = 0;
#endif

namespace
{
	constexpr size_t SIO2_FIFO_COMPACT_THRESHOLD = 256;

#if defined(ARCH_ARM32)
	static __forceinline void Sio2Copy64Bytes(u8* dst, const u8* src)
	{
		const uint8x16_t qword0 = vld1q_u8(src);
		const uint8x16_t qword1 = vld1q_u8(src + 16);
		const uint8x16_t qword2 = vld1q_u8(src + 32);
		const uint8x16_t qword3 = vld1q_u8(src + 48);
		vst1q_u8(dst, qword0);
		vst1q_u8(dst + 16, qword1);
		vst1q_u8(dst + 32, qword2);
		vst1q_u8(dst + 48, qword3);
	}

	static __forceinline void Sio2Copy256Bytes(u8* dst, const u8* src)
	{
		Sio2Copy64Bytes(dst, src);
		Sio2Copy64Bytes(dst + 64, src + 64);
		Sio2Copy64Bytes(dst + 128, src + 128);
		Sio2Copy64Bytes(dst + 192, src + 192);
	}

	static __forceinline void Sio2Copy320Bytes(u8* dst, const u8* src)
	{
		Sio2Copy256Bytes(dst, src);
		Sio2Copy64Bytes(dst + 256, src + 256);
	}

	static __forceinline void Sio2Copy640Bytes(u8* dst, const u8* src)
	{
		Sio2Copy320Bytes(dst, src);
		Sio2Copy320Bytes(dst + 320, src + 320);
	}

	static __forceinline void Sio2Copy16Bytes(u8* dst, const u8* src)
	{
		const uint8x16_t qword = vld1q_u8(src);
		vst1q_u8(dst, qword);
	}

	static __forceinline void Sio2Fill64Bytes(u8* dst, uint8x16_t value)
	{
		vst1q_u8(dst, value);
		vst1q_u8(dst + 16, value);
		vst1q_u8(dst + 32, value);
		vst1q_u8(dst + 48, value);
	}

	static __forceinline void Sio2Fill256Bytes(u8* dst, uint8x16_t value)
	{
		Sio2Fill64Bytes(dst, value);
		Sio2Fill64Bytes(dst + 64, value);
		Sio2Fill64Bytes(dst + 128, value);
		Sio2Fill64Bytes(dst + 192, value);
	}

	static __forceinline void Sio2Fill320Bytes(u8* dst, uint8x16_t value)
	{
		Sio2Fill256Bytes(dst, value);
		Sio2Fill64Bytes(dst + 256, value);
	}

	static __forceinline void Sio2Fill640Bytes(u8* dst, uint8x16_t value)
	{
		Sio2Fill320Bytes(dst, value);
		Sio2Fill320Bytes(dst + 320, value);
	}

	static __forceinline void Sio2CountNeonCopy(size_t qwords, size_t groups64, size_t groups256, bool exact_span = false, size_t exact_bytes = 0)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSio2FifoNeonQwords += static_cast<u32>(qwords);
		g_qemuSio2FifoNeon64ByteGroups += static_cast<u32>(groups64);
		g_qemuSio2FifoNeon256ByteGroups += static_cast<u32>(groups256);
		if (exact_span)
		{
			g_qemuSio2FifoExactSpanCopies++;
			if (exact_bytes == 16)
				g_qemuSio2FifoExact16ByteCopies++;
			else if (exact_bytes == 320)
				g_qemuSio2FifoExact320ByteCopies++;
			else if (exact_bytes == 640)
				g_qemuSio2FifoExact640ByteCopies++;
		}
#else
		(void)qwords;
		(void)groups64;
		(void)groups256;
		(void)exact_span;
		(void)exact_bytes;
#endif
	}

	static __forceinline void Sio2CountNeonFill(size_t qwords, size_t groups64, size_t groups256)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSio2FifoNeonFillQwords += static_cast<u32>(qwords);
		g_qemuSio2FifoNeonFill64ByteGroups += static_cast<u32>(groups64);
		g_qemuSio2FifoNeonFill256ByteGroups += static_cast<u32>(groups256);
#else
		(void)qwords;
		(void)groups64;
		(void)groups256;
#endif
	}

	static __forceinline void Sio2CopyBytes(u8* dst, const u8* src, size_t bytes)
	{
		u8* cdst = dst;
		const u8* csrc = src;

		// PCSX2 owner: SIO/Sio2.cpp::Sio2ByteFifo packet copies. The Vita PAD
		// path uses small poll packets and larger direct-DMA packets, so keep
		// those exact byte spans out of the grouped remainder loop.
		switch (bytes)
		{
			case 640:
				Sio2Copy640Bytes(cdst, csrc);
				Sio2CountNeonCopy(40, 10, 2, true, 640);
				return;
			case 320:
				Sio2Copy320Bytes(cdst, csrc);
				Sio2CountNeonCopy(20, 5, 1, true, 320);
				return;
			case 256:
				Sio2Copy256Bytes(cdst, csrc);
				Sio2CountNeonCopy(16, 4, 1, true, 256);
				return;
			case 64:
				Sio2Copy64Bytes(cdst, csrc);
				Sio2CountNeonCopy(4, 1, 0, true, 64);
				return;
			case 16:
				Sio2Copy16Bytes(cdst, csrc);
				Sio2CountNeonCopy(1, 0, 0, true, 16);
				return;
			default:
				break;
		}

		const size_t groups256 = bytes >> 8;
		for (size_t i = 0; i < groups256; i++)
		{
			if ((i + 1) < groups256)
				__builtin_prefetch(csrc + 256, 0, 1);

			Sio2Copy256Bytes(cdst, csrc);
			csrc += 256;
			cdst += 256;
		}

		const size_t remaining_bytes = bytes & 255;
		const size_t groups64 = remaining_bytes >> 6;
		for (size_t i = 0; i < groups64; i++)
		{
			if ((i + 1) < groups64)
				__builtin_prefetch(csrc + 64, 0, 1);

			Sio2Copy64Bytes(cdst, csrc);
			csrc += 64;
			cdst += 64;
		}

		const size_t tail_bytes = bytes & 63;
		const size_t tail_qwords = tail_bytes >> 4;
		for (size_t i = 0; i < tail_qwords; i++)
		{
			const uint8x16_t qword = vld1q_u8(csrc);
			vst1q_u8(cdst, qword);
			csrc += 16;
			cdst += 16;
		}

		if (tail_bytes & 8)
		{
			const uint8x8_t half = vld1_u8(csrc);
			vst1_u8(cdst, half);
			csrc += 8;
			cdst += 8;
		}

		for (size_t i = 0; i < (tail_bytes & 7); i++)
			cdst[i] = csrc[i];

		Sio2CountNeonCopy((groups256 << 4) + (groups64 << 2) + tail_qwords, (groups256 << 2) + groups64, groups256);
	}

	static __forceinline void Sio2FillBytes(u8* dst, u8 value, size_t bytes)
	{
		u8* cdst = dst;
		const uint8x16_t qword = vdupq_n_u8(value);

		switch (bytes)
		{
			case 640:
				Sio2Fill640Bytes(cdst, qword);
				Sio2CountNeonFill(40, 10, 2);
				return;
			case 320:
				Sio2Fill320Bytes(cdst, qword);
				Sio2CountNeonFill(20, 5, 1);
				return;
			case 256:
				Sio2Fill256Bytes(cdst, qword);
				Sio2CountNeonFill(16, 4, 1);
				return;
			case 64:
				Sio2Fill64Bytes(cdst, qword);
				Sio2CountNeonFill(4, 1, 0);
				return;
			case 16:
				vst1q_u8(cdst, qword);
				Sio2CountNeonFill(1, 0, 0);
				return;
			default:
				break;
		}

		const size_t groups256 = bytes >> 8;
		for (size_t i = 0; i < groups256; i++)
		{
			Sio2Fill256Bytes(cdst, qword);
			cdst += 256;
		}

		const size_t remaining_bytes = bytes & 255;
		const size_t groups64 = remaining_bytes >> 6;
		for (size_t i = 0; i < groups64; i++)
		{
			Sio2Fill64Bytes(cdst, qword);
			cdst += 64;
		}

		const size_t tail_bytes = bytes & 63;
		const size_t tail_qwords = tail_bytes >> 4;
		for (size_t i = 0; i < tail_qwords; i++)
		{
			vst1q_u8(cdst, qword);
			cdst += 16;
		}

		if (tail_bytes & 8)
		{
			const uint8x8_t half = vdup_n_u8(value);
			vst1_u8(cdst, half);
			cdst += 8;
		}

		for (size_t i = 0; i < (tail_bytes & 7); i++)
			cdst[i] = value;

		Sio2CountNeonFill((groups256 << 4) + (groups64 << 2) + tail_qwords, (groups256 << 2) + groups64, groups256);
	}
#else
	static __forceinline void Sio2CopyBytes(u8* dst, const u8* src, size_t bytes)
	{
		std::memcpy(dst, src, bytes);
	}

	static __forceinline void Sio2FillBytes(u8* dst, u8 value, size_t bytes)
	{
		std::memset(dst, value, bytes);
	}
#endif
}

bool Sio2ByteFifo::empty() const
{
	return m_head == m_data.size();
}

size_t Sio2ByteFifo::size() const
{
	return m_data.size() - m_head;
}

u8 Sio2ByteFifo::front() const
{
	return m_data[m_head];
}

void Sio2ByteFifo::push_back(u8 value)
{
	if (m_head != 0 && m_data.size() == m_data.capacity())
		CompactConsumed();

	m_data.push_back(value);
}

void Sio2ByteFifo::push_back(const u8* source, size_t bytes)
{
	if (bytes == 0)
		return;

	CompactConsumed();
	const size_t offset = m_data.size();
	m_data.resize(offset + bytes);
	Sio2CopyBytes(m_data.data() + offset, source, bytes);
}

void Sio2ByteFifo::push_fill(u8 value, size_t bytes)
{
	if (bytes == 0)
		return;

	CompactConsumed();
	const size_t offset = m_data.size();
	m_data.resize(offset + bytes);
	Sio2FillBytes(m_data.data() + offset, value, bytes);
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuSio2FifoBulkFillBytes += static_cast<u32>(bytes);
#endif
}

void Sio2ByteFifo::pop_front()
{
	m_head++;
	NormalizeConsumed();
}

size_t Sio2ByteFifo::pop_front(u8* destination, size_t bytes)
{
	const size_t copied = std::min(bytes, size());
	if (copied == 0)
		return 0;

	Sio2CopyBytes(destination, m_data.data() + m_head, copied);
	m_head += copied;
	NormalizeConsumed();
	return copied;
}

void Sio2ByteFifo::clear()
{
	m_data.clear();
	m_head = 0;
}

void Sio2ByteFifo::reserve(size_t capacity)
{
	if (capacity <= (m_data.capacity() - m_head))
		return;

	CompactConsumed();
	m_data.reserve(capacity);
}

bool Sio2ByteFifo::DoState(StateWrapper& sw, u32 portable_limit)
{
	if (sw.IsPortableReplay() && size() > portable_limit)
		return false;

	u32 length = static_cast<u32>(size());
	sw.Do(&length);
	if (sw.HasError() || (sw.IsPortableReplay() && length > portable_limit))
		return false;

	if (sw.IsReading())
	{
		clear();
		reserve(length);

		for (u32 i = 0; i < length; i++)
		{
			u8 value = 0;
			sw.Do(&value);
			push_back(value);
		}
	}
	else
	{
		for (u32 i = 0; i < length; i++)
		{
			u8 value = m_data[m_head + i];
			sw.Do(&value);
		}
	}
	return sw.IsGood();
}

void Sio2ByteFifo::CompactConsumed()
{
	if (m_head == 0)
		return;

	const size_t remaining = size();
	if (remaining != 0)
	{
#if defined(ARCH_ARM32)
		// Consumed-prefix compaction is always a left move. The source stays
		// ahead of the destination, so the ARM32 FIFO copy loop preserves the
		// same byte stream as memmove while avoiding libc dispatch on Cortex-A9.
		Sio2CopyBytes(m_data.data(), m_data.data() + m_head, remaining);
#else
		std::memmove(m_data.data(), m_data.data() + m_head, remaining);
#endif
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSio2FifoCompactBytes += static_cast<u32>(remaining);
#endif
	}

	m_data.resize(remaining);
	m_head = 0;
}

void Sio2ByteFifo::NormalizeConsumed()
{
	if (m_head == m_data.size())
	{
		clear();
		return;
	}

	if (m_head >= SIO2_FIFO_COMPACT_THRESHOLD && (m_head * 2) >= m_data.size())
		CompactConsumed();
}

Sio2::Sio2() = default;
Sio2::~Sio2() = default;

bool Sio2::Initialize()
{
	this->SoftReset();

	for (size_t i = 0; i < CmdQueue.size(); i++)
	{
		CmdQueue[i] = 0;
	}

	for (size_t i = 0; i < PortCtrl0.size(); i++)
	{
		PortCtrl0[i] = 0;
		PortCtrl1[i] = 0;
	}

	dataIn = 0;
	dataOut = 0;
	SetCtrl(Sio2Ctrl::SIO2MAN_RESET);
	SetCmdStat(CmdStat::DISCONNECTED);
	PortStat = PortStat::DEFAULT;
	FifoStat = FifoStat::DEFAULT;
	FifoTxPos = 0;
	FifoRxPos = 0;
	iStat = 0;

	port = 0;

	g_Sio2FifoOut.clear();

	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			mcds[i][j].term = 0x55;
			mcds[i][j].port = i;
			mcds[i][j].slot = j;
			mcds[i][j].FLAG = 0x08;
			mcds[i][j].autoEjectTicks = 0;
		}
	}

	mcd = &mcds[0][0];
	return true;
}

bool Sio2::Shutdown()
{
	return true;
}

void Sio2::SoftReset()
{
	queueRead = false;
	queuePosition = 0;
	commandLength = 0;
	processedLength = 0;
	// Clear dmaBlockSize, in case the next SIO2 command is not sent over DMA11.
	dmaBlockSize = 0;
	queueComplete = false;

	// Anything in g_Sio2FifoIn which was not necessary to consume should be cleared out prior to the next SIO2 cycle.
	g_Sio2FifoIn.clear();

	// cmd_stat should always be reassembled based on the devices being probed by the packet.
	CmdStat = 0;
}

void Sio2::Interrupt()
{
	if (!iStat)
		iopIntcIrq(17);
	else
		DevCon.Warning("Nearly sent double SIO2 IRQ");

	iStat |= 1;
}

void Sio2::SetCtrl(u32 value)
{
	this->ctrl = value;

	if (this->ctrl & Sio2Ctrl::START_TRANSFER)
	{
		Interrupt();
	}
}

void Sio2::SetCmd(size_t position, u32 value)
{
	this->CmdQueue[position] = value;

	if (position == 0)
	{
		SoftReset();
	}
}

void Sio2::SetCmdStat(u32 value)
{
	this->CmdStat = value;
}

void Sio2::Pad()
{
	MultitapProtocol& mtap = g_MultitapArr.at(port);
	PadBase* pad = Pad::GetPad(port, mtap.GetPadSlot());

	// Update the third nibble with which ports have been accessed
	if (this->CmdStat & CmdStat::ONE_PORT_OPEN)
	{
		this->CmdStat &= ~(CmdStat::ONE_PORT_OPEN);
		this->CmdStat |= CmdStat::TWO_PORTS_OPEN;
	}
	else
	{
		this->CmdStat |= CmdStat::ONE_PORT_OPEN;
	}

	// This bit is always set, whether the pad is present or missing
	this->CmdStat |= CmdStat::NO_DEVICES_MISSING;

	// If the currently accessed pad is missing, also tick those bits
	if (pad->GetType() == Pad::ControllerType::NotConnected || pad->ejectTicks)
	{
		if (!port)
		{
			this->CmdStat |= CmdStat::PORT_1_MISSING;
		}
		else
		{
			this->CmdStat |= CmdStat::PORT_2_MISSING;
		}
	}

	g_Sio2FifoOut.push_back(0xff);
	pad->SoftReset();

	if (pad->ejectTicks)
	{
		const size_t bytes = g_Sio2FifoIn.size();
		g_Sio2FifoIn.clear();
		g_Sio2FifoOut.push_fill(0xff, bytes);
	}
	// Then for every byte in g_Sio2FifoIn, pass to PAD and see what it kicks back to us.
	else
	{
		while (!g_Sio2FifoIn.empty())
		{
			const u8 commandByte = g_Sio2FifoIn.front();
			g_Sio2FifoIn.pop_front();
			const u8 responseByte = pad->SendCommandByte(commandByte);
			g_Sio2FifoOut.push_back(responseByte);
		}
	}

	// If the pad is "ejected", then decrement one tick.
	// This needs to happen AFTER anything else which might
	// consider if the pad is "ejected"!
	if (pad->ejectTicks)
	{
		pad->ejectTicks -= 1;
	}
}

void Sio2::Multitap()
{
	const bool multitapEnabled = EmuConfig.Pad.IsMultitapPortEnabled(this->port);
	
	// Update the third nibble with which ports have been accessed
	if (this->CmdStat & CmdStat::ONE_PORT_OPEN)
	{
		this->CmdStat &= ~(CmdStat::ONE_PORT_OPEN);
		this->CmdStat |= CmdStat::TWO_PORTS_OPEN;
	}
	else
	{
		this->CmdStat |= CmdStat::ONE_PORT_OPEN;
	}

	// This bit is always set, whether the pad is present or missing
	this->CmdStat |= CmdStat::NO_DEVICES_MISSING;

	// If the currently accessed multitap is missing, also tick those bits.
	// MTAPMAN is special though.
	// 
	// For PADMAN and pads, the bits represented by PORT_1_MISSING and PORT_2_MISSING
	// are always faithful - suppose your game only opened port 2 for some reason,
	// then a disconnect value would look like 0x0002D100.
	//
	// MTAPMAN however does not check the bit set by 0x00020000. It only checks the bit
	// set by 0x00010000. So even if port 2 is being addressed, cmd stat should be 0x0001D100
	// (or 0x0001D200 if there are both ports being accessed in that packet).
	if (!multitapEnabled)
	{
		this->CmdStat |= CmdStat::PORT_1_MISSING;
	}

	g_MultitapArr.at(this->port).SendToMultitap();
}

void Sio2::Infrared()
{
	SetCmdStat(CmdStat::DISCONNECTED);

	g_Sio2FifoIn.pop_front();

	if (g_Sio2FifoOut.size() < commandLength)
		g_Sio2FifoOut.push_fill(0xff, commandLength - g_Sio2FifoOut.size());
}

void Sio2::Memcard()
{
	MultitapProtocol& mtap = g_MultitapArr.at(this->port);

	mcd = &mcds[port][mtap.GetMemcardSlot()];

	// Check if auto ejection is active. If so, set cmd stat to DISCONNECTED,
	// and zero out the fifo to simulate dead air over the wire.
	if (mcd->autoEjectTicks)
	{
		SetCmdStat(CmdStat::DISCONNECTED);
		g_Sio2FifoOut.push_back(0xff); // Because Sio2::Write pops the first g_Sio2FifoIn member

		const size_t bytes = g_Sio2FifoIn.size();
		g_Sio2FifoIn.clear();
		g_Sio2FifoOut.push_fill(0xff, bytes);

		return;
	}

	SetCmdStat(mcd->IsPresent() ? CmdStat::CONNECTED : CmdStat::DISCONNECTED);

	const u8 commandByte = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 responseByte = mcd->IsPresent() ? 0x00 : 0xff;
	g_Sio2FifoOut.push_back(responseByte);
	g_Sio2FifoOut.push_back(responseByte);
	u8 ps1Input = 0;
	u8 ps1Output = 0;

	switch (commandByte)
	{
		case MemcardCommand::PROBE:
			g_MemoryCardProtocol.Probe();
			break;
		case MemcardCommand::UNKNOWN_WRITE_DELETE_END:
			g_MemoryCardProtocol.UnknownWriteDeleteEnd();
			break;
		case MemcardCommand::SET_ERASE_SECTOR:
		case MemcardCommand::SET_WRITE_SECTOR:
		case MemcardCommand::SET_READ_SECTOR:
			g_MemoryCardProtocol.SetSector();
			break;
		case MemcardCommand::GET_SPECS:
			g_MemoryCardProtocol.GetSpecs();
			break;
		case MemcardCommand::SET_TERMINATOR:
			g_MemoryCardProtocol.SetTerminator();
			break;
		case MemcardCommand::GET_TERMINATOR:
			g_MemoryCardProtocol.GetTerminator();
			break;
		case MemcardCommand::WRITE_DATA:
			g_MemoryCardProtocol.WriteData();
			break;
		case MemcardCommand::READ_DATA:
			g_MemoryCardProtocol.ReadData();
			break;
		case MemcardCommand::PS1_READ:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Read(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_STATE:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1State(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_WRITE:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Write(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_POCKETSTATION:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Pocketstation(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::READ_WRITE_END:
			g_MemoryCardProtocol.ReadWriteEnd();
			break;
		case MemcardCommand::ERASE_BLOCK:
			g_MemoryCardProtocol.EraseBlock();
			break;
		case MemcardCommand::UNKNOWN_BOOT:
			g_MemoryCardProtocol.UnknownBoot();
			break;
		case MemcardCommand::AUTH_XOR:
			g_MemoryCardProtocol.AuthXor();
			break;
		case MemcardCommand::AUTH_F3:
			g_MemoryCardProtocol.AuthF3();
			break;
		case MemcardCommand::AUTH_F7:
			g_MemoryCardProtocol.AuthF7();
			break;
		default:
			Console.Warning("%s() Unhandled memcard command %02X, things are about to break!", __FUNCTION__, commandByte);
			break;
	}
}

void Sio2::Write(u8 data)
{
	Sio2Log.WriteLn("%s(%02X) SIO2 DATA Write", __FUNCTION__, data);

	if (!queueRead)
	{
		// No more queue positions to access, but the game is still sending us SIO2 writes. Lets ignore them.
		if (queuePosition >= CmdQueue.size())
		{
			Console.Warning("%s(%02X) Received data after exhausting all queue entries!", __FUNCTION__, data);
			return;
		}

		const u32 currentCmd = CmdQueue[queuePosition];
		port = currentCmd & Sio2Cmd::PORT;
		commandLength = (currentCmd >> 8) & Sio2Cmd::COMMAND_LENGTH_MASK;
		queueRead = true;

		// The freshly read cmd position had a length of 0, so we are done handling SIO2 commands until
		// the next cmd writes.
		if (commandLength == 0)
		{
			queueComplete = true;
		}

		// If the prior command did not need to fully pop g_Sio2FifoIn, do so now,
		// so that the next command isn't trying to read the last command's leftovers.
		g_Sio2FifoIn.clear();
	}

	if (queueComplete)
	{
		return;
	}

	g_Sio2FifoIn.push_back(data);

	// We have received as many command bytes as we expect, and...
	//
	// ... These were from direct writes into IOP memory (DMA block size is zero when direct writes occur)
	// ... These were from SIO2 DMA (DMA block size is non-zero when SIO2 DMA occurs)
	if ((g_Sio2FifoIn.size() == g_Sio2.commandLength && g_Sio2.dmaBlockSize == 0) || g_Sio2FifoIn.size() == g_Sio2.dmaBlockSize)
	{
		ProcessQueuedCommand(data);
	}
}

void Sio2::WriteBytes(const u8* source, size_t bytes)
{
#if SIO2LOG_ENABLE
	for (size_t i = 0; i < bytes; i++)
		Write(source[i]);
#else
	if (bytes == 0)
		return;

	if (queueRead || queueComplete || dmaBlockSize == 0 || bytes != dmaBlockSize)
	{
		g_Sio2FifoIn.reserve(g_Sio2FifoIn.size() + bytes);
		for (size_t i = 0; i < bytes; i++)
			Write(source[i]);
		return;
	}

	// No more queue positions to access, but the game is still sending us SIO2 writes. Lets ignore them.
	if (queuePosition >= CmdQueue.size())
	{
		Console.Warning("%s(%02X) Received data after exhausting all queue entries!", __FUNCTION__, source[0]);
		return;
	}

	const u32 currentCmd = CmdQueue[queuePosition];
	port = currentCmd & Sio2Cmd::PORT;
	commandLength = (currentCmd >> 8) & Sio2Cmd::COMMAND_LENGTH_MASK;
	queueRead = true;

	// The freshly read cmd position had a length of 0, so we are done handling SIO2 commands until
	// the next cmd writes.
	if (commandLength == 0)
	{
		queueComplete = true;
		g_Sio2FifoIn.clear();
		return;
	}

	// If the prior command did not need to fully pop g_Sio2FifoIn, do so now,
	// so that the next command isn't trying to read the last command's leftovers.
	g_Sio2FifoIn.clear();
	g_Sio2FifoIn.push_back(source, bytes);
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuSio2FifoBulkWriteBytes += static_cast<u32>(bytes);
#endif
	ProcessQueuedCommand(source[bytes - 1]);
#endif
}

u8 Sio2::Read()
{
	u8 ret = 0xff;

	if (!g_Sio2FifoOut.empty())
	{
		ret = g_Sio2FifoOut.front();
		g_Sio2FifoOut.pop_front();
	}
	else
	{
		Console.Warning("%s() g_Sio2FifoOut underflow! Returning 0xff.", __FUNCTION__);
	}

	Sio2Log.WriteLn("%s() SIO2 DATA Read (%02X)", __FUNCTION__, ret);
	return ret;
}

void Sio2::ReadBytes(u8* destination, size_t bytes)
{
#if SIO2LOG_ENABLE
	for (size_t i = 0; i < bytes; i++)
		destination[i] = Read();
#else
	const size_t copied = g_Sio2FifoOut.pop_front(destination, bytes);
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuSio2FifoBulkReadBytes += static_cast<u32>(copied);
#endif

	for (size_t i = copied; i < bytes; i++)
		destination[i] = Read();
#endif
}

void Sio2::ProcessQueuedCommand(u8 log_data)
{
	// Go ahead and prep so the next write triggers a load of the new cmd value.
	g_Sio2.queueRead = false;
	g_Sio2.queuePosition++;

	// Check the SIO mode
	const u8 sioMode = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();

	switch (sioMode)
	{
		case SioMode::PAD:
			this->Pad();
			break;
		case SioMode::MULTITAP:
			this->Multitap();
			break;
		case SioMode::INFRARED:
			this->Infrared();
			break;
		case SioMode::MEMCARD:
			this->Memcard();
			break;
		default:
			Console.Error("%s(%02X) Unhandled SIO mode %02X", __FUNCTION__, log_data, sioMode);
			g_Sio2FifoOut.push_back(0xff);
			SetCmdStat(CmdStat::DISCONNECTED);
			break;
	}

	// If command was sent over SIO2 DMA, align g_Sio2FifoOut to the block size
	if (g_Sio2.dmaBlockSize > 0)
	{
		const size_t dmaDiff = g_Sio2FifoOut.size() % g_Sio2.dmaBlockSize;

		if (dmaDiff > 0)
		{
			const size_t padding = g_Sio2.dmaBlockSize - dmaDiff;
			g_Sio2FifoOut.push_fill(0x00, padding);
		}
	}
}

bool Sio2::DoState(StateWrapper& sw)
{
	if (!sw.DoMarker("Sio2"))
		return false;

	sw.Do(&CmdQueue);
	sw.Do(&PortCtrl0);
	sw.Do(&PortCtrl1);
	sw.Do(&dataIn);
	sw.Do(&dataOut);
	sw.Do(&ctrl);
	sw.Do(&CmdStat);
	sw.Do(&PortStat);
	sw.Do(&FifoStat);
	sw.Do(&FifoTxPos);
	sw.Do(&FifoRxPos);
	sw.Do(&iStat);
	sw.Do(&port);
	sw.Do(&queueRead);
	if (sw.IsPortableReplay())
	{
		auto do_portable_size = [&](size_t& value, u32 maximum) {
			if (sw.IsWriting() && value > maximum)
				return false;
			u32 portable_value = static_cast<u32>(value);
			sw.Do(&portable_value);
			if (sw.HasError() || portable_value > maximum)
				return false;
			if (sw.IsReading())
				value = portable_value;
			return true;
		};
		if (!do_portable_size(queuePosition, static_cast<u32>(CmdQueue.size())) ||
			!do_portable_size(commandLength, Sio2Cmd::COMMAND_LENGTH_MASK) ||
			!do_portable_size(processedLength, Sio2Cmd::COMMAND_LENGTH_MASK) ||
			!do_portable_size(dmaBlockSize, 0xffffu * 4u))
		{
			Console.Error("Portable replay SIO2 transfer state is invalid.");
			return false;
		}
	}
	else
	{
		sw.Do(&queuePosition);
		sw.Do(&commandLength);
		sw.Do(&processedLength);
		sw.Do(&dmaBlockSize);
	}
	sw.Do(&queueComplete);
	if (sw.IsPortableReplay() &&
		(sw.HasError() || port >= SIO::PORTS ||
		 (!queueComplete && queuePosition >= CmdQueue.size())))
	{
		Console.Error("Portable replay SIO2 queue provenance is invalid.");
		return false;
	}

	if (!g_Sio2FifoIn.DoState(sw, PORTABLE_SIO2_INPUT_FIFO_LIMIT) ||
		!g_Sio2FifoOut.DoState(sw, PORTABLE_SIO2_OUTPUT_FIFO_LIMIT))
	{
		Console.Error("Portable replay SIO2 FIFO state is invalid.");
		return false;
	}

	// CRCs for memory cards.
	// If the memory card hasn't changed when loading state, we can safely skip ejecting it.
	u64 mcdCrcs[SIO::PORTS][SIO::SLOTS];
	if (sw.IsWriting())
	{
		for (u32 port = 0; port < SIO::PORTS; port++)
		{
			for (u32 slot = 0; slot < SIO::SLOTS; slot++)
				mcdCrcs[port][slot] = mcds[port][slot].GetChecksum();
		}
	}
	sw.DoBytes(mcdCrcs, sizeof(mcdCrcs));

	if (sw.IsReading())
	{
		bool ejected = false;
		for (u32 port = 0; port < SIO::PORTS && !ejected; port++)
		{
			for (u32 slot = 0; slot < SIO::SLOTS; slot++)
			{
				if (mcdCrcs[port][slot] != mcds[port][slot].GetChecksum())
				{
					if (sw.IsPortableReplay())
					{
						Console.Error("Portable replay memory-card backing data does not match slot %u:%u.",
							port, slot);
						return false;
					}
					AutoEject::SetAll();
					ejected = true;
					break;
				}
			}
		}
	}
	if (sw.IsPortableReplay() && !sioDoPortableMemoryCardState(sw))
		return false;

	sw.Do(&sioLastFrameMcdBusy);
	return sw.IsGood();
}
