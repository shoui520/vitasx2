// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGsDrawTrace.h"

#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "common/Console.h"
#include "vita/VitaGxmTexture.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr u32 TRACE_MAGIC = 0x31534456; // "VDS1", little endian.
	constexpr u32 TRACE_VERSION = 11;
	constexpr u32 PACKET_TRACE_MAGIC = 0x31504756; // "VGP1", little endian.
	constexpr u32 PACKET_TRACE_VERSION = 2;
	constexpr u64 PACKET_STREAM_HASH_BASE = 1099511628211ull;
	constexpr u32 PRESENT_TRACE_MAGIC = 0x31525056; // "VPR1", little endian.
	constexpr u32 PRESENT_TRACE_VERSION = 1;
	constexpr size_t TRACE_BLOCK_RECORDS = 128;
	constexpr size_t TRACE_RT_CACHE_TEXTURES = 8;
	constexpr size_t TRACE_RT_TILE_COLUMNS = 4;
	constexpr size_t TRACE_RT_TILE_ROWS = 4;
	constexpr size_t TRACE_RT_TILES = TRACE_RT_TILE_COLUMNS * TRACE_RT_TILE_ROWS;
	// Canonical PCSX2 semantic layout. Do not derive these from sizeof():
	// GSVector4 has different internal alignment on AArch32 and x86-64.
	constexpr size_t TRACE_VS_WORDS = 12;
	constexpr size_t TRACE_PS_WORDS = 68;
	constexpr const char* TRACE_PATH = "ux0:data/vitasx2/gs-draw-trace.bin";
	constexpr const char* PACKET_TRACE_PATH =
		"ux0:data/vitasx2/gs-packet-trace.bin";
	constexpr const char* PRESENT_SOURCE_PATH =
		"ux0:data/vitasx2/gs-present-source.bin";
	constexpr const char* PRESENT_DISPLAY_PATH =
		"ux0:data/vitasx2/gs-present-display.bin";
	constexpr const char* TRACE_DONE_PATH =
		"ux0:data/vitasx2/gs-draw-trace.done";

	static_assert(VITASX2_GS_DRAW_TRACE_RECORDS > 0);
	static_assert(VITASX2_GS_DRAW_TRACE_SKIP_RECORDS >= 0);
	static_assert(VITASX2_GS_DRAW_TRACE_START_VSYNC >= 0);
	static_assert(VITASX2_GS_DRAW_TRACE_VSYNCS > 0);
	static_assert(VITASX2_GS_DRAW_TRACE_START_VSYNC <
		VITASX2_GS_DRAW_TRACE_VSYNCS);
	static_assert(VITASX2_GS_DRAW_TRACE_PACKETS >= 0);
	static_assert(VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC >= 0);

	struct TraceHeader
	{
		u32 magic = TRACE_MAGIC;
		u32 version = TRACE_VERSION;
		u32 header_size = sizeof(TraceHeader);
		u32 record_size = 0;
		u32 record_count = 0;
		u32 record_limit = VITASX2_GS_DRAW_TRACE_RECORDS;
		u32 skipped_records = VITASX2_GS_DRAW_TRACE_SKIP_RECORDS;
		u32 vsync_limit = VITASX2_GS_DRAW_TRACE_VSYNCS;
	};
	static_assert(sizeof(TraceHeader) == 32);

	struct PacketTraceHeader
	{
		u32 magic = PACKET_TRACE_MAGIC;
		u32 version = PACKET_TRACE_VERSION;
		u32 header_size = sizeof(PacketTraceHeader);
		u32 record_size = 0;
		u32 record_count = 0;
		u32 record_limit = VITASX2_GS_DRAW_TRACE_PACKETS;
		u32 seen_packets = 0;
		u32 reserved = 0;
	};
	static_assert(sizeof(PacketTraceHeader) == 32);

	struct PacketTraceRecord
	{
		u64 content_hash = 0;
		u64 stream_hash = 0;
		u64 worker_vsync = 0;
		u32 packet_index = 0;
		u32 qwords = 0;
		u8 source = 0xff;
		u8 mtvu_packet = 0;
		u16 reserved0 = 0;
		u32 reserved1 = 0;
	};
	static_assert(sizeof(PacketTraceRecord) == 40);

	struct PresentCaptureHeader
	{
		u32 magic = PRESENT_TRACE_MAGIC;
		u32 version = PRESENT_TRACE_VERSION;
		u32 header_size = sizeof(PresentCaptureHeader);
		u32 stage = 0;
		u32 vsync = 0;
		u32 width = 0;
		u32 height = 0;
		u32 pitch = 0;
		u32 bytes_per_pixel = 0;
		u32 texture_id = 0;
		u32 texture_state = 0;
		u32 writer_kind = 0;
		u64 content_generation = 0;
		u64 content_hash = 0;
		u64 channel_hash[4] = {};
		u64 tile_hash[TRACE_RT_TILES] = {};
		float source_rect[4] = {};
		float source_uv[4] = {};
		float destination_rect[4] = {};
		u32 content_bytes = 0;
		u32 channel_or = 0;
		u32 nonzero_tiles = 0;
		u32 status = 0;
		u64 writer_tfx_writes = 0;
		u64 writer_ps_lo = 0;
		u64 writer_ps_hi = 0;
		u64 writer_draw_area = 0;
		u64 writer_sample_area = 0;
		u32 writer_source_id = 0;
		u32 writer_source_size = 0;
		u32 writer_blend = 0;
		u32 writer_selector_keys = 0;
		u8 writer_topology = 0;
		u8 writer_color_mask = 0;
		u8 writer_last_rgb_topology = 0;
		u8 writer_last_rgb_color_mask = 0;
		u32 reserved = 0;
	};
	static_assert(sizeof(PresentCaptureHeader) == 352);

	struct TraceTexture
	{
		u32 id = 0;
		s16 width = 0;
		s16 height = 0;
		u8 levels = 0;
		u8 usage = 0;
		u8 format = 0;
		u8 state = 0;
	};
	static_assert(sizeof(TraceTexture) == 12);

	struct TraceRect
	{
		s32 x = 0;
		s32 y = 0;
		s32 z = 0;
		s32 w = 0;
	};
	static_assert(sizeof(TraceRect) == 16);

	struct TraceRecord
	{
		u64 ps_lo = 0;
		u64 ps_hi = 0;
		u64 cb_vs_hash = 0;
		u64 cb_ps_hash = 0;
		u64 vertex_hash = 0;
		u64 index_hash = 0;
		u64 tex_content_hash = 0;
		u64 pal_content_hash = 0;
		u64 backend_vertex_hash = 0;
		u64 backend_index_hash = 0;
		u64 bound_tex_content_hash = 0;
		u64 rt_source_content_hash = 0;
		u64 rt_source_generation = 0;
		u64 rt_source_channel_hash[4] = {};
		u64 rt_after_content_hash = 0;
		u64 rt_after_generation = 0;
		u64 rt_after_channel_hash[4] = {};
		u64 rt_after_tile_hash[TRACE_RT_TILES] = {};

		u32 tex_content_bytes = 0;
		u32 tex_channel_or = 0;
		u32 tex_content_generation = 0;
		u32 tex_content_levels = 0;
		u32 pal_content_bytes = 0;
		u32 pal_channel_or = 0;
		u32 pal_content_generation = 0;
		u32 pal_content_levels = 0;
		u32 bound_tex_id = 0;
		u32 bound_tex_content_bytes = 0;
		u32 bound_tex_channel_or = 0;
		u32 bound_tex_content_generation = 0;
		u32 bound_tex_content_levels = 0;
		u32 backend_index_count = 0;
		u32 backend_chunks = 0;
		u32 backend_sampler = 0;
		u32 backend_depth = 0;
		u32 backend_raster = 0;
		u32 backend_color_mask = 0;
		u32 backend_reserved = 0;
		u32 rt_source_content_bytes = 0;
		u32 rt_source_channel_or = 0;
		u32 rt_source_writer_draw = 0;
		u32 rt_source_status = 0;
		u32 rt_source_channel_writer[4] = {};
		u32 rt_after_content_bytes = 0;
		u32 rt_after_channel_or = 0;
		u32 rt_after_status = 0;
		u32 rt_after_nonzero_tiles = 0;

		u32 draw_index = 0;
		u32 frame = 0;
		u32 nverts = 0;
		u32 nindices = 0;
		u32 indices_per_prim = 0;
		u32 blend = 0;
		u32 detail = 0;
		u32 path_flags = 0;

		TraceTexture rt;
		TraceTexture ds;
		TraceTexture tex;
		TraceTexture pal;

		TraceRect scissor;
		TraceRect drawarea;
		TraceRect samplearea;

		u8 vs = 0;
		u8 sampler = 0;
		u8 colormask = 0;
		u8 depth = 0;
		u8 topology = 0;
		u8 tex_hazard = 0;
		u8 alpha_test = 0;
		u8 destination_alpha = 0;
		u8 datm = 0;
		u8 colclip_mode = 0;
		u8 outcome = 0;
		u8 flags = 0;
		u8 alpha_second_mask = 0;
		u8 alpha_second_depth = 0;
		u8 source = 0xff;
		u8 reserved1 = 0;
		u8 ps_fields[64] = {};
		// Preserve a small, fixed prefix of the logical GS vertex stream. Hashes
		// prove equality only when both backends use the same representation;
		// these words expose sprite/line expansion and provoking-color mistakes
		// when PCSX2 expands in its vertex shader but Vita expands on the CPU.
		u32 vertex_rgba[4] = {};
		u32 vertex_xy[4] = {};
		u32 vertex_z[4] = {};
		u32 vertex_uv[4] = {};
		u32 cb_vs_words[TRACE_VS_WORDS] = {};
		u32 cb_ps_words[TRACE_PS_WORDS] = {};
	};
	static_assert(sizeof(TraceRecord) == 1032);

	struct TextureContentSummary
	{
		u64 content_hash = 0;
		u32 content_bytes = 0;
		u32 channel_or = 0;
		u32 generation = 0;
		u32 known_levels = 0;
	};

	struct RenderTargetSnapshot
	{
		std::vector<u8> pixels;
		u64 generation = 0;
		u32 pitch = 0;
		u32 width = 0;
		u32 height = 0;
		u32 bytes_per_pixel = 0;
	};

	struct RenderTargetWriter
	{
		u64 generation = 0;
		u32 draw_index = 0;
		TraceRect drawarea;
		u8 color_mask = 0;
	};

	std::atomic<u32> s_requested_generation{0};
	u32 s_seen_generation = 0;
	std::FILE* s_trace_file = nullptr;
	std::array<TraceRecord, TRACE_BLOCK_RECORDS> s_records{};
	std::array<PacketTraceRecord, VITASX2_GS_DRAW_TRACE_PACKETS>
		s_packet_records{};
	size_t s_buffered_records = 0;
	u32 s_seen_records = 0;
	u32 s_written_records = 0;
	u32 s_vsyncs = 0;
	u32 s_rt_snapshot_count = 0;
	u32 s_rt_after_draw_count = 0;
	u32 s_seen_packets = 0;
	u32 s_packet_records_count = 0;
	std::unordered_map<u32, TextureContentSummary> s_texture_contents;
	std::unordered_map<u32, RenderTargetSnapshot> s_rt_snapshots;
	std::unordered_map<u32, std::vector<RenderTargetWriter>> s_rt_writers;
	const GSHWDrawConfig* s_current_config = nullptr;
	TraceRecord* s_current_record = nullptr;
	bool s_active = false;
	bool s_present_active = false;
	bool s_present_source_written = false;
	bool s_present_display_pending = false;
	const VitaGXM::GSTextureGXM* s_present_source_texture = nullptr;
	PresentCaptureHeader s_present_header{};

	u64 HashBytes(const void* data, size_t size)
	{
		if (!data || size == 0)
			return 0;
		const u8* bytes = static_cast<const u8*>(data);
		u64 hash = 14695981039346656037ull;
		for (size_t i = 0; i < size; i++)
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		return hash;
	}

	// Unlike FNV-1a, this ordered polynomial hash can be composed across
	// differently batched Transfer() calls. That lets the oracle prove that a
	// PCSX2 2+9-qword sequence is the same byte stream as one Vita 11-qword MTVU
	// packet without recording multi-megabyte packet payloads on the console.
	u64 HashPacketStream(const void* data, size_t size)
	{
		if (!data || size == 0)
			return 0;
		const u8* bytes = static_cast<const u8*>(data);
		u64 hash = 0;
		for (size_t i = 0; i < size; i++)
			hash = hash * PACKET_STREAM_HASH_BASE +
				static_cast<u64>(bytes[i]) + 1u;
		return hash;
	}

	void SummarizeLinearPixels(PresentCaptureHeader& header, const u8* pixels,
		u32 source_pitch)
	{
		if (!pixels || header.width == 0 || header.height == 0 ||
			header.bytes_per_pixel == 0)
		{
			header.status = 3;
			return;
		}
		header.content_hash = 14695981039346656037ull;
		for (u32 channel = 0; channel < 4; channel++)
			header.channel_hash[channel] = 14695981039346656037ull;
		for (u32 tile = 0; tile < TRACE_RT_TILES; tile++)
			header.tile_hash[tile] = 14695981039346656037ull;
		const u32 row_bytes = header.width * header.bytes_per_pixel;
		for (u32 y = 0; y < header.height; y++)
		{
			const u8* row = pixels + static_cast<size_t>(y) * source_pitch;
			const u32 tile_y = std::min<u32>(TRACE_RT_TILE_ROWS - 1,
				(y * TRACE_RT_TILE_ROWS) / header.height);
			for (u32 byte = 0; byte < row_bytes; byte++)
			{
				const u8 value = row[byte];
				header.content_hash =
					(header.content_hash ^ value) * 1099511628211ull;
				const u32 channel = byte % header.bytes_per_pixel;
				if (channel < 4)
				{
					header.channel_hash[channel] =
						(header.channel_hash[channel] ^ value) *
						1099511628211ull;
					header.channel_or |= static_cast<u32>(value) << (channel * 8);
				}
				const u32 pixel = byte / header.bytes_per_pixel;
				const u32 tile_x = std::min<u32>(TRACE_RT_TILE_COLUMNS - 1,
					(pixel * TRACE_RT_TILE_COLUMNS) / header.width);
				const u32 tile = tile_y * TRACE_RT_TILE_COLUMNS + tile_x;
				header.tile_hash[tile] =
					(header.tile_hash[tile] ^ value) * 1099511628211ull;
				if (value != 0)
					header.nonzero_tiles |= 1u << tile;
			}
		}
		header.content_bytes = row_bytes * header.height;
		header.pitch = row_bytes;
		header.status = 1;
	}

	bool WritePresentCapture(const char* path, PresentCaptureHeader& header,
		const u8* pixels, u32 source_pitch)
	{
		if (pixels)
			SummarizeLinearPixels(header, pixels, source_pitch);
		else if (header.status == 0)
			header.status = 3;
		std::FILE* file = std::fopen(path, "wb");
		if (!file)
			return false;
		bool wrote = std::fwrite(&header, sizeof(header), 1, file) == 1;
		const u32 row_bytes = header.width * header.bytes_per_pixel;
		for (u32 y = 0; pixels && wrote && y < header.height; y++)
		{
			wrote = std::fwrite(pixels + static_cast<size_t>(y) * source_pitch,
				row_bytes, 1, file) == 1;
		}
		wrote = wrote && std::fflush(file) == 0;
		std::fclose(file);
		return wrote;
	}

	void HashU32(u64& hash, u32 value)
	{
		for (u32 shift = 0; shift < 32; shift += 8)
			hash = (hash ^ static_cast<u8>(value >> shift)) * 1099511628211ull;
	}

	void CopyConstantWords(u32* destination, size_t word_offset,
		const void* source, size_t source_bytes)
	{
		std::memcpy(destination + word_offset, source, source_bytes);
	}

	void CopyTextureSummary(TraceRecord& record,
		const std::unordered_map<u32, TextureContentSummary>::const_iterator& summary,
		bool palette)
	{
		if (summary == s_texture_contents.end())
			return;
		if (palette)
		{
			record.pal_content_hash = summary->second.content_hash;
			record.pal_content_bytes = summary->second.content_bytes;
			record.pal_channel_or = summary->second.channel_or;
			record.pal_content_generation = summary->second.generation;
			record.pal_content_levels = summary->second.known_levels;
		}
		else
		{
			record.tex_content_hash = summary->second.content_hash;
			record.tex_content_bytes = summary->second.content_bytes;
			record.tex_channel_or = summary->second.channel_or;
			record.tex_content_generation = summary->second.generation;
			record.tex_content_levels = summary->second.known_levels;
		}
	}

	TraceTexture DescribeTexture(const GSTexture* texture)
	{
		TraceTexture result{};
		if (!texture)
			return result;
		const auto* gxm = static_cast<const VitaGXM::GSTextureGXM*>(texture);
		result.id = gxm->TelemetryId();
		result.width = static_cast<s16>(texture->GetWidth());
		result.height = static_cast<s16>(texture->GetHeight());
		result.levels = static_cast<u8>(texture->GetMipmapLevels());
		result.usage = static_cast<u8>(texture->GetUsage());
		result.format = static_cast<u8>(texture->GetFormat());
		result.state = static_cast<u8>(texture->GetState());
		return result;
	}

	TraceRect DescribeRect(const GSVector4i& rect)
	{
		return {rect.x, rect.y, rect.z, rect.w};
	}

	RenderTargetSnapshot* SnapshotRenderTarget(
		const VitaGXM::GSTextureGXM& texture, u64 content_generation,
		u32* status)
	{
		if (status)
			*status = 0;
		if (!texture.IsRenderTarget() ||
			texture.GetFormat() != GSTexture::Format::Color)
		{
			if (status)
				*status = 3;
			return nullptr;
		}

		const u32 id = texture.TelemetryId();
		if (s_rt_snapshots.find(id) == s_rt_snapshots.end() &&
			s_rt_snapshots.size() >= TRACE_RT_CACHE_TEXTURES)
		{
			// Bounded diagnostic storage. A later use of the evicted target simply
			// obtains a fresh synchronized snapshot.
			s_rt_snapshots.erase(s_rt_snapshots.begin());
		}
		RenderTargetSnapshot& snapshot = s_rt_snapshots[id];
		if (snapshot.generation == content_generation && !snapshot.pixels.empty())
			return &snapshot;
		if (s_rt_snapshot_count >=
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_RT_SNAPSHOTS))
		{
			if (status)
				*status = 2;
			return nullptr;
		}

		const u32 width = static_cast<u32>(texture.GetWidth());
		const u32 height = static_cast<u32>(texture.GetHeight());
		const u32 bytes_per_pixel = texture.NativeFormat().bytes_per_pixel;
		if (width == 0 || height == 0 || bytes_per_pixel == 0 ||
			width > std::numeric_limits<u32>::max() / bytes_per_pixel)
		{
			if (status)
				*status = 3;
			return nullptr;
		}
		const u32 pitch = width * bytes_per_pixel;
		const u64 size = static_cast<u64>(pitch) * height;
		if (size > std::numeric_limits<size_t>::max())
		{
			if (status)
				*status = 3;
			return nullptr;
		}
		snapshot.pixels.resize(static_cast<size_t>(size));
		if (!texture.CopyToLinear(0, GSVector4i(0, 0, width, height),
			snapshot.pixels.data(), pitch))
		{
			snapshot.pixels.clear();
			if (status)
				*status = 4;
			return nullptr;
		}
		snapshot.generation = content_generation;
		snapshot.pitch = pitch;
		snapshot.width = width;
		snapshot.height = height;
		snapshot.bytes_per_pixel = bytes_per_pixel;
		s_rt_snapshot_count++;
		return &snapshot;
	}

	void HashRenderTargetRect(const RenderTargetSnapshot& snapshot,
		const GSVector4i& requested_rect, u64* content_hash,
		u64 channel_hash[4], u32* content_bytes, u32* channel_or,
		u64 tile_hash[TRACE_RT_TILES], u32* nonzero_tiles)
	{
		const GSVector4i rect = requested_rect.rintersect(
			GSVector4i(0, 0, snapshot.width, snapshot.height));
		if (rect.rempty())
			return;
		*content_hash = 14695981039346656037ull;
		for (u32 channel = 0; channel < 4; channel++)
			channel_hash[channel] = 14695981039346656037ull;
		if (tile_hash)
		{
			for (u32 tile = 0; tile < TRACE_RT_TILES; tile++)
				tile_hash[tile] = 14695981039346656037ull;
		}
		const u32 row_bytes = static_cast<u32>(rect.width()) *
			snapshot.bytes_per_pixel;
		for (int y = rect.y; y < rect.w; y++)
		{
			const u8* row = snapshot.pixels.data() +
				static_cast<size_t>(y) * snapshot.pitch +
				static_cast<size_t>(rect.x) * snapshot.bytes_per_pixel;
			const u32 tile_y = std::min<u32>(TRACE_RT_TILE_ROWS - 1,
				(static_cast<u32>(y - rect.y) * TRACE_RT_TILE_ROWS) /
				static_cast<u32>(rect.height()));
			for (u32 byte = 0; byte < row_bytes; byte++)
			{
				const u8 value = row[byte];
				*content_hash = (*content_hash ^ value) * 1099511628211ull;
				const u32 channel = byte % snapshot.bytes_per_pixel;
				if (channel < 4)
				{
					channel_hash[channel] =
						(channel_hash[channel] ^ value) * 1099511628211ull;
					*channel_or |= static_cast<u32>(value) << (channel * 8);
				}
				if (tile_hash)
				{
					const u32 pixel = byte / snapshot.bytes_per_pixel;
					const u32 tile_x = std::min<u32>(TRACE_RT_TILE_COLUMNS - 1,
						(pixel * TRACE_RT_TILE_COLUMNS) /
						static_cast<u32>(rect.width()));
					const u32 tile = tile_y * TRACE_RT_TILE_COLUMNS + tile_x;
					tile_hash[tile] =
						(tile_hash[tile] ^ value) * 1099511628211ull;
					if (value != 0 && nonzero_tiles)
						*nonzero_tiles |= 1u << tile;
				}
			}
		}
		*content_bytes = row_bytes * static_cast<u32>(rect.height());
	}

	bool FlushRecords()
	{
		if (!s_trace_file || s_buffered_records == 0)
			return true;
		const size_t written = std::fwrite(s_records.data(), sizeof(TraceRecord),
			s_buffered_records, s_trace_file);
		if (written != s_buffered_records || std::fflush(s_trace_file) != 0)
		{
			Console.Error("Vita GS draw trace write failed after %u records.",
				s_written_records);
			return false;
		}
		s_written_records += static_cast<u32>(s_buffered_records);
		s_buffered_records = 0;
		s_current_config = nullptr;
		s_current_record = nullptr;
		return true;
	}

	bool FlushPacketRecords()
	{
		if constexpr (VITASX2_GS_DRAW_TRACE_PACKETS == 0)
			return true;
		std::FILE* packet_file = std::fopen(PACKET_TRACE_PATH, "wb");
		if (!packet_file)
		{
			Console.Error("Vita GS packet trace could not open %s.",
				PACKET_TRACE_PATH);
			return false;
		}
		PacketTraceHeader header;
		header.record_size = sizeof(PacketTraceRecord);
		header.record_count = s_packet_records_count;
		header.seen_packets = s_seen_packets;
		const bool wrote =
			std::fwrite(&header, sizeof(header), 1, packet_file) == 1 &&
			(s_packet_records_count == 0 ||
				std::fwrite(s_packet_records.data(), sizeof(PacketTraceRecord),
					s_packet_records_count, packet_file) == s_packet_records_count) &&
			std::fflush(packet_file) == 0;
		std::fclose(packet_file);
		if (!wrote)
			Console.Error("Vita GS packet trace write failed after %u packets.",
				s_packet_records_count);
		return wrote;
	}

	void CloseTrace(bool publish_completion)
	{
		if (!s_active)
			return;
		const bool flushed = FlushRecords();
		const bool packets_flushed = FlushPacketRecords();
		if (s_trace_file)
		{
			TraceHeader header;
			header.record_size = sizeof(TraceRecord);
			header.record_count = s_written_records;
			if (flushed && std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			{
				std::fwrite(&header, sizeof(header), 1, s_trace_file);
				std::fflush(s_trace_file);
			}
			std::fclose(s_trace_file);
			s_trace_file = nullptr;
		}
		s_active = false;
		if (publish_completion && flushed && packets_flushed)
		{
			if (std::FILE* done = std::fopen(TRACE_DONE_PATH, "wb"))
			{
				std::fprintf(done,
					"version=%u records=%u seen=%u vsyncs=%u packets=%u "
					"packets_seen=%u\n",
					TRACE_VERSION, s_written_records, s_seen_records, s_vsyncs,
					s_packet_records_count, s_seen_packets);
				std::fclose(done);
			}
			Console.WriteLn(
				"Vita GS draw trace complete: records=%u seen=%u vsyncs=%u.",
				s_written_records, s_seen_records, s_vsyncs);
		}
	}

	void StartIfRequested()
	{
		const u32 generation =
			s_requested_generation.load(std::memory_order_acquire);
		if (generation == s_seen_generation)
			return;
		if (s_active)
			CloseTrace(false);
		s_seen_generation = generation;
		s_seen_records = 0;
		s_written_records = 0;
		s_buffered_records = 0;
		s_vsyncs = 0;
		s_rt_snapshot_count = 0;
		s_rt_after_draw_count = 0;
		s_seen_packets = 0;
		s_packet_records_count = 0;
		s_present_source_written = false;
		s_present_display_pending = false;
		s_present_source_texture = nullptr;
		s_present_header = {};
		s_present_active =
			VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC > 0;
		std::remove(PRESENT_SOURCE_PATH);
		std::remove(PRESENT_DISPLAY_PATH);
		s_current_config = nullptr;
		s_current_record = nullptr;
		s_texture_contents.clear();
		s_rt_snapshots.clear();
		s_rt_writers.clear();
		s_trace_file = std::fopen(TRACE_PATH, "wb");
		if (!s_trace_file)
		{
			Console.Error("Vita GS draw trace could not open %s.", TRACE_PATH);
			return;
		}
		TraceHeader header;
		header.record_size = sizeof(TraceRecord);
		if (std::fwrite(&header, sizeof(header), 1, s_trace_file) != 1 ||
			std::fflush(s_trace_file) != 0)
		{
			std::fclose(s_trace_file);
			s_trace_file = nullptr;
			Console.Error("Vita GS draw trace header write failed.");
			return;
		}
		s_active = true;
		Console.WriteLn(
			"Vita GS draw trace armed at ELF entry: records=%u skip=%u "
			"start_vsync=%u vsync_limit=%u rt_snapshots=%u "
			"rt_after_draws=%u packets=%u present_vsync=%u.",
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_RECORDS),
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_SKIP_RECORDS),
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_START_VSYNC),
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_VSYNCS),
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_RT_SNAPSHOTS),
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_RT_AFTER_DRAWS),
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_PACKETS),
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC));
		if constexpr (VITASX2_GS_DRAW_TRACE_PACKETS > 0)
		{
			Console.WriteLn("Vita GS packet trace armed: packets=%u.",
				static_cast<u32>(VITASX2_GS_DRAW_TRACE_PACKETS));
		}
	}
}

