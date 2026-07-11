// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Cache.h"
#include "vtlb.h"

using namespace R5900;
using namespace vtlb_private;

namespace
{

	union alignas(64) CacheData
	{
		u8 bytes[64];
	};

	struct CacheTag
	{
		uptr rawValue;
		// You are able to configure a TLB entry with non-existant physical address without causing a bus error.
		// When this happens, the cache still fills with the data and when it gets evicted the data is lost.
		// We don't emulate memory access on a logic level, so we need to ensure that we don't try to load/store to a non-existant physical address.
		// This fixes the Find My Own Way demo.

		// The lower parts of a cache tags structure is as follows:
		// 31 - 12: The physical address cache tag.
		// 11: Used by PCSX2 to indicate if the physical address is valid.
		// 10 - 7: Unused.
		// 6: Dirty flag.
		// 5: Valid flag.
		// 4: LRF flag - least recently filled flag.
		// 3: Lock flag.
		// 2-0: Unused.

		enum Flags : decltype(rawValue)
		{
			DIRTY_FLAG = 0x40,
			VALID_FLAG = 0x20,
			LRF_FLAG = 0x10,
			LOCK_FLAG = 0x8,
			ALL_FLAGS = 0x7FF,
			ALL_BITS = 0xFFF
		};

		int flags() const
		{
			return rawValue & ALL_FLAGS;
		}

		bool isValid() const { return rawValue & VALID_FLAG; }
		bool isDirty() const { return rawValue & DIRTY_FLAG; }
		bool lrf() const { return rawValue & LRF_FLAG; }
		bool isLocked() const { return rawValue & LOCK_FLAG; }

		bool isDirtyAndValid() const
		{
			return (rawValue & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG);
		}

		void setValid() { rawValue |= VALID_FLAG; }
		void setDirty() { rawValue |= DIRTY_FLAG; }
		void setLocked() { rawValue |= LOCK_FLAG; }
		void clearValid() { rawValue &= ~VALID_FLAG; }
		void clearDirty() { rawValue &= ~DIRTY_FLAG; }
		void clearLocked() { rawValue &= ~LOCK_FLAG; }
		void toggleLRF() { rawValue ^= LRF_FLAG; }

		uptr addr() const { return rawValue & ~ALL_BITS; }

		void setAddr(uptr addr)
		{
			rawValue &= ALL_BITS;
			rawValue |= (addr & ~ALL_BITS);
		}

		bool matches(uptr other) const
		{
			return isValid() && addr() == (other & ~ALL_BITS);
		}

		void clear()
		{
			rawValue &= LRF_FLAG;
		}

		constexpr bool isValidPFN() const
		{
			return rawValue & 0x800;
		}

		constexpr void setValidPFN(bool valid)
		{
			if (valid)
				rawValue |= 0x800;
			else
				rawValue &= ~0x800;
		}
	};

	struct CacheLine
	{
		CacheTag& tag;
		CacheData& data;
		int set;

		uptr addr()
		{
			return tag.addr() | (set << 6);
		}

		void writeBackIfNeeded()
		{
			if (!tag.isDirtyAndValid())
				return;

			uptr target = addr();

			CACHE_LOG("Write back at %zx", target);
			if (tag.isValidPFN())
				*reinterpret_cast<CacheData*>(target) = data;
			tag.clearDirty();
		}

		void load(uptr ppf)
		{
			pxAssertMsg(!tag.isDirtyAndValid(), "Loaded a value into cache without writing back the old one!");

			tag.setAddr(ppf);
			if (!tag.isValidPFN())
			{
				// Reading from invalid physical addresses seems to return 0 on hardware
				std::memset(&data, 0, sizeof(data));
			}
			else
			{
				std::memcpy(&data, reinterpret_cast<void*>(ppf & ~0x3FULL), sizeof(data));
			}

			tag.setValid();
			tag.clearDirty();
		}

		void clear()
		{
			tag.clear();
			std::memset(&data, 0, sizeof(data));
		}
	};

	struct CacheSet
	{
		CacheTag tags[2];
		CacheData data[2];
	};

	struct Cache
	{
		CacheSet sets[64];

		int setIdxFor(u32 vaddr) const
		{
			return (vaddr >> 6) & 0x3F;
		}

		CacheLine lineAt(int idx, int way)
		{
			return {sets[idx].tags[way], sets[idx].data[way], idx};
		}
	};

	static Cache cache = {};
} // namespace

void resetCache()
{
	std::memset(&cache, 0, sizeof(cache));
}

