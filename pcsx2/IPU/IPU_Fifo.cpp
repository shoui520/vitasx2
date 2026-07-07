// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "DebugTools/IpuTrace.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "IPU/IPU_MultiISA.h"

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

alignas(16) IPU_Fifo ipu_fifo;

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuIpuFifoInputContiguousWrites = 0;
u32 g_qemuIpuFifoInputWrappedWrites = 0;
u32 g_qemuIpuFifoInputReads = 0;
u32 g_qemuIpuFifoOutputContiguousWrites = 0;
u32 g_qemuIpuFifoOutputWrappedWrites = 0;
u32 g_qemuIpuFifoOutputContiguousReads = 0;
u32 g_qemuIpuFifoOutputWrappedReads = 0;
u32 g_qemuIpuFifoNeonQwords = 0;
u32 g_qemuIpuFifoNeon64ByteGroups = 0;
u32 g_qemuIpuFifoFixedQwordCopies = 0;
#endif

#if defined(ARCH_ARM32)
static __forceinline void IpuFifoCopyQword(u32* to, const u32* from)
{
	const uint32x4_t qword = vld1q_u32(from);
	vst1q_u32(to, qword);
}

static __forceinline void IpuFifoCopyFourQwords(u32* to, const u32* from)
{
	const uint32x4_t qword0 = vld1q_u32(from);
	const uint32x4_t qword1 = vld1q_u32(from + 4);
	const uint32x4_t qword2 = vld1q_u32(from + 8);
	const uint32x4_t qword3 = vld1q_u32(from + 12);
	vst1q_u32(to, qword0);
	vst1q_u32(to + 4, qword1);
	vst1q_u32(to + 8, qword2);
	vst1q_u32(to + 12, qword3);
}

static __forceinline void IpuFifoCountNeonCopy(int qwords, int groups64)
{
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuIpuFifoNeonQwords += static_cast<u32>(qwords);
	g_qemuIpuFifoNeon64ByteGroups += static_cast<u32>(groups64);
#endif
}
#endif