void VitaGsDrawTraceNotifyElfEntry()
{
	s_requested_generation.fetch_add(1, std::memory_order_release);
}

void VitaGsDrawTraceVSync()
{
	StartIfRequested();
	if (!s_active && !s_present_active)
		return;
	s_vsyncs++;
	if (s_active &&
		s_vsyncs >= static_cast<u32>(VITASX2_GS_DRAW_TRACE_VSYNCS))
		CloseTrace(true);
}

void VitaGsDrawTraceRecordPacket(u8 source, const void* data, u32 size,
	bool mtvu_packet)
{
	StartIfRequested();
	if (!s_active)
		return;
	s_seen_packets++;
	if constexpr (VITASX2_GS_DRAW_TRACE_PACKETS == 0)
		return;
	if (s_vsyncs < static_cast<u32>(VITASX2_GS_DRAW_TRACE_START_VSYNC))
		return;
	if (s_packet_records_count >=
		static_cast<u32>(VITASX2_GS_DRAW_TRACE_PACKETS))
	{
		return;
	}
	PacketTraceRecord& record = s_packet_records[s_packet_records_count++];
	record.content_hash = HashBytes(data, size);
	record.stream_hash = HashPacketStream(data, size);
	record.worker_vsync = s_vsyncs;
	record.packet_index = s_seen_packets - 1;
	record.qwords = size / 16;
	record.source = source;
	record.mtvu_packet = mtvu_packet ? 1 : 0;
}

