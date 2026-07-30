// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "MemoryTypes.h"

#include "common/HostSys.h"
#include "common/SingleRegisterTypes.h"

static const uptr VTLB_AllocUpperBounds = _1gb * 2;

// Specialized function pointers for each read type
typedef  mem8_t vtlbMemR8FP(u32 addr);
typedef  mem16_t vtlbMemR16FP(u32 addr);
typedef  mem32_t vtlbMemR32FP(u32 addr);
typedef  mem64_t vtlbMemR64FP(u32 addr);
typedef  RETURNS_R128 vtlbMemR128FP(u32 addr);

// Specialized function pointers for each write type
typedef  void vtlbMemW8FP(u32 addr,mem8_t data);
typedef  void vtlbMemW16FP(u32 addr,mem16_t data);
typedef  void vtlbMemW32FP(u32 addr,mem32_t data);
typedef  void vtlbMemW64FP(u32 addr,mem64_t data);
typedef  void TAKES_R128 vtlbMemW128FP(u32 addr,r128 data);

template <size_t Width, bool Write> struct vtlbMemFP;

template<> struct vtlbMemFP<  8, false> { typedef vtlbMemR8FP   fn; static const uptr Index = 0; };
template<> struct vtlbMemFP< 16, false> { typedef vtlbMemR16FP  fn; static const uptr Index = 1; };
template<> struct vtlbMemFP< 32, false> { typedef vtlbMemR32FP  fn; static const uptr Index = 2; };
template<> struct vtlbMemFP< 64, false> { typedef vtlbMemR64FP  fn; static const uptr Index = 3; };
template<> struct vtlbMemFP<128, false> { typedef vtlbMemR128FP fn; static const uptr Index = 4; };
template<> struct vtlbMemFP<  8,  true> { typedef vtlbMemW8FP   fn; static const uptr Index = 0; };
template<> struct vtlbMemFP< 16,  true> { typedef vtlbMemW16FP  fn; static const uptr Index = 1; };
template<> struct vtlbMemFP< 32,  true> { typedef vtlbMemW32FP  fn; static const uptr Index = 2; };
template<> struct vtlbMemFP< 64,  true> { typedef vtlbMemW64FP  fn; static const uptr Index = 3; };
template<> struct vtlbMemFP<128,  true> { typedef vtlbMemW128FP fn; static const uptr Index = 4; };

typedef u32 vtlbHandler;

extern bool vtlb_Core_Alloc();
extern void vtlb_Core_Free();
extern void vtlb_Alloc_Ppmap();
extern void vtlb_Init();
extern void vtlb_Shutdown();
extern void vtlb_Reset();
extern void vtlb_ResetFastmem();

extern vtlbHandler vtlb_NewHandler();

extern vtlbHandler vtlb_RegisterHandler(
	vtlbMemR8FP* r8,vtlbMemR16FP* r16,vtlbMemR32FP* r32,vtlbMemR64FP* r64,vtlbMemR128FP* r128,
	vtlbMemW8FP* w8,vtlbMemW16FP* w16,vtlbMemW32FP* w32,vtlbMemW64FP* w64,vtlbMemW128FP* w128
);

extern void vtlb_ReassignHandler( vtlbHandler rv,
	vtlbMemR8FP* r8,vtlbMemR16FP* r16,vtlbMemR32FP* r32,vtlbMemR64FP* r64,vtlbMemR128FP* r128,
	vtlbMemW8FP* w8,vtlbMemW16FP* w16,vtlbMemW32FP* w32,vtlbMemW64FP* w64,vtlbMemW128FP* w128
);


extern void vtlb_MapHandler(vtlbHandler handler,u32 start,u32 size);
extern void vtlb_MapBlock(void* base,u32 start,u32 size,u32 blocksize=0);
extern void* vtlb_GetPhyPtr(u32 paddr);
//extern void vtlb_Mirror(u32 new_region,u32 start,u32 size); // -> not working yet :(
extern u32  vtlb_V2P(u32 vaddr);
extern void vtlb_DynV2P();

//virtual mappings
extern void vtlb_VMap(u32 vaddr,u32 paddr,u32 sz);
extern void vtlb_VMapBuffer(u32 vaddr,void* buffer,u32 sz);
extern void vtlb_VMapUnmap(u32 vaddr,u32 sz);
extern bool vtlb_ResolveFastmemMapping(uptr* addr);
extern bool vtlb_GetGuestAddress(uptr host_addr, u32* guest_addr);
extern void vtlb_UpdateFastmemProtection(u32 paddr, u32 size, PageProtectionMode prot);
extern bool vtlb_BackpatchLoadStore(uptr code_address, uptr fault_address);