void writebackCache()
{
	for (int i = 0; i < 64; i++)
	{
		for (int j = 0; j < 2; j++)
		{
			cache.lineAt(i, j).writeBackIfNeeded();
		}
	}
}

static bool findInCache(const CacheSet& set, uptr ppf, int* way)
{
	auto check = [&](int checkWay) -> bool {
		if (!set.tags[checkWay].matches(ppf))
			return false;

		*way = checkWay;
		return true;
	};

	return check(0) || check(1);
}

static int getFreeCache(u32 mem, int* way, bool validPFN)
{
	const int setIdx = cache.setIdxFor(mem);
	CacheSet& set = cache.sets[setIdx];
	VTLBVirtual vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];

	*way = set.tags[0].lrf() ^ set.tags[1].lrf();
	if (validPFN)
		pxAssertMsg(!vmv.isHandler(mem), "Cache currently only supports non-handler addresses!");

	uptr ppf = vmv.assumePtr(mem);

	[[unlikely]]
	if ((cpuRegs.CP0.n.Config & 0x10000) == 0)
		CACHE_LOG("Cache off!");

	if (findInCache(set, ppf, way))
	{
		[[unlikely]]
		if (set.tags[*way].isLocked())
		{
			// Check the other way
			if (set.tags[*way ^ 1].isLocked())
			{
				Console.Error("CACHE: SECOND WAY IS LOCKED.", setIdx, *way);
			}
			else
			{
				// Force the unlocked way
				*way ^= 1;
			}
		}
	}
	else
	{
		int newWay = set.tags[0].lrf() ^ set.tags[1].lrf();
		[[unlikely]]
		if (set.tags[newWay].isLocked())
		{
			// If the new way is locked, we force the unlocked way, ignoring the lrf bits.
			newWay = newWay ^ 1;
			[[unlikely]]
			if (set.tags[newWay].isLocked())
			{
				Console.Warning("CACHE: SECOND WAY IS LOCKED.", setIdx, *way);
			}
		}
		*way = newWay;

		CacheLine line = cache.lineAt(setIdx, newWay);
		line.writeBackIfNeeded();
		line.tag.setValidPFN(validPFN);
		line.load(ppf);
		line.tag.toggleLRF();
	}

	return setIdx;
}

template <bool Write, int Bytes>
void* prepareCacheAccess(u32 mem, int* way, int* idx, bool validPFN = true)
{
	*way = 0;
	*idx = getFreeCache(mem, way, validPFN);
	CacheLine line = cache.lineAt(*idx, *way);
	if (Write)
		line.tag.setDirty();
	u32 aligned = mem & ~(Bytes - 1);
	return &line.data.bytes[aligned & 0x3f];
}

template <typename Int>
void writeCache(u32 mem, Int value, bool validPFN)
{
	int way, idx;
	void* addr = prepareCacheAccess<true, sizeof(Int)>(mem, &way, &idx, validPFN);

	CACHE_LOG("writeCache%d %8.8x adding to %d, way %d, value %llx", 8 * sizeof(value), mem, idx, way, value);
	*reinterpret_cast<Int*>(addr) = value;
}

void writeCache8(u32 mem, u8 value, bool validPFN)
{
	writeCache<u8>(mem, value, validPFN);
}

void writeCache16(u32 mem, u16 value, bool validPFN)
{
	writeCache<u16>(mem, value, validPFN);
}

void writeCache32(u32 mem, u32 value, bool validPFN)
{
	writeCache<u32>(mem, value, validPFN);
}

void writeCache64(u32 mem, const u64 value, bool validPFN)
{
	writeCache<u64>(mem, value, validPFN);
}

void writeCache128(u32 mem, const mem128_t* value, bool validPFN)
{
	int way, idx;
	void* addr = prepareCacheAccess<true, sizeof(mem128_t)>(mem, &way, &idx, validPFN);

	CACHE_LOG("writeCache128 %8.8x adding to %d, way %x, lo %llx, hi %llx", mem, idx, way, value->lo, value->hi);
	*reinterpret_cast<mem128_t*>(addr) = *value;
}

template <typename Int>
Int readCache(u32 mem, bool validPFN)
{
	int way, idx;
	void* addr = prepareCacheAccess<false, sizeof(Int)>(mem, &way, &idx, validPFN);

	Int value = *reinterpret_cast<Int*>(addr);
	CACHE_LOG("readCache%d %8.8x from %d, way %d, value %llx", 8 * sizeof(value), mem, idx, way, value);
	return value;
}


u8 readCache8(u32 mem, bool validPFN)
{
	return readCache<u8>(mem, validPFN);
}

