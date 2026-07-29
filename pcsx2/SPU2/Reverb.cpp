// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/defs.h"
#include "GS/GSVector.h"

#include "common/Console.h"

#include <array>

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuSpu2ReciprocalModuloIndexes = 0;
u32 g_qemuSpu2SilentReverbSamples = 0;
#endif

#if defined(VITASX2_CPU_PROFILER)
u64 g_vitaSpu2SilentReverbSamples = 0;
u64 g_vitaSpu2SilentReverbInputRejects = 0;
u64 g_vitaSpu2SilentReverbIrqRejects = 0;
u64 g_vitaSpu2SilentReverbRangeRejects = 0;
u64 g_vitaSpu2SilentReverbStateRejects = 0;
#endif

namespace
{
	static constexpr u8 DIVIDER_ADD_MARKER = 0x40;
	static constexpr u8 DIVIDER_SHIFT_MASK = 0x1f;
	static constexpr u32 MIXER_OWNED_RAM_END = 0x27ff;

	struct ReverbZeroState
	{
		u32 start = 0;
		u32 end = 0;
		bool range_valid = false;
		bool checked = false;
		bool proven = false;
	};

	std::array<ReverbZeroState, 2> s_reverb_zero_states;

#if defined(VITASX2_QEMU_VALIDATION)
	bool s_silent_reverb_fast_path_enabled = true;
#endif

	static __attribute__((noinline)) bool SamplesAreZero(
		const s16* samples, u32 count)
	{
		for (u32 i = 0; i < count; i++)
		{
			if (samples[i] != 0)
				return false;
		}
		return true;
	}

	static __attribute__((noinline)) bool ReverbBuffersAreZero(
		const V_Core& core)
	{
		return SamplesAreZero(
				   &core.RevbDownBuf[0][0],
				   sizeof(core.RevbDownBuf) / sizeof(s16)) &&
			   SamplesAreZero(
				   &core.RevbUpBuf[0][0],
				   sizeof(core.RevbUpBuf) / sizeof(s16));
	}

	static __attribute__((noinline)) bool ReverbRamRangeIsZero(
		u32 start, u32 end)
	{
		return SamplesAreZero(
			&_spu2mem[start], (end - start) + 1u);
	}

	static __attribute__((noinline)) bool TryAdvanceSilentReverb(
		V_Core& core, const StereoOut32& input)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_silent_reverb_fast_path_enabled)
			return false;
