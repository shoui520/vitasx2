// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"

#include "common/BitUtils.h"
#include "common/Error.h"
#include "common/SettingsWrapper.h"
#include "common/Threading.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#if !defined(VITASX2_QEMU_VALIDATION)
#include <psp2/audioout.h>
#endif

#if defined(VITASX2_NATIVE_VALIDATION) && !defined(ARCH_ARM32)
class FreeSurroundDecoder
{
};
namespace soundtouch
{
	class SoundTouch
	{
	};
} // namespace soundtouch
#endif

namespace
{
	static constexpr const char* s_backend_names[] = {
		"Null",
		"Cubeb",
		"SDL",
	};

	static constexpr const char* s_expansion_mode_names[] = {
		"Disabled",
		"StereoLFE",
		"Quadraphonic",
		"QuadraphonicLFE",
		"Surround51",
		"Surround71",
	};

	static constexpr u8 s_expansion_input_channels[] = {2, 3, 5, 5, 6, 8};
	static constexpr u8 s_expansion_output_channels[] = {2, 3, 4, 5, 6, 8};

	static constexpr u32 VITA_AUDIO_OUTPUT_FRAMES = 512;
	static constexpr u32 VITA_AUDIO_CHANNELS = AudioStream::NUM_INPUT_CHANNELS;
	static constexpr u32 VITA_AUDIO_WORKER_STACK_SIZE = 16 * 1024;

	static s16 FloatToS16(float sample, float volume_scale)
	{
		if (!std::isfinite(sample))
			return 0;

		sample = std::clamp(sample * volume_scale, -1.0f, 1.0f);
		const float scaled = (sample < 0.0f) ? (sample * 32768.0f) : (sample * 32767.0f);
		return static_cast<s16>(scaled);
	}

	struct VitaAudioState
	{
		VitaAudioState(u32 sample_rate_, u32 buffer_size_, bool discard_output_)
			: sample_rate(sample_rate_)
			, buffer_size(AudioStream::GetAlignedBufferSize(buffer_size_))
			, samples(std::make_unique<AudioStream::SampleType[]>(buffer_size * VITA_AUDIO_CHANNELS))
			, discard_output(discard_output_)
		{
		}

		~VitaAudioState()
		{
			StopHardware();
		}

		u32 GetBufferedFramesRelaxed() const
		{
			if (discard_output)
				return 0;

			const u32 rpos = read_pos.load(std::memory_order_acquire);
			const u32 wpos = write_pos.load(std::memory_order_acquire);
			return (wpos >= rpos) ? (wpos - rpos) : (buffer_size - (rpos - wpos));
		}

		void EmptyBuffer()
		{
			staging_pos = 0;
			write_pos.store(read_pos.load(std::memory_order_acquire), std::memory_order_release);
		}

		void PushFrames(const AudioStream::SampleType* data, u32 num_frames)
		{
			if (discard_output || num_frames == 0)
				return;

			const u32 free_frames = buffer_size - GetBufferedFramesRelaxed();
			if (free_frames <= num_frames)
				return;

			u32 wpos = write_pos.load(std::memory_order_acquire);
			if ((buffer_size - wpos) <= num_frames)
			{
				const u32 end = buffer_size - wpos;
				const u32 start = num_frames - end;

				std::memcpy(&samples[wpos * VITA_AUDIO_CHANNELS], data, end * VITA_AUDIO_CHANNELS * sizeof(AudioStream::SampleType));
				if (start > 0)
					std::memcpy(samples.get(), data + end * VITA_AUDIO_CHANNELS, start * VITA_AUDIO_CHANNELS * sizeof(AudioStream::SampleType));

				wpos = start;
			}
			else
			{
				std::memcpy(&samples[wpos * VITA_AUDIO_CHANNELS], data, num_frames * VITA_AUDIO_CHANNELS * sizeof(AudioStream::SampleType));
				wpos += num_frames;
			}

			write_pos.store(wpos, std::memory_order_release);
		}