u16 readCache16(u32 mem, bool validPFN)
{
	return readCache<u16>(mem, validPFN);
}

u32 readCache32(u32 mem, bool validPFN)
{
	return readCache<u32>(mem, validPFN);
}

u64 readCache64(u32 mem, bool validPFN)
{
	return readCache<u64>(mem, validPFN);
}

RETURNS_R128 readCache128(u32 mem, bool validPFN)
{
	int way, idx;
	void* addr = prepareCacheAccess<false, sizeof(mem128_t)>(mem, &way, &idx, validPFN);
	r128 value = r128_load(addr);
	u64* vptr = reinterpret_cast<u64*>(&value);
	CACHE_LOG("readCache128 %8.8x from %d, way %d, lo %llx, hi %llx", mem, idx, way, vptr[0], vptr[1]);
	return value;
}

template <typename Op>
void doCacheHitOp(u32 addr, const char* name, u32 instr, Op op)
{
	const int index = cache.setIdxFor(addr);
	CacheSet& set = cache.sets[index];
	VTLBVirtual vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	uptr ppf = vmv.assumePtr(addr);
	int way;

	if (!findInCache(set, ppf, &way))
	{
		CACHE_LOG("CACHE %s NO HIT addr %x, index %d, tag0 %zx tag1 %zx", name, addr, index, set.tags[0].rawValue, set.tags[1].rawValue);
		return;
	}

	CACHE_LOG("CACHE %s addr %x, index %d, way %d, flags %x OP %x", name, addr, index, way, set.tags[way].flags(), instr);

	op(cache.lineAt(index, way));
}

void executeCacheOp(u32 instr, u32 addr)
{
	const u32 mode = (instr >> 16) & 0x1f;

	switch (mode)
	{
		case 0x1a: //DHIN (Data Cache Hit Invalidate)
			doCacheHitOp(addr, "DHIN", instr, [](CacheLine line) {
				line.clear();
			});
			break;

		case 0x18: //DHWBIN (Data Cache Hit WriteBack with Invalidate)
			doCacheHitOp(addr, "DHWBIN", instr, [](CacheLine line) {
				line.writeBackIfNeeded();
				line.clear();
			});
			break;

		case 0x1c: //DHWOIN (Data Cache Hit WriteBack Without Invalidate)
			doCacheHitOp(addr, "DHWOIN", instr, [](CacheLine line) {
				line.writeBackIfNeeded();
			});
			break;

		case 0x16: //DXIN (Data Cache Index Invalidate)
		{
			const int index = cache.setIdxFor(addr);
			const int way = addr & 0x1;
			CacheLine line = cache.lineAt(index, way);

			CACHE_LOG("CACHE DXIN addr %x, index %d, way %d, flag %x", addr, index, way, line.tag.flags());

			line.clear();
			break;
		}

		case 0x11: //DXLDT (Data Cache Load Data into TagLo)
		{
			const int index = cache.setIdxFor(addr);
			const int way = addr & 0x1;
			CacheLine line = cache.lineAt(index, way);

			cpuRegs.CP0.n.TagLo = *reinterpret_cast<u32*>(&line.data.bytes[addr & 0x3C]);

			CACHE_LOG("CACHE DXLDT addr %x, index %d, way %d, DATA %x OP %x", addr, index, way, cpuRegs.CP0.n.TagLo, instr);
			break;
		}

		case 0x10: //DXLTG (Data Cache Load Tag into TagLo)
		{
			const int index = (addr >> 6) & 0x3F;
			const int way = addr & 0x1;
			CacheLine line = cache.lineAt(index, way);

			// DXLTG demands that SYNC.L is called before this command, which forces the cache to write back, so presumably games are checking the cache has updated the memory
			// For speed, we will do it here.
			line.writeBackIfNeeded();

			// Our tags don't contain PS2 paddrs (instead they contain x86 addrs)
			cpuRegs.CP0.n.TagLo = line.tag.flags();

			CACHE_LOG("CACHE DXLTG addr %x, index %d, way %d, DATA %x OP %x ", addr, index, way, cpuRegs.CP0.n.TagLo, instr);
			CACHE_LOG("WARNING: DXLTG emulation supports flags only, things could break");
			break;
		}

		case 0x13: //DXSDT (Data Cache Store 32bits from TagLo)
		{
			const int index = (addr >> 6) & 0x3F;
			const int way = addr & 0x1;
			CacheLine line = cache.lineAt(index, way);

			*reinterpret_cast<u32*>(&line.data.bytes[addr & 0x3C]) = cpuRegs.CP0.n.TagLo;

			CACHE_LOG("CACHE DXSDT addr %x, index %d, way %d, DATA %x OP %x", addr, index, way, cpuRegs.CP0.n.TagLo, instr);
			break;
		}

		case 0x12: //DXSTG (Data Cache Store Tag from TagLo)
		{
			const int index = (addr >> 6) & 0x3F;
			const int way = addr & 0x1;
			CacheLine line = cache.lineAt(index, way);

			line.tag.setAddr(cpuRegs.CP0.n.TagLo);
			line.tag.rawValue &= ~CacheTag::ALL_FLAGS;
			line.tag.rawValue |= (cpuRegs.CP0.n.TagLo & CacheTag::ALL_FLAGS);

			CACHE_LOG("CACHE DXSTG addr %x, index %d, way %d, DATA %x OP %x", addr, index, way, cpuRegs.CP0.n.TagLo, instr);
			break;
		}

		case 0x14: //DXWBIN (Data Cache Index WriteBack Invalidate)
		{
			const int index = (addr >> 6) & 0x3F;
			const int way = addr & 0x1;
			CacheLine line = cache.lineAt(index, way);

			CACHE_LOG("CACHE DXWBIN addr %x, index %d, way %d, flags %x paddr %zx", addr, index, way, line.tag.flags(), line.addr());
			line.writeBackIfNeeded();
			line.clear();
			break;
		}

		case 0x7: //IXIN (Instruction Cache Index Invalidate)
		{
			//Not Implemented as we do not have instruction cache
			break;
		}

		case 0xC: //BFH (BTAC Flush)
		{
			//Not Implemented as we do not cache Branch Target Addresses.
			break;
		}

		default:
			DevCon.Warning("Cache mode %x not implemented", mode);
			break;
	}
}

