// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "VitaGxmRenderStore.h"

#include <algorithm>

namespace VitaGXM
{
	namespace
	{
		constexpr std::uint32_t DivideRoundUp(std::uint32_t value,
			std::uint32_t divisor)
		{
			return (value + divisor - 1u) / divisor;
		}
	}

	RenderStoreAllocator::TileRows& RenderStoreAllocator::Rows(Plane plane)
	{
		return plane == Plane::Color ? m_color_rows : m_depth_rows;
	}

	const RenderStoreAllocator::TileRows& RenderStoreAllocator::Rows(
		Plane plane) const
	{
		return plane == Plane::Color ? m_color_rows : m_depth_rows;
	}

	bool RenderStoreAllocator::CanPlace(const TileRows& rows,
		std::uint32_t tile_x, std::uint32_t tile_y, std::uint32_t tiles_w,
		std::uint32_t tiles_h) const
	{
		if (tiles_w == 0 || tiles_h == 0 || tile_x + tiles_w > TilesX ||
			tile_y + tiles_h > TilesY)
		{
			return false;
		}
		const std::uint64_t mask = tiles_w == 64 ? ~std::uint64_t{0} :
			((std::uint64_t{1} << tiles_w) - 1u) << tile_x;
		for (std::uint32_t y = tile_y; y < tile_y + tiles_h; y++)
		{
			if ((rows[y] & mask) != 0)
				return false;
		}
		return true;
	}

	void RenderStoreAllocator::SetOccupied(TileRows& rows,
		const Region& region, bool occupied)
	{
		const std::uint32_t tile_x = region.x / TileWidth;
		const std::uint32_t tile_y = region.y / TileHeight;
		const std::uint32_t tiles_w = region.width / TileWidth;
		const std::uint32_t tiles_h = region.height / TileHeight;
		const std::uint64_t mask = tiles_w == 64 ? ~std::uint64_t{0} :
			((std::uint64_t{1} << tiles_w) - 1u) << tile_x;
		for (std::uint32_t y = tile_y; y < tile_y + tiles_h; y++)
		{
			if (occupied)
				rows[y] |= mask;
			else
				rows[y] &= ~mask;
		}
	}

	bool RenderStoreAllocator::AllocateAt(Plane plane, std::uint32_t tile_x,
		std::uint32_t tile_y, std::uint32_t tiles_w, std::uint32_t tiles_h,
		Region* region)
	{
		if (!region || !CanPlace(Rows(plane), tile_x, tile_y, tiles_w, tiles_h))
			return false;
		auto slot = std::find_if(m_slots.begin(), m_slots.end(),
			[](const Slot& candidate) { return !candidate.live; });
		if (slot == m_slots.end())
			return false;

		std::uint16_t generation = m_next_generation++;
		if (generation == 0)
			generation = m_next_generation++;
		const std::uint16_t slot_index = static_cast<std::uint16_t>(
			std::distance(m_slots.begin(), slot));
		Region allocated{static_cast<std::uint16_t>(tile_x * TileWidth),
			static_cast<std::uint16_t>(tile_y * TileHeight),
			static_cast<std::uint16_t>(tiles_w * TileWidth),
			static_cast<std::uint16_t>(tiles_h * TileHeight), slot_index,
			generation};
		SetOccupied(Rows(plane), allocated, true);
		slot->region = allocated;
		slot->plane = plane;
		slot->live = true;
		*region = allocated;
		return true;
	}

	bool RenderStoreAllocator::Allocate(Plane plane, std::uint32_t width,
		std::uint32_t height, const Region* preferred, Region* region)
	{
		if (!region || width == 0 || height == 0 || width > StoreWidth ||
			height > StoreHeight)
		{
			return false;
		}
		const std::uint32_t tiles_w = DivideRoundUp(width, TileWidth);
		const std::uint32_t tiles_h = DivideRoundUp(height, TileHeight);
		if (preferred && *preferred && preferred->width >= tiles_w * TileWidth &&
			preferred->height >= tiles_h * TileHeight &&
			AllocateAt(plane, preferred->x / TileWidth,
				preferred->y / TileHeight, tiles_w, tiles_h, region))
		{
			return true;
		}

		// Bottom-left first-fit is deterministic and touches one 64-bit occupancy
		// word per tile row. Allocation is cold target-cache work, never per draw.
		for (std::uint32_t y = 0; y + tiles_h <= TilesY; y++)
		{
			for (std::uint32_t x = 0; x + tiles_w <= TilesX; x++)
			{
				if (AllocateAt(plane, x, y, tiles_w, tiles_h, region))
					return true;
			}
		}
		return false;
	}