		u32 PopFramesToFloat(AudioStream::SampleType* output, u32 num_frames)
		{
			const u32 frames_to_read = std::min(GetBufferedFramesRelaxed(), num_frames);
			if (frames_to_read == 0)
			{
				std::fill_n(output, num_frames * VITA_AUDIO_CHANNELS, 0.0f);
				return 0;
			}

			u32 rpos = read_pos.load(std::memory_order_acquire);
			const u32 end = std::min(buffer_size - rpos, frames_to_read);
			if (end > 0)
			{
				std::memcpy(output, &samples[rpos * VITA_AUDIO_CHANNELS], end * VITA_AUDIO_CHANNELS * sizeof(AudioStream::SampleType));
				rpos += end;
				rpos = (rpos == buffer_size) ? 0 : rpos;
			}

			const u32 start = frames_to_read - end;
			if (start > 0)
			{
				std::memcpy(output + end * VITA_AUDIO_CHANNELS, samples.get(), start * VITA_AUDIO_CHANNELS * sizeof(AudioStream::SampleType));
				rpos = start;
			}

			read_pos.store(rpos, std::memory_order_release);

			if (frames_to_read < num_frames)
				std::fill_n(output + frames_to_read * VITA_AUDIO_CHANNELS, (num_frames - frames_to_read) * VITA_AUDIO_CHANNELS, 0.0f);

			return frames_to_read;
		}

		u32 PopFramesToS16(s16* output, u32 num_frames)
		{
			const u32 frames_to_read = std::min(GetBufferedFramesRelaxed(), num_frames);
			const float volume_scale = static_cast<float>(volume.load(std::memory_order_relaxed)) / 100.0f;
			u32 out_index = 0;

			u32 rpos = read_pos.load(std::memory_order_acquire);
			const u32 end = std::min(buffer_size - rpos, frames_to_read);
			for (u32 i = 0; i < end * VITA_AUDIO_CHANNELS; i++)
				output[out_index++] = FloatToS16(samples[rpos * VITA_AUDIO_CHANNELS + i], volume_scale);
			rpos += end;
			rpos = (rpos == buffer_size) ? 0 : rpos;

			const u32 start = frames_to_read - end;
			for (u32 i = 0; i < start * VITA_AUDIO_CHANNELS; i++)
				output[out_index++] = FloatToS16(samples[i], volume_scale);
			if (start > 0)
				rpos = start;

			read_pos.store(rpos, std::memory_order_release);

			if (frames_to_read < num_frames)
				std::fill_n(output + frames_to_read * VITA_AUDIO_CHANNELS, (num_frames - frames_to_read) * VITA_AUDIO_CHANNELS, 0);

			return frames_to_read;
		}

		bool StartHardware(Error* error)
		{
			if (discard_output)
				return true;

#if defined(VITASX2_QEMU_VALIDATION)
			return true;
#else
			port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VITA_AUDIO_OUTPUT_FRAMES, sample_rate, SCE_AUDIO_OUT_MODE_STEREO);
			if (port < 0)
			{
				Error::SetStringFmt(error, "sceAudioOutOpenPort({}, {}, stereo) failed: 0x{:08x}", VITA_AUDIO_OUTPUT_FRAMES, sample_rate, static_cast<u32>(port));
				return false;
			}

			int full_volume[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
			sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
				full_volume);

			running.store(true, std::memory_order_release);
			thread.SetStackSize(VITA_AUDIO_WORKER_STACK_SIZE);
			if (!thread.Start([this]() { AudioThread(); }))
			{
				running.store(false, std::memory_order_release);
				sceAudioOutReleasePort(port);
				port = -1;
				Error::SetStringView(error, "failed to start Vita audio worker thread");
				return false;
			}

			return true;
#endif
		}

		void StopHardware()
		{
			running.store(false, std::memory_order_release);
			if (thread.Joinable())
				thread.Join();

#if !defined(VITASX2_QEMU_VALIDATION)
			if (port >= 0)
			{
				sceAudioOutReleasePort(port);
				port = -1;
			}
#endif
		}

