// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/defs.h"

#include "common/Assertions.h"

static constexpr s32 ADSR_MAX_VOL = 0x7fff;

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuSpu2AdsrSaturateClamps = 0;
u32 g_qemuSpu2VolumeSlideSaturateClamps = 0;
#endif

static __forceinline s32 ClampAdsrValue_reference(s32 value)
{
	return std::clamp<s32>(value, 0, INT16_MAX);
}

static __forceinline s32 ClampS16_reference(s32 value)
{
	return std::clamp<s32>(value, INT16_MIN, INT16_MAX);
}

static __forceinline s32 ClampNonPositiveS16_reference(s32 value)
{
	return std::clamp<s32>(value, INT16_MIN, 0);
}

static __forceinline s32 ClampAdsrValue_selected(s32 value)
{
#if defined(ARCH_ARM32)
	s32 result;
	__asm__("usat %0, #15, %1" : "=r"(result) : "r"(value));
#if defined(VITASX2_QEMU_VALIDATION)
	++::g_qemuSpu2AdsrSaturateClamps;
#endif
	return result;
#else
	return ClampAdsrValue_reference(value);
#endif
}

static __forceinline s32 ClampVolumeS16_selected(s32 value)
{
#if defined(ARCH_ARM32)
	s32 result;
	__asm__("ssat %0, #16, %1" : "=r"(result) : "r"(value));
#if defined(VITASX2_QEMU_VALIDATION)
	++::g_qemuSpu2VolumeSlideSaturateClamps;
#endif
	return result;
#else
	return ClampS16_reference(value);
#endif
}

static __forceinline s32 ClampVolumePositive_selected(s32 value)
{
#if defined(ARCH_ARM32)
	s32 result;
	__asm__("usat %0, #15, %1" : "=r"(result) : "r"(value));
#if defined(VITASX2_QEMU_VALIDATION)
	++::g_qemuSpu2VolumeSlideSaturateClamps;
#endif
	return result;
#else
	return ClampAdsrValue_reference(value);
#endif
}

static __forceinline s32 ClampVolumeNonPositive_selected(s32 value)
{
#if defined(ARCH_ARM32)
	s32 result;
	__asm__(
		"ssat %0, #16, %1\n\t"
		"cmp %0, #0\n\t"
		"movgt %0, #0"
		: "=&r"(result)
		: "r"(value)
		: "cc");
#if defined(VITASX2_QEMU_VALIDATION)
	++::g_qemuSpu2VolumeSlideSaturateClamps;
#endif
	return result;
#else
	return ClampNonPositiveS16_reference(value);
#endif
}

void V_ADSR::UpdateCache()
{
	CachedPhases[PHASE_ATTACK].Decr = false;
	CachedPhases[PHASE_ATTACK].Exp = AttackMode;
	CachedPhases[PHASE_ATTACK].Shift = AttackShift;
	CachedPhases[PHASE_ATTACK].Step = 7 - AttackStep;
	CachedPhases[PHASE_ATTACK].Target = ADSR_MAX_VOL;

	CachedPhases[PHASE_DECAY].Decr = true;
	CachedPhases[PHASE_DECAY].Exp = true;
	CachedPhases[PHASE_DECAY].Shift = DecayShift;
	CachedPhases[PHASE_DECAY].Step = -8;
	CachedPhases[PHASE_DECAY].Target = (SustainLevel + 1) << 11;

	CachedPhases[PHASE_SUSTAIN].Decr = SustainDir;
	CachedPhases[PHASE_SUSTAIN].Exp = SustainMode;
	CachedPhases[PHASE_SUSTAIN].Shift = SustainShift;
	CachedPhases[PHASE_SUSTAIN].Step = 7 - SustainStep;

	if (CachedPhases[PHASE_SUSTAIN].Decr)
		CachedPhases[PHASE_SUSTAIN].Step = ~CachedPhases[PHASE_SUSTAIN].Step;

	CachedPhases[PHASE_SUSTAIN].Target = 0;

	CachedPhases[PHASE_RELEASE].Decr = true;
	CachedPhases[PHASE_RELEASE].Exp = ReleaseMode;
	CachedPhases[PHASE_RELEASE].Shift = ReleaseShift;
	CachedPhases[PHASE_RELEASE].Step = -8;
	CachedPhases[PHASE_RELEASE].Target = 0;
}