extern void vtlb_ClearLoadStoreInfo();
extern void vtlb_AddLoadStoreInfo(uptr code_address, u32 code_size, u32 guest_pc, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr);
extern void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr);
extern bool vtlb_IsFaultingPC(u32 guest_pc);

//Memory functions

template< typename DataType >
extern DataType vtlb_memRead(u32 mem);
extern RETURNS_R128 vtlb_memRead128(u32 mem);

template< typename DataType >
extern void vtlb_memWrite(u32 mem, DataType value);
extern void TAKES_R128 vtlb_memWrite128(u32 mem, r128 value);

// "Safe" variants of vtlb, designed for external tools.
// These routines only access the various RAM, and will not call handlers
// which has the potential to change hardware state.
template <typename DataType>
extern DataType vtlb_ramRead(u32 mem, bool* result = nullptr);
template <typename DataType>
extern bool vtlb_ramWrite(u32 mem, const DataType& value);

// NOTE: Does not call MMIO handlers.
extern int vtlb_memSafeCmpBytes(u32 mem, const void* src, u32 size);
extern bool vtlb_memSafeReadBytes(u32 mem, void* dst, u32 size);
extern bool vtlb_memSafeWriteBytes(u32 mem, const void* src, u32 size);

using vtlb_ReadRegAllocCallback = int(*)();
extern int vtlb_DynGenReadNonQuad(u32 bits, bool sign, bool xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);
extern int vtlb_DynGenReadNonQuad_Const(u32 bits, bool sign, bool xmm, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);
extern int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);
extern int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);

extern void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg);
extern void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg);

extern void vtlb_DynGenDispatchers();

namespace vtlb_private
{
	static const uint VTLB_PAGE_BITS = 12;
	static const uint VTLB_PAGE_MASK = 4095;
	static const uint VTLB_PAGE_SIZE = 4096;

	static const uint VTLB_PMAP_SZ		= _1mb * 512;
	static const uint VTLB_PMAP_ITEMS	= VTLB_PMAP_SZ / VTLB_PAGE_SIZE;
	static const uint VTLB_VMAP_ITEMS	= _4gb / VTLB_PAGE_SIZE;

	static const uint VTLB_HANDLER_ITEMS = 128;

	static const uptr POINTER_SIGN_BIT = 1ULL << (sizeof(uptr) * 8 - 1);

	struct VTLBPhysical
	{
	private:
		sptr value;
		explicit VTLBPhysical(sptr value): value(value) { }
	public:
		VTLBPhysical(): value(0) {}
		/// Create from a pointer to raw memory
		static VTLBPhysical fromPointer(void *ptr) { return fromPointer((sptr)ptr); }
		/// Create from an integer representing a pointer to raw memory
		static VTLBPhysical fromPointer(sptr ptr);
		/// Create from a handler and address
		static VTLBPhysical fromHandler(vtlbHandler handler);

		/// Get the raw value held by the entry
		uptr raw() const { return value; }
		/// Returns whether or not this entry is a handler
		bool isHandler() const { return value < 0; }
		/// Assumes the entry is a pointer, giving back its value
		uptr assumePtr() const;
		/// Assumes the entry is a handler, and gets the raw handler ID
		u8 assumeHandler() const { return value; }
	};

	struct VTLBVirtual
	{
	private:
		uptr value;
		explicit VTLBVirtual(uptr value): value(value) { }
	public:
		VTLBVirtual(): value(0) {}
		VTLBVirtual(VTLBPhysical phys, u32 paddr, u32 vaddr);
		static VTLBVirtual fromPointer(uptr ptr, u32 vaddr) {
			return VTLBVirtual(VTLBPhysical::fromPointer(ptr), 0, vaddr);
		}