		void AudioThread()
		{
#if !defined(VITASX2_QEMU_VALIDATION)
			Threading::SetNameOfCurrentThread("Vita Audio");

			while (running.load(std::memory_order_acquire))
			{
				if (paused.load(std::memory_order_acquire))
					std::fill(output_buffer.begin(), output_buffer.end(), 0);
				else
					PopFramesToS16(output_buffer.data(), VITA_AUDIO_OUTPUT_FRAMES);

				sceAudioOutOutput(port, output_buffer.data());
			}
#endif
		}

		u32 sample_rate = 0;
		u32 buffer_size = 0;
		std::unique_ptr<AudioStream::SampleType[]> samples;
		std::array<AudioStream::SampleType, AudioStream::CHUNK_SIZE * VITA_AUDIO_CHANNELS> staging_buffer = {};
		u32 staging_pos = 0;
		std::atomic<u32> read_pos{0};
		std::atomic<u32> write_pos{0};
		std::atomic<u32> volume{100};
		std::atomic<bool> paused{false};
		std::atomic<bool> running{false};
		Threading::Thread thread;
		bool discard_output = false;

#if !defined(VITASX2_QEMU_VALIDATION)
		int port = -1;
		std::array<s16, VITA_AUDIO_OUTPUT_FRAMES * VITA_AUDIO_CHANNELS> output_buffer = {};
#endif
	};

	static std::mutex s_vita_audio_state_lock;
	static std::unordered_map<const AudioStream*, std::unique_ptr<VitaAudioState>> s_vita_audio_states;

	static VitaAudioState* FindVitaAudioState(const AudioStream* stream)
	{
		std::lock_guard lock(s_vita_audio_state_lock);
		const auto it = s_vita_audio_states.find(stream);
		return (it != s_vita_audio_states.end()) ? it->second.get() : nullptr;
	}

	static void AddVitaAudioState(const AudioStream* stream, std::unique_ptr<VitaAudioState> state)
	{
		std::lock_guard lock(s_vita_audio_state_lock);
		s_vita_audio_states.emplace(stream, std::move(state));
	}

	static void DestroyVitaAudioState(const AudioStream* stream)
	{
		std::unique_ptr<VitaAudioState> state;
		{
			std::lock_guard lock(s_vita_audio_state_lock);
			const auto it = s_vita_audio_states.find(stream);
			if (it == s_vita_audio_states.end())
				return;

			state = std::move(it->second);
			s_vita_audio_states.erase(it);
		}
	}

	static bool ShouldDiscardOutput(AudioBackend backend)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		return true;
#else
		return backend == AudioBackend::Null;
#endif
	}
} // namespace

AudioStream::DeviceInfo::DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_)
	: name(std::move(name_))
	, display_name(std::move(display_name_))
	, minimum_latency_frames(minimum_latency_)
{
}

AudioStream::DeviceInfo::~DeviceInfo() = default;

AudioStream::AudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: m_sample_rate(sample_rate)
	, m_parameters(parameters)
	, m_internal_channels(s_expansion_input_channels[static_cast<size_t>(parameters.expansion_mode)])
	, m_output_channels(s_expansion_output_channels[static_cast<size_t>(parameters.expansion_mode)])
	, m_buffer_size(GetBufferSizeForMS(sample_rate, parameters.buffer_ms))
	, m_target_buffer_size(m_buffer_size)
{
}

AudioStream::~AudioStream()
{
	DestroyVitaAudioState(this);
}

u32 AudioStream::GetAlignedBufferSize(u32 size)
{
	return Common::AlignUpPow2(size, CHUNK_SIZE);
}

u32 AudioStream::GetBufferSizeForMS(u32 sample_rate, u32 ms)
{
	return GetAlignedBufferSize((ms * sample_rate) / 1000u);
}

u32 AudioStream::GetMSForBufferSize(u32 sample_rate, u32 buffer_size)
{
	return (GetAlignedBufferSize(buffer_size) * 1000u) / sample_rate;
}

std::optional<AudioBackend> AudioStream::ParseBackendName(const char* str)
{
	for (u8 i = 0; i < static_cast<u8>(AudioBackend::Count); i++)
	{
		if (std::strcmp(str, s_backend_names[i]) == 0)
			return static_cast<AudioBackend>(i);
	}

	return std::nullopt;
}