void VitaGsDrawTraceRecordPresentSource(const GSTexture* texture,
	const GSVector4i& source_rect, const GSVector4& source_uv,
	const GSVector4& destination_rect)
{
	if constexpr (VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC == 0)
		return;
	if (!s_present_active || s_present_source_written ||
		s_present_source_texture ||
		!texture ||
		s_vsyncs < static_cast<u32>(VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC))
	{
		return;
	}

	const auto* const source =
		static_cast<const VitaGXM::GSTextureGXM*>(texture);
	s_present_header = {};
	s_present_header.stage = 1;
	s_present_header.vsync = s_vsyncs;
	s_present_header.texture_id = source->TelemetryId();
	s_present_header.texture_state = static_cast<u32>(source->GetState());
	s_present_header.source_rect[0] = static_cast<float>(source_rect.x);
	s_present_header.source_rect[1] = static_cast<float>(source_rect.y);
	s_present_header.source_rect[2] = static_cast<float>(source_rect.z);
	s_present_header.source_rect[3] = static_cast<float>(source_rect.w);
	s_present_header.source_uv[0] = source_uv.x;
	s_present_header.source_uv[1] = source_uv.y;
	s_present_header.source_uv[2] = source_uv.z;
	s_present_header.source_uv[3] = source_uv.w;
	s_present_header.destination_rect[0] = destination_rect.x;
	s_present_header.destination_rect[1] = destination_rect.y;
	s_present_header.destination_rect[2] = destination_rect.z;
	s_present_header.destination_rect[3] = destination_rect.w;

	const VitaGXM::TextureWriterTelemetry& writer = source->WriterTelemetry();
	s_present_header.writer_kind = static_cast<u32>(writer.kind);
	s_present_header.content_generation = writer.content_generation;
	s_present_header.writer_tfx_writes = writer.tfx_writes;
	s_present_header.writer_ps_lo = writer.ps_lo;
	s_present_header.writer_ps_hi = writer.ps_hi;
	s_present_header.writer_draw_area = writer.draw_area;
	s_present_header.writer_sample_area = writer.sample_area;
	s_present_header.writer_source_id = writer.source_id;
	s_present_header.writer_source_size = writer.source_size;
	s_present_header.writer_blend = writer.blend;
	s_present_header.writer_selector_keys = writer.selector_keys;
	s_present_header.writer_topology = writer.topology;
	s_present_header.writer_color_mask = writer.color_mask;
	s_present_header.writer_last_rgb_topology = writer.last_rgb_topology;
	s_present_header.writer_last_rgb_color_mask = writer.last_rgb_color_mask;
	s_present_source_texture = source;
}