u32 executeCacheDxltgTagSweep(u32 start_pc, u32 fallthrough_pc,
	u32 packed_guests, u32 packed_cycles)
{
	// PCSX2 owners: executeCacheOp()'s DXLTG case, COP0.cpp::MFC0(),
	// R5900OpcodeImpl.cpp scalar arithmetic, and x86 iR5900.cpp::iBranchTest().
	// The caller has matched the exact two-way kernel loop and packed its six
	// distinct GPRs plus the ordinary A32 block-boundary cycle costs.
	constexpr u32 COMPLETE = 0;
	constexpr u32 SELF = 1;
	constexpr u32 EVENT = 2;
	const unsigned result_guest = packed_guests & 0x1f;
	const unsigned comparison_guest = (packed_guests >> 5) & 0x1f;
	const unsigned lower_guest = (packed_guests >> 10) & 0x1f;
	const unsigned upper_guest = (packed_guests >> 15) & 0x1f;
	const unsigned induction_guest = (packed_guests >> 20) & 0x1f;
	const unsigned mask_guest = (packed_guests >> 25) & 0x1f;
	const u32 sync_cycles = packed_cycles & 0x3f;
	const u32 cache_cycles = (packed_cycles >> 6) & 0x3f;
	const u32 compare_cycles = (packed_cycles >> 12) & 0x3f;
	const u32 padding_sync_cycles = (packed_cycles >> 18) & 0x3f;
	const u32 induction_cycles = (packed_cycles >> 24) & 0x3f;
	const u64 induction_value = cpuRegs.GPR.r[induction_guest].UD[0];
	const u32 induction = static_cast<u32>(induction_value);
	const bool batchable = induction_value == static_cast<u64>(
		static_cast<s64>(static_cast<s32>(induction))) &&
		(induction & 63u) == 0 && induction < 4096;
	u32 completed_iterations = 0;

#if defined(VITASX2_QEMU_VALIDATION)
	extern u32 g_qemuCacheDxltgLoopHelperCalls;
	extern u32 g_qemuCacheDxltgLoopCompletedIterations;
	extern u32 g_qemuCacheDxltgLoopFallbackCalls;
	g_qemuCacheDxltgLoopHelperCalls++;
	if (!batchable)
		g_qemuCacheDxltgLoopFallbackCalls++;
#endif

	const auto finish = [&](u32 result) {
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuCacheDxltgLoopCompletedIterations += completed_iterations;
#endif
		return result;
	};
	const auto event_due = []() {
		return static_cast<s32>(static_cast<u32>(cpuRegs.cycle) -
			static_cast<u32>(cpuRegs.nextEventCycle)) >= 0;
	};
	const auto advance = [&](u32 cycles, u32 pc) {
		cpuRegs.cycle += cycles;
		cpuRegs.pc = pc;
		return event_due();
	};
	const auto load_tag = [&](u32 addr) {
		const int index = (addr >> 6) & 0x3f;
		const int way = addr & 1;
		CacheLine line = cache.lineAt(index, way);
		line.writeBackIfNeeded();
		cpuRegs.CP0.n.TagLo = line.tag.flags();
	};
	const auto compare_tag = [&]() {
		const u64 mfc0_value = static_cast<u64>(
			static_cast<s64>(static_cast<s32>(cpuRegs.CP0.n.TagLo)));
		const u64 masked = mfc0_value & cpuRegs.GPR.r[mask_guest].UD[0];
		const u32 sum = static_cast<u32>(masked) +
			cpuRegs.GPR.r[induction_guest].UL[0];
		const u64 value = static_cast<u64>(static_cast<s64>(static_cast<s32>(sum)));
		cpuRegs.GPR.r[comparison_guest].UD[0] =
			cpuRegs.GPR.r[upper_guest].UD[0] < value ? 1 : 0;
		cpuRegs.GPR.r[result_guest].UD[0] =
			value < cpuRegs.GPR.r[lower_guest].UD[0] ? 1 : 0;
		return cpuRegs.GPR.r[result_guest].UD[0] != 0;
	};

	for (;;)
	{
		if (advance(sync_cycles, start_pc + 4))
			return finish(EVENT);
		load_tag(cpuRegs.GPR.r[induction_guest].UL[0]);
		if (advance(cache_cycles, start_pc + 8))
			return finish(EVENT);
		if (advance(sync_cycles, start_pc + 12))
			return finish(EVENT);
		const bool first_taken = compare_tag();
		if (advance(compare_cycles, first_taken ? start_pc + 60 : start_pc + 40))
			return finish(EVENT);
		if (advance(first_taken ? sync_cycles : padding_sync_cycles, start_pc + 64))
			return finish(EVENT);

		load_tag(cpuRegs.GPR.r[induction_guest].UL[0] + 1);
		if (advance(cache_cycles, start_pc + 68))
			return finish(EVENT);
		if (advance(sync_cycles, start_pc + 72))
			return finish(EVENT);
		const bool second_taken = compare_tag();
		if (advance(compare_cycles, second_taken ? start_pc + 120 : start_pc + 100))
			return finish(EVENT);
		if (advance(second_taken ? sync_cycles : padding_sync_cycles, start_pc + 124))
			return finish(EVENT);

		const u32 updated_induction = cpuRegs.GPR.r[induction_guest].UL[0] + 64;
		cpuRegs.GPR.r[induction_guest].SD[0] = static_cast<s32>(updated_induction);
		const bool repeat = cpuRegs.GPR.r[induction_guest].SD[0] < 4096;
		cpuRegs.GPR.r[result_guest].UD[0] = repeat ? 1 : 0;
		completed_iterations++;
		if (advance(induction_cycles, repeat ? start_pc : fallthrough_pc))
			return finish(EVENT);
		if (!repeat)
			return finish(COMPLETE);
		if (!batchable)
			return finish(SELF);
	}
}

