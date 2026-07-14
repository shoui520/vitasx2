// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/defs.h"
#include "SPU2/spu2.h" // hopefully temporary, until I resolve lClocks depdendency
#include "IopMem.h"
#include "StateWrapper.h"

#include <array>
#include <cstring>

namespace SPU2Savestate
{
	// Arbitrary ID to identify SPU2 saves.
	static constexpr u32 SAVE_ID = 0x1227521;

	// versioning for saves.
	// Increment this when changes to the savestate system are made.
	static constexpr u32 SAVE_VERSION = 0x000e;

	// This version is independent of SAVE_VERSION above: the legacy DataBlock is
	// deliberately ABI-shaped, while this stream is shared by little-endian
	// x86-64 and AArch32 validation builds.
	static constexpr u32 PORTABLE_SAVE_ID = 0x32555053; // "SPU2"
	static constexpr u32 PORTABLE_SAVE_VERSION = 1;
	static constexpr s32 PORTABLE_NULL_IOP_OFFSET = -1;
	static constexpr u32 PORTABLE_IOP_RAM_SIZE = 0x200000;
	static constexpr u32 PORTABLE_SPU2_RAM_WORDS = 0x100000;

	static void wipe_the_cache()
	{
		memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));
	}

	static void DoVolumeSlide(StateWrapper& sw, V_VolumeSlide& volume)
	{
		sw.Do(&volume.Reg_VOL);
		sw.Do(&volume.Counter);
		sw.Do(&volume.Value);
	}

	static void DoVolumeSlideLR(StateWrapper& sw, V_VolumeSlideLR& volume)
	{
		DoVolumeSlide(sw, volume.Left);
		DoVolumeSlide(sw, volume.Right);
	}

	static bool DoPortableBool(StateWrapper& sw, bool& value)
	{
		u8 encoded = value ? 1 : 0;
		sw.Do(&encoded);
		if (!sw.IsGood() || encoded > 1)
			return false;
		if (sw.IsReading())
			value = encoded != 0;
		return true;
	}

	static void DoVolumeLR(StateWrapper& sw, V_VolumeLR& volume)
	{
		sw.Do(&volume.Left);
		sw.Do(&volume.Right);
	}

	static bool DoAdsr(StateWrapper& sw, V_ADSR& adsr)
	{
		// Serialize the register words, not the compiler-defined bitfield layout.
		sw.Do(&adsr.reg32);
		for (V_ADSR::CachedADSR& phase : adsr.CachedPhases)
		{
			if (!DoPortableBool(sw, phase.Decr) || !DoPortableBool(sw, phase.Exp))
				return false;
			sw.Do(&phase.Shift);
			sw.Do(&phase.Step);
			sw.Do(&phase.Target);
		}
		sw.Do(&adsr.Counter);
		sw.Do(&adsr.Value);
		sw.Do(&adsr.Phase);
		return true;
	}

	static bool DoVoice(StateWrapper& sw, V_Voice& voice)
	{
		DoVolumeSlideLR(sw, voice.Volume);
		if (!DoAdsr(sw, voice.ADSR))
			return false;
		sw.Do(&voice.Pitch);
		sw.Do(&voice.LoopStartA);
		sw.Do(&voice.StartA);
		sw.Do(&voice.NextA);
		sw.Do(&voice.Prev1);
		sw.Do(&voice.Prev2);
		if (!DoPortableBool(sw, voice.Modulated) || !DoPortableBool(sw, voice.Noise))
			return false;
		sw.Do(&voice.LoopMode);
		sw.Do(&voice.LoopFlags);
		sw.Do(&voice.SP);
		sw.Do(&voice.OutX);
		// SBuffer points into pcm_cache_data. The cache is intentionally omitted
		// by the legacy owner too and is rebuilt from NextA after loading.
		sw.DoArray(voice.DecodeFifo, std::size(voice.DecodeFifo));
		sw.Do(&voice.DecPosWrite);
		sw.Do(&voice.DecPosRead);
		return true;
	}

	static void DoVoiceGates(StateWrapper& sw, V_VoiceGates& gates)
	{
		sw.Do(&gates.DryL);
		sw.Do(&gates.DryR);
		sw.Do(&gates.WetL);
		sw.Do(&gates.WetR);
	}

	static void DoCoreGates(StateWrapper& sw, V_CoreGates& gates)
	{
		sw.Do(&gates.InpL);
		sw.Do(&gates.InpR);
		sw.Do(&gates.SndL);
		sw.Do(&gates.SndR);
		sw.Do(&gates.ExtL);
		sw.Do(&gates.ExtR);
	}

	static void DoReverb(StateWrapper& sw, V_Reverb& reverb)
	{
		sw.Do(&reverb.IN_COEF_L);
		sw.Do(&reverb.IN_COEF_R);
		sw.Do(&reverb.APF1_SIZE);
		sw.Do(&reverb.APF2_SIZE);
		sw.Do(&reverb.APF1_VOL);
		sw.Do(&reverb.APF2_VOL);
		sw.Do(&reverb.SAME_L_SRC);
		sw.Do(&reverb.SAME_R_SRC);
		sw.Do(&reverb.DIFF_L_SRC);
		sw.Do(&reverb.DIFF_R_SRC);
		sw.Do(&reverb.SAME_L_DST);
		sw.Do(&reverb.SAME_R_DST);
		sw.Do(&reverb.DIFF_L_DST);
		sw.Do(&reverb.DIFF_R_DST);
		sw.Do(&reverb.IIR_VOL);
		sw.Do(&reverb.WALL_VOL);
		sw.Do(&reverb.COMB1_L_SRC);
		sw.Do(&reverb.COMB1_R_SRC);
		sw.Do(&reverb.COMB2_L_SRC);
		sw.Do(&reverb.COMB2_R_SRC);
		sw.Do(&reverb.COMB3_L_SRC);
		sw.Do(&reverb.COMB3_R_SRC);
		sw.Do(&reverb.COMB4_L_SRC);
		sw.Do(&reverb.COMB4_R_SRC);
		sw.Do(&reverb.COMB1_VOL);
		sw.Do(&reverb.COMB2_VOL);
		sw.Do(&reverb.COMB3_VOL);
		sw.Do(&reverb.COMB4_VOL);
		sw.Do(&reverb.APF1_L_DST);
		sw.Do(&reverb.APF1_R_DST);
		sw.Do(&reverb.APF2_L_DST);
		sw.Do(&reverb.APF2_R_DST);
	}

	static void DoCoreRegs(StateWrapper& sw, V_CoreRegs& regs)
	{
		sw.Do(&regs.PMON);
		sw.Do(&regs.NON);
		sw.Do(&regs.VMIXL);
		sw.Do(&regs.VMIXR);
		sw.Do(&regs.VMIXEL);
		sw.Do(&regs.VMIXER);
		sw.Do(&regs.ENDX);
		sw.Do(&regs.MMIX);
		sw.Do(&regs.STATX);
		sw.Do(&regs.ATTR);
		sw.Do(&regs._1AC);
	}

	static bool DoCore(StateWrapper& sw, V_Core& core, s32& dma_offset, s32& dma_read_offset)
	{
		sw.Do(&core.Index);
		for (V_VoiceGates& gates : core.VoiceGates)
			DoVoiceGates(sw, gates);
		DoCoreGates(sw, core.DryGate);
		DoCoreGates(sw, core.WetGate);
		DoVolumeSlideLR(sw, core.MasterVol);
		DoVolumeLR(sw, core.ExtVol);
		DoVolumeLR(sw, core.InpVol);
		DoVolumeLR(sw, core.FxVol);
		for (V_Voice& voice : core.Voices)
		{
			if (!DoVoice(sw, voice))
				return false;
		}

		sw.Do(&core.IRQA);
		sw.Do(&core.TSA);
		sw.Do(&core.ActiveTSA);
		if (!DoPortableBool(sw, core.IRQEnable) || !DoPortableBool(sw, core.FxEnable) ||
			!DoPortableBool(sw, core.Mute) || !DoPortableBool(sw, core.AdmaInProgress))
		{
			return false;
		}
		sw.Do(&core.DMABits);
		sw.Do(&core.NoiseClk);
		sw.Do(&core.NoiseCnt);
		sw.Do(&core.NoiseOut);
		sw.Do(&core.AutoDMACtrl);
		sw.Do(&core.DMAICounter);
		sw.Do(&core.LastClock);
		sw.Do(&core.InputDataLeft);
		sw.Do(&core.InputDataTransferred);
		sw.Do(&core.InputPosWrite);
		sw.Do(&core.InputDataProgress);

		DoReverb(sw, core.Revb);
		for (auto& channel : core.RevbDownBuf)
			sw.DoArray(channel, std::size(channel));
		for (auto& channel : core.RevbUpBuf)
			sw.DoArray(channel, std::size(channel));
		sw.Do(&core.RevbSampleBufPos);
		sw.Do(&core.EffectsStartA);
		sw.Do(&core.EffectsEndA);
		DoCoreRegs(sw, core.Regs);
		sw.Do(&core.LastEffect.Left);
		sw.Do(&core.LastEffect.Right);
		sw.Do(&core.CoreEnabled);
		sw.Do(&core.AttrBit0);
		sw.Do(&core.DmaMode);
		if (!DoPortableBool(sw, core.DmaStarted))
			return false;
		sw.Do(&core.AutoDmaFree);
		sw.Do(&dma_offset);
		sw.Do(&dma_read_offset);
		sw.Do(&core.ReadSize);
		if (!DoPortableBool(sw, core.IsDMARead))
			return false;
		sw.Do(&core.KeyOn);
		sw.Do(&core.KeyOff);
		sw.Do(&core.psxSoundDataTransferControl);
		sw.Do(&core.psxSPUSTAT);
		return true;
	}

	static void DoSpdif(StateWrapper& sw, V_SPDIF& spdif)
	{
		sw.Do(&spdif.Out);
		sw.Do(&spdif.Info);
		sw.Do(&spdif.Unknown1);
		sw.Do(&spdif.Mode);
		sw.Do(&spdif.Media);
		sw.Do(&spdif.Unknown2);
		sw.Do(&spdif.Protection);
	}

	static bool EncodeIopPointer(const u16* pointer, s32* offset)
	{
		if (!pointer)
		{
			*offset = PORTABLE_NULL_IOP_OFFSET;
			return true;
		}
		if (!iopMem)
			return false;

		const uptr base = reinterpret_cast<uptr>(iopMem->Main);
		const uptr address = reinterpret_cast<uptr>(pointer);
		if (address < base || address >= base + PORTABLE_IOP_RAM_SIZE)
			return false;

		const uptr byte_offset = address - base;
		if ((byte_offset & (alignof(u16) - 1)) != 0)
			return false;

		*offset = static_cast<s32>(byte_offset);
		return true;
	}

	static bool DecodeIopPointer(s32 offset, u16** pointer)
	{
		if (offset == PORTABLE_NULL_IOP_OFFSET)
		{
			*pointer = nullptr;
			return true;
		}
		if (!iopMem || offset < 0 || static_cast<u32>(offset) >= PORTABLE_IOP_RAM_SIZE ||
			(static_cast<u32>(offset) & (alignof(u16) - 1)) != 0)
		{
			return false;
		}

		*pointer = reinterpret_cast<u16*>(iopMem->Main + static_cast<u32>(offset));
		return true;
	}

	static bool ValidateIopWordSpan(s32 pointer_offset, u32 word_offset, u32 word_count)
	{
		if (pointer_offset < 0 || (static_cast<u32>(pointer_offset) & 1u) != 0)
			return false;
		const u64 begin = static_cast<u32>(pointer_offset) +
			(static_cast<u64>(word_offset) * sizeof(u16));
		const u64 bytes = static_cast<u64>(word_count) * sizeof(u16);
		return begin <= PORTABLE_IOP_RAM_SIZE && bytes <= PORTABLE_IOP_RAM_SIZE - begin;
	}

	static bool ValidateCoreDma(const V_Core& core, s32 dma_offset, s32 dma_read_offset)
	{
		const u32 adma_bit = 1u << core.Index;
		const bool adma_enabled = (core.AutoDMACtrl & adma_bit) != 0;
		if (!adma_enabled && core.ReadSize != 0)
		{
			const s32 active_offset = core.IsDMARead ? dma_read_offset : dma_offset;
			// Dma.cpp::FinishDMAwrite()/FinishDMAread() support one circular
			// SPU-RAM revolution and index their first cache/RAM word directly.
			if (core.ActiveTSA >= PORTABLE_SPU2_RAM_WORDS ||
				core.ReadSize > PORTABLE_SPU2_RAM_WORDS || core.DMAICounter <= 0 ||
				!ValidateIopWordSpan(active_offset, 0, core.ReadSize))
			{
				return false;
			}
		}
		if (core.InputDataLeft != 0 &&
			(!adma_enabled || !core.AdmaInProgress ||
			 !ValidateIopWordSpan(dma_offset, core.InputDataProgress, core.InputDataLeft)))
		{
			return false;
		}
		return true;
	}

	static bool IsSignedVolume(s32 value)
	{
		return value >= -0x8000 && value <= 0x7fff;
	}

	static bool ValidateVolumeSlide(const V_VolumeSlide& volume)
	{
		// ADSR.cpp::V_VolumeSlide::RegSet() and Update() keep the current
		// value in signed-16 range. Update() adds and multiplies it as s32.
		return volume.Counter < 0x8000 && IsSignedVolume(volume.Value);
	}

	static bool ValidateVolume(const V_VolumeLR& volume)
	{
		// spu2sys.cpp::RegWrite_CoreExt() sign-extends both fixed volumes.
		return IsSignedVolume(volume.Left) && IsSignedVolume(volume.Right);
	}

	static bool IsGateMask(s32 value)
	{
		return value == 0 || value == -1;
	}

	static bool ValidateVoiceGates(const V_VoiceGates& gates)
	{
		return IsGateMask(gates.DryL) && IsGateMask(gates.DryR) &&
			IsGateMask(gates.WetL) && IsGateMask(gates.WetR);
	}

	static bool ValidateCoreGates(const V_CoreGates& gates)
	{
		return IsGateMask(gates.InpL) && IsGateMask(gates.InpR) &&
			IsGateMask(gates.SndL) && IsGateMask(gates.SndR) &&
			IsGateMask(gates.ExtL) && IsGateMask(gates.ExtR);
	}

	static bool ValidateGlobalState(s32 play_mode)
	{
		return OutPos < 0x200 &&
			(play_mode == 0 || play_mode == 1 || play_mode == 2 ||
			 play_mode == 4 || play_mode == 8);
	}

	static bool ValidateCore(const V_Core& core, u32 expected_index)
	{
		// Reverb.cpp::V_Core::DoReverb() uses this as an unmasked index into
		// both halves of the 128-sample up/down buffers.
		// spu2sys.cpp masks NoiseClk to its six-bit hardware register. Mixer.cpp
		// uses it as a shift count before applying the low two frequency bits.
		if (core.Index != expected_index || core.RevbSampleBufPos >= 64 ||
			core.NoiseClk > 0x3f || core.DMABits < 0 || core.DMABits > 7 ||
			core.CoreEnabled > 1 || core.AttrBit0 > 1 || core.DmaMode > 3 ||
			core.EffectsStartA >= PORTABLE_SPU2_RAM_WORDS ||
			core.EffectsEndA >= PORTABLE_SPU2_RAM_WORDS ||
			!ValidateCoreGates(core.DryGate) || !ValidateCoreGates(core.WetGate) ||
			!ValidateVolumeSlide(core.MasterVol.Left) ||
			!ValidateVolumeSlide(core.MasterVol.Right) ||
			!ValidateVolume(core.ExtVol) || !ValidateVolume(core.InpVol) ||
			!ValidateVolume(core.FxVol))
			return false;
		for (const V_VoiceGates& gates : core.VoiceGates)
		{
			if (!ValidateVoiceGates(gates))
				return false;
		}
		for (const V_Voice& voice : core.Voices)
		{
			const V_ADSR& adsr = voice.ADSR;
			if (voice.LoopStartA >= PORTABLE_SPU2_RAM_WORDS ||
				voice.StartA >= PORTABLE_SPU2_RAM_WORDS ||
				voice.NextA >= PORTABLE_SPU2_RAM_WORDS ||
				voice.LoopMode < 0 || voice.LoopMode > 1 ||
				voice.SP < 0 || voice.SP > 0xfff ||
				!IsSignedVolume(voice.Prev1) || !IsSignedVolume(voice.Prev2) ||
				!IsSignedVolume(voice.OutX) ||
				!ValidateVolumeSlide(voice.Volume.Left) ||
				!ValidateVolumeSlide(voice.Volume.Right) ||
				adsr.Phase > V_ADSR::PHASE_RELEASE || adsr.Counter >= 0x8000 ||
				adsr.Value < 0 || adsr.Value > UINT16_MAX)
				return false;
			for (const s32 sample : voice.DecodeFifo)
			{
				if (!IsSignedVolume(sample))
					return false;
			}

			// ADSR.cpp::CalculateAdsrImpl() indexes CachedPhases with Phase and
			// shifts by CachedADSR::Shift. UpdateCache() is the PCSX2 owner for
			// every active cached phase. A never-programmed voice legitimately
			// retains the static zero-initialized cache, otherwise require the
			// exact cache derived from the serialized register word.
			V_ADSR expected = {};
			expected.reg32 = adsr.reg32;
			expected.UpdateCache();
			bool all_zero = true;
			bool matches_registers = true;
			for (u32 phase_index = 0; phase_index < V_ADSR::ADSR_PHASES; phase_index++)
			{
				const V_ADSR::CachedADSR& actual = adsr.CachedPhases[phase_index];
				const V_ADSR::CachedADSR& derived = expected.CachedPhases[phase_index];
				all_zero = all_zero && !actual.Decr && !actual.Exp && actual.Shift == 0 &&
					actual.Step == 0 && actual.Target == 0;
				matches_registers = matches_registers &&
					actual.Decr == derived.Decr && actual.Exp == derived.Exp &&
					actual.Shift == derived.Shift && actual.Step == derived.Step &&
					actual.Target == derived.Target;
			}
			if (!all_zero && !matches_registers)
				return false;
		}
		return true;
	}
} // namespace SPU2Savestate

