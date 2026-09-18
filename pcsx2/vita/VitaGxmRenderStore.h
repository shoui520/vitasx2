// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace VitaGXM
{
	// SGX543 renders in 32x32 tiles. The store allocator therefore never packs
	// sub-tile rectangles: doing so would make unrelated logical targets share a
	// tile load/store owner and turn a cheap view switch into an implicit hazard.
	class RenderStoreAllocator final
	{
	public:
		static constexpr std::uint32_t TileWidth = 32;
		static constexpr std::uint32_t TileHeight = 32;
		static constexpr std::uint32_t StoreWidth = 2048;
		static constexpr std::uint32_t StoreHeight = 1024;
		static constexpr std::uint32_t TilesX = StoreWidth / TileWidth;
		static constexpr std::uint32_t TilesY = StoreHeight / TileHeight;
		static constexpr std::uint32_t MaximumRegions = 128;

		enum class Plane : std::uint8_t
		{
			Color,
			Depth,
		};

		struct Region
		{
			std::uint16_t x = 0;
			std::uint16_t y = 0;
			std::uint16_t width = 0;
			std::uint16_t height = 0;
			std::uint16_t slot = 0xffffu;
			std::uint16_t generation = 0;

			explicit operator bool() const { return slot != 0xffffu; }
			bool operator==(const Region& other) const = default;
		};

		// Allocates independently in the color or depth plane. preferred supplies
		// the other attachment's exact raster origin; matching it is what lets a
		// FRAME/ZBUF pair remain in the same physical GXM scene.
		bool Allocate(Plane plane, std::uint32_t width, std::uint32_t height,
			const Region* preferred, Region* region);
		bool Release(Plane plane, const Region& region);
		bool IsLive(Plane plane, const Region& region) const;
		void Reset();

	private:
		struct Slot
		{
			Region region{};
			Plane plane = Plane::Color;
			bool live = false;
		};

		using TileRows = std::array<std::uint64_t, TilesY>;
		bool CanPlace(const TileRows& rows, std::uint32_t tile_x,
			std::uint32_t tile_y, std::uint32_t tiles_w,
			std::uint32_t tiles_h) const;
		void SetOccupied(TileRows& rows, const Region& region, bool occupied);
		bool AllocateAt(Plane plane, std::uint32_t tile_x,
			std::uint32_t tile_y, std::uint32_t tiles_w,
			std::uint32_t tiles_h, Region* region);
		TileRows& Rows(Plane plane);
		const TileRows& Rows(Plane plane) const;

		TileRows m_color_rows{};
		TileRows m_depth_rows{};
		std::array<Slot, MaximumRegions> m_slots{};
		std::uint16_t m_next_generation = 1;
	};

	// Stable GS identity, deliberately independent of GSTexture* lifetime. A
	// recycled host texture representing the same GS target must find the same
	// resident contents; an equal-sized target at another GS base must not.
	struct RenderStoreTargetKey
	{
		std::uint32_t base_block = 0;
		std::uint32_t buffer_width = 0;
		std::uint32_t psm = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t scale_bits = 0;
		bool depth = false;

		bool operator==(const RenderStoreTargetKey& other) const = default;
	};

	// GXM has one raster origin shared by color and depth. A logical FRAME/ZBUF
	// pair therefore owns one co-located region in the two independent planes.
	// The same logical target can occur in several pair residencies; versions
	// make those copies explicit rather than relying on target-switch order.
	class RenderStoreResidencyPlanner final
	{
	public:
		static constexpr std::uint16_t InvalidIndex = 0xffffu;
		static constexpr std::uint32_t MaximumTargets = 128;
		static constexpr std::uint32_t MaximumResidencies = 128;

		struct PairKey
		{
			std::optional<RenderStoreTargetKey> color;
			std::optional<RenderStoreTargetKey> depth;

			bool operator==(const PairKey& other) const = default;
		};

		struct Acquisition
		{
			std::uint16_t residency = InvalidIndex;
			RenderStoreAllocator::Region region{};
			bool load_color = false;
			bool load_depth = false;
			bool new_residency = false;

			explicit operator bool() const { return residency != InvalidIndex; }
		};

		bool Acquire(const PairKey& key, std::uint32_t width,
			std::uint32_t height, Acquisition* acquisition);
		bool MarkLoaded(std::uint16_t residency,
			RenderStoreAllocator::Plane plane);
		bool MarkWritten(std::uint16_t residency,
			RenderStoreAllocator::Plane plane);
		bool MarkBackingWritten(const RenderStoreTargetKey& key);
		bool MarkBackingCurrent(const RenderStoreTargetKey& key);
		bool BackingNeedsStore(const RenderStoreTargetKey& key) const;
		std::uint16_t LatestResidency(const RenderStoreTargetKey& key) const;
		std::uint64_t ContentVersion(const RenderStoreTargetKey& key) const;
		const PairKey* Key(std::uint16_t residency) const;
		const RenderStoreAllocator::Region* Region(std::uint16_t residency) const;
		void Reset();

	private:
		struct TargetState
		{
			RenderStoreTargetKey key{};
			std::uint64_t version = 1;
			std::uint64_t backing_version = 1;
			std::uint16_t latest_residency = InvalidIndex;
			bool live = false;
		};

		struct Residency
		{
			PairKey key{};
			RenderStoreAllocator::Region color_region{};
			RenderStoreAllocator::Region depth_region{};
			std::uint64_t color_version = 0;
			std::uint64_t depth_version = 0;
			bool live = false;
		};

		TargetState* FindOrCreateTarget(const RenderStoreTargetKey& key);
		TargetState* FindTarget(const RenderStoreTargetKey& key);
		const TargetState* FindTarget(const RenderStoreTargetKey& key) const;
		Residency* FindResidency(const PairKey& key);
		const Residency* GetResidency(std::uint16_t residency) const;
		Residency* GetResidency(std::uint16_t residency);

		RenderStoreAllocator m_allocator;
		std::array<TargetState, MaximumTargets> m_targets{};
		std::array<Residency, MaximumResidencies> m_residencies{};
	};
} // namespace VitaGXM
