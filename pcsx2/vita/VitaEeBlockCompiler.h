// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

namespace VitaA32
{
	class CodeBuffer;
	enum class Condition : u8;
	enum class ShiftType : u8;
}

namespace VitaEE
{
	// Persistent event callbacks receive a negative exact scaled-cycle count when
	// the generated source is a PCSX2-proven, side-effect-free unconditional
	// wait. The sign bit distinguishes it from every public BlockExitKind token;
	// small waits materialize as one A32 MVN-class immediate instead of MOVW/MOVT.
	inline constexpr u32 RETAINED_UNCONDITIONAL_WAIT_EVENT_MASK = 0x80000000u;
	inline constexpr u32 RETAINED_UNCONDITIONAL_WAIT_MAX_CYCLES = 0x7fffffffu;

	// These preserve PCSX2's exact multi-block wait-loop phase at a scheduler
	// boundary. The generated tail and the retained CPU0 event bridge share the
	// same implementation so skipped redispatches cannot drift in PC/cycle
	// placement.
	u32 AdvancePollCallWaitFromPcToEvent(u32 start_pc, u32 packed_cycles,
		u32 leaf_pc, u32 return_pc, u32 call_pc);
	u32 AdvanceTwoPredicateWaitFromPcToEvent(u32 start_pc,
		u32 prefix_cycles, u32 tail_cycles, u32 loop_pc, u32 tail_pc);
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_QEMU_PROVIDER_FIXTURE)
	void PublishTwoPredicateWaitSchedulerCertificateForValidation(
		u32 prefix_cycles, u32 tail_cycles, u32 loop_pc, u32 tail_pc);