	bool RenderStoreAllocator::IsLive(Plane plane, const Region& region) const
	{
		return region.slot < m_slots.size() && m_slots[region.slot].live &&
			m_slots[region.slot].plane == plane &&
			m_slots[region.slot].region == region;
	}

	bool RenderStoreAllocator::Release(Plane plane, const Region& region)
	{
		if (!IsLive(plane, region))
			return false;
		SetOccupied(Rows(plane), region, false);
		m_slots[region.slot].live = false;
		return true;
	}

	void RenderStoreAllocator::Reset()
	{
		m_color_rows.fill(0);
		m_depth_rows.fill(0);
		for (Slot& slot : m_slots)
			slot.live = false;
		m_next_generation = 1;
	}

	RenderStoreResidencyPlanner::TargetState*
	RenderStoreResidencyPlanner::FindTarget(const RenderStoreTargetKey& key)
	{
		auto target = std::find_if(m_targets.begin(), m_targets.end(),
			[&key](const TargetState& candidate) {
				return candidate.live && candidate.key == key;
			});
		return target == m_targets.end() ? nullptr : &*target;
	}

	const RenderStoreResidencyPlanner::TargetState*
	RenderStoreResidencyPlanner::FindTarget(const RenderStoreTargetKey& key) const
	{
		auto target = std::find_if(m_targets.begin(), m_targets.end(),
			[&key](const TargetState& candidate) {
				return candidate.live && candidate.key == key;
			});
		return target == m_targets.end() ? nullptr : &*target;
	}

	RenderStoreResidencyPlanner::TargetState*
	RenderStoreResidencyPlanner::FindOrCreateTarget(
		const RenderStoreTargetKey& key)
	{
		if (TargetState* const existing = FindTarget(key))
			return existing;
		auto target = std::find_if(m_targets.begin(), m_targets.end(),
			[](const TargetState& candidate) { return !candidate.live; });
		if (target == m_targets.end())
			return nullptr;
		*target = {};
		target->key = key;
		target->version = 1;
		target->backing_version = 1;
		target->live = true;
		return &*target;
	}

	RenderStoreResidencyPlanner::Residency*
	RenderStoreResidencyPlanner::FindResidency(const PairKey& key)
	{
		auto residency = std::find_if(m_residencies.begin(), m_residencies.end(),
			[&key](const Residency& candidate) {
				return candidate.live && candidate.key == key;
			});
		return residency == m_residencies.end() ? nullptr : &*residency;
	}

	RenderStoreResidencyPlanner::Residency*
	RenderStoreResidencyPlanner::GetResidency(std::uint16_t residency)
	{
		return residency < m_residencies.size() && m_residencies[residency].live ?
			&m_residencies[residency] : nullptr;
	}

	const RenderStoreResidencyPlanner::Residency*
	RenderStoreResidencyPlanner::GetResidency(std::uint16_t residency) const
	{
		return residency < m_residencies.size() && m_residencies[residency].live ?
			&m_residencies[residency] : nullptr;
	}

	bool RenderStoreResidencyPlanner::Acquire(const PairKey& key,
		std::uint32_t width, std::uint32_t height, Acquisition* acquisition)
	{
		if (!acquisition || (!key.color && !key.depth) || width == 0 || height == 0)
			return false;
		if ((key.color && key.color->depth) || (key.depth && !key.depth->depth))
			return false;

		TargetState* const color = key.color ? FindOrCreateTarget(*key.color) : nullptr;
		TargetState* const depth = key.depth ? FindOrCreateTarget(*key.depth) : nullptr;
		if ((key.color && !color) || (key.depth && !depth))
			return false;

		Residency* residency = FindResidency(key);
		const bool new_residency = !residency;
		if (!residency)
		{
			residency = std::find_if(m_residencies.begin(), m_residencies.end(),
				[](const Residency& candidate) { return !candidate.live; });
			if (residency == m_residencies.end())
				return false;

			RenderStoreAllocator::Region color_region;
			RenderStoreAllocator::Region depth_region;
			if (color && !m_allocator.Allocate(RenderStoreAllocator::Plane::Color,
					width, height, nullptr, &color_region))
			{
				return false;
			}
			const RenderStoreAllocator::Region* const preferred = color ?
				&color_region : nullptr;
			if (depth && !m_allocator.Allocate(RenderStoreAllocator::Plane::Depth,
					width, height, preferred, &depth_region))
			{
				if (color)
					m_allocator.Release(RenderStoreAllocator::Plane::Color, color_region);
				return false;
			}
			// Color and depth must use the same screen-space origin. Independent
			// occupancy planes make the identical rectangle legal.
			if (color && depth && (color_region.x != depth_region.x ||
					color_region.y != depth_region.y ||
					color_region.width != depth_region.width ||
					color_region.height != depth_region.height))
			{
				m_allocator.Release(RenderStoreAllocator::Plane::Color, color_region);
				m_allocator.Release(RenderStoreAllocator::Plane::Depth, depth_region);
				return false;
			}

			*residency = {};
			residency->key = key;
			residency->color_region = color_region;
			residency->depth_region = depth_region;
			residency->live = true;
		}

		const std::uint16_t index = static_cast<std::uint16_t>(
			std::distance(m_residencies.begin(), residency));
		acquisition->residency = index;
		acquisition->region = color ? residency->color_region :
			residency->depth_region;
		acquisition->load_color = color &&
			residency->color_version != color->version;
		acquisition->load_depth = depth &&
			residency->depth_version != depth->version;
		acquisition->new_residency = new_residency;
		return true;
	}