bool VitaGsDrawTraceNeedsPresentSourceCapture()
{
	return s_present_active && s_present_source_texture &&
		!s_present_source_written;
}

void VitaGsDrawTraceCapturePresentSource()
{
	if (!VitaGsDrawTraceNeedsPresentSourceCapture())
		return;
	const VitaGXM::GSTextureGXM* const source = s_present_source_texture;
	s_present_source_texture = nullptr;
	u32 status = 0;
	const RenderTargetSnapshot* const snapshot = SnapshotRenderTarget(*source,
		s_present_header.content_generation, &status);
	if (snapshot)
	{
		s_present_header.width = snapshot->width;
		s_present_header.height = snapshot->height;
		s_present_header.bytes_per_pixel = snapshot->bytes_per_pixel;
	}
	else
	{
		s_present_header.width = static_cast<u32>(source->GetWidth());
		s_present_header.height = static_cast<u32>(source->GetHeight());
		s_present_header.bytes_per_pixel = source->NativeFormat().bytes_per_pixel;
		s_present_header.status = status;
	}
	const bool wrote = WritePresentCapture(PRESENT_SOURCE_PATH,
		s_present_header, snapshot ? snapshot->pixels.data() : nullptr,
		snapshot ? snapshot->pitch : 0);
	s_present_source_written = true;
	s_present_display_pending = true;
	if (wrote)
	{
		Console.WriteLn(
			"Vita GS present source captured: vsync=%u texture=%u size=%ux%u "
			"hash=%016llx status=%u.",
			s_present_header.vsync, s_present_header.texture_id,
			s_present_header.width, s_present_header.height,
			static_cast<unsigned long long>(s_present_header.content_hash),
			s_present_header.status);
	}
	else
	{
		Console.Error("Vita GS present source capture write failed.");
	}
}