static __forceinline void IpuFifoCopyWords(u32* to, const u32* from, int words)
{
#if defined(ARCH_ARM32)
	const int fixed_qwords = words >> 2;
	if ((words & 3) == 0 && fixed_qwords > 0 && fixed_qwords <= 8)
	{
		u32* fixed_to = to;
		const u32* fixed_from = from;

		if (fixed_qwords >= 4)
		{
			if (fixed_qwords == 8)
				__builtin_prefetch(fixed_from + 16, 0, 1);

			IpuFifoCopyFourQwords(fixed_to, fixed_from);
			fixed_to += 16;
			fixed_from += 16;
			if (fixed_qwords == 8)
				IpuFifoCopyFourQwords(fixed_to, fixed_from);
		}

		switch (fixed_qwords & 3)
		{
			case 3:
				IpuFifoCopyQword(fixed_to + 8, fixed_from + 8);
				[[fallthrough]];
			case 2:
				IpuFifoCopyQword(fixed_to + 4, fixed_from + 4);
				[[fallthrough]];
			case 1:
				IpuFifoCopyQword(fixed_to, fixed_from);
				break;
			default:
				break;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIpuFifoFixedQwordCopies;
#endif
		IpuFifoCountNeonCopy(fixed_qwords, fixed_qwords >> 2);
		return;
	}

	const int groups64 = words >> 4;
	for (int i = 0; i < groups64; i++)
	{
		if ((i + 1) < groups64)
			__builtin_prefetch(from + 16, 0, 1);

		IpuFifoCopyFourQwords(to, from);
		from += 16;
		to += 16;
	}

	const int tail_words = words & 15;
	const int tail_qwords = tail_words >> 2;
	for (int i = 0; i < tail_qwords; i++)
	{
		IpuFifoCopyQword(to, from);
		from += 4;
		to += 4;
	}

	switch (tail_words & 3)
	{
		case 3:
			to[2] = from[2];
			[[fallthrough]];
		case 2:
			to[1] = from[1];
			[[fallthrough]];
		case 1:
			to[0] = from[0];
			break;
		default:
			break;
	}
	IpuFifoCountNeonCopy((groups64 << 2) + tail_qwords, groups64);
	return;
#endif
	memcpy(to, from, words << 2);
}

void IPU_Fifo::init()
{
	out.readpos = 0;
	out.writepos = 0;
	in.readpos = 0;
	in.writepos = 0;
	std::memset(in.data, 0, sizeof(in.data));
	std::memset(out.data, 0, sizeof(out.data));
}

void IPU_Fifo_Input::clear()
{
	std::memset(data, 0, sizeof(data));
	g_BP.IFC = 0;
	ipuRegs.ctrl.IFC = 0;
	readpos = 0;
	writepos = 0;

	// Because the FIFO is drained it will request more data immediately
	IPUCoreStatus.DataRequested = true;

	if (ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
	{
		CPU_INT(DMAC_TO_IPU, 4);
	}
}

void IPU_Fifo_Output::clear()
{
	std::memset(data, 0, sizeof(data));
	ipuRegs.ctrl.OFC = 0;
	readpos = 0;
	writepos = 0;
}

void IPU_Fifo::clear()
{
	in.clear();
	out.clear();
}

std::string IPU_Fifo_Input::desc() const
{
	return StringUtil::StdStringFromFormat("IPU Fifo Input: readpos = 0x%x, writepos = 0x%x, data = %p", readpos, writepos, data);
}

std::string IPU_Fifo_Output::desc() const
{
	return StringUtil::StdStringFromFormat("IPU Fifo Output: readpos = 0x%x, writepos = 0x%x, data = %p", readpos, writepos, data);
}

int IPU_Fifo_Input::write(const u32* pMem, int size)
{
	const int transfer_size = std::min(size, 8 - (int)g_BP.IFC);
	if (!transfer_size) return 0;

	const int words = transfer_size << 2;
	const int contiguous_words = 32 - writepos;
	if (words <= contiguous_words)
	{
		IpuFifoCopyWords(&data[writepos], pMem, words);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIpuFifoInputContiguousWrites;
#endif
	}
	else
	{
		const int first_words = contiguous_words;
		const int second_words = words - first_words;

		IpuFifoCopyWords(&data[writepos], pMem, first_words);
		pMem += first_words;
		IpuFifoCopyWords(&data[0], pMem, second_words);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIpuFifoInputWrappedWrites;
#endif
	}

	writepos = (writepos + words) & 31;

	g_BP.IFC += transfer_size;

	if (g_BP.IFC == 8)
		IPUCoreStatus.DataRequested = false;

	return transfer_size;
}

int IPU_Fifo_Input::read(void *value)
{
	// wait until enough data to ensure proper streaming.
	if (g_BP.IFC <= 1)
	{
		// IPU FIFO is empty and DMA is waiting so lets tell the DMA we are ready to put data in the FIFO
		IPUCoreStatus.DataRequested = true;

		if(ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
		{
			CPU_INT( DMAC_TO_IPU, std::min(8U, ipu1ch.qwc));
		}

		if (g_BP.IFC == 0) return 0;
		pxAssert(g_BP.IFC > 0);
	}

	IpuFifoCopyWords(static_cast<u32*>(value), &data[readpos], 4);

#if defined(VITASX2_QEMU_VALIDATION)
	++g_qemuIpuFifoInputReads;
#endif

	readpos = (readpos + 4) & 31;
	g_BP.IFC--;
	return 1;
}

int IPU_Fifo_Output::write(const u32 *value, uint size)
{
	pxAssertMsg(size>0, "Invalid size==0 when calling IPU_Fifo_Output::write");

	const int transfer_size = std::min(size, 8 - (uint)ipuRegs.ctrl.OFC);
	if(!transfer_size) return 0;

	Pcsx2Trace::RecordIpuOutputWrite(ipu_cmd.current, value,
		static_cast<u32>(transfer_size << 4));

	const int words = transfer_size << 2;
	const int contiguous_words = 32 - writepos;
	if (words <= contiguous_words)
	{
		IpuFifoCopyWords(&data[writepos], value, words);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIpuFifoOutputContiguousWrites;
#endif
	}
	else
	{
		const int first_words = contiguous_words;
		const int second_words = words - first_words;

		IpuFifoCopyWords(&data[writepos], value, first_words);
		value += first_words;
		IpuFifoCopyWords(&data[0], value, second_words);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIpuFifoOutputWrappedWrites;
#endif
	}

	writepos = (writepos + words) & 31;

	ipuRegs.ctrl.OFC += transfer_size;

	if(ipu0ch.chcr.STR)
		IPU_INT_FROM(1);

	return transfer_size;
}

void IPU_Fifo_Output::read(void *value, uint size)
{
	pxAssert(ipuRegs.ctrl.OFC >= size);
	ipuRegs.ctrl.OFC -= size;

	// Zeroing the read data is not needed, since the ringbuffer design will never read back
	// the zero'd data anyway. --air

	const int words = static_cast<int>(size << 2);
	const int contiguous_words = 32 - readpos;
	if (words <= contiguous_words)
	{
		IpuFifoCopyWords(static_cast<u32*>(value), &data[readpos], words);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIpuFifoOutputContiguousReads;
#endif
	}
	else
	{
		const int first_words = contiguous_words;
		const int second_words = words - first_words;

		IpuFifoCopyWords(static_cast<u32*>(value), &data[readpos], first_words);
		value = static_cast<u32*>(value) + first_words;
		IpuFifoCopyWords(static_cast<u32*>(value), &data[0], second_words);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIpuFifoOutputWrappedReads;
#endif
	}

	readpos = (readpos + words) & 31;
}

void ReadFIFO_IPUout(mem128_t* out)
{
	pxAssertMsg(ipuRegs.ctrl.OFC > 0, "Attempted read from IPUout's FIFO, but the FIFO is empty!");
	if (ipuRegs.ctrl.OFC == 0) [[unlikely]]
		return;
	ipu_fifo.out.read(out, 1);

	// Games should always check the fifo before reading from it -- so if the FIFO has no data
	// its either some glitchy game or a bug in pcsx2.
}

void WriteFIFO_IPUin(const mem128_t* value)
{
	IPU_LOG( "WriteFIFO/IPUin <- 0x%08X.%08X.%08X.%08X", value->_u32[0], value->_u32[1], value->_u32[2], value->_u32[3]);

	//committing every 16 bytes
	if( ipu_fifo.in.write(value->_u32, 1) > 0 )
	{
		if (ipuRegs.ctrl.BUSY /*&& IPUCoreStatus.WaitingOnIPUTo*/)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(2 * BIAS);
		}
	}
}
