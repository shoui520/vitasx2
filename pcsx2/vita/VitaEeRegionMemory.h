// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/VitaEeRegionIR.h"

#include <string>
#include <vector>

namespace VitaEE
{
	class BlockExecutor;
}

namespace VitaEE::RegionIR
{
	// Reference implementation of the direct-memory proof which the Phase 3
	// A32 backend will inline. It accepts only one-page, non-handler mappings
	// backed by retail EE RAM. Stores additionally require every touched
	// 64-byte source chunk to be unowned before the write occurs.
	struct VtlbMemoryContext
	{
		const u8* ram_source_page_live_flags = nullptr;
		const u8* ram_source_chunk_live_bits = nullptr;
	};

	VtlbMemoryContext MakeVtlbMemoryContext(const VitaEE::BlockExecutor& executor);
	RegionMemoryInterface MakeVtlbMemoryInterface(VtlbMemoryContext* context);
	MemoryProbeResult ProbeVtlbMemory(void* context,
		const MemoryRequest& request);
} // namespace VitaEE::RegionIR

namespace VitaEE::RegionMemoryPlan
{
	enum class TerminationKind : u8
	{
		UnsignedLess,
		EqualEndpoint,
		// A distinct signed recurrence reaches zero on the backedge.  This is the
		// ordinary memset/fill shape where the memory pointer advances while a
		// remaining-count GPR decrements.
		DecrementToZero,
		// A signed post-decrement recurrence continues while the updated value is
		// nonnegative.  For an admitted nonnegative low-word entry and a positive
		// power-of-two step, the exact do-while trip count is floor(entry / step)+1.
		DecrementWhileNonNegative,
		// A signed post-decrement recurrence continues while the updated value is
		// positive. For an admitted positive low-word entry and a positive
		// power-of-two step, the exact do-while trip count is ceil(entry / step).
		DecrementWhilePositive,
	};

	struct Access
	{
		RegionIR::ValueId operation = RegionIR::INVALID_VALUE;
		u32 block = RegionIR::INVALID_BLOCK;
		s32 induction_offset = 0;
		u32 width = 0;
		bool store = false;
	};

	// The single loop-control proof shared by every affine memory stream in a
	// plan.  Keeping this separate is both a soundness property (streams from
	// different candidate counters cannot be combined) and a Cortex-A9 cost
	// property: trip count and aggregate event horizon are computed once.
	struct LoopControl
	{
		bool valid = false;
		u32 header_block = RegionIR::INVALID_BLOCK;
		u8 counter_gpr = 0;
		u8 bound_gpr = 0;
		// An acyclic region prefix may establish the loop counter independently of
		// canonical entry state (the common SDK shape is `li counter, 4`).  Keep
		// that source-proven header value explicit: entry guards must never read the
		// stale architectural GPR merely because the loop recurrence names it.
		bool counter_seed_is_immediate = false;
		u32 counter_seed_immediate = 0;
		// SLTI/SLTIU counted loops compare against an immediate rather than a
		// second architectural GPR.  Retaining that decoded fact lets the entry
		// certificate derive the exact trip count without manufacturing guest
		// state or rereading the source opcode in the backend.
		bool bound_is_immediate = false;
		u32 bound_immediate = 0;
		s32 counter_stride = 0;
		bool signed_counter_compare = false;
		u8 trip_count_adjustment = 0;
		u32 maximum_iteration_scaled_cycles = 0;
		TerminationKind termination = TerminationKind::UnsignedLess;

		bool operator==(const LoopControl&) const = default;
	};

	// Derive the exact trip count when every dynamic loop-control input was
	// reduced to a source-proven header constant.  This is shared by admission
	// and A32 emission so their decrement/increasing-endpoint arithmetic cannot
	// drift apart.
	bool CalculateImmediateTripCount(const LoopControl& control, u32* trip_count);
	// Pure reference form of the same admitted low-word contract. Callers supply
	// the header values after proving the required zero-extension/high-word entry
	// guards. Immediate seeds/bounds in the descriptor override the arguments.
	bool CalculateTripCount(const LoopControl& control, u32 counter_seed,
		u32 bound, u32* trip_count);