bool VitaGsDrawTraceNeedsDisplayCapture()
{
	return s_present_active && s_present_display_pending;
}

void VitaGsDrawTraceRecordDisplay(const void* pixels, u32 pitch,
	u32 width, u32 height, u32 bytes_per_pixel)
{
	if (!VitaGsDrawTraceNeedsDisplayCapture())
		return;
	s_present_display_pending = false;
	s_present_active = false;
	PresentCaptureHeader display = s_present_header;
	display.stage = 2;
	display.width = width;
	display.height = height;
	display.pitch = 0;
	display.bytes_per_pixel = bytes_per_pixel;
	display.content_generation = 0;
	display.content_hash = 0;
	std::memset(display.channel_hash, 0, sizeof(display.channel_hash));
	std::memset(display.tile_hash, 0, sizeof(display.tile_hash));
	display.content_bytes = 0;
	display.channel_or = 0;
	display.nonzero_tiles = 0;
	display.status = 0;
	const bool wrote = WritePresentCapture(PRESENT_DISPLAY_PATH, display,
		static_cast<const u8*>(pixels), pitch);
	if (wrote)
	{
		Console.WriteLn(
			"Vita GS display captured: vsync=%u size=%ux%u hash=%016llx "
			"status=%u.",
			display.vsync, display.width, display.height,
			static_cast<unsigned long long>(display.content_hash), display.status);
	}
	else
	{
		Console.Error("Vita GS display capture write failed.");
	}
}