void executeCacheDxwbinPairRange(u32 addr, u32 pair_count)
{
	// PCSX2 owner: executeCacheOp()'s DXWBIN case. The kernel walks one set
	// per 64-byte step and selects the two ways with address bits 0 and 1.
	// Keep the two operations ordered so dirty writeback, invalid-PFN loss,
	// retained LRF state, and data clearing are identical to two CACHE ops.
	for (u32 i = 0; i < pair_count; i++, addr += 64)
	{
		for (u32 way_offset = 0; way_offset < 2; way_offset++)
		{
			const u32 operation_addr = addr + way_offset;
			const int index = cache.setIdxFor(operation_addr);
			const int way = operation_addr & 1;
			CacheLine line = cache.lineAt(index, way);
			line.writeBackIfNeeded();
			line.clear();
		}
	}
}

namespace R5900
{
	namespace Interpreter
	{
		namespace OpcodeImpl
		{

			extern int Dcache;
			void CACHE()
			{
				u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
				// CACHE_LOG("cpuRegs.GPR.r[_Rs_].UL[0] = %x, IMM = %x RT = %x", cpuRegs.GPR.r[_Rs_].UL[0], _Imm_, _Rt_);
				executeCacheOp(cpuRegs.code, addr);
			}
		} // end namespace OpcodeImpl

	} // namespace Interpreter
} // namespace R5900