#endif

		ReverbZeroState& state =
			s_reverb_zero_states[core.Index & 1u];
		const u32 start = core.EffectsStartA & 0x3f'ffffu;
		const u32 end =
			(core.EffectsEndA & 0x3f'ffffu) | 0xffffu;
		if (!state.range_valid || state.start != start ||
			state.end != end)
		{
			state.start = start;
			state.end = end;
			state.range_valid = true;
			state.checked = false;
			state.proven = false;
		}

		// A nonzero wet input makes the FIR history and usually the effects
		// work area nonzero. Do not repeatedly scan for a naturally decayed
		// state; a later guest RAM write or work-area change can re-arm the
		// exact proof.
		if ((input.Left | input.Right) != 0)
		{
			state.checked = true;
			state.proven = false;
#if defined(VITASX2_CPU_PROFILER)
			g_vitaSpu2SilentReverbInputRejects++;
#endif
			return false;
		}

		// Either SPU2 core can observe this core's reverb addresses through
		// IRQA. The zero lowering omits those address visits, so both observers
		// must be disabled.
		if (Cores[0].IRQEnable || Cores[1].IRQEnable)
		{
#if defined(VITASX2_CPU_PROFILER)
			g_vitaSpu2SilentReverbIrqRejects++;
#endif
			return false;
		}

		if (start <= MIXER_OWNED_RAM_END ||
			end >= 0x10'0000u)
		{
			state.checked = true;
			state.proven = false;
#if defined(VITASX2_CPU_PROFILER)
			g_vitaSpu2SilentReverbRangeRejects++;
#endif
			return false;
		}

		if (!state.checked)
		{
			// Mixer output and AutoDMA own 0x0000..0x27ff and write through
			// deliberately lightweight paths. Restrict this proof to a
			// contiguous, non-aliasing work area above those windows; all
			// remaining guest RAM writers notify the proof explicitly.
			state.proven =
				ReverbBuffersAreZero(core) &&
				ReverbRamRangeIsZero(start, end);
			state.checked = true;
		}

		if (!state.proven)
		{
#if defined(VITASX2_CPU_PROFILER)
			g_vitaSpu2SilentReverbStateRejects++;
#endif
			return false;
		}

		// PCSX2's full zero-input step writes only zeros to already-zero
		// history/RAM, advances this cursor once, and returns stereo zero.
		core.RevbSampleBufPos =
			(core.RevbSampleBufPos + 1u) & 63u;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSpu2SilentReverbSamples++;
#endif
#if defined(VITASX2_CPU_PROFILER)
		g_vitaSpu2SilentReverbSamples++;
#endif
		return true;
	}

	// Cortex-A9 has no integer divide instruction. RevbGetIndexer() otherwise
	// reaches __aeabi_uidivmod fourteen times per active core and sample. Cache
	// the exact unsigned reciprocal for each core's work area and use the
	// libdivide branchfull quotient construction in the sample-rate hot path.
	struct ReverbDivider
	{
		u32 start = 0;
		u32 divisor = 0;
		u32 magic = 0;
		u8 more = 0;
	};

	std::array<ReverbDivider, 2> s_reverb_dividers;

	static ReverbDivider MakeReverbDivider(u32 start, u32 divisor)
	{
		ReverbDivider result;
		result.start = start;
		result.divisor = divisor;

		const u32 floor_log_2_divisor =
			31u - static_cast<u32>(__builtin_clz(divisor));
		if ((divisor & (divisor - 1)) == 0)
		{
			result.more = static_cast<u8>(floor_log_2_divisor);
			return result;
		}

		const u64 numerator = 1ull << (32u + floor_log_2_divisor);
		u32 proposed_magic = static_cast<u32>(numerator / divisor);
		const u32 remainder = static_cast<u32>(
			numerator - static_cast<u64>(proposed_magic) * divisor);
		const u32 error = divisor - remainder;

		if (error < (1u << floor_log_2_divisor))
		{
			result.more = static_cast<u8>(floor_log_2_divisor);
		}
		else
		{
			proposed_magic += proposed_magic;
			const u32 twice_remainder = remainder + remainder;
			if (twice_remainder >= divisor ||
				twice_remainder < remainder)
			{
				proposed_magic++;
			}
			result.more = static_cast<u8>(
				floor_log_2_divisor | DIVIDER_ADD_MARKER);
		}

		result.magic = proposed_magic + 1;
		return result;
	}

	static __forceinline u32 DivideWithReverbDivider(
		u32 numerator, const ReverbDivider& divider)
	{
		if (divider.magic == 0)
			return numerator >> divider.more;

		u32 quotient = static_cast<u32>(
			(static_cast<u64>(divider.magic) * numerator) >> 32);
		if (divider.more & DIVIDER_ADD_MARKER)
		{
			const u32 adjusted = ((numerator - quotient) >> 1) + quotient;
			return adjusted >> (divider.more & DIVIDER_SHIFT_MASK);
		}
		return quotient >> divider.more;
	}

	static __attribute__((noinline)) u32 RevbGetReciprocalIndexer(
		u32 phase, u32 offset, const ReverbDivider& divider)
	{
		const u32 numerator = phase + offset;
		const u32 quotient = DivideWithReverbDivider(numerator, divider);
		const u32 remainder = numerator - quotient * divider.divisor;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSpu2ReciprocalModuloIndexes++;
#endif
		return (remainder + divider.start) & 0xf'ffff;
	}

	static const ReverbDivider& GetReverbDivider(const V_Core& core)
	{
		// Effects registers can change at any sample boundary, so derived
		// reciprocal state is reused only while both operands of PCSX2's
		// original modulo expression remain identical.
		const u32 start = core.EffectsStartA & 0x3f'ffff;
		const u32 end = (core.EffectsEndA & 0x3f'ffff) | 0xffff;
		const u32 divisor = (end - start) + 1;
		ReverbDivider& divider = s_reverb_dividers[core.Index & 1u];
		if (divider.start != start || divider.divisor != divisor)
			divider = MakeReverbDivider(start, divisor);
		return divider;
	}
} // namespace

void NotifyReverbRamWrite(u32 address, u32 words)
{
	if (words == 0)
		return;

	address &= 0xf'ffffu;
	const u32 first_words =
		std::min(words, 0x10'0000u - address);
	const u32 end = address + first_words - 1u;
	for (ReverbZeroState& state : s_reverb_zero_states)
	{
		if (state.range_valid && address <= state.end &&
			end >= state.start)
		{
			state.checked = false;
			state.proven = false;
		}
	}

	if (first_words != words)
		NotifyReverbRamWrite(0, words - first_words);
}

void InvalidateAllReverbZeroState()
{
	for (ReverbZeroState& state : s_reverb_zero_states)
		state = {};
}

#if defined(VITASX2_QEMU_VALIDATION)
void SetSilentReverbFastPathForValidation(bool enabled)
{
	s_silent_reverb_fast_path_enabled = enabled;
	InvalidateAllReverbZeroState();
}
#endif

void V_Core::AnalyzeReverbPreset()
{
	Console.WriteLn("Reverb Parameter Update for Core %d:", Index);
	Console.WriteLn("----------------------------------------------------------");

	Console.WriteLn("    IN_COEF_L, IN_COEF_R       0x%08x, 0x%08x", Revb.IN_COEF_L, Revb.IN_COEF_R);
	Console.WriteLn("    APF1_SIZE, APF2_SIZE       0x%08x, 0x%08x", Revb.APF1_SIZE, Revb.APF2_SIZE);
	Console.WriteLn("    APF1_VOL, APF2_VOL         0x%08x, 0x%08x", Revb.APF1_VOL, Revb.APF2_VOL);

	Console.WriteLn("    COMB1_VOL                  0x%08x", Revb.COMB1_VOL);
	Console.WriteLn("    COMB2_VOL                  0x%08x", Revb.COMB2_VOL);
	Console.WriteLn("    COMB3_VOL                  0x%08x", Revb.COMB3_VOL);
	Console.WriteLn("    COMB4_VOL                  0x%08x", Revb.COMB4_VOL);

	Console.WriteLn("    COMB1_L_SRC, COMB1_R_SRC   0x%08x, 0x%08x", Revb.COMB1_L_SRC, Revb.COMB1_R_SRC);
	Console.WriteLn("    COMB2_L_SRC, COMB2_R_SRC   0x%08x, 0x%08x", Revb.COMB2_L_SRC, Revb.COMB2_R_SRC);
	Console.WriteLn("    COMB3_L_SRC, COMB3_R_SRC   0x%08x, 0x%08x", Revb.COMB3_L_SRC, Revb.COMB3_R_SRC);
	Console.WriteLn("    COMB4_L_SRC, COMB4_R_SRC   0x%08x, 0x%08x", Revb.COMB4_L_SRC, Revb.COMB4_R_SRC);

	Console.WriteLn("    SAME_L_SRC, SAME_R_SRC     0x%08x, 0x%08x", Revb.SAME_L_SRC, Revb.SAME_R_SRC);
	Console.WriteLn("    DIFF_L_SRC, DIFF_R_SRC     0x%08x, 0x%08x", Revb.DIFF_L_SRC, Revb.DIFF_R_SRC);
	Console.WriteLn("    SAME_L_DST, SAME_R_DST     0x%08x, 0x%08x", Revb.SAME_L_DST, Revb.SAME_R_DST);
	Console.WriteLn("    DIFF_L_DST, DIFF_R_DST     0x%08x, 0x%08x", Revb.DIFF_L_DST, Revb.DIFF_R_DST);
	Console.WriteLn("    IIR_VOL, WALL_VOL          0x%08x, 0x%08x", Revb.IIR_VOL, Revb.WALL_VOL);

	Console.WriteLn("    APF1_L_DST                 0x%08x", Revb.APF1_L_DST);
	Console.WriteLn("    APF1_R_DST                 0x%08x", Revb.APF1_R_DST);
	Console.WriteLn("    APF2_L_DST                 0x%08x", Revb.APF2_L_DST);
	Console.WriteLn("    APF2_R_DST                 0x%08x", Revb.APF2_R_DST);

	Console.WriteLn("    EffectStartA               0x%x", EffectsStartA & 0x3f'ffff);
	Console.WriteLn("    EffectsEndA                0x%x", EffectsEndA & 0x3f'ffff);
	Console.WriteLn("----------------------------------------------------------");
}

__forceinline s32 V_Core::RevbGetIndexer(s32 offset)
{
	u32 start = EffectsStartA & 0x3f'ffff;
	u32 end = (EffectsEndA & 0x3f'ffff) | 0xffff;

	u32 x = ((Cycles >> 1) + offset) % ((end - start) + 1);

	x += start;

	return x & 0xf'ffff;
}

#if defined(VITASX2_QEMU_VALIDATION)
u32 Spu2ReverbReciprocalIndexForValidation(
	u32 start, u32 divisor, u32 phase, u32 offset)
{
	const ReverbDivider divider = MakeReverbDivider(start, divisor);
	return RevbGetReciprocalIndexer(phase, offset, divider);
}
#endif

StereoOut32 V_Core::DoReverb(StereoOut32 Input)
{
	if (EffectsStartA >= EffectsEndA)
	{
		return StereoOut32::Empty;
	}

	Input = clamp_mix(Input);
	if (TryAdvanceSilentReverb(*this, Input))
		return StereoOut32::Empty;

	RevbDownBuf[0][RevbSampleBufPos] = Input.Left;
	RevbDownBuf[1][RevbSampleBufPos] = Input.Right;
	RevbDownBuf[0][RevbSampleBufPos | 64] = Input.Left;
	RevbDownBuf[1][RevbSampleBufPos | 64] = Input.Right;

	bool R = Cycles & 1;
	const ReverbDivider& divider = GetReverbDivider(*this);
	const u32 phase = Cycles >> 1;

	// Calculate the read/write addresses we'll be needing for this session of reverb.

	const u32 same_src = RevbGetReciprocalIndexer(
		phase, R ? Revb.SAME_R_SRC : Revb.SAME_L_SRC, divider);
	const u32 same_dst = RevbGetReciprocalIndexer(
		phase, R ? Revb.SAME_R_DST : Revb.SAME_L_DST, divider);
	const u32 same_prv = RevbGetReciprocalIndexer(
		phase, R ? Revb.SAME_R_DST - 1 : Revb.SAME_L_DST - 1, divider);

	const u32 diff_src = RevbGetReciprocalIndexer(
		phase, R ? Revb.DIFF_L_SRC : Revb.DIFF_R_SRC, divider);
	const u32 diff_dst = RevbGetReciprocalIndexer(
		phase, R ? Revb.DIFF_R_DST : Revb.DIFF_L_DST, divider);
	const u32 diff_prv = RevbGetReciprocalIndexer(
		phase, R ? Revb.DIFF_R_DST - 1 : Revb.DIFF_L_DST - 1, divider);

	const u32 comb1_src = RevbGetReciprocalIndexer(
		phase, R ? Revb.COMB1_R_SRC : Revb.COMB1_L_SRC, divider);
	const u32 comb2_src = RevbGetReciprocalIndexer(
		phase, R ? Revb.COMB2_R_SRC : Revb.COMB2_L_SRC, divider);
	const u32 comb3_src = RevbGetReciprocalIndexer(
		phase, R ? Revb.COMB3_R_SRC : Revb.COMB3_L_SRC, divider);
	const u32 comb4_src = RevbGetReciprocalIndexer(
		phase, R ? Revb.COMB4_R_SRC : Revb.COMB4_L_SRC, divider);

	const u32 apf1_src = RevbGetReciprocalIndexer(
		phase, R ? (Revb.APF1_R_DST - Revb.APF1_SIZE) :
			(Revb.APF1_L_DST - Revb.APF1_SIZE), divider);
	const u32 apf1_dst = RevbGetReciprocalIndexer(
		phase, R ? Revb.APF1_R_DST : Revb.APF1_L_DST, divider);
	const u32 apf2_src = RevbGetReciprocalIndexer(
		phase, R ? (Revb.APF2_R_DST - Revb.APF2_SIZE) :
			(Revb.APF2_L_DST - Revb.APF2_SIZE), divider);
	const u32 apf2_dst = RevbGetReciprocalIndexer(
		phase, R ? Revb.APF2_R_DST : Revb.APF2_L_DST, divider);

	// -----------------------------------------
	//          Optimized IRQ Testing !
	// -----------------------------------------

	// This test is enhanced by using the reverb effects area begin/end test as a
	// shortcut, since all buffer addresses are within that area.  If the IRQA isn't
	// within that zone then the "bulk" of the test is skipped, so this should only
	// be a slowdown on a few evil games.

	for (int i = 0; i < 2; i++)
	{
		if (FxEnable && Cores[i].IRQEnable && ((Cores[i].IRQA >= EffectsStartA) && (Cores[i].IRQA <= EffectsEndA)))
		{
			if ((Cores[i].IRQA == same_src) || (Cores[i].IRQA == diff_src) ||
				(Cores[i].IRQA == same_dst) || (Cores[i].IRQA == diff_dst) ||
				(Cores[i].IRQA == same_prv) || (Cores[i].IRQA == diff_prv) ||

				(Cores[i].IRQA == comb1_src) || (Cores[i].IRQA == comb2_src) ||
				(Cores[i].IRQA == comb3_src) || (Cores[i].IRQA == comb4_src) ||

				(Cores[i].IRQA == apf1_dst) || (Cores[i].IRQA == apf1_src) ||
				(Cores[i].IRQA == apf2_dst) || (Cores[i].IRQA == apf2_src))
			{
				//printf("Core %d IRQ Called (Reverb). IRQA = %x\n",i,addr);
				SetIrqCall(i);
			}
		}
	}

	// Reverb algorithm pretty much directly ripped from http://drhell.web.fc2.com/ps1/
	// minus the 35 step FIR which just seems to break things.

	s32 in, same, diff, apf1, apf2, out;

#define MUL(x, y) ((x) * (y) >> 15)
	in = MUL(R ? Revb.IN_COEF_R : Revb.IN_COEF_L, ReverbDownsample(*this, R));

	same = MUL(Revb.IIR_VOL, in + MUL(Revb.WALL_VOL, _spu2mem[same_src]) - _spu2mem[same_prv]) + _spu2mem[same_prv];
	diff = MUL(Revb.IIR_VOL, in + MUL(Revb.WALL_VOL, _spu2mem[diff_src]) - _spu2mem[diff_prv]) + _spu2mem[diff_prv];

	out = MUL(Revb.COMB1_VOL, _spu2mem[comb1_src]) + MUL(Revb.COMB2_VOL, _spu2mem[comb2_src]) + MUL(Revb.COMB3_VOL, _spu2mem[comb3_src]) + MUL(Revb.COMB4_VOL, _spu2mem[comb4_src]);

	apf1 = out - MUL(Revb.APF1_VOL, _spu2mem[apf1_src]);
	out = _spu2mem[apf1_src] + MUL(Revb.APF1_VOL, apf1);
	apf2 = out - MUL(Revb.APF2_VOL, _spu2mem[apf2_src]);
	out = _spu2mem[apf2_src] + MUL(Revb.APF2_VOL, apf2);

	// According to no$psx the effects always run but don't always write back, see check in V_Core::Mix
	if (FxEnable)
	{
		_spu2mem[same_dst] = clamp_mix(same);
		_spu2mem[diff_dst] = clamp_mix(diff);
		_spu2mem[apf1_dst] = clamp_mix(apf1);
		_spu2mem[apf2_dst] = clamp_mix(apf2);
	}

	out = clamp_mix(out);

	RevbUpBuf[R][RevbSampleBufPos] = out;
	RevbUpBuf[!R][RevbSampleBufPos] = 0;

	RevbUpBuf[R][RevbSampleBufPos | 64] = out;
	RevbUpBuf[!R][RevbSampleBufPos | 64] = 0;

	RevbSampleBufPos = (RevbSampleBufPos + 1) & 63;

	return ReverbUpsample(*this);
}