	// One affine pointer family governed by Plan::control. All listed accesses
	// use the same low-32-bit entry GPR and byte stride. The generated entry
	// guard applies the shared trip count to the minimum/maximum offsets, proves
	// mapping and ownership for the complete range, and then removes the exact
	// per-access VTLB/SMC checks from the loop body.
	struct CountedRange
	{
		bool valid = false;
		u32 header_block = RegionIR::INVALID_BLOCK;
		// The loop induction register can be established by an acyclic region
		// prefix from a different canonical entry GPR.  Keep that exact affine
		// seed beside the recurrence instead of making the entry guard read the
		// stale same-numbered architectural register.  The zero value is invalid;
		// ordinary entry-header loops use induction_gpr with an offset of zero.
		u8 entry_induction_gpr = 0;
		s32 entry_induction_offset = 0;
		// The ordinary form uses induction_gpr itself as the pointer.  A common
		// array-indexed loop instead addresses invariant_base_gpr +
		// (induction_gpr << induction_scale_shift) + displacement.  Keeping that
		// relationship explicit lets the entry guard prove the complete span while
		// the loop body retains its original effective-address arithmetic.
		u8 invariant_base_gpr = 0;
		u8 induction_gpr = 0;
		u8 induction_scale_shift = 0;
		u32 stride = 0;
		u32 alignment = 1;
		s32 minimum_offset = 0;
		// End of the furthest access relative to one iteration's pointer.  This is
		// used with a proven trip count when the loop-control GPR is not the pointer.
		s32 maximum_offset_end = 0;
		std::vector<Access> accesses;
	};

	// One invariant base plus a data-dependent offset whose complete unsigned
	// range is proven from Region IR (initially zero-extended byte/halfword loads,
	// masks, shifts, and adds). Unlike CountedRange, its extent does not grow with
	// the trip count. The generated entry guard validates this conservative table
	// span once; every listed access can then bypass its mid-loop VTLB exit.
	struct BoundedRange
	{
		bool valid = false;
		u32 header_block = RegionIR::INVALID_BLOCK;
		// A base-free address proven entirely from Region IR. The persistent A32
		// backend may use it only when its complete low-RAM identity contract is
		// active; other backends discard this optimization and retain the ordinary
		// translation path.
		bool absolute_address = false;
		u8 base_gpr = 0;
		u32 alignment = 1;
		s32 minimum_offset = 0;
		s32 maximum_offset_end = 0;
		std::vector<Access> accesses;
	};

	// Exact certificate for one natural-loop iteration. The body is
	// acyclic after removing its sole backedge, so this conservative maximum
	// proves that source-block event polls inside the current iteration cannot
	// observe anything. Product A32 re-proves the following iteration at the
	// backedge and falls back before entering it when the horizon is too close.
	struct IterationTiming
	{
		bool valid = false;
		u32 header_block = RegionIR::INVALID_BLOCK;
		u32 backedge_source_block = RegionIR::INVALID_BLOCK;
		u32 maximum_scaled_cycles = 0;
	};

	struct Plan
	{
		IterationTiming timing{};
		// Entry guards run before the region preheader. An internal-header loop may
		// use them only when every required seed is either a source-proven constant
		// represented in LoopControl or an affine canonical-entry mapping represented
		// by its CountedRange.  This flag records that the complete selected plan has
		// such an entry representation; it does not imply same-numbered GPRs.
		bool header_seeds_match_entry = false;
		// The natural-loop timing/control certificate is useful independently of
		// affine memory.  It authorizes one aggregate event-horizon check and a
		// structural profitability guard for ordinary counted loops.  Memory
		// ranges, when present, additionally remove per-access VTLB/SMC work.
		LoopControl control{};
		// Disjoint affine streams governed by the same natural-loop continuation.
		// An operation appears in at most one range. Unlisted operations retain the
		// ordinary per-access VTLB, alignment, and source-ownership checks.
		std::vector<CountedRange> counted_ranges;
		// Invariant-base, bounded-index streams governed by the same loop/event
		// certificate. Operations already covered by counted_ranges never appear
		// here.
		std::vector<BoundedRange> bounded_ranges;
	};