void VitaGsDrawTraceRecordConfig(u32 draw_index, u32 frame, u8 source,
	const GSHWDrawConfig& config)
{
	StartIfRequested();
	if (!s_active)
		return;
	if (s_vsyncs < static_cast<u32>(VITASX2_GS_DRAW_TRACE_START_VSYNC))
		return;
	if (s_written_records + s_buffered_records >=
		static_cast<u32>(VITASX2_GS_DRAW_TRACE_RECORDS))
	{
		CloseTrace(true);
		return;
	}
	s_seen_records++;
	if (s_seen_records <=
		static_cast<u32>(VITASX2_GS_DRAW_TRACE_SKIP_RECORDS))
	{
		return;
	}
	if (s_buffered_records == s_records.size() && !FlushRecords())
	{
		CloseTrace(false);
		return;
	}

	TraceRecord& record = s_records[s_buffered_records++];
	record = {};
	record.source = source;
	record.ps_lo = config.ps.key_lo;
	record.ps_hi = config.ps.key_hi;
	record.cb_vs_hash = HashBytes(&config.cb_vs, sizeof(config.cb_vs));
	record.cb_ps_hash = HashBytes(&config.cb_ps, sizeof(config.cb_ps));
	record.vertex_hash = HashBytes(config.verts,
		static_cast<size_t>(config.nverts) * sizeof(GSVertex));
	record.index_hash = HashBytes(config.indices,
		static_cast<size_t>(config.nindices) * sizeof(u16));
	for (u32 i = 0; i < std::min<u32>(config.nverts, 4); i++)
	{
		const GSVertex& vertex = config.verts[i];
		record.vertex_rgba[i] = vertex.RGBAQ.U32[0];
		record.vertex_xy[i] = static_cast<u32>(vertex.XYZ.X) |
			(static_cast<u32>(vertex.XYZ.Y) << 16);
		record.vertex_z[i] = vertex.XYZ.Z;
		record.vertex_uv[i] = vertex.UV;
	}
	if (config.tex)
	{
		const auto* texture =
			static_cast<const VitaGXM::GSTextureGXM*>(config.tex);
		CopyTextureSummary(record,
			s_texture_contents.find(texture->TelemetryId()), false);
	}
	if (config.pal)
	{
		const auto* palette =
			static_cast<const VitaGXM::GSTextureGXM*>(config.pal);
		CopyTextureSummary(record,
			s_texture_contents.find(palette->TelemetryId()), true);
	}
	record.draw_index = draw_index;
	record.frame = frame;
	record.nverts = config.nverts;
	record.nindices = config.nindices;
	record.indices_per_prim = config.indices_per_prim;
	record.blend = config.blend.key;
	record.rt = DescribeTexture(config.rt);
	record.ds = DescribeTexture(config.ds);
	record.tex = DescribeTexture(config.tex);
	record.pal = DescribeTexture(config.pal);
	record.scissor = DescribeRect(config.scissor);
	record.drawarea = DescribeRect(config.drawarea);
	record.samplearea = DescribeRect(config.samplearea);
	record.vs = config.vs.key;
	record.sampler = config.sampler.key;
	record.colormask = config.colormask.key;
	record.depth = config.depth.key;
	record.topology = static_cast<u8>(config.topology);
	record.tex_hazard = static_cast<u8>(config.tex_hazard);
	record.alpha_test = static_cast<u8>(config.alpha_test);
	record.destination_alpha = static_cast<u8>(config.destination_alpha);
	record.datm = static_cast<u8>(config.datm);
	record.colclip_mode = static_cast<u8>(config.colclip_mode);
	record.alpha_second_mask = config.alpha_second_pass.colormask.key;
	record.alpha_second_depth = config.alpha_second_pass.depth.key;
	record.flags = (config.require_one_barrier ? 1u : 0u) |
		(config.require_full_barrier ? 2u : 0u) |
		(config.line_expand ? 4u : 0u) |
		(config.alpha_second_pass.enable ? 8u : 0u) |
		(config.blend_multi_pass.enable ? 16u : 0u) |
		(config.ps.no_color ? 32u : 0u) |
		(config.ps.no_color1 ? 64u : 0u);
	const u8 ps_fields[64] = {
		static_cast<u8>(config.ps.aem_fmt),
		static_cast<u8>(config.ps.pal_fmt),
		static_cast<u8>(config.ps.dst_fmt),
		static_cast<u8>(config.ps.depth_fmt),
		static_cast<u8>(config.ps.aem),
		static_cast<u8>(config.ps.fba),
		static_cast<u8>(config.ps.fog),
		static_cast<u8>(config.ps.iip),
		static_cast<u8>(config.ps.date),
		static_cast<u8>(config.ps.atst),
		static_cast<u8>(config.ps.afail),
		static_cast<u8>(config.ps.ztst),
		static_cast<u8>(config.ps.fst),
		static_cast<u8>(config.ps.tfx),
		static_cast<u8>(config.ps.tcc),
		static_cast<u8>(config.ps.wms),
		static_cast<u8>(config.ps.wmt),
		static_cast<u8>(config.ps.adjs),
		static_cast<u8>(config.ps.adjt),
		static_cast<u8>(config.ps.ltf),
		static_cast<u8>(config.ps.shuffle),
		static_cast<u8>(config.ps.shuffle_same),
		static_cast<u8>(config.ps.real16src),
		static_cast<u8>(config.ps.process_ba),
		static_cast<u8>(config.ps.process_rg),
		static_cast<u8>(config.ps.shuffle_across),
		static_cast<u8>(config.ps.write_rg),
		static_cast<u8>(config.ps.fbmask),
		static_cast<u8>(config.ps.blend_a),
		static_cast<u8>(config.ps.blend_b),
		static_cast<u8>(config.ps.blend_c),
		static_cast<u8>(config.ps.blend_d),
		static_cast<u8>(config.ps.fixed_one_a),
		static_cast<u8>(config.ps.blend_hw),
		static_cast<u8>(config.ps.a_masked),
		static_cast<u8>(config.ps.colclip_hw),
		static_cast<u8>(config.ps.rta_correction),
		static_cast<u8>(config.ps.rta_source_correction),
		static_cast<u8>(config.ps.colclip),
		static_cast<u8>(config.ps.blend_mix),
		static_cast<u8>(config.ps.round_inv),
		static_cast<u8>(config.ps.pabe),
		static_cast<u8>(config.ps.no_color),
		static_cast<u8>(config.ps.no_color1),
		static_cast<u8>(config.ps.channel),
		static_cast<u8>(config.ps.dither),
		static_cast<u8>(config.ps.dither_adjust),
		static_cast<u8>(config.ps.zclamp),
		static_cast<u8>(config.ps.zfloor),
		static_cast<u8>(config.ps.tcoffsethack),
		static_cast<u8>(config.ps.tex_is_fb),
		static_cast<u8>(config.ps.automatic_lod),
		static_cast<u8>(config.ps.manual_lod),
		static_cast<u8>(config.ps.point_sampler),
		static_cast<u8>(config.ps.region_rect),
		static_cast<u8>(config.ps.scanmsk),
		static_cast<u8>(config.ps.aa1),
		static_cast<u8>(config.ps.abe),
		static_cast<u8>(config.ps.sw_aniso),
		static_cast<u8>(config.ps.rov_color),
		static_cast<u8>(config.ps.rov_depth),
		static_cast<u8>(config.ps.urban_chaos_hle),
		static_cast<u8>(config.ps.tales_of_abyss_hle),
		0,
	};
	std::memcpy(record.ps_fields, ps_fields, sizeof(record.ps_fields));
	CopyConstantWords(record.cb_vs_words, 0,
		&config.cb_vs.vertex_scale, sizeof(config.cb_vs.vertex_scale));
	CopyConstantWords(record.cb_vs_words, 2,
		&config.cb_vs.vertex_offset, sizeof(config.cb_vs.vertex_offset));
	CopyConstantWords(record.cb_vs_words, 4,
		&config.cb_vs.texture_scale, sizeof(config.cb_vs.texture_scale));
	CopyConstantWords(record.cb_vs_words, 6,
		&config.cb_vs.texture_offset, sizeof(config.cb_vs.texture_offset));
	CopyConstantWords(record.cb_vs_words, 8,
		&config.cb_vs.point_size, sizeof(config.cb_vs.point_size));
	CopyConstantWords(record.cb_vs_words, 10,
		&config.cb_vs.max_depth, sizeof(config.cb_vs.max_depth));
	CopyConstantWords(record.cb_vs_words, 11,
		&config.cb_vs.line_aa1_width, sizeof(config.cb_vs.line_aa1_width));

	CopyConstantWords(record.cb_ps_words, 0,
		&config.cb_ps.FogColor_AREF, sizeof(config.cb_ps.FogColor_AREF));
	CopyConstantWords(record.cb_ps_words, 4,
		&config.cb_ps.WH, sizeof(config.cb_ps.WH));
	CopyConstantWords(record.cb_ps_words, 8,
		&config.cb_ps.TA_MaxDepth_Af, sizeof(config.cb_ps.TA_MaxDepth_Af));
	CopyConstantWords(record.cb_ps_words, 12,
		&config.cb_ps.FbMask, sizeof(config.cb_ps.FbMask));
	CopyConstantWords(record.cb_ps_words, 16,
		&config.cb_ps.HalfTexel, sizeof(config.cb_ps.HalfTexel));
	CopyConstantWords(record.cb_ps_words, 20,
		&config.cb_ps.MinMax, sizeof(config.cb_ps.MinMax));
	CopyConstantWords(record.cb_ps_words, 24,
		&config.cb_ps.LODParams, sizeof(config.cb_ps.LODParams));
	CopyConstantWords(record.cb_ps_words, 28,
		&config.cb_ps.STRange, sizeof(config.cb_ps.STRange));
	CopyConstantWords(record.cb_ps_words, 32,
		&config.cb_ps.ChannelShuffle, sizeof(config.cb_ps.ChannelShuffle));
	CopyConstantWords(record.cb_ps_words, 36,
		&config.cb_ps.ChannelShuffleOffset,
		sizeof(config.cb_ps.ChannelShuffleOffset));
	CopyConstantWords(record.cb_ps_words, 38,
		&config.cb_ps.TCOffsetHack, sizeof(config.cb_ps.TCOffsetHack));
	CopyConstantWords(record.cb_ps_words, 40,
		&config.cb_ps.STScale, sizeof(config.cb_ps.STScale));
	CopyConstantWords(record.cb_ps_words, 44,
		&config.cb_ps.DitherMatrix, sizeof(config.cb_ps.DitherMatrix));
	CopyConstantWords(record.cb_ps_words, 60,
		&config.cb_ps.ScaleFactor, sizeof(config.cb_ps.ScaleFactor));
	CopyConstantWords(record.cb_ps_words, 64,
		&config.cb_ps.LineCovScale, sizeof(config.cb_ps.LineCovScale));

	s_current_config = &config;
	s_current_record = &record;
}

