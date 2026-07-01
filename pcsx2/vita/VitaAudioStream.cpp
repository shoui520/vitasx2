// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"

#include "common/BitUtils.h"
#include "common/SettingsWrapper.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

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

AudioStream::~AudioStream() = default;

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
	return 0;
}

void AudioStream::SetPaused(bool paused)
{
	m_paused = paused;
}

void AudioStream::SetOutputVolume(u32 volume)
{
	m_volume = volume;
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
	m_stretch_enabled = enabled;
}

void AudioStream::BeginWrite(SampleType** buffer_ptr, u32* num_frames)
{
	if (buffer_ptr)
		*buffer_ptr = nullptr;
	if (num_frames)
		*num_frames = 0;
}

void AudioStream::WriteFrame(const SampleType* frame)
{
}

void AudioStream::EndWrite(u32 num_frames)
{
}

void AudioStream::WriteChunk(const SampleType* chunk)
{
}

void AudioStream::EmptyBuffer()
{
}

std::vector<std::pair<std::string, std::string>> AudioStream::GetDriverNames(AudioBackend backend)
{
	return {};
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetOutputDevices(AudioBackend backend, const char* driver)
{
	return {};
}

std::unique_ptr<AudioStream> AudioStream::CreateStream(AudioBackend backend, u32 sample_rate, const AudioStreamParameters& parameters,
	const char* driver_name, const char* device_name, bool stretch_enabled, Error* error)
{
	std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, parameters));
	stream->SetStretchEnabled(false);
	return stream;
}

std::unique_ptr<AudioStream> AudioStream::CreateNullStream(u32 sample_rate, u32 buffer_ms)
{
	AudioStreamParameters params;
	params.buffer_ms = static_cast<u16>(buffer_ms);
	std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, params));
	stream->SetOutputVolume(0);
	return stream;
}

void AudioStream::BaseInitialize(SampleReader sample_reader, bool stretch_enabled)
{
	m_stretch_enabled = false;
}

void AudioStream::ReadFrames(SampleType* samples, u32 num_frames)
{
	std::fill_n(samples, num_frames * NUM_INPUT_CHANNELS, 0.0f);
}

void AudioStream::StereoSampleReaderImpl(SampleType* dest, const SampleType* src, u32 num_frames)
{
	std::memcpy(dest, src, sizeof(SampleType) * num_frames * NUM_INPUT_CHANNELS);
}

void AudioStream::InternalWriteFrames(const SampleType* data, u32 num_frames)
{
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