bool SPU2::DoPortableState(StateWrapper& sw)
{
	using namespace SPU2Savestate;

	std::array<s32, 2> dma_offsets = {{PORTABLE_NULL_IOP_OFFSET, PORTABLE_NULL_IOP_OFFSET}};
	std::array<s32, 2> dma_read_offsets = {{PORTABLE_NULL_IOP_OFFSET, PORTABLE_NULL_IOP_OFFSET}};
	if (sw.IsWriting())
	{
		for (u32 i = 0; i < std::size(Cores); i++)
		{
			if (!ValidateCore(Cores[i], i) ||
				!EncodeIopPointer(Cores[i].DMAPtr, &dma_offsets[i]) ||
				!EncodeIopPointer(Cores[i].DMARPtr, &dma_read_offsets[i]) ||
				!ValidateCoreDma(Cores[i], dma_offsets[i], dma_read_offsets[i]))
			{
				return false;
			}
		}
	}

	u32 save_id = PORTABLE_SAVE_ID;
	u32 save_version = PORTABLE_SAVE_VERSION;
	sw.Do(&save_id);
	sw.Do(&save_version);
	if (!sw.IsGood() || save_id != PORTABLE_SAVE_ID || save_version != PORTABLE_SAVE_VERSION)
		return false;

	// Register and sample RAM are byte-addressed PS2 state. Keeping them as byte
	// spans avoids imposing a host integer representation on their contents.
	sw.DoBytes(spu2regs, sizeof(spu2regs));
	sw.DoBytes(_spu2mem, sizeof(_spu2mem));
	for (u32 i = 0; i < std::size(Cores); i++)
	{
		if (!DoCore(sw, Cores[i], dma_offsets[i], dma_read_offsets[i]))
			return false;
	}
	DoSpdif(sw, Spdif);
	sw.Do(&OutPos);
	sw.Do(&InputPos);
	sw.Do(&Cycles);
	sw.Do(&lClocks);
	s32 portable_play_mode = static_cast<s32>(PlayMode);
	if (sw.IsWriting() && !ValidateGlobalState(portable_play_mode))
		return false;
	sw.Do(&portable_play_mode);
	if (!sw.IsGood() || !ValidateGlobalState(portable_play_mode))
		return false;

	if (sw.IsReading())
	{
		std::array<u16*, 2> dma_pointers = {};
		std::array<u16*, 2> dma_read_pointers = {};
		for (u32 i = 0; i < std::size(Cores); i++)
		{
			if (!ValidateCore(Cores[i], i) ||
				!ValidateCoreDma(Cores[i], dma_offsets[i], dma_read_offsets[i]) ||
				!DecodeIopPointer(dma_offsets[i], &dma_pointers[i]) ||
				!DecodeIopPointer(dma_read_offsets[i], &dma_read_pointers[i]))
			{
				return false;
			}
		}

		PlayMode = portable_play_mode;
		wipe_the_cache();
		for (u32 i = 0; i < std::size(Cores); i++)
		{
			Cores[i].DMAPtr = dma_pointers[i];
			Cores[i].DMARPtr = dma_read_pointers[i];
			for (V_Voice& voice : Cores[i].Voices)
			{
				const u32 cache_index = voice.NextA / pcm_WordsPerBlock;
				voice.SBuffer = pcm_cache_data[cache_index].Sampledata;
			}
		}
	}

	return true;
}