void VitaGsDrawTraceRecordTextureContent(
	const VitaGXM::GSTextureGXM& texture, u32 level,
	const GSVector4i& updated_rect)
{
	StartIfRequested();
	if (!s_active)
		return;
	const VitaGXM::TextureLevelLayout* layout = texture.Level(level);
	const u8* data = static_cast<const u8*>(texture.LevelData(level));
	const u32 bytes_per_pixel = texture.NativeFormat().bytes_per_pixel;
	if (!layout || !data || bytes_per_pixel == 0)
		return;

	TextureContentSummary& summary = s_texture_contents[texture.TelemetryId()];
	summary.generation++;
	if (updated_rect.x == 0 && updated_rect.y == 0 &&
		updated_rect.z == static_cast<int>(layout->width) &&
		updated_rect.w == static_cast<int>(layout->height))
	{
		summary.known_levels |= 1u << level;
	}
	summary.content_hash = 14695981039346656037ull;
	summary.content_bytes = 0;
	summary.channel_or = 0;
	for (u32 known_level = 0;
		known_level < static_cast<u32>(texture.GetMipmapLevels()) && known_level < 32;
		known_level++)
	{
		if ((summary.known_levels & (1u << known_level)) == 0)
			continue;
		const VitaGXM::TextureLevelLayout* known_layout = texture.Level(known_level);
		const u8* known_data =
			static_cast<const u8*>(texture.LevelData(known_level));
		if (!known_layout || !known_data)
			continue;
		const u32 row_bytes = known_layout->width * bytes_per_pixel;
		const u32 level_bytes = row_bytes * known_layout->height;
		HashU32(summary.content_hash, known_level);
		HashU32(summary.content_hash, level_bytes);
		for (u32 y = 0; y < known_layout->height; y++)
		{
			const u8* row = known_data + static_cast<size_t>(y) * known_layout->pitch;
			for (u32 x = 0; x < row_bytes; x++)
			{
				summary.content_hash =
					(summary.content_hash ^ row[x]) * 1099511628211ull;
				const u32 channel = x % bytes_per_pixel;
				if (channel < 4)
					summary.channel_or |= static_cast<u32>(row[x]) << (channel * 8);
			}
		}
		summary.content_bytes = static_cast<u32>(std::min<u64>(
			static_cast<u64>(summary.content_bytes) + level_bytes,
			std::numeric_limits<u32>::max()));
	}
}

void VitaGsDrawTraceRecordBackendState(const GSHWDrawConfig& config,
	const VitaGXM::GSTextureGXM& bound_texture,
	const void* staged_vertices, u32 staged_vertex_bytes,
	const void* staged_indices, u32 staged_index_bytes,
	u32 index_count, u32 sampler_state, u32 depth_state,
	u32 raster_state, u32 color_mask)
{
	if (!s_active || s_current_config != &config || !s_current_record)
		return;
	const u64 vertex_hash = HashBytes(staged_vertices, staged_vertex_bytes);
	const u64 index_hash = HashBytes(staged_indices, staged_index_bytes);
	// Draws larger than the bounded staging arena are submitted in chunks. Keep
	// their ordering observable without retaining the transient buffers.
	s_current_record->backend_vertex_hash =
		(s_current_record->backend_vertex_hash * 1099511628211ull) ^ vertex_hash;
	s_current_record->backend_index_hash =
		(s_current_record->backend_index_hash * 1099511628211ull) ^ index_hash;
	s_current_record->backend_index_count += index_count;
	s_current_record->backend_chunks++;
	s_current_record->backend_sampler = sampler_state;
	s_current_record->backend_depth = depth_state;
	s_current_record->backend_raster = raster_state;
	s_current_record->backend_color_mask = color_mask;
	s_current_record->bound_tex_id = bound_texture.TelemetryId();
	const auto summary = s_texture_contents.find(bound_texture.TelemetryId());
	if (summary != s_texture_contents.end())
	{
		s_current_record->bound_tex_content_hash = summary->second.content_hash;
		s_current_record->bound_tex_content_bytes = summary->second.content_bytes;
		s_current_record->bound_tex_channel_or = summary->second.channel_or;
		s_current_record->bound_tex_content_generation = summary->second.generation;
		s_current_record->bound_tex_content_levels = summary->second.known_levels;
	}
}

bool VitaGsDrawTraceRenderTargetSourceNeedsSync(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation)
{
	if (!s_active || s_current_config != &config || !s_current_record ||
		!texture.IsRenderTarget() || texture.GetFormat() != GSTexture::Format::Color)
	{
		return false;
	}
	const auto snapshot = s_rt_snapshots.find(texture.TelemetryId());
	return snapshot == s_rt_snapshots.end() ||
		snapshot->second.generation != content_generation;
}