	enum class BuildFailure : u8
	{
		None,
		InvalidProgram,
		NotCountedAffine,
	};

	struct BuildResult
	{
		Plan plan{};
		BuildFailure failure = BuildFailure::None;
		// Cold diagnostic classification for a valid program whose memory proof
		// could not select one reducible ownership unit. Zero means reducibility
		// passed (or was not reached), one is CFG selection, and two is the
		// stricter single-latch/acyclic-body contract. This never affects admission.
		u8 reducibility_rejection = 0;
		std::string detail;

		explicit operator bool() const { return failure == BuildFailure::None; }
	};

	// Cortex-A9-local break-even evidence derived solely from the verified loop
	// control and memory effects.  This is the single authority used both by the
	// cold runtime triage and by final A32 publication: triage may avoid spending a
	// scarce compile token, but can never admit a shape which the backend rejects.
	struct A9ProfitabilityCertificate
	{
		// The first-class persistent-dispatch benchmark crosses the 2x gate for
		// a scalar read-only multi-block loop at 4096 complete iterations.  Keep
		// this independently measured target floor separate from the store-stream
		// and direct-call-boundary models below.
		static constexpr u32 DEFAULT_MINIMUM_READ_ONLY_ITERATIONS = 4096;
		// A branchy three-block scalar CFG with all memory fallibility, including
		// store ownership, moved to entry is safely beyond 2x after roughly 8192
		// decoded guest instructions on the physical Cortex-A9. Express the floor as
		// work so larger generic regions need fewer observed latches without keying
		// admission on one source image.
		static constexpr u32 DEFAULT_MINIMUM_OBSERVED_PREFLIGHTED_WORK = 8192;
		// Four-block reducible COP1 fixtures with lazy O/U exceptional veneers
		// cross the complete first-class persistent-dispatch 2x gate at 256
		// iterations on the physical Cortex-A9.  Require at least 4,608 decoded
		// guest instructions so product admission stays beyond that measured point
		// without depending on one source image or fixed loop count.
		static constexpr u32 DEFAULT_MINIMUM_OBSERVED_COP1_WORK = 4608;
		// The complete first-class persistent-dispatch VU0 affine FMAC stream is
		// exact and measures 1.47x/2.25x/2.60x at one/four/eight iterations on the
		// physical Cortex-A9.  Four is therefore the independently measured 2x
		// product floor; keep it separate because vector-state and pre-entry idle
		// costs are not interchangeable with scalar memory work.
		static constexpr u32 DEFAULT_MINIMUM_OBSERVED_VU0_FMAC_ITERATIONS = 4;
		// A directly called affine VU0 transform leaf remains inside the persistent
		// dispatcher, but unlike a natural loop has no architectural trip count to
		// guard.  The physical Cortex-A9 fixture measures the complete generated-
		// lookup/JR-return lifecycle, including exact per-access LQC2/SQC2 checks,
		// over 512 calls. Require that many generated entry observations before
		// replacing tier zero. This is a heat certificate; the semantic certificate
		// below remains independent of source identity.
		static constexpr u32 DEFAULT_MINIMUM_OBSERVED_VU0_FMAC_LEAF_INVOCATIONS = 512;

		// Event-scoped admission is expressed in decoded guest work, not the exact
		// size of the first retail loop which proved the mechanism.  The global
		// RegionIR/source/code limits remain authoritative bounds; this certificate
		// only proves that the selected natural-loop cost class moves every fallible
		// repeated memory operation to its entry preflight.