struct SPU2Savestate::DataBlock
{
	u32 spu2id;          // SPU2 state identifier lets ZeroGS/PeopsSPU2 know this isn't their state)
	u8 unkregs[0x10000]; // SPU2 raw register memory
	u8 mem[0x200000];    // SPU2 raw sample memory

	u32 version; // SPU2 version identifier
	V_Core Cores[2];
	V_SPDIF Spdif;
	u16 OutPos;
	u16 InputPos;
	u32 Cycles;
	u64 lClocks;
	int PlayMode;
};

s32 SPU2Savestate::FreezeIt(DataBlock& spud)
{
	spud.spu2id = SAVE_ID;
	spud.version = SAVE_VERSION;

	memcpy(spud.unkregs, spu2regs, sizeof(spud.unkregs));
	memcpy(spud.mem, _spu2mem, sizeof(spud.mem));

	memcpy(spud.Cores, Cores, sizeof(Cores));
	memcpy(&spud.Spdif, &Spdif, sizeof(Spdif));

	// Convert pointers to offsets so we can safely restore them when loading.
	// We use -1 for null, and anything else as an offset from iop memory.
#define FIX_POINTER(x) \
	if (!(x)) \
	{ \
		x = reinterpret_cast<decltype(x)>(-1); \
	} \
	else \
	{ \
		pxAssert(reinterpret_cast<const u8*>((x)) >= iopPhysMem(0) && reinterpret_cast<const u8*>((x)) < iopPhysMem(0x1fffff)); \
		x = reinterpret_cast<decltype(x)>(reinterpret_cast<const u8*>((x)) - iopPhysMem(0)); \
	}

	for (u32 i = 0; i < 2; i++)
	{
		V_Core& core = spud.Cores[i];
		FIX_POINTER(core.DMAPtr);
		FIX_POINTER(core.DMARPtr);
	}

#undef FIX_POINTER

	spud.OutPos = OutPos;
	spud.InputPos = InputPos;
	spud.Cycles = Cycles;
	spud.lClocks = lClocks;
	spud.PlayMode = PlayMode;

	// note: Don't save the cache.  PCSX2 doesn't offer a safe method of predicting
	// the required size of the savestate prior to saving, plus this is just too
	// "implementation specific" for the intended spec of a savestate.  Let's just
	// force the user to rebuild their cache instead.

	return 0;
}

