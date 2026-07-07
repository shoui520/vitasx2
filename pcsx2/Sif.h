// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

static const int FIFO_SIF_W = 128;

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuSifFifoContiguousWrites;
extern u32 g_qemuSifFifoContiguousReads;
extern u32 g_qemuSifFifoWrappedWrites;
extern u32 g_qemuSifFifoWrappedReads;
extern u32 g_qemuSifFifoJunkWrites;
extern u32 g_qemuSifFifoJunkScalarWords;
extern u32 g_qemuSifFifoNeonQwords;
extern u32 g_qemuSifFifoNeon64ByteGroups;
extern u32 g_qemuSifFifoNeon256ByteGroups;
extern u32 g_qemuSifFifoNeon512ByteGroups;
#endif

#if defined(ARCH_ARM32)
static __forceinline void SifFifoCopy16Words(u32* to, const u32* from)
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

static __forceinline void SifFifoCopy64Words(u32* to, const u32* from)
{
	SifFifoCopy16Words(to, from);
	SifFifoCopy16Words(to + 16, from + 16);
	SifFifoCopy16Words(to + 32, from + 32);
	SifFifoCopy16Words(to + 48, from + 48);
}

static __forceinline void SifFifoCopy128Words(u32* to, const u32* from)
{
	SifFifoCopy64Words(to, from);
	SifFifoCopy64Words(to + 64, from + 64);
}
#endif