const char* AudioStream::GetBackendName(AudioBackend backend)
{
	const u8 index = static_cast<u8>(backend);
	return (index < static_cast<u8>(AudioBackend::Count)) ? s_backend_names[index] : "";
}

const char* AudioStream::GetBackendDisplayName(AudioBackend backend)
{
	return GetBackendName(backend);
}

const char* AudioStream::GetExpansionModeName(AudioExpansionMode mode)
{
	const u8 index = static_cast<u8>(mode);
	return (index < static_cast<u8>(AudioExpansionMode::Count)) ? s_expansion_mode_names[index] : "";
}

const char* AudioStream::GetExpansionModeDisplayName(AudioExpansionMode mode)
{
	return GetExpansionModeName(mode);
}

std::optional<AudioExpansionMode> AudioStream::ParseExpansionMode(const char* name)
{
	for (u8 i = 0; i < static_cast<u8>(AudioExpansionMode::Count); i++)
	{
		if (std::strcmp(name, s_expansion_mode_names[i]) == 0)
			return static_cast<AudioExpansionMode>(i);
	}

	return std::nullopt;
}

u32 AudioStream::GetBufferedFramesRelaxed() const
{
	if (VitaAudioState* state = FindVitaAudioState(this))
		return state->GetBufferedFramesRelaxed();

	return 0;
}

void AudioStream::SetPaused(bool paused)
{
	m_paused = paused;
	if (VitaAudioState* state = FindVitaAudioState(this))
		state->paused.store(paused, std::memory_order_release);
}

void AudioStream::SetOutputVolume(u32 volume)
{
	m_volume = volume;
	if (VitaAudioState* state = FindVitaAudioState(this))
		state->volume.store(volume, std::memory_order_release);
}

void AudioStream::SetNominalRate(float tempo)
{
	m_nominal_rate = tempo;
}

void AudioStream::UpdateTargetTempo(float tempo)
{
	m_nominal_rate = tempo;
}

void AudioStream::SetStretchEnabled(bool enabled)
{
	m_stretch_enabled = false;
}

void AudioStream::BeginWrite(SampleType** buffer_ptr, u32* num_frames)
{
	VitaAudioState* state = FindVitaAudioState(this);
	if (!state)
	{
		if (buffer_ptr)
			*buffer_ptr = nullptr;
		if (num_frames)
			*num_frames = 0;
		return;
	}

	if (buffer_ptr)
		*buffer_ptr = &state->staging_buffer[state->staging_pos];
	if (num_frames)
		*num_frames = CHUNK_SIZE - (state->staging_pos / NUM_INPUT_CHANNELS);
}

void AudioStream::WriteFrame(const SampleType* frame)
{
	VitaAudioState* state = FindVitaAudioState(this);
	if (!state)
		return;

	const u32 remaining_frames = CHUNK_SIZE - (state->staging_pos / NUM_INPUT_CHANNELS);
	if (remaining_frames == 0)
		return;

	std::memcpy(&state->staging_buffer[state->staging_pos], frame, sizeof(SampleType) * NUM_INPUT_CHANNELS);
	EndWrite(1);
}

void AudioStream::EndWrite(u32 num_frames)
{
	VitaAudioState* state = FindVitaAudioState(this);
	if (!state || m_volume == 0)
		return;

	const u32 remaining_frames = CHUNK_SIZE - (state->staging_pos / NUM_INPUT_CHANNELS);
	num_frames = std::min(num_frames, remaining_frames);
	state->staging_pos += num_frames * NUM_INPUT_CHANNELS;
	if ((state->staging_pos / NUM_INPUT_CHANNELS) < CHUNK_SIZE)
		return;

	state->staging_pos = 0;
	WriteChunk(state->staging_buffer.data());
}

void AudioStream::WriteChunk(const SampleType* chunk)
{
	if (!IsExpansionEnabled() && !IsStretchEnabled())
	{
		InternalWriteFrames(chunk, CHUNK_SIZE);
		return;
	}
}