template <typename ClampValue>
static __forceinline bool CalculateAdsrImpl(V_ADSR& adsr, int voiceidx, ClampValue clamp_value)
{
	(void)voiceidx;
	pxAssume(adsr.Phase != V_ADSR::PHASE_STOPPED);

	auto& p = adsr.CachedPhases.at(adsr.Phase);

	// maybe not correct for the "infinite" settings
	u32 counter_inc = 0x8000 >> std::max(0, p.Shift - 11);
	s32 level_inc = p.Step << std::max(0, 11 - p.Shift);

	if (p.Exp)
	{
		if (!p.Decr && adsr.Value > 0x6000)
		{
			counter_inc >>= 2;
		}

		if (p.Decr)
		{
			level_inc = (s16)((level_inc * adsr.Value) >> 15);
		}
	}

	counter_inc = std::max<u32>(1, counter_inc);
	adsr.Counter += counter_inc;

	if (adsr.Counter >= 0x8000)
	{
		adsr.Counter = 0;
		adsr.Value = clamp_value(adsr.Value + level_inc);
	}

	// Stay in sustain until key off or silence
	if (adsr.Phase == V_ADSR::PHASE_SUSTAIN)
	{
		return adsr.Value != 0;
	}

	// Check if target is reached to advance phase
	if ((!p.Decr && adsr.Value >= p.Target) || (p.Decr && adsr.Value <= p.Target))
	{
		adsr.Phase++;
	}

	// All phases done, stop the voice
	if (adsr.Phase > V_ADSR::PHASE_RELEASE)
	{
		return false;
	}

	return true;
}

bool V_ADSR::Calculate(int voiceidx)
{
	return CalculateAdsrImpl(*this, voiceidx, ClampAdsrValue_selected);
}

void V_ADSR::Attack()
{
	Phase = PHASE_ATTACK;
	Counter = 0;
	Value = 0;
}

void V_ADSR::Release()
{
	if (Phase != PHASE_STOPPED)
	{
		Phase = PHASE_RELEASE;
		Counter = 0;
	}
}

void V_VolumeSlide::RegSet(u16 src)
{
	Reg_VOL = src;
	if (!Enable)
	{
		Value = SignExtend16(src << 1);
	}
}

template <typename ClampS16, typename ClampPositive, typename ClampNonPositive>
static __forceinline void UpdateVolumeSlideImpl(
	V_VolumeSlide& slide, ClampS16 clamp_s16, ClampPositive clamp_positive, ClampNonPositive clamp_non_positive)
{
	if (!slide.Enable)
		return;

	s32 step_size = 7 - slide.Step;

	if (slide.Decr)
	{
		step_size = ~step_size;
	}

	u32 counter_inc = 0x8000 >> std::max(0, slide.Shift - 11);
	s32 level_inc = step_size << std::max(0, 11 - slide.Shift);

	if (slide.Exp)
	{
		if (!slide.Decr && slide.Value > 0x6000)
		{
			counter_inc >>= 2;
		}

		if (slide.Decr)
		{
			level_inc = (s16)((level_inc * slide.Value) >> 15);
		}
	}

	// Allow counter_inc to be zero only in when all bits
	// of the rate field are set
	if (slide.Step != 3 && slide.Shift != 0x1f)
	{
		counter_inc = std::max<u32>(1, counter_inc);
	}
	slide.Counter += counter_inc;

	// If negative phase "increase" to -0x8000 or "decrease" towards 0
	// Unless in Exp + Decr modes
	if (!(slide.Exp && slide.Decr))
	{
		level_inc = slide.Phase ? -level_inc : level_inc;
	}

	if (slide.Counter >= 0x8000)
	{
		slide.Counter = 0;

		if (!slide.Decr)
		{
			slide.Value = clamp_s16(slide.Value + level_inc);
		}
		else if (slide.Exp || !slide.Phase)
		{
			slide.Value = clamp_positive(slide.Value + level_inc);
		}
		else
		{
			slide.Value = clamp_non_positive(slide.Value + level_inc);
		}
	}
}

void V_VolumeSlide::Update()
{
	UpdateVolumeSlideImpl(
		*this,
		ClampVolumeS16_selected,
		ClampVolumePositive_selected,
		ClampVolumeNonPositive_selected);
}

#if defined(VITASX2_QEMU_VALIDATION)
bool Spu2AdsrCalculateReferenceForValidation(V_ADSR* adsr, int voiceidx)
{
	return CalculateAdsrImpl(*adsr, voiceidx, ClampAdsrValue_reference);
}

bool Spu2AdsrCalculateSelectedForValidation(V_ADSR* adsr, int voiceidx)
{
	return adsr->Calculate(voiceidx);
}

void Spu2VolumeSlideUpdateReferenceForValidation(V_VolumeSlide* slide)
{
	UpdateVolumeSlideImpl(
		*slide,
		ClampS16_reference,
		ClampAdsrValue_reference,
		ClampNonPositiveS16_reference);
}

void Spu2VolumeSlideUpdateSelectedForValidation(V_VolumeSlide* slide)
{
	slide->Update();
}
#endif