	bool RenderStoreResidencyPlanner::MarkLoaded(std::uint16_t index,
		RenderStoreAllocator::Plane plane)
	{
		Residency* const residency = GetResidency(index);
		if (!residency)
			return false;
		const std::optional<RenderStoreTargetKey>& key =
			plane == RenderStoreAllocator::Plane::Color ?
			residency->key.color : residency->key.depth;
		TargetState* const target = key ? FindTarget(*key) : nullptr;
		if (!target)
			return false;
		if (plane == RenderStoreAllocator::Plane::Color)
			residency->color_version = target->version;
		else
			residency->depth_version = target->version;
		return true;
	}

	bool RenderStoreResidencyPlanner::MarkWritten(std::uint16_t index,
		RenderStoreAllocator::Plane plane)
	{
		Residency* const residency = GetResidency(index);
		if (!residency)
			return false;
		const std::optional<RenderStoreTargetKey>& key =
			plane == RenderStoreAllocator::Plane::Color ?
			residency->key.color : residency->key.depth;
		TargetState* const target = key ? FindTarget(*key) : nullptr;
		if (!target)
			return false;
		target->version++;
		if (target->version == 0)
			target->version++;
		target->latest_residency = index;
		if (plane == RenderStoreAllocator::Plane::Color)
			residency->color_version = target->version;
		else
			residency->depth_version = target->version;
		return true;
	}

	bool RenderStoreResidencyPlanner::MarkBackingCurrent(
		const RenderStoreTargetKey& key)
	{
		TargetState* const target = FindTarget(key);
		if (!target)
			return false;
		target->backing_version = target->version;
		return true;
	}

	bool RenderStoreResidencyPlanner::MarkBackingWritten(
		const RenderStoreTargetKey& key)
	{
		TargetState* const target = FindOrCreateTarget(key);
		if (!target)
			return false;
		target->version++;
		if (target->version == 0)
			target->version++;
		target->backing_version = target->version;
		target->latest_residency = InvalidIndex;
		return true;
	}

	bool RenderStoreResidencyPlanner::BackingNeedsStore(
		const RenderStoreTargetKey& key) const
	{
		const TargetState* const target = FindTarget(key);
		return target && target->backing_version != target->version;
	}

	std::uint16_t RenderStoreResidencyPlanner::LatestResidency(
		const RenderStoreTargetKey& key) const
	{
		const TargetState* const target = FindTarget(key);
		return target ? target->latest_residency : InvalidIndex;
	}

	std::uint64_t RenderStoreResidencyPlanner::ContentVersion(
		const RenderStoreTargetKey& key) const
	{
		const TargetState* const target = FindTarget(key);
		return target ? target->version : 0;
	}

	const RenderStoreResidencyPlanner::PairKey*
	RenderStoreResidencyPlanner::Key(std::uint16_t index) const
	{
		const Residency* const residency = GetResidency(index);
		return residency ? &residency->key : nullptr;
	}

	const RenderStoreAllocator::Region*
	RenderStoreResidencyPlanner::Region(std::uint16_t index) const
	{
		const Residency* const residency = GetResidency(index);
		if (!residency)
			return nullptr;
		return residency->key.color ? &residency->color_region :
			&residency->depth_region;
	}

	void RenderStoreResidencyPlanner::Reset()
	{
		m_allocator.Reset();
		for (TargetState& target : m_targets)
			target = {};
		for (Residency& residency : m_residencies)
			residency = {};
	}
} // namespace VitaGXM