void AudioStream::EmptyBuffer()
{
	if (VitaAudioState* state = FindVitaAudioState(this))
		state->EmptyBuffer();
}

std::vector<std::pair<std::string, std::string>> AudioStream::GetDriverNames(AudioBackend backend)
{
	if (backend == AudioBackend::Null)
		return {};

	return {{"vita", "Vita Audio"}};
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetOutputDevices(AudioBackend backend, const char* driver)
{
	if (backend == AudioBackend::Null)
		return {};

	return {DeviceInfo("default", "Vita Audio", VITA_AUDIO_OUTPUT_FRAMES)};
}

std::unique_ptr<AudioStream> AudioStream::CreateStream(AudioBackend backend, u32 sample_rate, const AudioStreamParameters& parameters,
	const char* driver_name, const char* device_name, bool stretch_enabled, Error* error)
{
	AudioStreamParameters vita_parameters = parameters;
	vita_parameters.expansion_mode = AudioExpansionMode::Disabled;

	std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, vita_parameters));
	std::unique_ptr<VitaAudioState> state =
		std::make_unique<VitaAudioState>(sample_rate, stream->GetBufferSize(), ShouldDiscardOutput(backend));
	if (!state->StartHardware(error))
		return nullptr;

	state->volume.store(stream->GetOutputVolume(), std::memory_order_release);
	AddVitaAudioState(stream.get(), std::move(state));
	stream->SetStretchEnabled(false);
	return stream;
}

std::unique_ptr<AudioStream> AudioStream::CreateNullStream(u32 sample_rate, u32 buffer_ms)
{
	AudioStreamParameters params;
	params.buffer_ms = static_cast<u16>(buffer_ms);
	std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, params));
	AddVitaAudioState(stream.get(), std::make_unique<VitaAudioState>(sample_rate, stream->GetBufferSize(), true));
	stream->SetOutputVolume(0);
	return stream;
}

void AudioStream::BaseInitialize(SampleReader sample_reader, bool stretch_enabled)
{
	m_stretch_enabled = false;
}

void AudioStream::ReadFrames(SampleType* samples, u32 num_frames)
{
	VitaAudioState* state = FindVitaAudioState(this);
	if (!state || state->discard_output || state->paused.load(std::memory_order_acquire))
	{
		std::fill_n(samples, num_frames * NUM_INPUT_CHANNELS, 0.0f);
		return;
	}

	state->PopFramesToFloat(samples, num_frames);
	if (m_volume != 100)
	{
		const float volume_scale = static_cast<float>(m_volume) / 100.0f;
		for (u32 i = 0; i < num_frames * NUM_INPUT_CHANNELS; i++)
			samples[i] *= volume_scale;
	}
}

void AudioStream::StereoSampleReaderImpl(SampleType* dest, const SampleType* src, u32 num_frames)
{
	std::memcpy(dest, src, sizeof(SampleType) * num_frames * NUM_INPUT_CHANNELS);
}

void AudioStream::InternalWriteFrames(const SampleType* data, u32 num_frames)
{
	if (VitaAudioState* state = FindVitaAudioState(this))
		state->PushFrames(data, num_frames);
}

void AudioStream::AllocateBuffer()
{
}

void AudioStream::DestroyBuffer()
{
}

void AudioStream::ExpandAllocate()
{
}

void AudioStream::StretchAllocate()
{
}

void AudioStream::StretchDestroy()
{
}

void AudioStream::StretchWriteBlock(const float* block)
{
}

float AudioStream::AddAndGetAverageTempo(float val)
{
	return val;
}

void AudioStream::UpdateStretchTempo()
{
}

void AudioStream::StretchUnderrun()
{
}

void AudioStream::StretchOverrun()
{
}