s32 SPU2Savestate::ThawIt(DataBlock& spud)
{
	if (spud.spu2id != SAVE_ID || spud.version < SAVE_VERSION)
	{
		fprintf(stderr, "\n*** SPU2 Warning:\n");
		if (spud.spu2id == SAVE_ID)
			fprintf(stderr, "\tSavestate version is from an older version of PCSX2.\n");
		else
			fprintf(stderr, "\tThe savestate you are trying to load is incorrect or corrupted.\n");

		fprintf(stderr,
				"\tAudio may not recover correctly.  Save your game to memorycard, reset,\n\n"
				"\tand then continue from there.\n\n");

		// Do *not* reset the cores.
		// We'll need some "hints" as to how the cores should be initialized, and the
		// only way to get that is to use the game's existing core settings and hope
		// they kinda match the settings for the savestate (IRQ enables and such).

		// adpcm cache : Clear all the cache flags and buffers.

		wipe_the_cache();
	}
	else
	{
		memcpy(spu2regs, spud.unkregs, sizeof(spud.unkregs));
		memcpy(_spu2mem, spud.mem, sizeof(spud.mem));

		memcpy(Cores, spud.Cores, sizeof(Cores));
		memcpy(&Spdif, &spud.Spdif, sizeof(Spdif));

		// Reverse the pointer offset from above.
#define FIX_POINTER(x) \
	if ((x) == reinterpret_cast<decltype(x)>(-1)) \
	{ \
		x = nullptr; \
	} \
	else \
	{ \
		pxAssert(reinterpret_cast<size_t>((x)) <= 0x1fffff); \
		x = reinterpret_cast<decltype(x)>(iopPhysMem(0) + reinterpret_cast<size_t>((x))); \
	}

		for (u32 i = 0; i < 2; i++)
		{
			V_Core& core = Cores[i];
			FIX_POINTER(core.DMAPtr);
			FIX_POINTER(core.DMARPtr);
		}

#undef FIX_POINTER

		OutPos = spud.OutPos;
		InputPos = spud.InputPos;
		Cycles = spud.Cycles;
		lClocks = spud.lClocks;
		PlayMode = spud.PlayMode;

		wipe_the_cache();

		// Go through the V_Voice structs and recalculate SBuffer pointer from
		// the NextA setting.

		for (int c = 0; c < 2; c++)
		{
			for (int v = 0; v < 24; v++)
			{
				const int cacheIdx = Cores[c].Voices[v].NextA / pcm_WordsPerBlock;
				Cores[c].Voices[v].SBuffer = pcm_cache_data[cacheIdx].Sampledata;
			}
		}
	}
	return 0;
}

s32 SPU2Savestate::SizeIt()
{
	return sizeof(DataBlock);
}