		/// Get the raw value held by the entry
		uptr raw() const { return value; }
		/// Returns whether or not this entry is a handler
		bool isHandler(u32 vaddr) const { return (sptr)(value + vaddr) < 0; }
		/// Assumes the entry is a pointer, giving back its value
		uptr assumePtr(u32 vaddr) const;
		/// Assumes the entry is a handler, and gets the raw handler ID
		u8 assumeHandlerGetID() const { return value; }
		/// Assumes the entry is a handler, and gets the physical address
		u32 assumeHandlerGetPAddr(u32 vaddr) const { return static_cast<u32>((value + vaddr - assumeHandlerGetID()) & ~POINTER_SIGN_BIT); }
		/// Assumes the entry is a handler, returning it as a void*
		void *assumeHandlerGetRaw(int index, bool write) const;
		/// Assumes the entry is a handler, returning it
		template <size_t Width, bool Write>
		typename vtlbMemFP<Width, Write>::fn *assumeHandler() const;
	};

	struct MapData
	{
		// first indexer -- 8/16/32/64/128 bit tables [values 0-4]
		// second indexer -- read/write  [0 or 1]
		// third indexer -- 128 possible handlers!
		void* RWFT[5][2][VTLB_HANDLER_ITEMS];

		VTLBPhysical pmap[VTLB_PMAP_ITEMS]; //512KB // PS2 physical to x86 physical

		VTLBVirtual* vmap;                //4MB (allocated by vtlb_init) // PS2 virtual to x86 physical

		u32* ppmap;               //4MB (allocated by vtlb_init) // PS2 virtual to PS2 physical

		uptr fastmem_base;
#if defined(ARCH_ARM32)
		uptr host_memory_base;
#endif

		MapData()
		{
			vmap = NULL;
			ppmap = NULL;
			fastmem_base = 0;
#if defined(ARCH_ARM32)
			host_memory_base = 0;
#endif
		}
	};

	alignas(64) extern MapData vtlbdata;

	// ARM32 cannot reserve PCSX2's 4 GiB fastmem aperture. While the default
	// low virtual window is still the identity-mapped retail EE RAM range, the
	// A32 JIT can nevertheless address that overwhelmingly common range through
	// host_memory_base without consulting the 4 MiB virtual-page table. A low
	// vmap mutation which changes that identity relation clears this proof before
	// generated code can be reused; identity TLB publication preserves it.
	bool HasDefaultMainRamIdentityWindow();
	void SetDefaultMainRamIdentityWindow(bool enabled);

	inline uptr VTLBPhysical::assumePtr() const
	{
#if defined(ARCH_ARM32)
		return vtlbdata.host_memory_base + value;
#else
		return value;
#endif
	}

	inline uptr VTLBVirtual::assumePtr(u32 vaddr) const
	{
#if defined(ARCH_ARM32)
		return vtlbdata.host_memory_base + value + vaddr;
#else
		return value + vaddr;
#endif
	}

	inline void *VTLBVirtual::assumeHandlerGetRaw(int index, bool write) const
	{
		return vtlbdata.RWFT[index][write][assumeHandlerGetID()];
	}

	template <size_t Width, bool Write>
	typename vtlbMemFP<Width, Write>::fn *VTLBVirtual::assumeHandler() const
	{
		using FP = vtlbMemFP<Width, Write>;
		return (typename FP::fn *)assumeHandlerGetRaw(FP::Index, Write);
	}
}

enum vtlb_ProtectionMode
{
	ProtMode_None = 0, // page is 'unaccounted' -- neither protected nor unprotected
	ProtMode_Write, // page is under write protection (exception handler)
	ProtMode_Manual, // page is under manual protection (self-checked at execution)
	ProtMode_NotRequired // page doesn't require any protection
};

extern vtlb_ProtectionMode mmap_GetRamPageInfo(u32 paddr);
extern void mmap_MarkCountedRamPage(u32 paddr);
extern void mmap_ResetBlockTracking();

enum VitaEePreincrementByteZeroFillResult : u32
{
	VITA_EE_PREINCREMENT_BYTE_ZERO_FILL_COMPLETE = 0,
	VITA_EE_PREINCREMENT_BYTE_ZERO_FILL_SELF = 1,
	VITA_EE_PREINCREMENT_BYTE_ZERO_FILL_EVENT = 2,
	VITA_EE_PREINCREMENT_BYTE_ZERO_FILL_REDISPATCH = 3,
};

// Executes the exact ADDIU/SB-zero/BNE pre-increment byte-fill loop. Direct
// vTLB pages batch only to a page, completion, or event seam; handler and SMC
// pages execute one architecturally ordered store and return canonically.
u32 VitaEeExecutePreincrementByteZeroFill(u32 start_pc, u32 fallthrough_pc,
	u32 block_cycles, u32 packed_guests);