static __forceinline void SifFifoCopyWords(u32* to, const u32* from, int words)
{
#if defined(ARCH_ARM32)
	const int groups512 = words >> 7;
	for (int i = 0; i < groups512; i++)
	{
		if ((i + 1) < groups512)
			__builtin_prefetch(from + 128, 0, 1);

		SifFifoCopy128Words(to, from);
		from += 128;
		to += 128;
	}

	const int remaining_after_512 = words & 127;
	const int groups256 = remaining_after_512 >> 6;
	for (int i = 0; i < groups256; i++)
	{
		if ((i + 1) < groups256)
			__builtin_prefetch(from + 64, 0, 1);

		SifFifoCopy64Words(to, from);
		from += 64;
		to += 64;
	}

	const int remaining_after_256 = remaining_after_512 & 63;
	const int groups64 = remaining_after_256 >> 4;
	for (int i = 0; i < groups64; i++)
	{
		if ((i + 1) < groups64)
			__builtin_prefetch(from + 16, 0, 1);

		SifFifoCopy16Words(to, from);
		from += 16;
		to += 16;
	}

	const int tail_words = remaining_after_256 & 15;
	const int qwords = tail_words >> 2;
	for (int i = 0; i < qwords; i++)
	{
		const uint32x4_t qword = vld1q_u32(from);
		vst1q_u32(to, qword);
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
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuSifFifoNeonQwords += (groups512 << 5) + (groups256 << 4) + (groups64 << 2) + qwords;
	g_qemuSifFifoNeon64ByteGroups += (groups512 << 3) + (groups256 << 2) + groups64;
	g_qemuSifFifoNeon256ByteGroups += (groups512 << 1) + groups256;
	g_qemuSifFifoNeon512ByteGroups += groups512;
#endif
	return;
#endif
	memcpy(to, from, words << 2);
}

// Despite its name, this is actually the IOP's DMAtag, which itself also contains
// the EE's DMAtag in its upper 64 bits.  Note that only the lower 24 bits of 'data' is
// the IOP's chain transfer address (loaded into MADR).  Bits 30 and 31 are transfer stop
// bits of some sort.
struct sifData
{
	s32 data;
	s32 words;

	tDMA_TAG	tag_lo;		// EE DMA tag
	tDMA_TAG	tag_hi;		// EE DMA tag
};

struct sifFifo
{
	u32 data[FIFO_SIF_W];
	u32 junk[4];
	s32 readPos;
	s32 writePos;
	s32 size;

	s32 sif_free()
	{
		return FIFO_SIF_W - size;
	}

	void write(u32 *from, int words)
	{
		if (words > 0)
		{
			if ((FIFO_SIF_W - size) < words)
				DevCon.Warning("Not enough space in SIF0 FIFO!\n");

			const int contiguous = FIFO_SIF_W - writePos;
			if (words <= contiguous)
			{
				SifFifoCopyWords(&data[writePos], from, words);
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuSifFifoContiguousWrites;
#endif
			}
			else
			{
				const int wP0 = contiguous;
				const int wP1 = words - wP0;

				SifFifoCopyWords(&data[writePos], from, wP0);
				SifFifoCopyWords(&data[0], &from[wP0], wP1);
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuSifFifoWrappedWrites;
#endif
			}

			writePos = (writePos + words) & (FIFO_SIF_W - 1);
			size += words;
		}
		SIF_LOG("  SIF + %d = %d (pos=%d)", words, size, writePos);
	}

	// Junk data writing
	// 
	// If there is not enough data produced from the IOP, it will always use the previous full quad word to
	// fill in the missing data.
	// One thing to note, when the IOP transfers the EE tag, it transfers a whole QW of data, which will include
	// the EE Tag and the next IOP tag, since the EE reads 1QW of data for DMA tags.
	//
	// So the data used will be as follows:
	// Less than 1QW = Junk data is made up of the EE tag + address (64 bits) and the following IOP tag (64 bits).
	// More than 1QW = Junk data is made up of the last complete QW of data that was transferred in this packet.
	//
	// Data is always offset in to the junk by the amount the IOP actually transferred, so if it sent 2 words
	// it will read words 3 and 4 out of the junk to fill the space.
	//
	// PS2 test results:
	//
	// Example of less than 1QW being sent with the only data being set being 0x69
	//
	//	addr 0x1500a0 value 0x69        <-- actual data (junk behind this would be the EE tag)
	//	addr 0x1500a4 value 0x1500a0    <-- EE address
	//	addr 0x1500a8 value 0x8001a170  <-- following IOP tag
	//	addr 0x1500ac value 0x10        <-- following IOP tag word count
	//
	// Example of more than 1QW being sent with the data going from 0x20 to 0x25
	//
	//	addr 0x150080 value 0x21 <-- start of previously completed QW
	//	addr 0x150084 value 0x22
	//	addr 0x150088 value 0x23
	//	addr 0x15008c value 0x24 <-- end of previously completed QW
	//	addr 0x150090 value 0x25 <-- end of recorded data
	//	addr 0x150094 value 0x22 <-- from position 2 of the previously completed quadword
	//	addr 0x150098 value 0x23 <-- from position 3 of the previously completed quadword
	//	addr 0x15009c value 0x24 <-- from position 4 of the previously completed quadword

	void writeJunk(int words)
	{
		if (words > 0)
		{
			// Get the start position of the previously completed whole QW.
			// Position is in word (32bit) units.
			const int transferredWords = 4 - words;
			const int prevQWPos = (writePos - (4 + transferredWords)) & (FIFO_SIF_W - 1);

			// PCSX2 owner: Sif.h::writeJunk(). Missing SIF0 packet words are
			// always 1-3 scalar words, so avoid the NEON copy setup while
			// preserving the original circular FIFO/junk offsets exactly.
			for (int i = 0; i < 4; i++)
				junk[i] = data[(prevQWPos + i) & (FIFO_SIF_W - 1)];

			const int wP0 = std::min((FIFO_SIF_W - writePos), words);
			const int wP1 = words - wP0;
			for (int i = 0; i < wP0; i++)
				data[writePos + i] = junk[4 - wP0 + i];
			for (int i = 0; i < wP1; i++)
				data[i] = junk[wP0 + i];

			writePos = (writePos + words) & (FIFO_SIF_W - 1);
			size += words;
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuSifFifoJunkWrites;
			g_qemuSifFifoJunkScalarWords += 4 + words;
#endif

			SIF_LOG("  SIF + %d = %d Junk (pos=%d)", words, size, writePos);
		}
	}

	void read(u32 *to, int words)
	{
		if (words > 0)
		{
			const int contiguous = FIFO_SIF_W - readPos;
			if (words <= contiguous)
			{
				SifFifoCopyWords(to, &data[readPos], words);
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuSifFifoContiguousReads;
#endif
			}
			else
			{
				const int wP0 = contiguous;
				const int wP1 = words - wP0;

				SifFifoCopyWords(to, &data[readPos], wP0);
				SifFifoCopyWords(&to[wP0], &data[0], wP1);
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuSifFifoWrappedReads;
#endif
			}

			readPos = (readPos + words) & (FIFO_SIF_W - 1);
			size -= words;
		}
		SIF_LOG("  SIF - %d = %d (pos=%d)", words, size, readPos);
	}
	void clear()
	{
		std::memset(data, 0, sizeof(data));
		readPos = 0;
		writePos = 0;
		size = 0;
	}
};

struct old_sif_structure
{
	sifFifo fifo; // Used in both.
	s32 chain; // Not used.
	s32 end; // Only used for EE.
	s32 tagMode; // No longer used.
	s32 counter; // Used to keep track of how much is left in IOP.
	struct sifData data; // Only used in IOP.
};

struct sif_ee
{
	bool end; // Only used for EE.
	bool busy;

	s32 cycles;
};

struct sif_iop
{
	bool end;
	bool busy;

	s32 cycles;
	s32 writeJunk;

	s32 counter; // Used to keep track of how much is left in IOP.
	struct sifData data; // Only used in IOP.
};

struct _sif
{
	sifFifo fifo; // Used in both.
	sif_ee ee;
	sif_iop iop;
};

extern _sif sif0, sif1, sif2;

extern void sifReset();

extern void SIF0Dma();
extern void SIF1Dma();
extern void SIF2Dma();

extern void dmaSIF0();
extern void dmaSIF1();
extern void dmaSIF2();

extern void EEsif0Interrupt();
extern void EEsif1Interrupt();
extern void EEsif2Interrupt();

extern void sif0Interrupt();
extern void sif1Interrupt();
extern void sif2Interrupt();

extern bool ReadFifoSingleWord();
extern bool WriteFifoSingleWord();

#define sif0data sif0.iop.data.data
#define sif1data sif1.iop.data.data
#define sif2data sif2.iop.data.data

#define sif0words sif0.iop.data.words
#define sif1words sif1.iop.data.words
#define sif2words sif2.iop.data.words

#define sif0tag DMA_TAG(sif0data)
#define sif1tag DMA_TAG(sif1data)
#define sif2tag DMA_TAG(sif2data)