		enum class Kind : u8
		{
			None,
			CountedMemory,
			ObservedBoundaryElision,
			ReadOnlyLoop,
			ObservedPreflightedLoop,
			ObservedCop1Residency,
			ObservedVu0FmacStream,
			ObservedVu0AcyclicFmacLeaf,
			// Appended so existing telemetry kind values remain stable. This is the
			// independently measured repeated unit which retains both basic lazy-O/U
			// COP1 arithmetic and multiple direct affine VU0 FMAC leaves.
			ObservedCop1Vu0Residency,
		};

		// Cold-only structural evidence explaining why a verified COP1/VU0 unit
		// did not enter one of the independently measured Cortex-A9 cost classes.
		// These bits are diagnostic output, never admission inputs. Keep each cause
		// orthogonal so one bounded product run can expose every missing category
		// instead of reporting whichever fail-closed test happened to run first.
		enum DiagnosticFlag : u32
		{
			DiagnosticNone = 0,
			DiagnosticUnsupportedShape = 1u << 0,
			DiagnosticNonRepeatedState = 1u << 1,
			DiagnosticUnsupportedVu0Semantic = 1u << 2,
			DiagnosticUnsupportedVu0Binding = 1u << 3,
			DiagnosticUnsupportedCop1Semantic = 1u << 4,
			DiagnosticObserverMismatch = 1u << 5,
			DiagnosticIncompleteFmacGraph = 1u << 6,
			DiagnosticIncompleteCop1Graph = 1u << 7,
			DiagnosticUncoveredMemory = 1u << 8,
			DiagnosticDirectCallCoverage = 1u << 9,
			DiagnosticMissingVectorIo = 1u << 10,
			DiagnosticIdleCoverage = 1u << 11,
			DiagnosticBelowMeasuredFloor = 1u << 12,
		};

		Kind kind = Kind::None;
		u32 diagnostic_flags = DiagnosticNone;
		u32 work_per_iteration = 0;
		u32 repeated_blocks = 0;
		u32 covered_scalar_loads = 0;
		u32 minimum_profitable_iterations = 0;
		u64 maximum_iterations = 0;
		bool has_unconditional_store = false;
		bool has_header_vector_access = false;
		bool maximum_iterations_known = false;

		explicit operator bool() const
		{
			if (kind == Kind::ObservedBoundaryElision)
				return minimum_profitable_iterations != 0;
			if (kind == Kind::ReadOnlyLoop)
				return minimum_profitable_iterations != 0 &&
					(!maximum_iterations_known ||
					 maximum_iterations >= minimum_profitable_iterations);
			if (kind == Kind::ObservedPreflightedLoop ||
				kind == Kind::ObservedCop1Residency ||
				kind == Kind::ObservedVu0FmacStream ||
				kind == Kind::ObservedVu0AcyclicFmacLeaf ||
				kind == Kind::ObservedCop1Vu0Residency)
				return minimum_profitable_iterations != 0;
			return kind == Kind::CountedMemory &&
			       minimum_profitable_iterations != 0 &&
			       has_unconditional_store &&
			       (!maximum_iterations_known ||
			        maximum_iterations >= minimum_profitable_iterations);
		}
	};

	A9ProfitabilityCertificate BuildA9ProfitabilityCertificate(
		const RegionIR::Program& program, const Plan& plan,
		u32 minimum_counted_memory_work,
		u32 minimum_observed_region_work,
		bool use_measured_scalar_floor = true,
		u32 minimum_read_only_iterations =
			A9ProfitabilityCertificate::DEFAULT_MINIMUM_READ_ONLY_ITERATIONS,
		u32 minimum_observed_vu0_fmac_iterations =
			A9ProfitabilityCertificate::DEFAULT_MINIMUM_OBSERVED_VU0_FMAC_ITERATIONS,
		u32 minimum_observed_vu0_fmac_leaf_invocations =
			A9ProfitabilityCertificate::
				DEFAULT_MINIMUM_OBSERVED_VU0_FMAC_LEAF_INVOCATIONS);

	// The caller must already have passed RegionIR::Verify. Failure to prove a
	// range is an ordinary optimization miss, never a program-support decision.
	BuildResult Build(const RegionIR::Program& program);
} // namespace VitaEE::RegionMemoryPlan