#endif

	// Immutable code ranges consumed when a backward EE wait proof follows a
	// static JAL into a pure load leaf. The loop block already owns its branch
	// range; the executor attaches these disjoint ranges to the same recClear()
	// and RAM-source invalidation lifetime.
	struct PollCallWaitLoopSourceProof
	{
		bool valid = false;
		u32 call_pc = 0;
		u32 leaf_pc = 0;
		u32 call_scaled_cycles = 0;
		u32 leaf_scaled_cycles = 0;
		std::array<u32, 2> call_opcodes{};
		std::array<u32, 2> leaf_opcodes{};
	};

	// A two-stage RAM predicate loop can contain an early forward exit before
	// its final backward branch:
	//
	//   lw predicate_a,...       lw predicate_b,...
	//   [andi predicate_a,...]   beq predicate_b,zero,loop
	//   bne predicate_a,zero,out nop
	//   nop
	//
	// Reaching the backward edge proves both predicate results are zero, so
	// those are also the exact architectural values at either PCSX2 block
	// scheduler seam. Keep the complete contiguous source range attached to the
	// generated tail because it depends on the earlier forward-exit block too.
	struct TwoPredicateWaitLoopSourceProof
	{
		bool valid = false;
		u32 loop_pc = 0;
		u32 tail_pc = 0;
		u32 instruction_count = 0;
		u32 prefix_scaled_cycles = 0;
		u32 tail_scaled_cycles = 0;
		std::array<u32, 7> opcodes{};
	};

	template <typename T, size_t Capacity>
	class FixedCompileBuffer
	{
		static_assert(Capacity != 0);
		static_assert(std::is_trivially_copyable_v<T>);
		static_assert(std::is_trivially_destructible_v<T>);

	public:
		bool push_back(const T& value)
		{
			if (m_size >= Capacity)
				return false;

			// memcpy starts the lifetime of these implicit-lifetime records without
			// default-constructing every unused slot. Several tail records deliberately
			// default sentinel fields to SIZE_MAX; constructing all N/2N slots would
			// turn this process-lifetime workspace into more than 256 KiB of packaged
			// initialized data instead of zero-fill storage.
			std::memcpy(m_storage[m_size].bytes, &value, sizeof(T));
			m_size++;
			return true;
		}

		void clear() { m_size = 0; }
		size_t size() const { return m_size; }
		T& operator[](size_t index)
		{
			return *std::launder(reinterpret_cast<T*>(m_storage[index].bytes));
		}
		const T& operator[](size_t index) const
		{
			return *std::launder(reinterpret_cast<const T*>(m_storage[index].bytes));
		}

	private:
		struct Slot
		{
			alignas(T) std::byte bytes[sizeof(T)];
		};
		static_assert(sizeof(Slot) == sizeof(T));

		// Slot pointer arithmetic stays within a real Slot array. Each indexed
		// access then launders the T object whose lifetime push_back() started in
		// that slot, rather than pretending the independently created records form
		// a T array.
		Slot m_storage[Capacity];
		size_t m_size = 0;
	};

	class BlockExecutor;

	enum class DirectContinuationKind : u8
	{
		SchedulerTestedTail,
		Pcsx2ShortSplit,
		A32PhysicalFragment,
		HotRegionInternalStaticBranch,
	};

	enum class CompatibleVtlbGuardKind : u8
	{
		None,
		Read,
		Write,
	};

	struct CompatibleVtlbFastEntryOffsets
	{
		size_t read = static_cast<size_t>(-1);
		size_t write = static_cast<size_t>(-1);

		size_t For(CompatibleVtlbGuardKind kind) const
		{
			return kind == CompatibleVtlbGuardKind::Read ? read :
				kind == CompatibleVtlbGuardKind::Write ? write :
				static_cast<size_t>(-1);
		}
	};

	struct DirectLinkSlot
	{
		u32 target_pc = 0;
		size_t target_offset = static_cast<size_t>(-1);
		size_t fallback_offset = static_cast<size_t>(-1);
		size_t secondary_target_offset = static_cast<size_t>(-1);
		size_t canonical_target_offset = static_cast<size_t>(-1);
		u32 canonical_fallback_instruction = 0;
		u32 embedded_active_instruction = 0;
		u32 embedded_source_opcodes[2]{};
		bool branch_on_taken = false;
		bool branch_on_unsigned_less = false;
		bool branch_if_no_event = false;
		bool secondary_branch_unconditional = false;
		bool embedded_compatible_continuation = false;
		bool embedded_continuation_active = false;
		bool patched_to_resident_entry = false;
		bool patched_to_compatible_entry = false;
		bool patched_to_canonical_entry = false;
		bool requires_compatible_entry = false;
		bool canonicalizes_reclaimed_vtlb_hosts = false;
		bool compatible_scheduler_countdown = false;
		bool compatible_vtlb_pointer = false;
		bool prevalidated_vtlb_read_pointer = false;
		bool prevalidated_vtlb_write_pointer = false;
		u8 compatible_entry_instructions = 0;
		u8 compatible_entry_loads = 0;
		u8 compatible_words = 0;
		u8 compatible_dirty_words = 0;
		bool valid = false;
	};

	enum class SchedulerLinkRepresentation : u8
	{
		CycleLowMinusNextEventLow,
	};

	enum class SchedulerLinkProvenance : u8
	{
		CanonicalOrCompatibleScheduler,
	};

	struct SchedulerLinkMapping
	{
		static constexpr u8 NO_HOST = 0xff;

		u8 host = NO_HOST;
		SchedulerLinkRepresentation representation =
			SchedulerLinkRepresentation::CycleLowMinusNextEventLow;
		SchedulerLinkProvenance provenance =
			SchedulerLinkProvenance::CanonicalOrCompatibleScheduler;

		bool IsValid() const { return host != NO_HOST; }
		bool operator==(const SchedulerLinkMapping& rhs) const
		{
			return host == rhs.host && representation == rhs.representation &&
				provenance == rhs.provenance;
		}
	};

	enum class VtlbPointerLinkWidth : u8
	{
		Byte8,
		BytePair8,
		Word32,
		Qword128,
	};

	enum class VtlbPointerLinkDirection : u8
	{
		Read,
		Write,
	};

	enum class VtlbPointerLinkRepresentation : u8
	{
		DirectHostAddress,
	};

	enum class VtlbPointerLinkProvenance : u8
	{
		VtlbVirtualMapping,
	};

	struct VtlbPointerLinkMapping
	{
		static constexpr u8 NO_HOST = 0xff;

		u8 host = NO_HOST;
		u8 guest_address = 0;
		// Read mappings name architectural result GPRs; a write mapping uses
		// guest_result as its architectural value source and leaves result2 zero.
		u8 guest_result = 0;
		u8 guest_result2 = 0;
		u8 stride = 0;
		// The owning block may contain the access after its entry. access_pc names
		// the exact guest instruction; access_block_pc names the compatible target.
		u32 access_block_pc = 0;
		u32 access_pc = 0;
		u32 advance_pc = 0;
		VtlbPointerLinkWidth width = VtlbPointerLinkWidth::Word32;
		VtlbPointerLinkDirection direction = VtlbPointerLinkDirection::Read;
		VtlbPointerLinkRepresentation representation =
			VtlbPointerLinkRepresentation::DirectHostAddress;
		VtlbPointerLinkProvenance provenance =
			VtlbPointerLinkProvenance::VtlbVirtualMapping;

		bool IsValid() const { return host != NO_HOST; }
		bool operator==(const VtlbPointerLinkMapping& rhs) const
		{
			return host == rhs.host && guest_address == rhs.guest_address &&
				guest_result == rhs.guest_result && guest_result2 == rhs.guest_result2 &&
				stride == rhs.stride && access_block_pc == rhs.access_block_pc &&
				access_pc == rhs.access_pc && advance_pc == rhs.advance_pc &&
				width == rhs.width && direction == rhs.direction &&
				representation == rhs.representation && provenance == rhs.provenance;
		}
	};

	enum class VtlbStaticPageLinkRepresentation : u8
	{
		DirectHostBase,
	};

	enum class VtlbStaticPageLinkProvenance : u8
	{
		GuardedVtlbVirtualMapping,
	};

	struct VtlbStaticPageLinkMapping
	{
		static constexpr u8 NO_HOST = 0xff;

		u8 host = NO_HOST;
		u8 guest_base = 0;
		u8 alignment_mask = 0;
		u16 max_offset_end = 0;
		u16 access_count = 0;
		u32 access_block_pc = 0;
		// A nonzero PC names a same-register ADDIU after the final access. The
		// translated page is valid through that instruction, then poisoned so the
		// next compatible entry translates the advanced architectural base once.
		u32 invalidate_after_pc = 0;
		VtlbStaticPageLinkRepresentation representation =
			VtlbStaticPageLinkRepresentation::DirectHostBase;
		VtlbStaticPageLinkProvenance provenance =
			VtlbStaticPageLinkProvenance::GuardedVtlbVirtualMapping;

		bool IsValid() const { return host != NO_HOST; }
		bool operator==(const VtlbStaticPageLinkMapping& rhs) const
		{
			return host == rhs.host && guest_base == rhs.guest_base &&
				alignment_mask == rhs.alignment_mask &&
				max_offset_end == rhs.max_offset_end &&
				access_count == rhs.access_count &&
				access_block_pc == rhs.access_block_pc &&
				invalidate_after_pc == rhs.invalidate_after_pc &&
				representation == rhs.representation &&
				provenance == rhs.provenance;
		}
	};

	enum class GprLinkWidth : u8
	{
		Low32,
		Low64,
	};

	enum class GprLinkDirtyState : u8
	{
		Clean,
		WriteBack,
	};

	enum class GprLinkRepresentation : u8
	{
		Architectural,
	};

	enum class GprLinkProvenance : u8
	{
		CanonicalOrCompatibleGpr,
	};

	struct GprLinkMapping
	{
		static constexpr u8 NO_HOST = 0xff;

		u8 guest = 0;
		u8 low_host = NO_HOST;
		u8 high_host = NO_HOST;
		GprLinkWidth width = GprLinkWidth::Low32;
		GprLinkDirtyState dirty = GprLinkDirtyState::Clean;
		GprLinkRepresentation representation = GprLinkRepresentation::Architectural;
		GprLinkProvenance provenance = GprLinkProvenance::CanonicalOrCompatibleGpr;

		bool operator==(const GprLinkMapping& rhs) const
		{
			return guest == rhs.guest && low_host == rhs.low_host &&
				high_host == rhs.high_host && width == rhs.width && dirty == rhs.dirty &&
				representation == rhs.representation && provenance == rhs.provenance;
		}
	};

	enum class GprQwordLinkRepresentation : u8
	{
		RawArchitecturalBacking,
	};

	enum class GprQwordLinkProvenance : u8
	{
		CanonicalOrCompatibleGpr,
	};

	struct GprQwordLinkMapping
	{
		static constexpr u8 NO_HOST = 0xff;

		u8 host_qreg = NO_HOST;
		u8 guest = 0;
		GprQwordLinkRepresentation representation =
			GprQwordLinkRepresentation::RawArchitecturalBacking;
		GprQwordLinkProvenance provenance =
			GprQwordLinkProvenance::CanonicalOrCompatibleGpr;

		bool IsValid() const { return host_qreg != NO_HOST; }
		bool operator==(const GprQwordLinkMapping& rhs) const
		{
			return host_qreg == rhs.host_qreg && guest == rhs.guest &&
				representation == rhs.representation && provenance == rhs.provenance;
		}
	};

	enum class PredicateLinkRepresentation : u8
	{
		ArchitecturalZeroOrOne,
	};

	enum class PredicateLinkProvenance : u8
	{
		UnsignedLessThanToNotEqualZero,
	};

	struct PredicateLinkMapping
	{
		static constexpr u8 NO_HOST = 0xff;

		u8 host = NO_HOST;
		u8 guest = 0;
		u32 producer_block_pc = 0;
		u32 producer_pc = 0;
		u32 consumer_pc = 0;
		PredicateLinkRepresentation representation =
			PredicateLinkRepresentation::ArchitecturalZeroOrOne;
		PredicateLinkProvenance provenance =
			PredicateLinkProvenance::UnsignedLessThanToNotEqualZero;

		bool IsValid() const { return host != NO_HOST; }
		bool operator==(const PredicateLinkMapping& rhs) const
		{
			return host == rhs.host && guest == rhs.guest &&
				producer_block_pc == rhs.producer_block_pc &&
				producer_pc == rhs.producer_pc && consumer_pc == rhs.consumer_pc &&
				representation == rhs.representation && provenance == rhs.provenance;
		}
	};

	struct GprLinkSignature
	{
		static constexpr u8 MAX_PINS = 5;
		static constexpr u8 MAX_BLOCKS = 3;
		static constexpr u8 FIRST_HOST = 7;
		static constexpr u8 DEFAULT_FIRST_HOST = 9;
		static constexpr u8 LAST_CALLEE_HOST = 11;
		static constexpr u8 LINK_REGISTER_HOST = 14;
		static constexpr u8 SCHEDULER_HOST = 6;
		static constexpr u8 VTLB_POINTER_HOST = 12;
		static constexpr u8 VTLB_WRITE_POINTER_HOST = 3;
		static constexpr u8 VTLB_STATIC_PAGE_HOST = 11;
		static constexpr u8 PREDICATE_HOST = 5;

		GprLinkMapping mappings[MAX_PINS]{};
		SchedulerLinkMapping scheduler{};
		VtlbPointerLinkMapping vtlb_pointer{};
		VtlbPointerLinkMapping vtlb_write_pointer{};
		VtlbStaticPageLinkMapping vtlb_static_page{};
		GprQwordLinkMapping gpr_qword{};
		PredicateLinkMapping predicate{};
		u32 block_pcs[MAX_BLOCKS]{};
		u8 count = 0;
		u8 block_count = 0;

		bool IsValid() const;
		bool ContainsPc(u32 pc) const;
		bool HasWriteBack() const;
		bool HasSchedulerCountdown() const { return scheduler.IsValid(); }
		bool HasVtlbPointer() const { return vtlb_pointer.IsValid(); }
		bool HasVtlbWritePointer() const { return vtlb_write_pointer.IsValid(); }
		bool HasVtlbStaticPage() const { return vtlb_static_page.IsValid(); }
		bool HasGprQword() const { return gpr_qword.IsValid(); }
		bool HasPredicate() const { return predicate.IsValid(); }
		bool ReclaimsVtlbHosts() const;
		u8 WordCount() const;
		u8 DirtyWordCount() const;
		bool operator==(const GprLinkSignature& rhs) const
		{
			if (count != rhs.count || block_count != rhs.block_count ||
				!(scheduler == rhs.scheduler) ||
				!(vtlb_pointer == rhs.vtlb_pointer) ||
				!(vtlb_write_pointer == rhs.vtlb_write_pointer) ||
				!(vtlb_static_page == rhs.vtlb_static_page) ||
				!(gpr_qword == rhs.gpr_qword) ||
				!(predicate == rhs.predicate) ||
				block_pcs[0] != rhs.block_pcs[0] || block_pcs[1] != rhs.block_pcs[1] ||
				block_pcs[2] != rhs.block_pcs[2])
				return false;
			for (u8 i = 0; i < count; i++)
			{
				if (!(mappings[i] == rhs.mappings[i]))
					return false;
			}
			return true;
		}
	};

	struct DirectLinkSlots
	{
		DirectLinkSlot slots[2]{};
	};

	void RefreshRawGpr0KnownZero();

	enum class SignedBranchCondition : u8
	{
		LessThanZero,
		GreaterEqualZero,
		LessEqualZero,
		GreaterThanZero,
	};

	class BlockCompiler
	{
		struct CompileScratch;
		friend class BlockExecutor;

		enum class MmiVectorOp : u8
		{
			AddWord,
			SubtractWord,
			CompareGreaterSignedWord,
			MaxSignedWord,
			AddHalfword,
			SubtractHalfword,
			CompareGreaterSignedHalfword,
			MaxSignedHalfword,
			AddByte,
			SubtractByte,
			CompareGreaterSignedByte,
			CompareEqualWord,
			MinSignedWord,
			CompareEqualHalfword,
			MinSignedHalfword,
			CompareEqualByte,
			SaturatingAddSignedWord,
			SaturatingSubtractSignedWord,
			SaturatingAddSignedHalfword,
			SaturatingSubtractSignedHalfword,
			SaturatingAddSignedByte,
			SaturatingSubtractSignedByte,
			SaturatingAddUnsignedWord,
			SaturatingSubtractUnsignedWord,
			SaturatingAddUnsignedHalfword,
			SaturatingSubtractUnsignedHalfword,
			SaturatingAddUnsignedByte,
			SaturatingSubtractUnsignedByte,
			BitwiseAnd,
			BitwiseXor,
			BitwiseOr,
			BitwiseNor,
		};

		enum class MmiUnaryVectorOp : u8
		{
			AbsoluteSignedWord,
			AbsoluteSignedHalfword,
		};

		enum class MmiImmediateShiftOp : u8
		{
			ShiftLeftHalfword,
			ShiftRightLogicalHalfword,
			ShiftRightArithmeticHalfword,
			ShiftLeftWord,
			ShiftRightLogicalWord,
			ShiftRightArithmeticWord,
		};

		enum class MmiVariableWordShiftOp : u8
		{
			ShiftLeftLogical,
			ShiftRightLogical,
			ShiftRightArithmetic,
		};

		enum class MmiHalfwordShuffleOp : u8
		{
			Pinth,
			Pinteh,
			Pexeh,
			Prevh,
			Pexch,
			Pcpyh,
		};

		enum class MmiWordShuffleOp : u8
		{
			Pcpyld,
			Pcpyud,
			Pexew,
			Prot3w,
			Pexcw,
		};

		enum class MmiFiveBitOp : u8
		{
			Expand,
			Pack,
		};

		enum class MmiUpperInterleaveOp : u8
		{
			Word,
			Halfword,
			Byte,
		};

		enum class ScalarLoadWidth : u8
		{
			Byte,
			Halfword,
			Word,
			Dword,
		};

		enum class ScalarStoreWidth : u8
		{
			Byte,
			Halfword,
			Word,
			Dword,
		};

	public:
		using RamWriteInvalidationCallback = void (*)(void* context,
			u32 backing_offset, u32 size);

		// Private EE chain ABI: persistent entry captures its optional event bridge
		// from r3 before generated blocks may use it; r4-r11 and LR/PC are saved by
		// either the callable block prologue or the persistent dispatcher.
		static constexpr u16 LINK_FRAME_REGISTER_MASK = 0x0ff8u;
		static constexpr u8 PERSISTENT_LINK_METADATA_SIZE = 24;
		static constexpr u8 PERSISTENT_LINK_CONTEXT_OFFSET = 0;
		static constexpr u8 PERSISTENT_LINK_CALLBACK_OFFSET = 4;
		static constexpr u8 PERSISTENT_LINK_EXIT_VALUE_OFFSET = 8;
		static constexpr u8 PERSISTENT_LINK_EVENT_CALLBACK_OFFSET = 12;
		static constexpr u8 PERSISTENT_LINK_VTLB_VMAP_OFFSET = 16;
		static constexpr u8 PERSISTENT_LINK_VTLB_HOST_BASE_OFFSET = 20;
		// PCSX2 recRecompile() can own one complete 4 KiB source page plus an
		// atomic branch follower on the next page. The executor and this reusable
		// compile workspace share that exact discovery ceiling.
		static constexpr u32 MAX_COMPILE_INSTRUCTIONS = 1025;
		// A page-positive direct RAM store uses this finer conservative
		// source-ownership granularity before entering the exact PCSX2
		// recClear-equivalent callback.
		static constexpr u8 RAM_SOURCE_GUARD_CHUNK_SHIFT = 6;

		explicit BlockCompiler(VitaA32::CodeBuffer& code,
			const u8* ram_source_page_live_flags = nullptr,
			const u8* ram_source_chunk_live_bits = nullptr,
			void* ram_write_invalidation_context = nullptr,
			RamWriteInvalidationCallback ram_write_invalidation_callback = nullptr);

#if defined(VITASX2_QEMU_VALIDATION)
		void SetVtlbLinkedEntryPcPublicationEnabled(bool enabled)
		{
			m_vtlb_linked_entry_pc_publication_enabled = enabled;
		}
		void SetCompatibleLikelyTakenSuffixEnabled(bool enabled)
		{
			m_compatible_likely_taken_suffix_enabled = enabled;
		}
		void SetCompatiblePredicateEntryVariantEnabled(bool enabled)
		{
			m_compatible_predicate_entry_variant_enabled = enabled;
		}
		void SetEmbeddedCompatibleContinuationEnabled(bool enabled)
		{
			m_embedded_compatible_continuation_enabled = enabled;
		}
		void SetFusedDirectEventLinkEnabled(bool enabled)
		{
			m_fused_direct_event_link_enabled = enabled;
		}
		void SetCombinedCompatibleTakenEventEnabled(bool enabled)
		{
			m_combined_compatible_taken_event_enabled = enabled;
		}
		void SetCompatibleVtlbWriteGuardHoistEnabled(bool enabled)
		{
			m_compatible_vtlb_write_guard_hoist_enabled = enabled;
		}
		void SetCompatibleVtlbReadGuardHoistEnabled(bool enabled)
		{
			m_compatible_vtlb_read_guard_hoist_enabled = enabled;
		}
		void SetDirectLinkRejectionProfilingEnabled(bool enabled)
		{
			m_direct_link_rejection_profiling_enabled = enabled;
		}
#endif

		static bool CanCompileOpcode(u32 op);
		static bool IsSupportedBranchOpcode(u32 op);
		static bool IsBranchLikely(u32 op);
		static bool CanCompileDelaySlotOpcode(u32 op);
		static bool CanCompileDelaySlotOpcode(u32 branch_op, u32 delay_op);
		static bool IsExactPreincrementByteZeroFillLoop(u32 start_pc, u32 instruction_count,
			unsigned* pointer_guest = nullptr, unsigned* end_guest = nullptr);
		static bool IsExactFourWordFillLoop(u32 start_pc, u32 instruction_count,
			unsigned* pointer_guest = nullptr, unsigned* end_guest = nullptr,
			unsigned* value_guest = nullptr);
		static bool IsExactPreincrementWordFillLoop(u32 start_pc, u32 instruction_count,
			unsigned* pointer_guest = nullptr, unsigned* end_guest = nullptr,
			unsigned* value_guest = nullptr, unsigned* result_guest = nullptr);
		static bool IsExactSelfAddressPairScan(u32 start_pc, u32 instruction_count,
			unsigned* result_guest = nullptr, unsigned* pointer_guest = nullptr,
			unsigned* count_guest = nullptr);
		static bool IsExactWordCopyLoop(u32 start_pc, u32 instruction_count,
			unsigned* value_guest = nullptr, unsigned* source_guest = nullptr,
			unsigned* destination_guest = nullptr, unsigned* end_guest = nullptr);
		static bool IsExactGsCsrVsintPollLoop(u32 start_pc, u32 instruction_count,
			unsigned* base_guest = nullptr, unsigned* result_guest = nullptr);
		static bool IsExactDmacChcrStrPollLoop(u32 start_pc,
			u32 instruction_count, unsigned* base_guest = nullptr,
			unsigned* result_guest = nullptr, unsigned* shift = nullptr,
			u16* mask = nullptr);
		static bool IsExactSignedCountdownLoop(u32 start_pc, u32 instruction_count,
			unsigned* countdown_guest = nullptr, unsigned* delay_result_guest = nullptr);
		static bool BuildGprLinkSignature(u32 first_pc, u32 first_instruction_count,
			u32 second_pc, u32 second_instruction_count, GprLinkSignature* signature,
			bool reclaim_vtlb_hosts = true);
		static bool BuildGprLinkSignature(const u32* block_pcs,
			const u32* block_instruction_counts, u8 block_count,
			GprLinkSignature* signature, bool reclaim_vtlb_hosts = true);

		bool BeginBlock(bool use_vtlb_registers = false, bool use_cop1_exponent_mask_register = false,
			bool use_vu0_base_register = false, size_t* linked_entry_offset = nullptr,
			u32 linked_entry_pc = 0, bool linked_entry_needs_pc_sync = false);
		bool EmitCpuProfilerBlockPc(u32 start_pc, bool preserve_temporaries = false);
		bool EmitHotRegionEntryCounter(u32* counter, void* request_slot,
			const void* request_value, u32 request_threshold,
			size_t* counter_offset, u8* counter_instruction_count);
		bool CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit, const void* event_exit,
			u32* scaled_cycles = nullptr, DirectLinkSlots* direct_links = nullptr,
			const void* indirect_lookup_pages_slot = nullptr, const void* direct_linking_enabled_flag = nullptr,
			size_t* linked_entry_offset = nullptr, bool persistent_dispatch_exits = false,
			size_t* resident_self_link_entry_offset = nullptr,
			u8* resident_self_link_entry_loads = nullptr,
			const GprLinkSignature* gpr_link_signature = nullptr,
			size_t* compatible_link_entry_offset = nullptr,
			u8* compatible_link_entry_loads = nullptr,
			CompatibleVtlbFastEntryOffsets* compatible_vtlb_fast_entries = nullptr,
			DirectContinuationKind direct_continuation_kind =
				DirectContinuationKind::SchedulerTestedTail,
			bool* scheduler_test_elided_continuation_emitted = nullptr,
			const void* scheduler_test_elided_direct_exit = nullptr,
			const void* retained_wait_event_exit = nullptr,
			PollCallWaitLoopSourceProof* poll_call_wait_loop_source_proof = nullptr,
			TwoPredicateWaitLoopSourceProof* two_predicate_wait_loop_source_proof = nullptr,
			u32 hot_region_guard_cycles = 0,
			const void* hot_region_fallback_entry = nullptr,
			u32* hot_region_entry_counter = nullptr,
			void* hot_region_request_slot = nullptr,
			const void* hot_region_request_value = nullptr,
			u32 hot_region_request_threshold = 0,
			size_t* hot_region_counter_offset = nullptr,
			u8* hot_region_counter_instruction_count = nullptr);
		bool EmitOpcode(u32 op, u32 pc = 0, u32 raw_cycles_through_instruction = 0,
			const void* event_exit = nullptr, bool branch_delay_slot = false,
			u32 branch_delay_selected_pc = UINT32_MAX,
			u32 branch_delay_fallthrough_pc = UINT32_MAX);
		bool EndBlockReturn(u8 value);
		bool EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit,
			DirectLinkSlot* direct_link = nullptr, DirectLinkSlot* taken_link = nullptr,
			const void* indirect_lookup_pages_slot = nullptr, const void* direct_linking_enabled_flag = nullptr,
			bool wait_loop_taken = false, bool defer_pc_writeback = false,
			u32 direct_pc = 0, u32 taken_pc = 0, bool conditional_pc = false,
			bool indirect_pc_writeback = false, bool preserve_dirty_direct_link = false,
			bool preserve_dirty_taken_link = false,
			const PollCallWaitLoopSourceProof* poll_call_wait_loop = nullptr,
			const TwoPredicateWaitLoopSourceProof* two_predicate_wait_loop = nullptr);
		bool EndBlockWithSchedulerElidedDirectContinuation(u32 block_cycles,
			const void* scheduler_test_elided_direct_exit, DirectLinkSlot* direct_link,
			bool defer_pc_writeback, u32 direct_pc);
		bool EndBlockWithSchedulerElidedConditionalContinuation(u32 block_cycles,
			const void* scheduler_test_elided_direct_exit,
			DirectLinkSlot* not_taken_link, DirectLinkSlot* taken_link,
			bool defer_pc_writeback, u32 not_taken_pc, u32 taken_pc);
		bool EmitHotRegionEntryHorizonGuard(
			u32 source_cycles, const void* fallback_entry);
		bool EndBlockWithLikelyCycleTest(u32 taken_cycles, u32 not_taken_cycles, const void* direct_exit,
			const void* event_exit, DirectLinkSlot* not_taken_link = nullptr,
			DirectLinkSlot* taken_link = nullptr, bool wait_loop_taken = false,
			bool defer_pc_writeback = false, u32 not_taken_pc = 0, u32 taken_pc = 0,
			bool preserve_dirty_not_taken_link = false,
			bool preserve_dirty_taken_link = false,
			const PollCallWaitLoopSourceProof* poll_call_wait_loop = nullptr);
		static bool RequiresBlockEndAfterOpcode(u32 op);
		static bool RequiresFollowingInstructionInBlock(u32 op);
		static bool RequiresTraceWindowEndAfterOpcode(u32 op);
		static bool CalculateScaledCyclesForRange(u32 start_pc, u32 instruction_count,
			bool omit_final_likely_delay_slot, u32* scaled_cycles);
		static bool IsConstantFlushCacheSyscallBlock(u32 start_pc, u32 instruction_count);
		static bool DoesSplitPreserveScaledCycleTimeline(u32 start_pc,
			u32 instruction_count, u32 prefix_instruction_count);
		static bool DoesSplitAfterChargedPrefixPreserveScaledCycleTimeline(
			u32 dependency_start_pc, u32 dependency_instruction_count,
			u32 segment_start_instruction, u32 charged_prefix_cycles,
			u32 segment_prefix_instruction_count);

	private:
		BlockCompiler(VitaA32::CodeBuffer& code, CompileScratch& compile_scratch,
			const u8* ram_source_page_live_flags,
			const u8* ram_source_chunk_live_bits,
			void* ram_write_invalidation_context,
			RamWriteInvalidationCallback ram_write_invalidation_callback,
			bool reset_compile_scratch);
		static CompileScratch& DefaultCompileScratch();

		static bool CalculateScaledCycleStateForRange(u32 start_pc,
			u32 instruction_count, bool omit_final_likely_delay_slot,
			u32* committed_scaled_cycles, u32* final_scaled_cycles);
		static bool IsConstantFlushCacheSyscallAt(u32 start_pc, u32 instruction_index);
		bool IsConstantFlushCacheSyscall(u32 op) const;
		bool EmitLinkFrameReturn();
		bool EmitEmbeddedCompatibleLikelyContinuation(const void* direct_exit,
			const void* event_exit, DirectLinkSlot* direct_link,
			bool defer_pc_writeback, u32 target_pc, bool* emitted);
		bool EndBlockWithCompatibleLikelyTakenSuffix(u32 taken_cycles, u32 not_taken_cycles,
			const void* direct_exit, const void* event_exit, DirectLinkSlot* not_taken_link,
			DirectLinkSlot* taken_link, size_t not_taken_branch, bool defer_pc_writeback,
			u32 not_taken_pc, u32 taken_pc, bool preserve_dirty_not_taken_link,
			bool preserve_dirty_taken_link);
		bool EmitStageCompatiblePredicate();
		bool EmitPrepareCompatiblePredicateEdge(u32 target_pc);
		bool EdgeLeavesGprLinkSignature(u32 target_pc) const;
		bool EdgeLeavesReclaimedVtlbHosts(u32 target_pc) const;
		bool EmitReclaimedVtlbCanonicalEdge();
		bool EmitReloadGprPinsAfterClobber(u16 host_mask);
		bool EmitReloadAllGprPinsFromBacking();
		bool EmitExitToTarget(const void* target, u8 callable_token,
			size_t* persistent_branch_offset = nullptr,
			u32* persistent_branch_instruction = nullptr);
		bool EmitAndImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitOrrImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitEorImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitBicImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch);
		bool EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch, VitaA32::Condition condition);
		bool EmitDirectLinkTail(const void* direct_exit, DirectLinkSlot* direct_link,
			bool defer_pc_writeback = false, u32 pc = 0,
			bool sync_private_fallback = false,
			bool scheduler_test_elided_fallback = false);
		bool EmitTakenDirectLinkTail(const void* direct_exit, size_t target_branch,
			DirectLinkSlot* direct_link, bool defer_pc_writeback = false, u32 pc = 0,
			bool sync_private_fallback = false,
			bool scheduler_test_elided_fallback = false);
		bool EmitIndirectDispatchTail(const void* lookup_pages_slot, const void* direct_linking_enabled_flag,
			const void* direct_exit, bool defer_pc_writeback = false);
		bool EmitGeneratedDispatchLookup(const void* lookup_pages_slot,
			const void* direct_exit, bool defer_pc_writeback,
			bool special_exception_lookup);
		bool EmitEventExitReturn(const void* event_exit,
			u32 persistent_event_token = 0xe7u);
		bool EmitIndirectCycleTestExit(const void* event_exit,
			bool allow_generated_lookup);
		bool EmitDeferredPcWriteback(bool defer_pc_writeback, u32 direct_pc, u32 taken_pc,
			bool conditional_pc, bool indirect_pc_writeback = false);
		static bool IsWaitLoopBody(u32 loop_start_pc, u32 loop_end_pc, u32 branch_pc,
			PollCallWaitLoopSourceProof* poll_call_wait_loop_source_proof = nullptr,
			TwoPredicateWaitLoopSourceProof* two_predicate_wait_loop_source_proof = nullptr);
		bool EmitWaitLoopFastForwardTail(const void* event_exit,
			bool defer_pc_writeback = false, u32 pc = 0,
			u32 persistent_event_token = 0xe7u,
			const PollCallWaitLoopSourceProof* poll_call_wait_loop = nullptr,
			const TwoPredicateWaitLoopSourceProof* two_predicate_wait_loop = nullptr,
			u32 tail_scaled_cycles = 0);
		bool EndBlockWithWaitLoopFastForward(u32 block_cycles,
			const void* event_exit, bool retain_across_events = false);
		static bool IsRetainableUnconditionalWaitBlock(u32 start_pc,
			u32 instruction_count);
		bool CompileRetainedUnconditionalWaitBlock(u32 start_pc,
			u32 instruction_count, const void* retained_wait_event_exit,
			u32* scaled_cycles, size_t* linked_entry_offset);
		bool CompilePreincrementByteZeroFillLoop(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool CompileFourWordFillLoop(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool CompilePreincrementWordFillLoop(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool CompileSelfAddressPairScan(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool CompileWordCopyLoop(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool CompileGsCsrVsintPollLoop(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool CompileDmacChcrStrPollLoop(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool CompileSignedCountdownLoop(u32 start_pc, u32 instruction_count,
			const void* direct_exit, const void* event_exit, u32* scaled_cycles,
			DirectLinkSlots* direct_links, size_t* linked_entry_offset);
		bool EmitSPECIAL(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitCOP0(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitMFC0Fast(u32 op, u32 raw_cycles_through_instruction);
		bool EmitMFC0CountFast(u32 op, u32 scaled_cycles_through_instruction);
		bool EmitMFC0PerfCounterFast(u32 op, u32 scaled_cycles_through_instruction);
		bool EmitMTC0Fast(u32 op, u32 raw_cycles_through_instruction);
		bool EmitTLBWriteInBlock(u32 op, u32 next_pc, const void* helper);
		bool EmitSetNextEventDelta4FromCurrentCycle();
		bool EmitEIEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitERETEventExit(u32 op, u32 raw_cycles_through_instruction,
			const void* event_exit, bool apply_delayed_di = false);
		bool EmitTLBRInBlock();
		bool EmitTLBPInBlock();
		bool EmitDIDelayedStatusClear();
		bool EmitCOP1(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitCOP1MoveControlFast(u32 op);
		bool EmitCOP1ArithmeticFast(u32 op);
		bool EmitCOP1DivSqrtFast(u32 op);
		bool EmitCOP1AccumulatorFast(u32 op);
		bool EmitCOP1ScalarWordFast(u32 op);
		bool EmitCOP1CompareFast(u32 op);
		bool EmitCOP1ConvertWordFast(u32 op);
		bool EmitCOP1ConvertSingleFast(u32 op);
		bool EmitCOP2(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot,
			u32 branch_delay_selected_pc, u32 branch_delay_fallthrough_pc);
		enum class Vu0SyncMode : u8
		{
			None,
			Sync,
			Finish,
		};
		Vu0SyncMode CurrentVu0SyncMode() const;
		struct Cop2ArithmeticFlagNeeds
		{
			bool status = true;
			bool mac = true;
		};
		Cop2ArithmeticFlagNeeds CurrentCop2ArithmeticFlagNeeds(u32 op) const;
		bool EmitCOP2IdleBranch(Vu0SyncMode sync_mode, size_t* vu0_idle);
		bool EmitCOP2VectorTransferBody(u32 op);
		bool EmitCOP2ControlReadBody(u32 op);
		bool EmitCOP2ControlWriteBody(u32 op);
		bool EmitCOP2MacroCodeWrite(u32 op);
		bool EmitCOP2MacroCopySelectedLanes(unsigned mask, unsigned source_address_reg,
			unsigned dest_address_reg, unsigned temp_qreg);
		bool EmitCOP2MacroStoreSelectedLanes(unsigned mask, unsigned value_qreg, unsigned address_reg);
		bool EmitCOP2MacroStoreVfSelectedLanes(unsigned vf_reg, unsigned mask, unsigned value_qreg,
			unsigned address_reg);
		bool EmitCOP2MacroBody(u32 op);
		bool EmitCOP2MacroArithmeticBody(u32 op);
		bool EmitCOP2MacroViBody(u32 op);
		bool EmitCOP2MacroViTransferBody(u32 op);
		bool EmitVu0RandomAdvance();
		bool EmitCOP2MacroRandomBody(u32 op);
		bool EmitVu0IndexedMemoryAddress(unsigned vi_reg);
		bool EmitCOP2MacroIndexedViMemoryBody(u32 op);
		bool EmitVu0ViLowHalfwordAdjust(unsigned vi_reg, bool decrement);
		bool EmitCOP2MacroIndexedVectorMemoryBody(u32 op);
		bool EmitCOP2MacroClipBody(u32 op);
		bool EmitCOP2MacroItofBody(u32 op);
		bool EmitCOP2MacroFtoiBody(u32 op);
		bool EmitCOP2MacroFdivBody(u32 op);
		bool EmitCOP2MacroMoveBody(u32 op);
		bool EmitCOP2MacroMinMaxBody(u32 op);
		bool EmitCOP2MacroFast(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit, u32 running_exit_pc,
			u32 running_fallthrough_pc);
		bool EmitCOP2InterlockCall(u32 op, bool wait_for_mbit);
		bool EmitCOP2VectorTransferFast(u32 op, u32 next_pc,
			u32 raw_cycles_through_instruction, const void* event_exit,
			bool register_jump_delay_slot);
		bool EmitCOP2ControlReadFast(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCOP2ControlWriteFast(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCACHE(u32 op);
		bool EmitSpecialExceptionExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot, const void* helper,
			bool force_event_dispatch);
		bool EmitSYSCALL(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit,
			bool branch_delay_slot);
		bool EmitBREAK(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit,
			bool branch_delay_slot);
		bool EmitTrapEventExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitADDIU(u32 op);
		bool EmitDADDIU(u32 op);
		bool EmitSLTI(u32 op);
		bool EmitSLTIU(u32 op);
		bool EmitANDI(u32 op);
		bool EmitORI(u32 op);
		bool EmitXORI(u32 op);
		bool EmitLUI(u32 op);
		bool EmitSLL(u32 op);
		bool EmitSRL(u32 op);
		bool EmitSRA(u32 op);
		bool EmitSLLV(u32 op);
		bool EmitSRLV(u32 op);
		bool EmitSRAV(u32 op);
		bool EmitMOVZ(u32 op);
		bool EmitMOVN(u32 op);
		bool EmitMULT(u32 op);
		bool EmitMULTU(u32 op);
		bool EmitMADD(u32 op);
		bool EmitMADDU(u32 op);
		bool EmitMADD1(u32 op);
		bool EmitMADDU1(u32 op);
		bool EmitMFHI1(u32 op);
		bool EmitMFLO1(u32 op);
		bool EmitMTHI1(u32 op);
		bool EmitMTLO1(u32 op);
		bool EmitMULT1(u32 op);
		bool EmitMULTU1(u32 op);
		bool EmitDIV(u32 op);
		bool EmitDIVU(u32 op);
		bool EmitDIV1(u32 op);
		bool EmitDIVU1(u32 op);
		bool EmitPLZCW(u32 op);
		bool EmitPMFHL(u32 op);
		bool EmitPMTHL(u32 op);
		bool EmitMFHI(u32 op);
		bool EmitMFLO(u32 op);
		bool EmitMTHI(u32 op);
		bool EmitMTLO(u32 op);
		bool EmitMFSA(u32 op);
		bool EmitMTSA(u32 op);
		bool EmitPSLLH(u32 op);
		bool EmitPSRLH(u32 op);
		bool EmitPSRAH(u32 op);
		bool EmitPSLLW(u32 op);
		bool EmitPSRLW(u32 op);
		bool EmitPSRAW(u32 op);
		bool EmitMMI(u32 op);
		bool EmitMMI0(u32 op);
		bool EmitMMI1(u32 op);
		bool EmitMMI2(u32 op);
		bool EmitMMI3(u32 op);
		bool EmitPMADDW(u32 op);
		bool EmitPMSUBW(u32 op);
		bool EmitPMADDH(u32 op);
		bool EmitPHMADH(u32 op);
		bool EmitPMSUBH(u32 op);
		bool EmitPHMSBH(u32 op);
		bool EmitPDIVW(u32 op);
		bool EmitPDIVUW(u32 op);
		bool EmitPDIVBW(u32 op);
		bool EmitPMADDUW(u32 op);
		bool EmitPMULTW(u32 op);
		bool EmitPMULTUW(u32 op);
		bool EmitPMULTH(u32 op);
		bool EmitPMFHI(u32 op);
		bool EmitPMFLO(u32 op);
		bool EmitPMTHI(u32 op);
		bool EmitPMTLO(u32 op);
		bool EmitMmiVectorOp(u32 op, MmiVectorOp operation);
		bool EmitMmiUnaryRtVectorOp(u32 op, MmiUnaryVectorOp operation);
		bool EmitMmiImmediateShiftOp(u32 op, MmiImmediateShiftOp operation);
		bool EmitMmiVariableWordShiftOp(u32 op, MmiVariableWordShiftOp operation);
		bool EmitMmiHalfwordShuffleOp(u32 op, MmiHalfwordShuffleOp operation);
		bool EmitMmiWordShuffleOp(u32 op, MmiWordShuffleOp operation);
		bool EmitMmiFiveBitOp(u32 op, MmiFiveBitOp operation);
		bool EmitMmiInterleaveOp(u32 op, MmiUpperInterleaveOp operation, bool upper_half);
		bool EmitMmiUpperInterleaveOp(u32 op, MmiUpperInterleaveOp operation);
		bool EmitMmiPackEvenOp(u32 op, MmiUpperInterleaveOp operation);
		bool EmitPADDW(u32 op);
		bool EmitPSUBW(u32 op);
		bool EmitPCGTW(u32 op);
		bool EmitPMAXW(u32 op);
		bool EmitPADDH(u32 op);
		bool EmitPSUBH(u32 op);
		bool EmitPCGTH(u32 op);
		bool EmitPMAXH(u32 op);
		bool EmitPADDB(u32 op);
		bool EmitPSUBB(u32 op);
		bool EmitPCGTB(u32 op);
		bool EmitPADDSW(u32 op);
		bool EmitPSUBSW(u32 op);
		bool EmitPEXTLW(u32 op);
		bool EmitPPACW(u32 op);
		bool EmitPADDSH(u32 op);
		bool EmitPSUBSH(u32 op);
		bool EmitPEXTLH(u32 op);
		bool EmitPPACH(u32 op);
		bool EmitPADDSB(u32 op);
		bool EmitPSUBSB(u32 op);
		bool EmitPEXTLB(u32 op);
		bool EmitPPACB(u32 op);
		bool EmitPEXT5(u32 op);
		bool EmitPPAC5(u32 op);
		bool EmitPABSW(u32 op);
		bool EmitPCEQW(u32 op);
		bool EmitPMINW(u32 op);
		bool EmitPADSBH(u32 op);
		bool EmitPABSH(u32 op);
		bool EmitPCEQH(u32 op);
		bool EmitPMINH(u32 op);
		bool EmitPCEQB(u32 op);
		bool EmitPADDUW(u32 op);
		bool EmitPSUBUW(u32 op);
		bool EmitPEXTUW(u32 op);
		bool EmitPADDUH(u32 op);
		bool EmitPSUBUH(u32 op);
		bool EmitPEXTUH(u32 op);
		bool EmitPADDUB(u32 op);
		bool EmitPSUBUB(u32 op);
		bool EmitPEXTUB(u32 op);
		bool EmitQFSRV(u32 op);
		bool EmitPSLLVW(u32 op);
		bool EmitPSRLVW(u32 op);
		bool EmitPSRAVW(u32 op);
		bool EmitPINTH(u32 op);
		bool EmitPCPYLD(u32 op);
		bool EmitPEXEH(u32 op);
		bool EmitPREVH(u32 op);
		bool EmitPEXEW(u32 op);
		bool EmitPROT3W(u32 op);
		bool EmitPINTEH(u32 op);
		bool EmitPCPYUD(u32 op);
		bool EmitPEXCH(u32 op);
		bool EmitPCPYH(u32 op);
		bool EmitPEXCW(u32 op);
		bool EmitPAND(u32 op);
		bool EmitPXOR(u32 op);
		bool EmitPOR(u32 op);
		bool EmitPNOR(u32 op);
		bool EmitREGIMM(u32 op, u32 pc);
		bool EmitMTSAB(u32 op);
		bool EmitMTSAH(u32 op);
		bool EmitJ(u32 op, u32 pc);
		bool EmitJAL(u32 op, u32 pc);
		bool EmitJR(u32 op, u32 pc);
		bool EmitJALR(u32 op, u32 pc);
		bool EmitBEQ(u32 op);
		bool EmitBNE(u32 op);
		bool EmitBLEZ(u32 op);
		bool EmitBGTZ(u32 op);
		bool EmitBEQL(u32 op);
		bool EmitBNEL(u32 op);
		bool EmitBLEZL(u32 op);
		bool EmitBGTZL(u32 op);
		bool EmitLB(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLH(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLW(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLBU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLHU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLWU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLWL(u32 op);
		bool EmitLWR(u32 op);
		bool EmitLD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLDL(u32 op);
		bool EmitLDR(u32 op);
		bool EmitLQ(u32 op);
		bool EmitLWC1(u32 op);
		bool EmitLQC2(u32 op);
		bool EmitSB(u32 op);
		bool EmitSH(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitSW(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitSWL(u32 op);
		bool EmitSWR(u32 op);
		bool EmitSD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitSDL(u32 op);
		bool EmitSDR(u32 op);
		bool EmitSQ(u32 op);
		bool EmitSWC1(u32 op);
		bool EmitSQC2(u32 op);
		bool EmitDSLLV(u32 op);
		bool EmitDSRLV(u32 op);
		bool EmitDSRAV(u32 op);
		bool EmitDSLL(u32 op);
		bool EmitDSRL(u32 op);
		bool EmitDSRA(u32 op);
		bool EmitDSLL32(u32 op);
		bool EmitDSRL32(u32 op);
		bool EmitDSRA32(u32 op);
		bool EmitADDU(u32 op);
		bool EmitSUBU(u32 op);
		bool EmitDADDU(u32 op);
		bool EmitDSUBU(u32 op);
		bool EmitAND(u32 op);
		bool EmitOR(u32 op);
		bool EmitXOR(u32 op);
		bool EmitNOR(u32 op);
		bool EmitSLT(u32 op);
		bool EmitSLTU(u32 op);
		bool EmitShift32Immediate(u32 op, VitaA32::ShiftType shift, unsigned amount);
		bool EmitShift32Variable(u32 op, VitaA32::ShiftType shift);
		bool EmitShift64LeftImmediate(u32 op, unsigned amount);
		bool EmitShift64RightImmediate(u32 op, unsigned amount, bool arithmetic);
		bool EmitShift64LeftVariable(u32 op);
		bool EmitShift64RightVariable(u32 op, bool arithmetic);
		bool EmitConditionalMove(u32 op, bool move_on_zero);
		bool EmitMultiply(u32 op, bool signed_multiply, bool upper_pipeline = false);
		bool EmitMultiplyAdd(u32 op, bool signed_multiply, bool upper_pipeline);
		bool EmitPackedWordMultiply(u32 op, bool signed_multiply);
		bool EmitPackedSignedWordMultiplyAccumulate(u32 op, bool subtract);
		bool EmitPackedHalfwordMultiplyAccumulate(u32 op, bool subtract);
		bool EmitPackedHalfwordPairMultiply(u32 op, bool subtract);
		bool EmitPackedHalfwordMultiply(u32 op);
		bool EmitPackedUnsignedWordMultiplyAdd(u32 op);
		bool EmitScalarDivide(u32 op, bool signed_divide, bool upper_pipeline);
		bool EmitPackedWordDivide(u32 op, bool signed_divide);
		bool EmitPackedWordByHalfwordDivide(u32 op);
		bool EmitMoveFromHiLo(u32 op, size_t hilo_offset);
		bool EmitMoveToHiLo(u32 op, size_t hilo_offset);
		bool EmitMoveFullFromHiLo(u32 op, size_t hilo_offset);
		bool EmitMoveFullToHiLo(u32 op, size_t hilo_offset);
		bool EmitGoemonBlockStartHook(u32 start_pc);
		bool EmitLink(unsigned guest_reg, u32 pc);
		bool EmitJump(u32 pc, bool link);
		bool EmitRegisterJump(u32 op, u32 pc, bool link);
		bool EmitGoemonTranslateHostReg(unsigned host_reg);
		bool EmitCompareGpr64WithKnownForBranch(unsigned guest_reg, u32 low, u32 high);
		bool EmitCompareGpr64ForBranch(unsigned lhs_guest_reg, unsigned rhs_guest_reg);
		bool TryEvaluateConstantBranch(u32 op, bool* taken) const;
		bool TryEvaluateConstantRegimmLinkBranch(u32 op, bool* taken) const;
		bool EmitBranchEqual(u32 op, bool branch_on_equal);
		bool EmitBranchSigned(u32 op, SignedBranchCondition condition);
		bool EmitCop0Branch(u32 op);
		bool EmitCop1Branch(u32 op);
		bool EmitCop2Branch(u32 op);
		bool EmitSetLessThan64(unsigned guest_reg, bool signed_compare, unsigned lhs_low,
			unsigned lhs_high, unsigned rhs_low, unsigned rhs_high);
		bool EmitSetLessThan64Known(unsigned guest_reg, bool signed_compare,
			unsigned runtime_guest_reg, u32 known_low, u32 known_high, bool known_is_lhs);
		bool EmitSetLessThan64Imm(unsigned guest_reg, s32 imm, bool signed_compare,
			unsigned lhs_low, unsigned lhs_high);
		bool EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, const void* read_helper, bool sign_extend,
			bool branch_delay_slot, ScalarLoadWidth width, u8 alignment_mask, bool counter_read_event);
		bool EmitPartialWordLoad(u32 op, bool left);
		bool EmitPartialWordStore(u32 op, bool left);
		bool EmitPartialDwordLoad(u32 op, bool left);
		bool EmitPartialDwordStore(u32 op, bool left);
		bool EmitCounterReadFlagFromAddress(unsigned host_reg);
		bool EmitCounterReadEventExit(u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit, unsigned counter_flag_host);
		struct GprPinDirtyMasks
		{
			u8 low = 0;
			u8 high = 0;
		};
		bool EmitAddressErrorEventExit(u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool store, const GprPinDirtyMasks& dirty_pins);
		bool EmitSystemHelperEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* helper, const void* event_exit,
			u32 branch_fallthrough_pc = UINT32_MAX);
		bool FlushColdTails();
		void ClearGprConstState();
		void ClearSaConstState();
		void ClearCop1NormalizedState();
		bool IsCop1FprNormalized(unsigned fpr) const;
		bool IsCop1AccNormalized() const;
		bool TryGetKnownGprLow(unsigned guest_reg, u32* value) const;
		bool TryGetKnownGpr64(unsigned guest_reg, u32* low, u32* high) const;
		bool EmitStoreKnownSignExtended32(unsigned guest_reg, u32 value);
		bool EmitStoreKnownZeroExtended32(unsigned guest_reg, u32 value);
		bool EmitStoreKnown64(unsigned guest_reg, u32 low, u32 high);
		bool EmitStoreGprSignExtended32FromLow(unsigned guest_reg, unsigned host_low);
		bool EmitStoreGprZeroExtended32FromLow(unsigned guest_reg, unsigned host_low);
		unsigned SelectGprLowResultHost(unsigned guest_reg, unsigned fallback_host);
		unsigned SelectGprHighResultHost(unsigned guest_reg, unsigned fallback_host);
		void UpdateCop1NormalizedStateAfterOpcode(u32 op);
		bool TryGetKnownEffectiveAddress(u32 op, u32* address) const;
		bool BlockNeedsResidentVtlbRegisters(u32 start_pc, u32 instruction_count);
		enum class KnownVtlbFastPathKind : u8
		{
			Scalar,
			Qword,
			Cop1,
			Cop2,
			Partial,
		};
		bool TryEmitKnownVtlbNonHandlerHostAddress(u32 guest_addr, unsigned host_reg,
			KnownVtlbFastPathKind kind = KnownVtlbFastPathKind::Scalar);
		void UpdateGprConstStateAfterOpcode(u32 op, u32 pc);
		void StageGprPinsForBlock(u32 start_pc, u32 instruction_count, bool allow_r5,
			bool allow_r7, bool allow_r8,
			bool allow_r10, bool allow_r11, bool prefer_dirty_writes,
			bool preserve_self_link_state);
		void StageGprPinsForLinkSignature(u32 start_pc, u32 instruction_count,
			const GprLinkSignature& signature);
		void StageGprQCacheForBlock(u32 start_pc, u32 instruction_count);
		bool BlockWritesPinnedGpr(u32 start_pc, u32 instruction_count) const;
		void MarkGprPinsDirtyAtResidentSelfLinkEntry(u32 start_pc, u32 instruction_count);
		u8 GprPinEntryLoadInstructionCount() const;
		bool EmitGprPinLoads();
		bool EmitGprQCacheEntryLoads();
		bool EmitFlushDirtyGprPins();
		bool EmitFlushDirtyGprPinsForGuest(unsigned guest_reg);
		GprPinDirtyMasks CurrentGprPinDirtyMasks() const;
		bool EmitSyncGprPinsToBacking(const GprPinDirtyMasks* dirty_pins = nullptr,
			bool forwarded_value_is_architectural = false);
		bool EmitSyncForwardedBooleanBranchToBacking(bool value_is_architectural = false);
		bool EmitPrepareResidentForwardedBooleanForPreProducerSync();
		bool EmitPoisonCompatibleVtlbPointer();
		bool EmitInvalidateCompatibleVtlbPointers();
		bool EmitStageCompatibleVtlbStaticPage();
		bool EmitStageCompatibleVtlbPointer();
		bool EmitStageCompatibleVtlbPointerMapping(
			const VtlbPointerLinkMapping& pointer, bool access,
			size_t* unaligned_fallback, size_t* handler_fallback,
			GprPinDirtyMasks* dirty_pins, bool write_pointer);
		bool EmitStageCompatibleSchedulerCountdown(unsigned scratch_host,
			bool canonical_entry = true);
		bool EmitSyncCompatibleSchedulerCountdownToBacking();
		bool EmitStageResidentSchedulerCountdown(bool preserve_for_translation);
		bool EmitSyncResidentCycleLowToBacking();
		bool EmitSaveResidentSchedulerCountdown();
		bool EmitRestoreResidentSchedulerCountdown();
		bool EmitDeferredResidentUnsignedBranchSuffix(bool event_path = false);
		bool IsForwardedBooleanBranchResult(unsigned guest_reg) const;
		bool EmitStageRawGpr0Qword();
		bool EmitStageResidentVtlbQwordPointer();
		int FindGprPinIndex(unsigned guest_reg) const;
		int FindGprPinHost(unsigned guest_reg) const;
		int FindGprPinHighHost(unsigned guest_reg) const;
		bool TryDeferGprPinLowStore(unsigned guest_reg);
		bool TryDeferGprPinHighStore(unsigned guest_reg);
		void ClearGprQCache();
		void InvalidateGprQCacheForGuest(unsigned guest_reg);
		void InvalidateGprQCacheForQreg(unsigned qreg);
		void MarkGprQCache(unsigned guest_reg, unsigned qreg);
		int FindGprQCache(unsigned guest_reg) const;
		bool IsGprQCacheQregResident(unsigned qreg) const;
		bool GprQCacheGuestHasFutureQwordReadBeforeWrite(unsigned guest_reg) const;
		bool GprQCacheGuestHasFutureQfsrvSourceReadBeforeWrite(unsigned guest_reg) const;
		bool GprQCacheQregHasFutureQwordReadBeforeWrite(unsigned qreg) const;
		u32 GprQCacheGuestNextQwordReadDistanceBeforeWrite(unsigned guest_reg) const;
		u32 GprQCacheQregNextQwordReadDistanceBeforeWrite(unsigned qreg) const;
		unsigned SelectGprQCacheScratchQreg(u32 avoid_qreg_mask = 0) const;
		bool GprQCacheGuestDefinedBeforeCurrentInstruction(unsigned guest_reg) const;
		u16 GprQCacheGuestEntryQwordReadCount(unsigned guest_reg) const;
		bool PreserveGprQCacheGuestForFutureRead(unsigned guest_reg, unsigned cached_qreg,
			unsigned avoid_qreg0, unsigned avoid_qreg1, bool* preserved);
		bool EmitStoreGprQ128PreservingCachedSourceIfFutureRead(unsigned dest_guest_reg,
			unsigned source_guest_reg, unsigned cached_qreg, unsigned address_scratch, bool* preserved);
		bool EmitDeviceTracePreInstruction(u32 pc);
		bool EmitLoadCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high, unsigned address_scratch);
		bool EmitStoreCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high, unsigned address_scratch);
		bool EmitAddScaledCyclesToCpuLowWord(u32 cycles, unsigned host_low, unsigned scratch,
			size_t* carry_branch);
		bool EmitCycleCarryFixup(const size_t* carry_branches, size_t carry_branch_count,
			size_t resume_offset, unsigned scratch);
		bool EmitLoadCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch);
		bool EmitStoreCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch);
		bool EmitMoveQWordLaneToCore(unsigned host_reg, unsigned qreg, unsigned word);
		bool EmitMoveQWordLaneToCore(unsigned host_reg, unsigned qreg, unsigned word,
			VitaA32::Condition condition);
		bool EmitMoveCoreToQWordLane(unsigned qreg, unsigned word, unsigned host_reg);
		bool EmitMoveCoreToQWordLane(unsigned qreg, unsigned word, unsigned host_reg,
			VitaA32::Condition condition);
		bool EmitMoveQWordLane(unsigned dest_qreg, unsigned dest_word, unsigned source_qreg,
			unsigned source_word, unsigned host_scratch);
		bool EmitLoadQWordLane(unsigned qreg, unsigned word, unsigned address_reg, u16 offset,
			unsigned host_scratch);
		bool EmitStoreQWordLane(unsigned qreg, unsigned word, unsigned address_reg, u16 offset,
			unsigned host_scratch);
		bool EmitCop1ExponentMask(unsigned host_reg);
		bool EmitAndCop1ExponentMask(unsigned rd, unsigned rn, unsigned scratch);
		bool EmitAndCop1FractionMask(unsigned rd, unsigned rn);
		bool EmitAddScaledCyclesToCpu(u32 cycles);
		bool EmitEffectiveAddress(u32 op, unsigned host_reg);
		bool EmitCpuRegsAddress(unsigned host_reg, size_t offset);
		bool EmitLoadRawGpr0KnownZeroFlag(unsigned host_reg);
		bool EmitRefreshRawGpr0KnownZeroFromLow64(unsigned low_reg, unsigned high_reg);
		bool EmitVu0SyncIfRunning(unsigned preserve_reg = 16, unsigned save_reg = 16);
		bool EmitVu0RegisterAddress(unsigned host_reg, size_t offset);
		bool EmitVu0ClipflagAddress(unsigned host_reg);
		bool EmitVu0Vf0ConstantQ(unsigned qreg, unsigned host_scratch);
		bool EmitVu0VfAddress(unsigned host_reg, unsigned vf_reg);
		bool EmitVu0ViAddress(unsigned host_reg, unsigned vi_reg);
		bool EmitAlignQwordAddress(unsigned host_reg, unsigned scratch_reg);
		bool EmitVtlbNonHandlerHostAddress(unsigned host_reg, unsigned vmap_reg, unsigned scratch_reg,
			size_t* handler_fallback_branch, GprPinDirtyMasks* dirty_pins = nullptr);
		bool EmitVtlbNonHandlerHostAddress128(unsigned host_reg, unsigned vmap_reg, unsigned scratch_reg,
			size_t* handler_fallback_branch, GprPinDirtyMasks* dirty_pins = nullptr);
		bool IsCompatibleVtlbStaticPageAccess(u32 op, ScalarLoadWidth width,
			u8 alignment_mask) const;
		bool IsCompatibleVtlbStaticPageAccess(u32 op, ScalarStoreWidth width,
			u8 alignment_mask) const;
		bool IsCompatibleVtlbStaticPageCop1Access(u32 op) const;
		bool EmitCompatibleVtlbStaticPageAddress(u32 op, unsigned host_reg,
			size_t* fallback_branch);
		bool EmitLoadGprLow(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadCop2ControlSource(unsigned guest_reg, unsigned host_reg);
		bool EmitGprLowOperand(unsigned guest_reg, unsigned fallback_host, unsigned* operand_host);
		bool EmitLoadGprLowKnownValue(unsigned guest_reg, unsigned host_reg, bool value_known, u32 value);
		bool EmitLoadGprLowValue(unsigned guest_reg, unsigned host_reg);
		bool EmitGprLowValueOperand(unsigned guest_reg, unsigned fallback_host, unsigned* operand_host);
		bool EmitLoadPartialStoreLowValue(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadPartialStoreLowKnownValue(unsigned guest_reg, unsigned host_reg,
			bool value_known, u32 value);
		bool EmitLoadPartialDwordStoreByteValue(unsigned guest_reg, unsigned source_byte,
			unsigned host_reg, bool value_known, u32 low, u32 high);
		bool EmitRefreshGprPinFromBacking(unsigned guest_reg);
		bool TryEmitLoadGprWordFromQCache(unsigned guest_reg, unsigned word, unsigned host_reg,
			bool* emitted);
		bool EmitLoadGprWord(unsigned guest_reg, unsigned word, unsigned host_reg);
		bool EmitGprWordOperand(unsigned guest_reg, unsigned word, unsigned fallback_host,
			unsigned* operand_host);
		bool EmitGpr64ReadOperands(unsigned guest_reg, unsigned fallback_low, unsigned fallback_high,
			unsigned* low_operand_host, unsigned* high_operand_host);
		bool EmitLoadGpr64KnownValue(unsigned guest_reg, unsigned host_low, unsigned host_high,
			bool value_known, u32 low, u32 high);
		bool EmitLoadGpr64Value(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitGpr64ValueReadOperands(unsigned guest_reg, unsigned fallback_low, unsigned fallback_high,
			unsigned* low_operand_host, unsigned* high_operand_host);
		bool EmitLoadGprHigh(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitLoadGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch);
		bool ShouldLoadGprQ128SingleUseEntry(unsigned guest_reg) const;
		bool EmitLoadGprQ128SingleUseEntry(unsigned guest_reg, unsigned qreg,
			unsigned address_scratch);
		bool EmitStorePcFromHostReg(unsigned host_reg);
		bool EmitStorePcWithScratch(u32 pc, unsigned scratch_reg);
		bool EmitStoreBranchPc(u32 target_pc, u32 fallthrough_pc);
		bool EmitStorePc(u32 pc);
		bool EmitStoreGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch);
		bool EmitStoreGprQ128ToAddress(unsigned guest_reg, unsigned qreg, unsigned address_reg);
		bool EmitStoreGprDwordPair(unsigned guest_reg, unsigned low_d, unsigned high_d);
		bool EmitStoreGprWord(unsigned guest_reg, unsigned word, unsigned host_reg);
		bool TryEmitStoreGprLow64FromQCache(unsigned guest_reg, unsigned source_guest_reg, bool* emitted);
		bool TryEmitStoreGprLow64InvertFromQCache(unsigned guest_reg, unsigned source_guest_reg, bool* emitted);
		bool EmitStoreGprZero64(unsigned guest_reg);
		bool EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);

		struct ScalarLoadColdTail
		{
			size_t unaligned_fallback = static_cast<size_t>(-1);
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			u32 pc = 0;
			u32 raw_cycles_through_instruction = 0;
			const void* event_exit = nullptr;
			const void* read_helper = nullptr;
			ScalarLoadWidth width = ScalarLoadWidth::Byte;
			unsigned rt = 0;
			bool sign_extend = false;
			bool branch_delay_slot = false;
			bool counter_read_event = false;
			unsigned address_reg = 0;
			GprPinDirtyMasks dirty_pins{};
			size_t compatible_byte_pair_delay_join = static_cast<size_t>(-1);
			size_t static_page_fallback = static_cast<size_t>(-1);
			u32 op = 0;
		};

		bool EmitScalarLoadColdTail(const ScalarLoadColdTail& tail);
		bool EmitResolvedScalarLoadColdTail(const ScalarLoadColdTail& tail);

		struct ScalarStoreColdTail
		{
			size_t unaligned_fallback = static_cast<size_t>(-1);
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			u32 pc = 0;
			u32 raw_cycles_through_instruction = 0;
			const void* event_exit = nullptr;
			const void* write_helper = nullptr;
			unsigned rt = 0;
			ScalarStoreWidth width = ScalarStoreWidth::Byte;
			bool rt_low_known = false;
			bool rt_high_known = false;
			u32 rt_low = 0;
			u32 rt_high = 0;
			unsigned address_reg = 0;
			GprPinDirtyMasks dirty_pins{};
			size_t static_page_fallback = static_cast<size_t>(-1);
			u32 op = 0;
		};
		void CaptureScalarStoreValue(ScalarStoreColdTail* tail);
		bool EmitScalarStoreColdTail(const ScalarStoreColdTail& tail);
		bool EmitResolvedScalarStoreColdTail(const ScalarStoreColdTail& tail);

		struct QwordLoadColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned rt = 0;
			GprPinDirtyMasks dirty_pins{};
		};
		bool EmitQwordLoadColdTail(const QwordLoadColdTail& tail);

		struct QwordStoreColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned rt = 0;
			GprPinDirtyMasks dirty_pins{};
		};
		bool EmitQwordStoreColdTail(const QwordStoreColdTail& tail);

		struct Cop1WordMemoryColdTail
		{
			size_t static_page_fallback = static_cast<size_t>(-1);
			size_t unaligned_fallback = static_cast<size_t>(-1);
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			u32 op = 0;
			unsigned rt = 0;
			bool store = false;
			GprPinDirtyMasks dirty_pins{};
		};
		bool EmitCop1WordMemoryColdTail(const Cop1WordMemoryColdTail& tail);

		struct Cop2QwordMemoryColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned rt = 0;
			bool store = false;
			Vu0SyncMode sync_mode = Vu0SyncMode::Sync;
		};
		bool EmitCop2QwordMemoryColdTail(const Cop2QwordMemoryColdTail& tail);

		struct Vu0SyncColdTail
		{
			size_t running_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned preserve_reg = 16;
			unsigned save_reg = 16;
			Vu0SyncMode sync_mode = Vu0SyncMode::Sync;
		};
		bool EmitVu0SyncColdTail(const Vu0SyncColdTail& tail);

		enum class PartialMemoryOp : u8
		{
			WordLoadLeft,
			WordLoadRight,
			WordStoreLeft,
			WordStoreRight,
			DwordLoadLeft,
			DwordLoadRight,
			DwordStoreLeft,
			DwordStoreRight,
		};

		struct PartialMemoryColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			PartialMemoryOp op = PartialMemoryOp::WordLoadLeft;
			unsigned rt = 0;
			bool rt_low_known = false;
			u32 rt_low = 0;
			bool rt_high_known = false;
			u32 rt_high = 0;
			GprPinDirtyMasks dirty_pins{};
		};
		void CapturePartialStoreValue(PartialMemoryColdTail* tail);
		bool EmitPartialMemoryColdTail(const PartialMemoryColdTail& tail);

		struct RamStoreInvalidationColdTail
		{
			size_t live_source_branch = static_cast<size_t>(-1);
			size_t secondary_live_source_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
			u32 size = 0;
			bool may_cross_chunk = false;
		};
		bool EmitRamSourceStoreGuard(unsigned host_address_reg, u32 size,
			u8 post_increment = 0, bool may_cross_chunk = false);
		bool EmitRamStoreInvalidationColdTail(
			const RamStoreInvalidationColdTail& tail);

		// A guest instruction can enqueue at most one typed operation tail. VU0
		// synchronization likewise occurs at most once for LQC2/SQC2. The only
		// doubled collection is RAM invalidation: dynamic SWL/SWR emit mutually
		// exclusive general/full-lane store paths and each path needs its own SMC
		// guard. Keep this storage outside BlockCompiler's stack object and reuse it
		// for every cold compilation attempt.
		static constexpr size_t MAX_RAM_INVALIDATION_COLD_TAILS =
			static_cast<size_t>(MAX_COMPILE_INSTRUCTIONS) * 2;
		struct CompileScratch
		{
			FixedCompileBuffer<ScalarLoadColdTail, MAX_COMPILE_INSTRUCTIONS>
				scalar_load_cold_tails;
			FixedCompileBuffer<ScalarStoreColdTail, MAX_COMPILE_INSTRUCTIONS>
				scalar_store_cold_tails;
			FixedCompileBuffer<QwordLoadColdTail, MAX_COMPILE_INSTRUCTIONS>
				qword_load_cold_tails;
			FixedCompileBuffer<QwordStoreColdTail, MAX_COMPILE_INSTRUCTIONS>
				qword_store_cold_tails;
			FixedCompileBuffer<Cop1WordMemoryColdTail, MAX_COMPILE_INSTRUCTIONS>
				cop1_word_memory_cold_tails;
			FixedCompileBuffer<Cop2QwordMemoryColdTail, MAX_COMPILE_INSTRUCTIONS>
				cop2_qword_memory_cold_tails;
			FixedCompileBuffer<Vu0SyncColdTail, MAX_COMPILE_INSTRUCTIONS>
				vu0_sync_cold_tails;
			FixedCompileBuffer<PartialMemoryColdTail, MAX_COMPILE_INSTRUCTIONS>
				partial_memory_cold_tails;
			FixedCompileBuffer<RamStoreInvalidationColdTail,
				MAX_RAM_INVALIDATION_COLD_TAILS> ram_store_invalidation_cold_tails;
			std::array<u16,
				static_cast<size_t>(MAX_COMPILE_INSTRUCTIONS) * 32>
				gpr_q_cache_next_use_distances{};
		};

		VitaA32::CodeBuffer& m_code;
		CompileScratch& m_compile_scratch;
		const u8* m_ram_source_page_live_flags = nullptr;
		const u8* m_ram_source_chunk_live_bits = nullptr;
		void* m_ram_write_invalidation_context = nullptr;
		RamWriteInvalidationCallback m_ram_write_invalidation_callback = nullptr;
		FixedCompileBuffer<ScalarLoadColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_scalar_load_cold_tails;
		FixedCompileBuffer<ScalarStoreColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_scalar_store_cold_tails;
		FixedCompileBuffer<QwordLoadColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_qword_load_cold_tails;
		FixedCompileBuffer<QwordStoreColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_qword_store_cold_tails;
		FixedCompileBuffer<Cop1WordMemoryColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_cop1_word_memory_cold_tails;
		FixedCompileBuffer<Cop2QwordMemoryColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_cop2_qword_memory_cold_tails;
		FixedCompileBuffer<Vu0SyncColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_vu0_sync_cold_tails;
		FixedCompileBuffer<PartialMemoryColdTail, MAX_COMPILE_INSTRUCTIONS>&
			m_partial_memory_cold_tails;
		FixedCompileBuffer<RamStoreInvalidationColdTail,
			MAX_RAM_INVALIDATION_COLD_TAILS>& m_ram_store_invalidation_cold_tails;
		u16 m_saved_registers = 0;
		bool m_vtlb_registers_available = false;
		bool m_cop1_exponent_mask_available = false;
		bool m_vu0_base_available = false;
			static constexpr unsigned MAX_GPR_PINS = 5;
			static_assert(MAX_GPR_PINS <= 8);
			// Per-block read pins: guest GPR low words held in callee-saved host
			// registers for the whole block, optionally with a companion high word
			// for hot low64 scalar state. Most blocks remain write-through; a narrow
			// scalar/control plus scalar-memory subset defers pinned stores and flushes
			// at block exits or before helper/event cold seams. Persistent self-links
			// may re-enter after these loads because the exact same mapping remains
			// live; all other incoming edges rebuild it from backing state.
		u8 m_staged_pin_guest[MAX_GPR_PINS]{};
		u8 m_staged_pin_host[MAX_GPR_PINS]{};
		u8 m_staged_pin_high_host[MAX_GPR_PINS]{};
		bool m_staged_pin_needs_entry_load[MAX_GPR_PINS]{};
		u8 m_staged_pin_count = 0;
		u8 m_pin_guest[MAX_GPR_PINS]{};
		u8 m_pin_host[MAX_GPR_PINS]{};
		u8 m_pin_high_host[MAX_GPR_PINS]{};
		bool m_pin_needs_entry_load[MAX_GPR_PINS]{};
		u8 m_pin_count = 0;
		bool m_dirty_pins_enabled = false;
		bool m_pin_dirty_low[MAX_GPR_PINS]{};
		bool m_pin_dirty_high[MAX_GPR_PINS]{};
		bool m_gpr_const_known[32]{};
		u32 m_gpr_const_low[32]{};
		bool m_gpr_const_high_known[32]{};
		u32 m_gpr_const_high[32]{};
		bool m_sa_const_known = false;
		u8 m_sa_const_byte_offset = 0;
		bool m_cop1_fpr_normalized[32]{};
		bool m_cop1_acc_normalized = false;
		// True once the vuDouble() bit-select constant quads (physical Q8-Q11)
		// have been materialized in this block. Physical Q12-Q15 are ephemeral
		// normalize scratch in COP2 macro blocks and back the private ABI's logical
		// Q4-Q7 bank in qcache blocks; the block classifier makes those roles
		// mutually exclusive. Reset per block in BeginBlock().
		bool m_cop2_norm_consts_ready = false;
		// Once an in-block TLB write has executed, suffix memory operations must
		// not embed a compile-time vmv.assumePtr() from the preceding mapping.
		bool m_runtime_tlb_mapping_may_have_changed = false;
		bool m_gpr_q_cache_enabled = false;
		bool m_persistent_dispatch_exits = false;
#if defined(VITASX2_QEMU_VALIDATION)
		bool m_vtlb_linked_entry_pc_publication_enabled = false;
		bool m_compatible_likely_taken_suffix_enabled = true;
		bool m_compatible_predicate_entry_variant_enabled = true;
		bool m_embedded_compatible_continuation_enabled = true;
		bool m_fused_direct_event_link_enabled = true;
		bool m_combined_compatible_taken_event_enabled = true;
		bool m_compatible_vtlb_write_guard_hoist_enabled = true;
		bool m_compatible_vtlb_read_guard_hoist_enabled = true;
		bool m_direct_link_rejection_profiling_enabled = false;
#endif
		GprLinkSignature m_gpr_link_signature{};
		u8 m_branch_flag_host = 0;
		bool m_forwarded_boolean_branch = false;
		bool m_resident_forwarded_boolean_mask = false;
		u8 m_forwarded_boolean_guest = 0;
		u32 m_forwarded_boolean_producer_index = 0;
		bool m_resident_raw_gpr0_qword = false;
		u8 m_resident_raw_gpr0_entry_instructions = 0;
		bool m_compatible_raw_gpr0_qword = false;
		u8 m_compatible_raw_gpr0_entry_instructions = 0;
		bool m_resident_vtlb_qword_pointer = false;
		bool m_resident_cycle_low = false;
		bool m_resident_scheduler_countdown = false;
		bool m_compatible_scheduler_countdown = false;
		bool m_compatible_vtlb_pointer = false;
		bool m_compatible_vtlb_pointer_access = false;
		bool m_compatible_vtlb_write_pointer_access = false;
		bool m_compatible_vtlb_static_page_access = false;
		bool m_compatible_predicate_consumer = false;
		bool m_compatible_predicate_entry_variant = false;
		bool m_compatible_likely_taken_suffix = false;
		bool m_compatible_signed_byte_likely_self = false;
		bool m_branch_likely_predicate_flags_valid = false;
		u8 m_compatible_predicate_resident_host = PredicateLinkMapping::NO_HOST;
		size_t m_compatible_predicate_canonical_skip_delay = static_cast<size_t>(-1);
		size_t m_compatible_predicate_canonical_enter_delay = static_cast<size_t>(-1);
		size_t m_compatible_link_entry_offset = static_cast<size_t>(-1);
		CompatibleVtlbFastEntryOffsets m_compatible_vtlb_fast_entries{};
		bool m_reclaimed_vtlb_link_hosts = false;
		size_t m_compatible_vtlb_pointer_unaligned_fallback = static_cast<size_t>(-1);
		size_t m_compatible_vtlb_pointer_handler_fallback = static_cast<size_t>(-1);
		size_t m_compatible_vtlb_byte_pair_tail_index = static_cast<size_t>(-1);
		GprPinDirtyMasks m_compatible_vtlb_pointer_dirty_pins{};
		size_t m_compatible_vtlb_write_pointer_handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks m_compatible_vtlb_write_pointer_dirty_pins{};
		u8 m_compatible_vtlb_pointer_guard_instructions = 0;
		u8 m_compatible_vtlb_pointer_translation_instructions = 0;
		bool m_deferred_resident_unsigned_branch_suffix = false;
		bool m_emitting_deferred_resident_event_suffix = false;
		u32 m_deferred_resident_unsigned_compare_op = 0;
		u32 m_deferred_resident_delay_op = 0;
		u32 m_resident_vtlb_qword_store_op = 0;
		size_t m_resident_vtlb_qword_guard_offset = static_cast<size_t>(-1);
		size_t m_resident_vtlb_qword_handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks m_resident_vtlb_qword_dirty_pins{};
		u8 m_resident_vtlb_qword_guard_instructions = 0;
		u8 m_resident_vtlb_qword_translation_instructions = 0;
		u32 m_current_opcode = 0;
		u32 m_current_block_start_pc = 0;
		u32 m_current_block_instruction_count = 0;
		u32 m_current_instruction_index = 0;
		const void* m_current_direct_exit = nullptr;
		const void* m_current_indirect_lookup_pages_slot = nullptr;
		const void* m_current_direct_linking_enabled_flag = nullptr;
		static constexpr unsigned MAX_GPR_QCACHE = 8;
		u8 m_gpr_q_cache_guest[MAX_GPR_QCACHE]{};
		u8 m_gpr_q_cache_qreg[MAX_GPR_QCACHE]{};
		u8 m_gpr_q_cache_count = 0;
		u8 m_staged_gpr_q_cache_guest[MAX_GPR_QCACHE]{};
		u8 m_staged_gpr_q_cache_count = 0;
		size_t m_gpr_q_cache_next_use_distance_count = 0;
	};
} // namespace VitaEE