void AudioStreamParameters::LoadSave(SettingsWrapper& wrap, const char* section)
{
	wrap.EnumEntry(section, "ExpansionMode", expansion_mode, &AudioStream::ParseExpansionMode, &AudioStream::GetExpansionModeName, DEFAULT_EXPANSION_MODE);
	minimal_output_latency = wrap.EntryBitBool(section, "OutputLatencyMinimal", DEFAULT_OUTPUT_LATENCY_MINIMAL);
	buffer_ms = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "BufferMS", buffer_ms, DEFAULT_BUFFER_MS), 0, std::numeric_limits<u16>::max()));
	output_latency_ms = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "OutputLatencyMS", output_latency_ms, DEFAULT_OUTPUT_LATENCY_MS), 0, std::numeric_limits<u16>::max()));

	stretch_sequence_length_ms = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "StretchSequenceLengthMS", DEFAULT_STRETCH_SEQUENCE_LENGTH), 0, std::numeric_limits<u16>::max()));
	stretch_seekwindow_ms = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "StretchSeekWindowMS", DEFAULT_STRETCH_SEEKWINDOW), 0, std::numeric_limits<u16>::max()));
	stretch_overlap_ms = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "StretchOverlapMS", DEFAULT_STRETCH_OVERLAP), 0, std::numeric_limits<u16>::max()));
	stretch_use_quickseek = wrap.EntryBitBool(section, "StretchUseQuickSeek", DEFAULT_STRETCH_USE_QUICKSEEK);
	stretch_use_aa_filter = wrap.EntryBitBool(section, "StretchUseAAFilter", DEFAULT_STRETCH_USE_AA_FILTER);

	expand_block_size = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "ExpandBlockSize", DEFAULT_EXPAND_BLOCK_SIZE), 0, std::numeric_limits<u16>::max()));
	wrap.Entry(section, "ExpandCircularWrap", expand_circular_wrap, DEFAULT_EXPAND_CIRCULAR_WRAP);
	wrap.Entry(section, "ExpandShift", expand_shift, DEFAULT_EXPAND_SHIFT);
	wrap.Entry(section, "ExpandDepth", expand_depth, DEFAULT_EXPAND_DEPTH);
	wrap.Entry(section, "ExpandFocus", expand_focus, DEFAULT_EXPAND_FOCUS);
	wrap.Entry(section, "ExpandCenterImage", expand_center_image, DEFAULT_EXPAND_CENTER_IMAGE);
	wrap.Entry(section, "ExpandFrontSeparation", expand_front_separation, DEFAULT_EXPAND_FRONT_SEPARATION);
	wrap.Entry(section, "ExpandRearSeparation", expand_rear_separation, DEFAULT_EXPAND_REAR_SEPARATION);
	expand_low_cutoff = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "ExpandLowCutoff", DEFAULT_EXPAND_LOW_CUTOFF), 0, std::numeric_limits<u8>::max()));
	expand_high_cutoff = static_cast<u16>(std::clamp<int>(wrap.EntryBitfield(section, "ExpandHighCutoff", DEFAULT_EXPAND_HIGH_CUTOFF), 0, std::numeric_limits<u8>::max()));

	if (wrap.IsLoading())
	{
		stretch_sequence_length_ms = std::clamp<u16>(stretch_sequence_length_ms, 20, 100);
		stretch_seekwindow_ms = std::clamp<u16>(stretch_seekwindow_ms, 10, 30);
		stretch_overlap_ms = std::clamp<u16>(stretch_overlap_ms, 5, 15);

		expand_block_size = std::clamp<u16>(std::has_single_bit(expand_block_size) ? expand_block_size : std::bit_ceil(expand_block_size), 128, 8192);
		expand_circular_wrap = std::clamp(expand_circular_wrap, 0.0f, 360.0f);
		expand_shift = std::clamp(expand_shift, -1.0f, 1.0f);
		expand_depth = std::clamp(expand_depth, 0.0f, 5.0f);
		expand_focus = std::clamp(expand_focus, -1.0f, 1.0f);
		expand_center_image = std::clamp(expand_center_image, 0.0f, 1.0f);
		expand_front_separation = std::clamp(expand_front_separation, 0.0f, 10.0f);
		expand_rear_separation = std::clamp(expand_rear_separation, 0.0f, 10.0f);
		expand_low_cutoff = std::min<u8>(expand_low_cutoff, 100);
		expand_high_cutoff = std::min<u8>(expand_high_cutoff, 100);
	}
}