enum VitaEeFourWordFillResult : u32
{
	VITA_EE_FOUR_WORD_FILL_COMPLETE = 0,
	VITA_EE_FOUR_WORD_FILL_SELF = 1,
	VITA_EE_FOUR_WORD_FILL_EVENT = 2,
	VITA_EE_FOUR_WORD_FILL_REDISPATCH = 3,
};

// Executes the exact SW/ADDIU/SW/SW/SW/BNE four-word fill loop. Direct vTLB
// pages batch only whole iterations to a page, completion, or event seam;
// handler and SMC pages execute one architecturally ordered iteration.
u32 VitaEeExecuteFourWordFill(u32 start_pc, u32 fallthrough_pc,
	u32 block_cycles, u32 packed_guests);

enum VitaEePreincrementWordFillResult : u32
{
	VITA_EE_PREINCREMENT_WORD_FILL_COMPLETE = 0,
	VITA_EE_PREINCREMENT_WORD_FILL_SELF = 1,
	VITA_EE_PREINCREMENT_WORD_FILL_EVENT = 2,
	VITA_EE_PREINCREMENT_WORD_FILL_REDISPATCH = 3,
};

// Executes the exact ADDIU/SW/SUBU/SLTIU/BEQ/NOP word-fill loop. Direct pages
// batch to completion, page, or event; observable mappings execute one store.
u32 VitaEeExecutePreincrementWordFill(u32 start_pc, u32 fallthrough_pc,
	u32 packed_cycles, u32 packed_guests);

enum VitaEeSelfAddressPairScanResult : u32
{
	VITA_EE_SELF_ADDRESS_PAIR_SCAN_COMPLETE = 0,
	VITA_EE_SELF_ADDRESS_PAIR_SCAN_SELF = 1,
	VITA_EE_SELF_ADDRESS_PAIR_SCAN_EVENT = 2,
	VITA_EE_SELF_ADDRESS_PAIR_SCAN_MISMATCH = 3,
};

// Executes the exact two-LW self-address verification loop. Direct vTLB pages
// scan until mismatch, count limit, page boundary, or one of the three original
// scheduler seams; handler/crossing pairs execute one ordered iteration.
u32 VitaEeExecuteSelfAddressPairScan(u32 start_pc, u32 packed_guests,
	u32 packed_cycles, u32 load_cycles);

enum VitaEeWordCopyResult : u32
{
	VITA_EE_WORD_COPY_COMPLETE = 0,
	VITA_EE_WORD_COPY_SELF = 1,
	VITA_EE_WORD_COPY_EVENT = 2,
	VITA_EE_WORD_COPY_REDISPATCH = 3,
};

// Executes the exact LW/ADDIU/SW/ADDIU/BNE/NOP forward word-copy loop.
// Direct source/destination pages batch to completion, either page, or event;
// handler and protected mappings execute one architecturally ordered iteration.
u32 VitaEeExecuteWordCopy(u32 start_pc, u32 fallthrough_pc,
	u32 packed_cycles, u32 packed_guests);

enum VitaEeSignedHalfwordBytePackResult : u32
{
	VITA_EE_SIGNED_HALFWORD_BYTE_PACK_COMPLETE = 0,
	VITA_EE_SIGNED_HALFWORD_BYTE_PACK_SELF = 1,
	VITA_EE_SIGNED_HALFWORD_BYTE_PACK_EVENT = 2,
	VITA_EE_SIGNED_HALFWORD_BYTE_PACK_REDISPATCH = 3,
};

// Executes the exact one- or two-source signed-halfword clamp,
// four-PPACB/four-SQ conversion loops recognized by VitaEE::BlockCompiler.
// The two-source form performs wrapping PADDH before the clamp. Direct pages
// batch to the counter, page, or event seam. Observable mappings execute one
// complete, architecturally ordered iteration.
u32 VitaEeExecuteSignedHalfwordBytePack(u32 start_pc, u32 packed_control,
	u32 packed_guests0, u32 packed_guests1);

// --------------------------------------------------------------------------------------
//  Goemon game fix
// --------------------------------------------------------------------------------------
struct GoemonTlb {
	u32 valid;
	u32 unk1; // could be physical address also
	u32 unk2;
	u32 low_add;
	u32 physical_add;
	u32 unk3; // likely the size
	u32 high_add;
	u32 key; // uniq number attached to an allocation
	u32 unk5;
};
