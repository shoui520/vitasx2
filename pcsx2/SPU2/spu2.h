// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SaveState.h"
#include "IopCounters.h"

#include <memory>

struct Pcsx2Config;
class StateWrapper;

class AudioStream;

namespace SPU2
{
/// PS2/Native Sample Rate.
static constexpr u32 SAMPLE_RATE = 48000;

/// PSX Mode Sample Rate.
static constexpr u32 PSX_SAMPLE_RATE = 44100;

/// Open/close, call at VM startup/shutdown.
bool Open();
void Close();

/// Serializes the SPU2 machine state without host pointers, C++ padding, or
/// host-sized fields. This is the PCSX2-owned content format used by the
/// x86-to-AArch32 validation replay route; normal .p2s states keep using
/// SPU2freeze().
bool DoPortableState(StateWrapper& sw);

/// Reset, rebooting VM or going into PSX mode.
void Reset(bool psxmode);

/// Identifies any configuration changes and applies them.
void CheckForConfigChanges(const Pcsx2Config& old_config);

/// Returns the current output volume, irrespective of the configuration.
u32 GetOutputVolume();

enum PeriodicDeadlineConstraint : u32
{
	PeriodicDeadlineConstraintNone = 0,
	PeriodicDeadlineConstraintIrq = 1u << 0,
	PeriodicDeadlineConstraintDma = 1u << 1,
	PeriodicDeadlineConstraintAutoDma = 1u << 2,
};

/// Reports SPU2-owned effects which constrain the next semantic deadline.
/// AutoDMA can still batch, but refill and completion edges bound its horizon.
/// IRQ observation requires the next exact sample.
u32 GetPeriodicDeadlineConstraints();

/// Returns the exact IOP-cycle distance to the next periodic SPU2 observation
/// required by the current IRQ/DMA state. Ordinary register and DMA accesses
/// remain synchronous observers.
u32 GetNextPeriodicUpdateDelta();

/// Materializes SPU2 state through the current IOP cycle.
void SynchronizeToIopCycle();

/// Shortens an already-published periodic deadline after a write or DMA start
/// introduces an earlier SPU2 observer.
void ReschedulePeriodicUpdate();

#if defined(VITASX2_QEMU_VALIDATION)
void VitaSetSpu2PeriodicBatchEnabledForValidation(bool enabled);
void VitaSetSpu2StoppedVoiceFastPathEnabledForValidation(bool enabled);
#endif

/// Directly updates the output volume without going through the configuration.
void SetOutputVolume(u32 volume);

/// Sets up muting and unmuting and reports success or failure.
bool SetOutputMuted(const bool muted);

/// Returns true if the output is muted (distinct from 0%).
bool IsOutputMuted();

/// Updates the current volume based on running state.
void UpdateOutputVolume();

/// Saves the current volume based on running state.
void SaveOutputVolume();

/// Pauses/resumes the output stream.
void SetOutputPaused(bool paused);

/// Clears output buffers in no-sync mode, prevents long delays after fast forwarding.
void OnTargetSpeedChanged();

/// Returns true if we're currently running in PSX mode.
bool IsRunningPSXMode();

/// Returns the current sample rate the SPU2 is operating at.
u32 GetConsoleSampleRate();

/// Tells SPU2 to forward audio packets to GSCapture.
void SetAudioCaptureActive(bool active);
bool IsAudioCaptureActive();
} // namespace SPU2

void SPU2write(u32 mem, u16 value);
u16 SPU2read(u32 mem);

void SPU2async();
s32 SPU2freeze(FreezeAction mode, freezeData* data);

void SPU2readDMA4Mem(u16* pMem, u32 size);
void SPU2writeDMA4Mem(u16* pMem, u32 size);
void SPU2interruptDMA4();
void SPU2interruptDMA7();
void SPU2readDMA7Mem(u16* pMem, u32 size);
void SPU2writeDMA7Mem(u16* pMem, u32 size);

extern u64 lClocks;

extern void CounterUpdate(u32 DMAICounter);
extern void TimeUpdate(u32 cClocks);
extern void SPU2_FastWrite(u32 rmem, u16 value);

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuSpu2VoiceVolumeSlideUpdated;
extern u32 g_qemuSpu2VoiceVolumeSlideSkipped;
extern u32 g_qemuSpu2MasterVolumeSlideUpdated;
extern u32 g_qemuSpu2MasterVolumeSlideSkipped;
extern u32 g_qemuSpu2ZeroVoiceGateSkipped;
extern u32 g_qemuSpu2NonzeroVoiceGateMixed;
extern u32 g_qemuSpu2DecodeFifoNeonStores;
extern u32 g_qemuSpu2DecodeFifoWrappedStores;
extern u32 g_qemuSpu2MixerIrqDisabledChecksSkipped;
extern u32 g_qemuSpu2PitchClampUsat;
extern u32 g_qemuSpu2StoppedVoiceFastSamples;
extern u64 g_qemuSpu2OutputHash;
extern u32 g_qemuSpu2OutputSamples;
extern u32 g_qemuSpu2DmaCopyNeonQwords;
extern u32 g_qemuSpu2DmaCopyNeon64ByteGroups;
extern u32 g_qemuSpu2DmaCopyNeon128ByteGroups;
extern u32 g_qemuSpu2DmaCopyNeon256ByteGroups;
extern u32 g_qemuSpu2DmaCopyNeon1024ByteGroups;
extern u32 g_qemuSpu2DmaCopyExactSpanCopies;
extern u32 g_qemuSpu2DmaCopyExact512ByteCopies;
extern u32 g_qemuSpu2DmaCopyExact1024ByteCopies;
extern u32 g_qemuSpu2DmaCopyExact1536ByteCopies;
extern u32 g_qemuSpu2DmaCopyExact2048ByteCopies;
#endif