void VitaGsDrawTraceRecordRenderTargetSource(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation)
{
	if (!s_active || s_current_config != &config || !s_current_record ||
		!texture.IsRenderTarget())
	{
		return;
	}
	if (texture.GetFormat() != GSTexture::Format::Color)
	{
		s_current_record->rt_source_status = 3;
		return;
	}

	const u32 id = texture.TelemetryId();
	u32 status = 0;
	const RenderTargetSnapshot* const snapshot =
		SnapshotRenderTarget(texture, content_generation, &status);
	if (!snapshot)
	{
		s_current_record->rt_source_status = status;
		return;
	}

	const GSVector4i rect = config.samplearea.rintersect(
		GSVector4i(0, 0, snapshot->width, snapshot->height));
	if (rect.rempty())
	{
		s_current_record->rt_source_status = 3;
		return;
	}
	u64 hash = 14695981039346656037ull;
	u64 channel_hash[4] = {
		14695981039346656037ull, 14695981039346656037ull,
		14695981039346656037ull, 14695981039346656037ull,
	};
	u32 channel_or = 0;
	u32 content_bytes = 0;
	HashRenderTargetRect(*snapshot, rect, &hash, channel_hash,
		&content_bytes, &channel_or, nullptr, nullptr);
	s_current_record->rt_source_content_hash = hash;
	s_current_record->rt_source_generation = content_generation;
	s_current_record->rt_source_content_bytes = content_bytes;
	s_current_record->rt_source_channel_or = channel_or;
	s_current_record->rt_source_status = 1;
	std::memcpy(s_current_record->rt_source_channel_hash, channel_hash,
		sizeof(channel_hash));
	const auto writer = s_rt_writers.find(id);
	if (writer != s_rt_writers.end())
	{
		u32 best_channel_area[4] = {};
		bool found_writer = false;
		for (auto it = writer->second.rbegin(); it != writer->second.rend(); ++it)
		{
			if (it->generation > content_generation)
				continue;
			const GSVector4i writer_rect(it->drawarea.x, it->drawarea.y,
				it->drawarea.z, it->drawarea.w);
			const GSVector4i overlap = rect.rintersect(writer_rect);
			if (overlap.rempty())
				continue;
			const u32 area = static_cast<u32>(overlap.width()) *
				static_cast<u32>(overlap.height());
			if (!found_writer)
			{
				s_current_record->rt_source_writer_draw = it->draw_index;
				found_writer = true;
			}
			for (u32 channel = 0; it->draw_index != 0 && channel < 4; channel++)
			{
				if ((it->color_mask & (1u << channel)) != 0 &&
					area > best_channel_area[channel])
				{
					best_channel_area[channel] = area;
					s_current_record->rt_source_channel_writer[channel] =
						it->draw_index;
				}
			}
		}
	}
}

void VitaGsDrawTraceRecordRenderTargetWrite(const GSHWDrawConfig& config,
	const VitaGXM::GSTextureGXM& texture, u64 content_generation)
{
	if (!s_active)
		return;
	RenderTargetWriter writer;
	writer.generation = content_generation;
	writer.draw_index =
		(s_current_config == &config && s_current_record) ?
			s_current_record->draw_index : 0;
	writer.drawarea = DescribeRect(config.drawarea);
	writer.color_mask = config.colormask.key;
	s_rt_writers[texture.TelemetryId()].push_back(writer);
}

bool VitaGsDrawTraceRenderTargetAfterDrawNeedsSync(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation)
{
	(void)content_generation;
	return s_active && s_current_config == &config && s_current_record &&
		texture.IsRenderTarget() &&
		texture.GetFormat() == GSTexture::Format::Color &&
		s_rt_after_draw_count <
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_RT_AFTER_DRAWS);
}

void VitaGsDrawTraceRecordRenderTargetAfterDraw(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation)
{
	if (!s_active || s_current_config != &config || !s_current_record ||
		s_rt_after_draw_count >=
			static_cast<u32>(VITASX2_GS_DRAW_TRACE_RT_AFTER_DRAWS))
	{
		return;
	}
	s_rt_after_draw_count++;
	const GSVector4i rect = config.drawarea.rintersect(
		GSVector4i(0, 0, texture.GetWidth(), texture.GetHeight()));
	if (rect.rempty())
	{
		s_current_record->rt_after_status = 3;
		return;
	}
	const u32 bytes_per_pixel = texture.NativeFormat().bytes_per_pixel;
	if (bytes_per_pixel == 0 ||
		static_cast<u32>(rect.width()) >
			std::numeric_limits<u32>::max() / bytes_per_pixel)
	{
		s_current_record->rt_after_status = 3;
		return;
	}
	RenderTargetSnapshot snapshot;
	snapshot.width = static_cast<u32>(rect.width());
	snapshot.height = static_cast<u32>(rect.height());
	snapshot.bytes_per_pixel = bytes_per_pixel;
	snapshot.pitch = snapshot.width * bytes_per_pixel;
	snapshot.pixels.resize(static_cast<size_t>(snapshot.pitch) * snapshot.height);
	if (!texture.CopyToLinear(0, rect, snapshot.pixels.data(), snapshot.pitch))
	{
		s_current_record->rt_after_status = 4;
		return;
	}
	HashRenderTargetRect(snapshot,
		GSVector4i(0, 0, snapshot.width, snapshot.height),
		&s_current_record->rt_after_content_hash,
		s_current_record->rt_after_channel_hash,
		&s_current_record->rt_after_content_bytes,
		&s_current_record->rt_after_channel_or,
		s_current_record->rt_after_tile_hash,
		&s_current_record->rt_after_nonzero_tiles);
	s_current_record->rt_after_generation = content_generation;
	s_current_record->rt_after_status = 1;
}

void VitaGsDrawTraceRecordDeviceOutcome(const GSHWDrawConfig& config,
	VitaGsDrawTraceOutcome outcome, u32 path_flags, u32 detail)
{
	if (!s_active || s_current_config != &config || !s_current_record)
		return;
	s_current_record->outcome = static_cast<u8>(outcome);
	s_current_record->path_flags = path_flags;
	s_current_record->detail = detail;
}

#else

void VitaGsDrawTraceNotifyElfEntry() {}
void VitaGsDrawTraceVSync() {}
void VitaGsDrawTraceRecordPacket(u8, const void*, u32, bool) {}
void VitaGsDrawTraceRecordPresentSource(
	const GSTexture*, const GSVector4i&, const GSVector4&, const GSVector4&) {}
bool VitaGsDrawTraceNeedsPresentSourceCapture() { return false; }
void VitaGsDrawTraceCapturePresentSource() {}
bool VitaGsDrawTraceNeedsDisplayCapture() { return false; }
void VitaGsDrawTraceRecordDisplay(const void*, u32, u32, u32, u32) {}
void VitaGsDrawTraceRecordConfig(u32, u32, u8, const GSHWDrawConfig&) {}
void VitaGsDrawTraceRecordTextureContent(
	const VitaGXM::GSTextureGXM&, u32, const GSVector4i&) {}
void VitaGsDrawTraceRecordBackendState(const GSHWDrawConfig&,
	const VitaGXM::GSTextureGXM&, const void*, u32, const void*, u32,
	u32, u32, u32, u32, u32) {}
bool VitaGsDrawTraceRenderTargetSourceNeedsSync(
	const GSHWDrawConfig&, const VitaGXM::GSTextureGXM&, u64) { return false; }
void VitaGsDrawTraceRecordRenderTargetSource(
	const GSHWDrawConfig&, const VitaGXM::GSTextureGXM&, u64) {}
void VitaGsDrawTraceRecordRenderTargetWrite(
	const GSHWDrawConfig&, const VitaGXM::GSTextureGXM&, u64) {}
bool VitaGsDrawTraceRenderTargetAfterDrawNeedsSync(
	const GSHWDrawConfig&, const VitaGXM::GSTextureGXM&, u64) { return false; }
void VitaGsDrawTraceRecordRenderTargetAfterDraw(
	const GSHWDrawConfig&, const VitaGXM::GSTextureGXM&, u64) {}
void VitaGsDrawTraceRecordDeviceOutcome(const GSHWDrawConfig&,
	VitaGsDrawTraceOutcome, u32, u32) {}

#endif
